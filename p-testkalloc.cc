#define CHICKADEE_OPTIONAL_PROCESS 1
#include "u-lib.hh"

void process_main() {
    // Your code here!
    // Running `testkalloc` should cause the kernel to run buddy allocator
    // tests. How you make this work is up to you.
    int r = sys_testkalloc();
    assert_eq(r, 0);
    console_printf(CS_SUCCESS "testkalloc succeeded!\n");

    // This test runs before `sys_exit` is implemented, so we can’t use it.
    while (true) {
    }

}
