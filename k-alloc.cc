#include "kernel.hh"
#include "k-lock.hh"

static spinlock page_lock;
// static uintptr_t next_free_pa;

// should both be initialized in init_kalloc
// need to be protected by lock
static size_t total_physpages = 0;
static size_t allocated_pages = 0;

// buddy allocator
const int min_order = 12;
const int max_order = 21;

// struct free_list_entry {
//     const uintptr_t block_start_;
//     list_links link_;

//     free_list_entry(uintptr_t block_start)
//         : block_start_(block_start) {
//     }
// };

struct page_entry {
    bool allocatable = false; // whether the page is in a range marked as available (i.e. not reserved or kernel)
    bool free;
    int order;
    list_links link_;
};

// page is in a reserved/kernel range:
//  allocatable = false
//  (other fields invalid)
// page is in an available range and starts an order block:
//  allocatable = true
//  free = true or false
//  order = (relevant order)
// page is in an order block but doesn't start it:
//  allocatable = true
//  free = (same as corresponding order block)
//  order = -1
// page is at the edge of an available range beyond any aligned order blocks:
//  allocatable = true
//  free = true (defaults to this and should never be touched during allocation)
//  order = -1

page_entry pages[MEMSIZE_PHYSICAL / PAGESIZE];
list<page_entry, &page_entry::link_> free_lists[max_order + 1];

uintptr_t addr_from_entry(page_entry* entry) {
    uintptr_t addr_diff = reinterpret_cast<uintptr_t>(entry) - reinterpret_cast<uintptr_t>(&pages[0]);
    uintptr_t page_diff = addr_diff / sizeof(page_entry);
    assert(addr_diff % sizeof(page_entry) == 0);
    return page_diff * PAGESIZE;
}

// Test functions

void validate_free(uintptr_t addr) {
    assert((addr & PAGEOFFMASK) == 0);
    page_entry* header = &pages[addr / PAGESIZE]; // not copyable
    assert(header->allocatable);
    assert(header->free);
    assert(header->order >= min_order && header->order <= max_order);
    // assert(header.link_);
    size_t block_sz_bytes = 1u << header->order;
    size_t block_sz_pages = block_sz_bytes / PAGESIZE;
    for (size_t pg_offset = 1; pg_offset < block_sz_pages; pg_offset++) {
        page_entry* entry = &pages[(addr / PAGESIZE) + pg_offset];
        assert(entry->allocatable);
        if (!entry->free) {
            log_printf("FAILURE: pg at %p (offset from order %i block, offset %zu pages) NOT FREE\n", (addr / PAGESIZE) + pg_offset, header->order, pg_offset);
            log_printf("Buddy page number: %p\n", addr / PAGESIZE);
        }
        assert(entry->free);
        assert(entry->order == -1);
        // assert(!entry.link_);
    } // for now this is the canonical way to iterate over `pages`
}

void validate_allocated(uintptr_t addr) {
    assert((addr & PAGEOFFMASK) == 0);
    page_entry* header = &pages[addr / PAGESIZE]; // not copyable
    assert(header->allocatable);
    assert(!header->free);
    assert(header->order >= min_order && header->order <= max_order);
    size_t block_sz_bytes = 1u << header->order;
    size_t block_sz_pages = block_sz_bytes / PAGESIZE;
    for (size_t pg_offset = 1; pg_offset < block_sz_pages; pg_offset++) {
        page_entry* entry = &pages[(addr / PAGESIZE) + pg_offset];
        assert(entry->allocatable);
        assert(!entry->free);
        assert(entry->order == -1);
    }
}

void validate_free_lists() {
    log_printf("Validating `free_lists`\n");
    for (int order = min_order; order <= max_order; order++) {
        for (page_entry* entry = free_lists[order].front();
             entry != nullptr;
             entry = free_lists[order].next(entry)) {
            if (entry) {
                // log_printf("Found free list with entry of value %zu\n", addr_from_entry(entry));
                validate_free(addr_from_entry(entry));
            }
        }
    }
}

// Validate all pages in the array `pages`. That is, iterate through all pages and check that they
// are either validly non-allocatable, the header of a free or allocated buddy, or the member of a
// free or allocated buddy.
// Also checks some other invariants.
void validate_all_pages() {
    log_printf("Validating `pages`\n");
    uintptr_t addr = 0;
    while (addr < MEMSIZE_PHYSICAL) {
        assert((addr & PAGEOFFMASK) == 0);
        page_entry* entry = &pages[addr / PAGESIZE];
        if (!entry->allocatable) {
            addr += PAGESIZE;
            continue;
        }
        assert(entry->order >= min_order && entry->order <= max_order);
        int addr_limited_order = lsb(addr) - 1;
        assert(entry->order <= addr_limited_order);
        if (entry->free) {
            log_printf("Found free block of order %i at %p\n", entry->order, addr);
            validate_free(addr);
            // if this might be the first of two buddies...
            if (entry->order < max_order && addr % (1u << (entry->order + 1)) == 0) {
                // and it also isn't as large as it can be given its alignment...
                if (entry->order < addr_limited_order) {
                    // it must have a buddy, or its buddy would be outside the range
                    uintptr_t buddy_addr = (addr + (1u << entry->order));
                    page_entry* buddy = &pages[buddy_addr / PAGESIZE];
                    assert(!buddy->allocatable || buddy->free);
                }
            }
        } else {
            log_printf("Found allocated block of order %i at %p\n", entry->order, addr);
            validate_allocated(addr);
        }
        addr += (1u << entry->order);
    }
    log_printf("Validation of `pages` succeeded.\n");
}

// Helper functions for init_kalloc

void mark_range(const memrange* range) {
    for (uintptr_t i = range->first(); i < range->last(); i += PAGESIZE) {
        assert((i & PAGEOFFMASK) == 0);
        pages[i / PAGESIZE].allocatable = true;
        pages[i / PAGESIZE].free = true;
        pages[i / PAGESIZE].order = -1;
    }
}

void make_order_blocks(const memrange* range) {
    // Need to freaking round up to the next address that is a power of 2, check if it is within the range, then take the largest order of 2 which fits within available space and also evenly divides the new starting address.
    uintptr_t addr = range->first();
    while (addr < range->last()) {
        assert((addr & PAGEOFFMASK) == 0);
        int space_limited_order = msb(range->last() - addr) - 1;
        int addr_limited_order = lsb(addr) - 1; // least significant bit = order of 2 this divides evenly by
        int order = min(space_limited_order, addr_limited_order);
        order = min(order, max_order);
        log_printf("space order %u\n", space_limited_order);
        log_printf("addr order %u\n", addr_limited_order);
        log_printf("found existent range of order %u\n", order);
        if (order >= min_order) {
            // log_printf("starting at: %p\n", aligned_addr);
            // log_printf("dividing by: %p\n", 1u << order);
            log_printf("found available block of size %zu pages\n", (1u << order) / PAGESIZE);
            log_printf("block starts at: %zu\n", addr);
            assert(addr % (1u << order) == 0);
            // Update free lists
            page_entry* entry = &pages[addr / PAGESIZE];
            free_lists[order].push_front(entry);
            // Update pages array
            entry->order = order;

            addr += (1u << order);
        } else {
            panic("Order too small\n");
        }
    }
}


// init_kalloc
//    Initialize stuff needed by `kalloc`. Called from `init_hardware`,
//    after `physical_ranges` is initialized.
void init_kalloc() {
    spinlock_guard guard(page_lock);
    // log_printf("order of physical memory: %zu\n", msb(MEMSIZE_PHYSICAL) - 1);
    // iterate using physical ranges, increment both total_physpages and allocated_pages
    auto range = physical_ranges.begin();
    while (range != physical_ranges.end() && range->first() < MEMSIZE_PHYSICAL
           ) {
        log_printf("Range start: %p\n", range->first());
        assert((range->size() & PAGEOFFMASK) == 0); // assert assumption that ranges are in size of pages
        size_t range_pgsz = range->size() / PAGESIZE;
        if (range->type() == mem_nonexistent) {
            log_printf("Woah! I found a nonexistent range of size %zu pages\n", range_pgsz);
        } else if (range->type() == mem_available) {            
            mark_range(range); // for now this will be true even for pages where order alignment prevents them from being used
            make_order_blocks(range);
        } else {
            log_printf("found reserved range of size %zu pages\n", range_pgsz);
            allocated_pages += range_pgsz;
        }
        total_physpages += range_pgsz; // may lead to size mismatch with actual usable physical memory (since the available ranges need to start at powers of 2)
        // move to next range
        ++range;
    }

    log_printf("Final checks:\n");
    for (uintptr_t pg = 0; pg < MEMSIZE_PHYSICAL / PAGESIZE; pg++) {
        if (pages[pg].allocatable && pages[pg].order != -1) {
            // assert(pages[pg].free);
            log_printf("Found page of order %u\n", pages[pg].order);
            log_printf("at page %zu, addr %p\n", pg, pg * PAGESIZE);
        }
    }
    
    // free_list_entry* bad_entry = knew<free_list_entry>(0x1000);    
    // free_lists[13].push_front(bad_entry);
    validate_free_lists();
    assert(max_order >= min_order);
    assert(max_order >= 21);
    log_printf("kalloc initialized successfully\n");
}

size_t kget_total_physpages() {
    spinlock_guard guard(page_lock);
    return total_physpages;
}

size_t kget_allocated_physpages() {
    spinlock_guard guard(page_lock);
    return allocated_pages;
}

// Split the buddy starting at `addr` into two buddies. Caller must hold `page_lock`.
void split_buddy(uintptr_t addr) {
    assert(page_lock.is_locked());
    // SPLIT:
    //   (to be super paranoid: check that every page within this buddy apart from the first is allocatable, free, and order == -1)
    validate_free(addr);
    
    //  (pages) deincrement this buddy's order
    page_entry* first_buddy_header = &pages[addr / PAGESIZE];
    int new_order = first_buddy_header->order - 1;
    assert(new_order >= min_order);
    //  (pages) find the midway point, assert allocatable, free, and order == -1, change order to this buddy's order
    uintptr_t second_buddy_addr = addr + (1u << new_order);
    page_entry* second_buddy_header = &pages[second_buddy_addr / PAGESIZE];
    assert(second_buddy_header->allocatable
           && second_buddy_header->free
           && second_buddy_header->order == -1);
    first_buddy_header->order = new_order;
    second_buddy_header->order = new_order;
    
    //  remove addr from list; add addr and midway point to list of order below
    // free_list_entry* first_list_entry = first_buddy_header.list_entry;
    // free_list_entry* second_list_entry = knew<free_list_entry>(second_buddy_addr);
    first_buddy_header->link_.erase(); // remove entry from old order's list
    free_lists[new_order].push_front(first_buddy_header);
    free_lists[new_order].push_front(second_buddy_header);
    validate_free(addr);
}

// Claim the buddy located at `addr`. Caller must hold `page_lock`.
void take_buddy(uintptr_t addr) {
    assert(page_lock.is_locked());
    // TAKE:
    // mark all pages within buddy as non-free, remove ptr from free list, then return ptr
    // increment allocated_pages
    validate_free(addr);
    
    log_printf("Taking buddy starting at %p\n", addr);
    page_entry* header = &pages[addr / PAGESIZE];
    header->free = false;

    size_t block_sz_bytes = (1u << header->order);
    size_t block_sz_pages = block_sz_bytes / PAGESIZE;
    for (size_t pg_offset = 1; pg_offset < block_sz_pages; pg_offset++) {
        assert(false); // this should not happen until things larger than a pagesize can be allocated
        pages[(addr / PAGESIZE) + pg_offset].free = false;
    }

    validate_allocated(addr);
    
    header->link_.erase(); // remove from free list
        
    // tell sanitizers the allocated page is accessible
    asan_mark_memory(addr, block_sz_bytes, false);
    // initialize to `int3`
    memset(pa2kptr<void*>(addr), 0xCC, block_sz_bytes);
    // update stats
    allocated_pages += block_sz_pages;
}

// kalloc(sz)
//    Allocate and return a pointer to at least `sz` contiguous bytes of
//    memory. Returns `nullptr` if `sz == 0` or on failure.
//
//    The caller should initialize the returned memory before using it.
//    The handout allocator sets returned memory to 0xCC (this corresponds
//    to the x86 `int3` instruction and may help you debug).
//
//    If `sz` is a multiple of `PAGESIZE`, the returned pointer is guaranteed
//    to be page-aligned.
//
//    The handout code does not free memory and allocates memory in units
//    of pages.
void* kalloc(size_t sz) {    
    if (sz == 0 || sz > PAGESIZE) {
        return nullptr;
    }
    
    auto irqs = page_lock.lock();
    uintptr_t addr = 0;
    
    // basically we find a buddy, split as needed, and then take it

    // iterate over free_lists from target_order to max_order (inclusive)
    int target_order = max(msb(sz) - 1, min_order); // require at least min_order
    int order = target_order;
    while (order <= max_order) {
        //  if we find a non-empty free list, set ptr to the first list entry and break
        page_entry* entry = free_lists[order].front();
        if (entry) {
            addr = addr_from_entry(entry);
            break;
        }
        // log_printf("no blocks of order %i...\n", order);
        order++;
    }
    // if ptr is still null, return ptr (null)
    if (!addr) {
        log_printf("Out of memory\n");
        assert(false);
        page_lock.unlock(irqs);
        return nullptr;
    }
    // if order is greater than target, split buddies!!!
    //  deincrement order, rinse and repeat until order == target (should always work)
    while (order > target_order) {
        split_buddy(addr);
        order--;
    }
    take_buddy(addr);
    
    validate_free_lists();
    page_lock.unlock(irqs);
    return pa2kptr<void*>(addr);
}

// free an individual buddy. Should be followed by calling `merge_buddies(addr)` until no more merges are possible. Caller should hold `page_lock`.
void give_buddy(uintptr_t addr) {
    assert(page_lock.is_locked());
    validate_allocated(addr);
    
    //  (pages) mark all pages in buddy as free, (list) add it to the appropriate free list
    page_entry* header = &pages[addr / PAGESIZE];
    header->free = true;
    // log_printf("Giving back a buddy of size %zu\n", 1u << header->order);
    
    size_t block_sz_bytes = 1u << header->order;
    size_t block_sz_pages = block_sz_bytes / PAGESIZE;
    for (size_t pg_offset = 1; pg_offset < block_sz_pages; pg_offset++) {
        pages[(addr / PAGESIZE) + pg_offset].free = true;
    }

    free_lists[header->order].push_front(header);
    
    // log_printf("I'd like to free %p\n", addr);

    // tell sanitizers to poison range
    asan_mark_memory(addr, block_sz_bytes, true);
    // update stats
    allocated_pages -= block_sz_pages;    
}

// Try to merge two buddies, one of which is pointed to by `addr`. Functions the same way whether you pick the first or second buddy.
// Upon successful merge, return the first address of the new merged buddy. If the other buddy to merge isn't free, return 0.
// This should never unironically return 0: the zero page should never be the header of a valid buddy.
// Caller should hold `page_lock`.
uintptr_t merge_buddies(uintptr_t addr) {
    assert(page_lock.is_locked());
    validate_free(addr); // the one given should be free
    // Merge(ptr):
    int old_order = pages[addr / PAGESIZE].order;
    int new_order = old_order + 1;
    //  (pages) find root buddy; this involves dividing and re-multiplying ptr by 2^(next order) (make sure to convert to pages)
    uintptr_t first_buddy_addr = (addr >> new_order) << new_order;
    //   then find next buddy by adding 2^(order)
    uintptr_t second_buddy_addr = first_buddy_addr + (1ul << old_order);
    assert((first_buddy_addr == addr) != (second_buddy_addr == addr)); // just checking my math. (using != as XOR)
    //   check whether both buddy headers are free and the same order
    page_entry* first_entry = &pages[first_buddy_addr / PAGESIZE];
    page_entry* second_entry = &pages[second_buddy_addr / PAGESIZE];
    assert(first_entry->free || second_entry->free); // this is really paranoid
    if (!first_entry->allocatable || !second_entry->allocatable) {
        return 0;
    }
    if (first_entry->order != second_entry->order) {
        return 0;
    }
    if (!first_entry->free || !second_entry->free) {
        //   one isn't free: return nullptr
        return 0;
    }
    //   both are free:
    log_printf("about to assert on %p...\n", first_buddy_addr);
    validate_free(first_buddy_addr);
    log_printf("done asserting\n");
    validate_free(second_buddy_addr);
    //    (pages) set beginning header to (next order) and midpoint header to order -1
    assert(new_order = first_entry->order + 1);
    first_entry->order = new_order;
    second_entry->order = -1;
    //    (list) remove list entries for both buddies; add one entry to the (next order) list
    first_entry->link_.erase();
    second_entry->link_.erase();
    free_lists[new_order].push_front(first_entry);
    //    (paranoia: free check on new merged block)
    validate_free(first_buddy_addr);
    log_printf("Validated that %p is free\n", first_buddy_addr);
    //    (can also have an (externally callable?) global check to see if the data structures exactly match)
    //    return the start address of the new merged block
    return first_buddy_addr;
    //    (paranoia: at least one of the buddies should be free, as in its header is allocatable, free, and order != -1, and the other should be either free or correctly allocated: alloctable, all pages non-free, and order != -1 only on the header)
}

// kfree(ptr)
//    Free a pointer previously returned by `kalloc`. Does nothing if
//    `ptr == nullptr`.
void kfree(void* ptr) {
    // if (ptr) {
    //     // tell sanitizers the freed page is inaccessible
    //     asan_mark_memory(ka2pa(ptr), PAGESIZE, true);
    // }

    if (!ptr) {
        return;
    }
    auto irqs = page_lock.lock();

    uintptr_t addr = reinterpret_cast<uintptr_t>(ka2pa(ptr));
    // In this case the situation is reversed. We give, and then merge as needed.
    give_buddy(addr);
    // Rinse and repeat merging
    int merges = 0;
    log_printf("On merge %i, merging at %p...\n", merges, addr);
    addr = merge_buddies(addr);
    while (addr) {
        if (merges > max_order - min_order) {
            panic("Too many merges");
        }
        merges++;
        log_printf("On merge %i, merging at %p...\n", merges, addr);
        // !!!!!! There is a race condition going on before this call, because a merge on the address returned by a merge should never assert fail.
        addr = merge_buddies(addr);
    }
    
    validate_free_lists();
    
    page_lock.unlock(irqs);
}


// operator new, operator delete
//    Expressions like `new (std::nothrow) T(...)` and `delete x` work,
//    and call kalloc/kfree.
void* operator new(size_t sz, const std::nothrow_t&) noexcept {
    return kalloc(sz);
}
void* operator new(size_t sz, std::align_val_t, const std::nothrow_t&) noexcept {
    return kalloc(sz);
}
void* operator new[](size_t sz, const std::nothrow_t&) noexcept {
    return kalloc(sz);
}
void* operator new[](size_t sz, std::align_val_t, const std::nothrow_t&) noexcept {
    return kalloc(sz);
}
void operator delete(void* ptr) noexcept {
    kfree(ptr);
}
void operator delete(void* ptr, size_t) noexcept {
    kfree(ptr);
}
void operator delete(void* ptr, std::align_val_t) noexcept {
    kfree(ptr);
}
void operator delete(void* ptr, size_t, std::align_val_t) noexcept {
    kfree(ptr);
}
void operator delete[](void* ptr) noexcept {
    kfree(ptr);
}
void operator delete[](void* ptr, size_t) noexcept {
    kfree(ptr);
}
void operator delete[](void* ptr, std::align_val_t) noexcept {
    kfree(ptr);
}
void operator delete[](void* ptr, size_t, std::align_val_t) noexcept {
    kfree(ptr);
}
