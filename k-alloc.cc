#include "kernel.hh"
#include "k-lock.hh"

static spinlock page_lock;
static uintptr_t next_free_pa;
// should both be initialized in init_kalloc
// ??? atomics?
static size_t total_physpages = 0;
static size_t allocated_pages = 0;

// init_kalloc
//    Initialize stuff needed by `kalloc`. Called from `init_hardware`,
//    after `physical_ranges` is initialized.
void init_kalloc() {
    // iterate using physical ranges, increment both total_physpages and allocated_pages
    // should it be vmiter?
    auto range = physical_ranges.begin();
    while (range != physical_ranges.end()) {
        assert((range->size() & PAGEOFFMASK) == 0); // assuming ranges are in size of pages
        size_t range_pgsz = range->size() / PAGESIZE;
        if (!(range->type() == mem_available) && !(range->type() == mem_nonexistent)) {
            // ??? not sure exactly what nonexistent means
            allocated_pages += range_pgsz; 
        }
        if (!(range->type() == mem_nonexistent)) {
            total_physpages += range_pgsz;
        }
        // move to next range
        ++range;
    }
    log_printf("total physpages: %zu\n", total_physpages);
    log_printf("total allocated: %zu\n", allocated_pages);
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
    while (next_free_pa != physical_ranges.limit()) {
        if (physical_ranges.type(next_free_pa) == mem_available) {
            ptr = pa2kptr<void*>(next_free_pa);
            next_free_pa += PAGESIZE;
            break;
        }
        next_free_pa += PAGESIZE;
    }
    // auto range = physical_ranges.find(next_free_pa);
    // while (range != physical_ranges.end()) {
    //     if (range->type() == mem_available) {
    //         // use this page
    //         ptr = pa2kptr<void*>(next_free_pa);
    //         next_free_pa += PAGESIZE;
    //         break;
    //     } else {
    //         // move to next range
    //         next_free_pa = range->last();
    //         ++range;
    //     }
    // }

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
