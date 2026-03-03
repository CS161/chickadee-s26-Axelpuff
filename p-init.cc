#include "u-lib.hh"

extern uint8_t end[];

uint8_t* heap_top;
uint8_t* stack_bottom;

void process_main() {
    pid_t p = sys_getpid();
    pid_t ppid = sys_getppid();
    assert(p == 1);
    assert(ppid == 1);

    // reap zombies until no more zombies are left
    while (sys_waitpid(0, nullptr) != E_CHILD) {}
    sys_exit(0);
}
