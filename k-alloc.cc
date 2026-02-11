#include "kernel.hh"
#include "k-lock.hh"

static spinlock page_lock;
static uintptr_t next_free_pa;
// should both be initialized in init_kalloc
// ??? atomics?
static size_t total_physpages = 0;
static size_t allocated_pages = 0;

// buddy allocator
const int min_order = 12;
const int max_order = 21;

struct page_entry {
    bool allocatable; // whether the page is in a range marked as available (i.e. not reserved or kernel)
    bool free;
    int order;
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

page_entry pages[MEMSIZE_PHYSICAL / 2];

struct free_list_entry {
    const uintptr_t block_start_;
    list_links link_;

    free_list_entry(uintptr_t block_start)
        : block_start_(block_start) {
    }
};

list<free_list_entry, &free_list_entry::link_> free_lists[max_order + 1];




void validate_free(uintptr_t ptr) {
    assert((ptr & PAGEOFFMASK) == 0);
    page_entry header = pages[ptr / PAGESIZE];
    assert(header.allocatable);
    assert(header.free);
    assert(header.order >= min_order && header.order <= max_order);
    size_t block_sz_bytes = 1u << header.order;
    size_t block_sz_pages = block_sz_bytes / PAGESIZE;
    for (size_t pg_offset = 1; pg_offset < block_sz_pages; pg_offset++) {
        page_entry entry = pages[(ptr / PAGESIZE) + pg_offset];
        assert(entry.allocatable);
        assert(entry.free);
        assert(entry.order == -1);
    }
}

void validate_free_lists() {
    log_printf("Validating `free_lists`\n");
    for (int order = min_order; order <= max_order; order++) {
        for (free_list_entry* list_entry = free_lists[order].front();
             list_entry != nullptr;
             list_entry = free_lists[order].next(list_entry)) {
            if (list_entry) {
                log_printf("Found free list with entry of value %zu\n", list_entry->block_start_);
                validate_free(list_entry->block_start_);
            }
        }
    }
}

void mark_range(const memrange* range, bool allocatable) {
    for (uintptr_t i = range->first(); i < range->last(); i += PAGESIZE) {
        assert((i & PAGEOFFMASK) == 0);
        if (allocatable) {
            pages[i / PAGESIZE].allocatable = true;
            pages[i / PAGESIZE].free = true;
            pages[i / PAGESIZE].order = -1;
        } else {
            pages[i / PAGESIZE].allocatable = false;
            // all other members should never be accessed in this case
        }
    }
}

void make_order_blocks(const memrange* range) {
    // Need to freaking round up to the next address that is a power of 2, check if it is within the range, then take the largest order of 2 which fits within available space and also evenly divides the new starting address.
    uintptr_t aligned_addr = round_up_pow2(range->first());
    // log_printf("order aligned addr %p\n", aligned_addr);
    if (aligned_addr < range->last()) {
        assert(range->last() - aligned_addr <= range->size());
        int space_limited_order = msb(range->last() - aligned_addr) - 1;
        int addr_limited_order = msb(aligned_addr) - 1;
        int order = min(space_limited_order, addr_limited_order);
        order = min(order, max_order);
        // log_printf("space order %u\n", space_limited_order);
        // log_printf("addr order %u\n", addr_limited_order);
        log_printf("found existent range of order %u\n", order);
        if (order >= min_order) {
            // log_printf("starting at: %p\n", aligned_addr);
            // log_printf("dividing by: %p\n", 1u << order);
            assert(aligned_addr % (1u << order) == 0);
            log_printf("found existent range of size %zu pages\n", range->size() / PAGESIZE);
            assert((aligned_addr & PAGEOFFMASK) == 0);
            // Update pages array
            pages[aligned_addr / PAGESIZE].order = order;
            // Update free lists
            log_printf("Block starts at: %zu\n", aligned_addr);
            free_list_entry* entry = knew<free_list_entry>(aligned_addr);
            free_lists[order].push_front(entry);
        } else {
            log_printf("Order too small\n");
        }
    }
}

// init_kalloc
//    Initialize stuff needed by `kalloc`. Called from `init_hardware`,
//    after `physical_ranges` is initialized.
void init_kalloc() {
    // log_printf("order of physical memory: %zu\n", msb(MEMSIZE_PHYSICAL) - 1);
    // iterate using physical ranges, increment both total_physpages and allocated_pages
    auto range = physical_ranges.begin();
    while (range != physical_ranges.end()) {
        assert((range->size() & PAGEOFFMASK) == 0); // assert assumption that ranges are in size of pages
        size_t range_pgsz = range->size() / PAGESIZE;
        if (range->type() == mem_nonexistent) {
            // log_printf("Woah! I found a nonexistent range of size %zu pages\n", range_pgsz);
            mark_range(range, false);
        } else if (range->type() == mem_available) {            
            mark_range(range, true); // for now this will be true even for pages where order alignment prevents them from being used
            make_order_blocks(range);
        } else {
            // log_printf("found reserved range of size %zu pages\n", range_pgsz);
            allocated_pages += range_pgsz;
            mark_range(range, false);
        }
        total_physpages += range_pgsz; // may lead to size mismatch with actual usable physical memory (since the available ranges need to start at powers of 2)
        // move to next range
        ++range;
    }

    log_printf("Final checks:\n");
    for (uintptr_t pg = 0; pg < MEMSIZE_PHYSICAL / 2; pg++) {
        if (pages[pg].allocatable && pages[pg].order != -1) {
            assert(pages[pg].free);
            log_printf("Found page of order %u\n", pages[pg].order);
        }
    }
    
    // free_list_entry* bad_entry = knew<free_list_entry>(0x1000);    
    // free_lists[13].push_front(bad_entry);
    validate_free_lists();
    assert(max_order >= min_order);
    // !!! I can't get a sufficiently large order without modifying k-init to get even more starting memory
    assert(max_order >= 21);
}

size_t kget_total_physpages() {
    return total_physpages;
}

size_t kget_allocated_physpages() {
    return allocated_pages;
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
    void* ptr = nullptr;

    int target_order = max(msb(sz) - 1, min_order); // require min_order
    // iterate over free_lists from target_order to max_order (inclusive)
    //  if we find a non-empty free list, set ptr to the first list entry and break
    // if ptr is still null, return ptr (null)
    // if order is greater than target, split buddies!!!
    // SPLIT:
    //  {
    //  deincrement this buddy's order
    //  find the midway point, assert allocatable, free, and order == -1, change order to this buddy's order
    //   (to be super paranoid: check that every page within this buddy apart from the first is allocatable, free, and order == -1)
    //  remove ptr from list; add ptr and midway point to list of order below
    //  }
    //  deincrement order, rinse and repeat until order == target (should always work)
    // TAKE:
    // (if order is equal to target,) mark all pages within buddy as non-free, remove ptr from free list, then return ptr
    // increment allocated_pages

    // helper function void split_buddy(addr) (uses pages[addr / PAGESIZE] metadata)
    // helper function void take_buddy(addr)

    // basically we find, split as needed, and then take
    
    // OLD!!!!
    // skip over reserved and kernel memory
    auto range = physical_ranges.find(next_free_pa);
    while (range != physical_ranges.end()) {
        if (range->type() == mem_available) {
            // use this page
            ptr = pa2kptr<void*>(next_free_pa);
            next_free_pa += PAGESIZE;
            break;
        } else {
            // move to next range
            next_free_pa = range->last();
            ++range;
        }
    }

    page_lock.unlock(irqs);

    if (ptr) {
        // tell sanitizers the allocated page is accessible
        asan_mark_memory(ka2pa(ptr), PAGESIZE, false);
        // initialize to `int3`
        memset(ptr, 0xCC, PAGESIZE);
        // update stats
        ++allocated_pages;
    }
    return ptr;
}

// kfree(ptr)
//    Free a pointer previously returned by `kalloc`. Does nothing if
//    `ptr == nullptr`.
void kfree(void* ptr) {
    if (ptr) {
        // tell sanitizers the freed page is inaccessible
        asan_mark_memory(ka2pa(ptr), PAGESIZE, true);
        // remember to deincrement allocated pages
    }
    // In this case the situation is reversed. We give, and then merge as needed.
    // Give:
    //  (pages) mark all pages in buddy as free, (list) add it to the appropriate free list
    //  deincrement allocated_pages
    // Merge(ptr): functions the same way whether you pick the first or second buddy.
    //  (pages) find root buddy; this involves dividing and re-multiplying ptr by 2^(next order) (make sure to convert to pages)
    //   then find next buddy by adding 2^(order)
    //   check whether both buddy headers are free
    //    (paranoia: at least one of the buddies should be free, as in its header is allocatable, free, and order != -1, and the other should be either free or correctly allocated: alloctable, all pages non-free, and order != -1 only on the header)
    //   one isn't free: return nullptr
    //   both are free:
    //    (pages) set beginning header to (next order) and midpoint header to order -1
    //    (list) remove list entries for both buddies; add one entry to the (next order) list
    //    (paranoia: free check on new merged block)
    //    (can also have an (externally callable?) global check to see if the data structures exactly match)
    //    return the start address of the new merged block
    // Rinse and repeat merging
    
    // helper functions:
    // validate_free(ptr): Validates that the layout of (pages) starting at ptr is correct for a free allocation (uses pages[ptr / PAGESIZE].order to determine order)
    // validate_allocated(ptr): Ditto, for allocated
    // validate_all_free(ptr): Traverses free_lists and calls validate_free on all pointers it finds
    log_printf("kfree not implemented yet\n");
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
