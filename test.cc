#include <cstdio>
#include <cstdint>

#define GEOFF -1

uint32_t test() {
    uint32_t borges = GEOFF;
    return borges;
}

int main(int argc, char** argv) {
    uintptr_t rsp;
    int blah = static_cast<int>(test());
    printf("blah: %i\n", blah);
    asm volatile("movq %%rsp, %0" : "=r" (rsp));
    printf("%%rsp 0x%lx\n", rsp);
    printf("argc %d\n", argc);
    printf("argv %p (%%rsp+0x%lx)\n",
           argv, reinterpret_cast<uintptr_t>(argv) - rsp);
    for (int i = 0; i < argc; ++i) {
        printf("argv[%d] @%p: %p (%%rsp+0x%lx) \"%s\"\n", i, &argv[i],
               argv[i], reinterpret_cast<uintptr_t>(argv[i]) - rsp,
               argv[i]);
    }
}
