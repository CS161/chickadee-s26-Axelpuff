#include "kernel.hh"
#include "k-lock.hh"

static spinlock page_lock;
static uintptr_t next_free_pa;
// should both be initialized in init_kalloc
// ??? atomics?
static size_t total_physpages = 0;
static size_t allocated_pages = 0;

// buddy allocator
static unsigned int min_order = 12;
static unsigned int max_order = 0;

// init_kalloc
//    Initialize stuff needed by `kalloc`. Called from `init_hardware`,
//    after `physical_ranges` is initialized.
void init_kalloc() {
    // iterate using physical ranges, increment both total_physpages and allocated_pages
    auto range = physical_ranges.begin();
    while (range != physical_ranges.end()) {
        assert((range->size() & PAGEOFFMASK) == 0); // assuming ranges are in size of pages
        size_t range_pgsz = range->size() / PAGESIZE;
        if (range->type() == mem_nonexistent) {
            log_printf("Woah! I found a nonexistent range of size %zu\n", range->size());
        } else if (range->type() == mem_available) {            
            // need to freaking round up to the next address that is a power of 2, check if it is within the range, then take the largest order of 2 which fits within available space and also evenly divides the new starting address.
            uintptr_t aligned_addr = round_up_pow2(range->first());
            log_printf("order aligned addr %p\n", aligned_addr);
            if (aligned_addr < range->last()) {
                assert(range->last() - aligned_addr <= range->size());
                unsigned int space_limited_order = msb(range->last() - aligned_addr) - 1;
                unsigned int addr_limited_order = msb(aligned_addr) - 1;
                unsigned int order = min(space_limited_order, addr_limited_order);
                // log_printf("space order %u\n", space_limited_order);
                // log_printf("addr order %u\n", addr_limited_order);
                // log_printf("found existent range of order %u\n", order);
                if (order > max_order) {
                    max_order = order;
                }
                // log_printf("starting at: %p\n", aligned_addr);
                // log_printf("dividing by: %p\n", 1u << order);
                assert(aligned_addr % (1u << order) == 0);
                // log_printf("found existent range of size %zu pages\n", range_pgsz);
            } else {
            }
        } else {
            allocated_pages += range_pgsz; 
        }
        total_physpages += range_pgsz; // may lead to size mismatch with actual usable physical memory (since the available ranges need to start at powers of 2)
        // move to next range
        ++range;
    }
    log_printf("total physpages: %zu\n", total_physpages);
    log_printf("total allocated: %zu\n", allocated_pages);

    // max_order = msb(total_physpages - allocated_pages) + msb(PAGESIZE) - 2;
    // log_printf("Max order: %u\n", max_order);
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
