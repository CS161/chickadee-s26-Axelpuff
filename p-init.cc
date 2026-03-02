#include "u-lib.hh"

extern uint8_t end[];

uint8_t* heap_top;
uint8_t* stack_bottom;

void process_main() {
    pid_t p = sys_getpid();
    pid_t ppid = sys_getppid();
    assert(p == 1);
    assert(ppid == 1);
    
    while (true) {
      // reap children until no more are left for now
      while (sys_waitpid(0, nullptr, W_NOHANG) != E_AGAIN) {}
      sys_yield();
    }
}
