#include "u-lib.hh"

// u-malloc.cc
//    Userspace heap allocator for Chickadee.
//
//    Layout: each allocation is preceded by a size_t header that records
//    the allocated block size (header included).  free() reads this header
//    to return the block to the right power-of-two free list.
//
//    Pages are obtained from sys_page_alloc(addr), which maps one page at
//    a specific virtual address.  The heap grows from the first page-aligned
//    address after the linker `end` symbol upward toward the stack.

// Linker symbol: first address not occupied by text/data/bss.
extern uint8_t end[];

static constexpr int    MIN_CLASS  = 3;   // smallest bucket: 2^3 = 8 bytes
static constexpr int    MAX_CLASS  = 20;  // largest  bucket: 2^20 = 1 MB
static constexpr int    NUM_CLASSES = MAX_CLASS - MIN_CLASS + 1;

// Header stored immediately before every payload returned by malloc.
// The `size` field is the total block size including this header.
struct alloc_header {
    size_t size;
};
static constexpr size_t HDR = sizeof(alloc_header);

// Free list: a singly-linked list of previously-freed blocks per size class.
struct free_block {
    free_block* next;
};
static free_block* free_lists[NUM_CLASSES];

// Bump pointer into the heap region [heap_ptr, heap_committed).
static uint8_t* heap_ptr       = nullptr;
static uint8_t* heap_committed = nullptr;  // first unmapped byte

// Initialize heap_ptr on first use.
static void heap_init() {
    heap_ptr = reinterpret_cast<uint8_t*>(
        round_up(reinterpret_cast<uintptr_t>(end), PAGESIZE)
    );
    heap_committed = heap_ptr;
}

// Ensure at least `need` bytes are available at heap_ptr.
// Returns false if sys_page_alloc fails.
static bool heap_ensure(size_t need) {
    while ((size_t)(heap_committed - heap_ptr) < need) {
        if (sys_page_alloc(heap_committed) < 0) {
            return false;
        }
        heap_committed += PAGESIZE;
    }
    return true;
}

// size_class: smallest class k such that (1 << (k + MIN_CLASS)) >= sz.
// Returns -1 if sz > (1 << MAX_CLASS).
static int size_class(size_t sz) {
    int k = 0;
    size_t block = (size_t)1 << MIN_CLASS;
    while (block < sz && k < NUM_CLASSES - 1) {
        ++k;
        block <<= 1;
    }
    return (block >= sz) ? k : -1;
}

extern "C" {

void* malloc(size_t size) {
    if (heap_ptr == nullptr) {
        heap_init();
    }
    if (size == 0) {
        size = 1;
    }
    // Total block size: header + payload, rounded up to the bucket size.
    size_t total = size + HDR;
    int cls = size_class(total);
    size_t block_size = (cls >= 0) ? ((size_t)1 << (cls + MIN_CLASS)) : total;

    void* payload;

    if (cls >= 0 && free_lists[cls]) {
        // Reuse a freed block from the free list.
        // The free_block link lives in the payload area (at b+HDR),
        // so the alloc_header at b still has the correct size field.
        free_block* b = free_lists[cls];
        payload = reinterpret_cast<uint8_t*>(b) + HDR;
        free_lists[cls] = reinterpret_cast<free_block*>(payload)->next;
    } else {
        // Bump-allocate from the heap.
        if (!heap_ensure(block_size)) {
            return nullptr;
        }
        alloc_header* hdr = reinterpret_cast<alloc_header*>(heap_ptr);
        hdr->size = block_size;
        payload   = heap_ptr + HDR;
        heap_ptr += block_size;
    }

    return payload;
}

void free(void* ptr) {
    if (!ptr) {
        return;
    }
    alloc_header* hdr = reinterpret_cast<alloc_header*>(
        reinterpret_cast<uint8_t*>(ptr) - HDR
    );
    size_t block_size = hdr->size;
    int cls = size_class(block_size);
    if (cls < 0) {
        return; // oversized block: leak it (rare, and avoids complexity)
    }
    // Store next in the payload area (ptr) so hdr->size stays valid.
    // free_lists[cls] stores HEADER pointers; malloc's reuse path adds HDR
    // to get the payload, so the list entry must point to the header, not ptr.
    free_block* b  = reinterpret_cast<free_block*>(ptr);
    b->next        = free_lists[cls];
    free_lists[cls] = reinterpret_cast<free_block*>(hdr);
}

void* calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    if (nmemb && total / nmemb != size) {
        return nullptr; // overflow
    }
    void* p = malloc(total);
    if (p) {
        memset(p, 0, total);
    }
    return p;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return nullptr;
    }
    alloc_header* hdr = reinterpret_cast<alloc_header*>(
        reinterpret_cast<uint8_t*>(ptr) - HDR
    );
    size_t old_payload = hdr->size - HDR;
    void* p = malloc(size);
    if (p) {
        memcpy(p, ptr, old_payload < size ? old_payload : size);
        free(ptr);
    }
    return p;
}

} // extern "C"
