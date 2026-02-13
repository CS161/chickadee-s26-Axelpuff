CS 161 Problem Set 1 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset1collab.md`.

Answers to written questions
----------------------------

1. The maximum size it supports is PAGESIZE (it immediately returns `nullptr` if `sz > PAGESIZE`).
2. The first address it returns is 0xffff800000001000. This makes sense because the 0xffff indicates that it is in high canonical memory, making it a kernel pointer in the kernel heap.
3. The highest address printed is 0xffff8000001ff000.
4. It returns high canonical addresses (they start at 0xffff8000) that are not high enough to be kernel text. The line `ptr = pa2kptr<void*>(next_free_pa);` converts the physical address (pa) provided by `physical_ranges` into a kernel pointer (kptr).
5. One dirty way to do this is to go into `k-init.cc` and change `physical_ranges.set(0, MEMSIZE_PHYSICAL, mem_available);` to add 0x100000 to the second argument.
6.
```
    while (next_free_pa != physical_ranges.limit()) {
        if (physical_ranges.type(next_free_pa) == mem_available) {
            ptr = pa2kptr<void*>(next_free_pa);
            next_free_pa += PAGESIZE;
            break;
        }
        next_free_pa += PAGESIZE;
    }
```
7. The loop using `find()` may be more efficient since it completely skips over unavailable ranges rather than iterating through every page.
8. Without `page_lock`, there could be a race condition where a second thread re-allocates the page at `next_free_pa` before the first thread that allocated it has the chance to increment `next_free_pa`.

## B.
1. Inside the loop that marks addresses with range type `mem_kernel`. Line 86: `mark(pa, f_kernel);`
2. Line 96: `mark(ka2pa(p), f_kernel | f_process(pid));`
3. All page table pages need to be treated as protected memory. If a process could modify its own page table, it could change the physical addresses to new ones outside of what the kernel assigned it and violate process isolation. On the other hand, the process' virtual memory accessed using `vmiter` is what the process is supposed to be able to access and modify.
4. It should be `mem_kernel` since the process table exists in kernel memory.
5. There is no qualitative difference. This is because `vmiter.next()` essentially does the same thing as incrementing by page size, except that it skips non-present regions, making it imperceptibly faster.
6. Given that the OS runs with X CPUs, there are X+1 periods. This means that one of these early pages missing from the viewer is allocated for each CPU. X - 1 of the periods are before the page table and the last 2 are immediately after the page table, separated by one normal page allocation. (These missing pages in the viewer really threw me off in A part 1.) The extra period on top of the CPU periods is the array `v_` allocated *inside* the `memusage::refresh()` function call.
7. After a lot of thinking (this took me a very long time), I figured out that a reasonable implementation would be to have memusage iterate through the CPUs the same way it does the processes, since the CPU idle tasks aren't listed anywhere else. I used `kptr2pa` on the `idle_task_` pointers and the `v_` pointer in order to mark them with `f_kernel` so that they would have the appropriate flags.

## C.
1. `kernel_entry` in k-exception.S: this is the one that starts in `bootentry.S`. It sets the stack pointer up at the top of a CPU stack (and preinitializes this stack) before the line `jmp _Z12kernel_startPKc`.
2. `syscall_entry`: when a process uses a `syscall` instruction, `syscall_entry` changes %rsp to point to the kernel stack and sets up the kernel stack before entering into C++ to handle the syscall (`call _ZN4proc7syscallEP8regstate`).
3. `exception_entry` (Synchronous exception): handled similarly to `syscall`, enters at `call _ZN4proc9exceptionEP8regstate`.
4. `alt_exception_entry` (Asynchronous exception): the CPU gets kicked over to a specific non-process CPU stack defined at boot time, so it has to set up a kernel task stack and jump to it (`jmp exception_entry_finish`) before handling the exception as before.
5. Initializing a new process' stack (see `resume_regstate`): When a process is initialized from regstate, the jump occurs at the second `iretq` in `restore_and_iret`.
6. `_ZN4proc5yieldEv` (Kernel voluntary yield): stores stuff on kernel stack, switches to CPU stack, then jumps to the CPU scheduler (`jmp _ZN8cpustate8scheduleEv`).
7. Initializing an application processor stack (`ap_entry`): this sets up a CPU stack and jumps to `cpu::init_ap()` (`movabsq $_ZN8cpustate7init_apEv, %rbx`).

Grading notes
-------------
