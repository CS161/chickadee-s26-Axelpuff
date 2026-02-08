CS 161 Problem Set 1 Answers
============================
Leave your name out of this file. Put collaboration notes and credit in
`pset1collab.md`.

Answers to written questions
----------------------------

1. The maximum size it supports is PAGESIZE (it immediately returns `nullptr` if `sz > PAGESIZE`).
2. The first address it returns is 0xffff800000001000. This kernel pointer corresponds to the first free physical memory returned by `physical_ranges.find`. The first three pages (excluding the zero page) are not reserved/kernel memory, whereas the next six are.
3. The highest address printed is 0xffff8000001ff000.
4. It returns high canonical addresses (they start at 0xffff8000) that are not high enough to be kernel text. The line `ptr = pa2kptr<void*>(next_free_pa);` converts the physical address (pa) provided by `physical_ranges` into a kernel pointer (kptr).
5. (???) Changing MEMSIZE_PHYSICAL to 0x300000UL in `kernel.hh` seems to work (although this isn't a .cc file).
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
7. (???) The loop using `find()` may be faster, since it iterates by moving to the next range instead of iterating over all the previous pages.
8. Without `page_lock`, there could be a race condition where a second thread re-allocates the page at `next_free_pa` before the first thread that allocated it has the chance to increment `next_free_pa`.

## B.
1. (???) Line 86: `mark(pa, f_kernel);`
2. Line 96: `mark(ka2pa(p), f_kernel | f_process(pid));`
3. All page table pages need to be treated as protected memory. If a process could modify its own page table, it could change the physical addresses to new ones outside of what the kernel assigned it and violate process isolation. On the other hand, the process' virtual memory accessed using `vmiter` is what the process is supposed to be able to access and modify.
4. It should be `mem_kernel` since the process table exists in kernel memory.
5. (???) There is no qualitative difference. This is because `vmiter.next()` essentially does the same thing as incrementing by page size, except that it skips non-present regions.
6. Given that the OS runs with X CPUs, there are X+1 periods. This means that one of these early pages missing from the viewer is allocated for each CPU. X - 1 of the periods are before the page table and the last 2 are immediately after the page table, separated by one normal page allocation. (These missing pages in the viewer really threw me off in A part 1.) The extra period on top of the CPU periods is the array `v_` allocated *inside* the `memusage::refresh()` function call.
7. After a lot of thinking, I figured out that a reasonable implementation would be to have memusage iterate through the CPUs the same way it does the processes, since the CPU idle tasks aren't listed anywhere else. I used `kptr2pa` on the `idle_task_` pointers and the `v_` pointer in order to mark them with `f_kernel` so that they would have the appropriate flags.

## C.
- (boot freebie)
- Syscall: when a process uses a `syscall` instruction, `syscall_entry` changes %rsp to point to the kernel stack and sets up the kernel stack before entering into C++ to handle the syscall.
- Synchronous exception: handled similarly to `syscall`.
- Asynchronous exception: the CPU gets kicked over to a specific non-process CPU stack defined at boot time, so it has to set up a kernel task stack before handling the exception as before.
- Initializing a new process' stack: (see `resume_regstate`).
- Kernel voluntary yield
- Asynchronous kernel exception

- (?) Initializing a new CPU stack (`ap_entry`): 

Grading notes
-------------
