#include "kernel.hh"
#include "k-ahci.hh"
#include "k-apic.hh"
#include "k-chkfs.hh"
#include "k-chkfsiter.hh"
#include "k-devices.hh"
#include "k-vmiter.hh"
#include "k-vfs.hh"
#include "obj/k-firstprocess.h"
#include <cinttypes>


// kernel.cc
//
//    This is the kernel.

// # timer interrupts so far on CPU 0
std::atomic<unsigned long> ticks;

static void tick();
static void start_initial_process(pid_t pid, const char* program_name);

// kernel_start(command)
//    Initialize the hardware and processes and start running. The `command`
//    string is an optional string passed from the boot loader.

void kernel_start(const char* command) {
  init_hardware();
  consoletype = CONSOLE_NORMAL;
  console_clear();

  // set up process descriptors
  for (pid_t i = 0; i < NPROC; i++) {
    ptable[i] = nullptr;
  }

  // set up file table
  {
    spinlock_guard guard(file_table_lock);
    for (int i = 0; i < N_FILE; i++) {
      spinlock_guard guard_file(file_table[i].file_lock);
      file_table[i].type = FTYPE_NONE;
    }
  }

  // set up keyboard/console vnode
  vnode* kcvn = knew<vnode>(&kc_vops);
  {
    spinlock_guard guard(kcvn->refcount_lock);
    kcvn->refcount = 1; // ??? is this incremented per file pointing to this vnode or what
  }

  {
    file* kc_file = &file_table[KC_FILE_NUM];
    spinlock_guard guard(file_table_lock);
    spinlock_guard guard_file(kc_file->file_lock);
    kc_file->type = FTYPE_VNODE;
    kc_file->refcount_ = 0; 
    kc_file->flags = FREAD | FWRITE;
    kc_file->off_ = 0;
    kc_file->vnode_ = kcvn;
    kc_file->ops = &vn_fops;
  }
  
  // start init
  start_initial_process(1, "init");

  // start first process
  start_initial_process(2, CHICKADEE_FIRST_PROCESS);

  // start running processes
  cpus[0].schedule();
}


// start_initial_process(pid, name)
//    Load application program `name` as process number `pid`.
//    This loads the application's code and data into memory, sets its
//    %rip and %rsp, gives it a stack page, and marks it as runnable.
//    Only called at initial boot time.

void start_initial_process(pid_t pid, const char* name) {
  // look up process image in initfs
  int mindex = memfile::initfs_lookup(name, memfile::required);
  x86_64_pagetable* pt = knew_pagetable();
  assert(mindex >= 0 && pt);

  // load code and data into pagetable
  memfile_loader ld(mindex, pt);
  int r = proc::load(ld);
  assert(r >= 0);

  // allocate process, initialize registers
  proc* p = knew<proc>();
  p->id_ = pid;
  p->parent_id_ = 1;
  p->init_user(pt);
  p->regs_->reg_rip = ld.entry_rip_;
  
  // initialize stack
  void* stkpg = kalloc(PAGESIZE);
  assert(stkpg);
  vmiter(p, MEMSIZE_VIRTUAL - PAGESIZE).map(stkpg, PTE_PWU);
  p->regs_->reg_rsp = MEMSIZE_VIRTUAL;

  // initialize fd table
  for (int i = 0; i < 3; i++) {
    p->fd_table[i] = KC_FILE_NUM;
    file_incref(&file_table[KC_FILE_NUM]);
  }
  for (int i = 3; i < N_FILEDESC; i++) {
    p->fd_table[i] = FD_EMPTY;
  }

  // map console
  vmiter(p, ktext2pa(console)).map(console, PTE_PWU);

  // add to process table (requires lock in case another CPU is already
  // running processes)
  {
    spinlock_guard guard_h(phierarchy_lock);
    spinlock_guard guard(ptable_lock);
    assert(!ptable[pid]);
    ptable[pid] = p;
    // if this process is not init, make it init's child
    // assumption: the init process is its own parent, but not its own child. 
    if (pid != 1) {
      assert(ptable[1]);
      ptable[1]->children.push_front(p);
    }
  }

  // add to run queue
  cpus[pid % ncpu].enqueue(p);
}

// proc::exception(reg)
//    Exception handler (for interrupts, traps, and faults).
//
//    The register values from exception time are stored in `reg`.
//    The processor responds to an exception by saving application state on
//    the current CPU stack, then jumping to kernel assembly code (in
//    k-exception.S). That code transfers the state to the current kernel
//    task's stack, then calls proc::exception().

void proc::exception(regstate* regs) {
  // It can be useful to log events using `// log_printf`.
  // Events logged this way are stored in the host's `log.txt` file.
  // log_printf("proc %d: exception %d @%p\n", id_, regs->reg_intno, regs->reg_rip);

  // Record most recent user-mode %rip.
  if ((regs->reg_cs & 3) != 0) {
    recent_user_rip_ = regs->reg_rip;
  }

  // Show the current cursor location.
  consolestate::get().cursor();


  // Actually handle the exception.
  switch (regs->reg_intno) {

  case INT_IRQ + IRQ_TIMER: {
    cpustate* cpu = this_cpu();
    if (cpu->cpuindex_ == 0) {
      tick();
    }
    lapicstate::get().ack();
    regs_ = regs;

    int q = ticks & (WHEEL_QUEUES - 1);
    sleep_wq_wheel[q].notify_all();
    
    yield_noreturn();
    break;                  /* will not be reached */
  }

  case INT_PF: {              // pagefault exception
    // Analyze faulting address and access type.
    uintptr_t addr = rdcr2();
    const char* operation = regs->reg_errcode & PFERR_WRITE
      ? "write" : "read";
    const char* problem = regs->reg_errcode & PFERR_PRESENT
      ? "protection problem" : "missing page";

    if ((regs->reg_cs & 3) == 0) {
      panic_at(*regs, "Kernel page fault for %p (%s %s)!\n",
	       addr, operation, problem);
    }

    error_printf(CS_ERROR "Process %d page fault for %p (%s %s, rip=%p)!\n",
		 id_, addr, operation, problem, regs->reg_rip);
    pstate_ = proc::ps_faulted;
    yield();
    break;
  }

  case INT_IRQ + IRQ_KEYBOARD:
    keyboardstate::get().handle_interrupt();
    break;

  default:
    if (sata_disk && regs->reg_intno == INT_IRQ + sata_disk->irq_) {
      sata_disk->handle_interrupt();
    } else {
      panic_at(*regs, "Unexpected exception %d!\n", regs->reg_intno);
    }
    break;                  /* will not be reached */

  }

  // return to interrupted context
}

// utility to avoid hard-coding the stack bottom canary offset
[[gnu::noinline]] void* proc::stack_bottom_canary_ptr() {
    // // log_printf("ptr: %p\n", &(this->stack_bottom_canary));
    // // log_printf("canary value: %i\n", this->stack_bottom_canary);
    // // log_printf("ptr2: %p\n", this);
    // // log_printf("val2: %i\n", this->canary);
    return &(this->stack_bottom_canary);
}

// helper function for waitpid, stores two `int`s as one `uintptr_t`
uintptr_t format_waitpid_return(int exit_status, int syscall_return) {
  // log_printf("I AM TRYING MY BEST TO RETURN EXIT STATUS %i AND SUCCESS VALUE %i\n", exit_status, syscall_return);
  unsigned int high_bits = static_cast<unsigned int>(exit_status);
  unsigned int low_bits = static_cast<unsigned int>(syscall_return);
  uintptr_t rax = static_cast<uintptr_t>(high_bits) << 32;
  return rax + low_bits;
}

void cleanup_pagetable(x86_64_pagetable *pagetable, uintptr_t max_addr);

// MUST BE CALLED holding `phierarchy_lock`
proc* proc::find_zombie_child() {
  proc* p = this->children.front();
  while (p != nullptr) {
    assert(p->parent_id_ == this->id_);
    if (p->pstate_ == ps_zombie) {
      break;
    } else {
      p = this->children.next(p);
    }
  }
  return p;
}

// cleans up (euphemism) zombie child process and returns exit status
// MUST BE CALLED holding both `phierarchy_lock` and `ptable_lock`
int proc::cleanup_and_return_status(proc* p) {
  // log_printf("checking process %d...\n", p->id_);
  assert(p != this);
  pid_t pid = p->id_;
  int exit_status = p->exit_status_;
	    
  p->pstate_ = ps_collected; // pointless?
  ptable[pid] = nullptr;
  p->child_links_.erase();
	    
  // log_printf("Trying to clean up process %d\n", pid);
  delete p;
  // log_printf("Cleaned up process %d\n", pid);
  return exit_status;
}

// proc::syscall(regs)
//    System call handler.
//
//    The register values from system call time are stored in `regs`.
//    The return value from `proc::syscall()` is returned to the user
//    process in `%rax`.

uintptr_t proc::syscall(regstate* regs) {
  //// log_printf("proc %d: syscall %ld @%p\n", id_, regs->reg_rax, regs->reg_rip);
    // // log_printf("Size of regstate: %zu\n", sizeof(*regs));
    // // log_printf("Distance between canary and offset: %" PRIuPTR "\n", reinterpret_cast<uintptr_t>(&this->stack_bottom_canary) - reinterpret_cast<uintptr_t>(this));
    // // log_printf("canary value: %i\n", this->stack_bottom_canary);

  // Record most recent user-mode %rip.
  recent_user_rip_ = regs->reg_rip;

  switch (regs->reg_rax) {

  case SYSCALL_CONSOLETYPE:
    if (consoletype != (int) regs->reg_rdi) {
      console_clear();
    }
    consoletype = regs->reg_rdi;
    return 0;

  case SYSCALL_PANIC:
    panic_at(*regs, "process %d called sys_panic()", id_);
    break;                  // will not be reached

  case SYSCALL_KTEST:
    if (regs->reg_rdi == 1) {
      return ktest_wait_queues();
    }
    return -1;

  case SYSCALL_GETPID:
    return id_;

  case SYSCALL_YIELD:
    yield();
    return 0;

  case SYSCALL_PAGE_ALLOC: {
    uintptr_t addr = regs->reg_rdi;
    if (addr >= VA_LOWEND || (addr & 0xFFF) != 0) {
      return E_FAULT;
    }
    // The handout code does not allow allocating a page over an existing
    // page. If you do allow this, beware of memory leaks and, especially,
    // TLB invalidation (see the memory iterator documentation).
    vmiter it(this, addr);
    if (it.present()) {
      return E_BUSY;
    }
    void* pg = kalloc(PAGESIZE);
    if (!pg || it.try_map(ka2pa(pg), PTE_PWU) < 0) {
      return E_NOMEM;
    }
    return 0;
  }

  case SYSCALL_PAUSE: {
    sti();
    for (uintptr_t delay = 0; delay < 1000000; ++delay) {
      pause();
    }
    return 0;
  }

  case SYSCALL_FORK:
    return syscall_fork(regs);

  case SYSCALL_EXIT: {
    // log_printf("Process %ld is exiting...\n", this->id_);

    // ??? Does it matter where this goes within the function?
    if (this->id_ == 1) {
      process_halt();
      break; // will not be reached
    }
    
    {
      spinlock_guard guard_h(phierarchy_lock);  
      // reparent kids
      for (proc* p = this->children.front();
	   p != nullptr;
	   p = this->children.front()) {
	// log_printf("Reparenting %d to init\n", p->id_);
	p->parent_id_ = 1;
	this->children.erase(p);
      
	spinlock_guard guard(ptable_lock);  
	assert(ptable[1]);
	ptable[1]->children.push_front(p);
	// log_printf("Done reparenting %d\n", p->id_);
      }
    }
    
    x86_64_pagetable* pt;
    {
      spinlock_guard guard(ptable_lock);  
      pt = this->pagetable_;
      this->pagetable_ = nullptr;
    }
    
    set_pagetable(early_pagetable);
    cleanup_pagetable(pt, MEMSIZE_VIRTUAL);
    regs_ = regs; // ??? inefficient? this state is never used
    
    {
      spinlock_guard guard_h(phierarchy_lock);
      spinlock_guard guard(ptable_lock);
      // mark as zombie
      this->pstate_ = ps_zombie;
      // set exit status
      this->exit_status_ = regs->reg_rdi;
      // wake up waiters (won't activate until lock is released)
      proc_exit_wq.notify_all();
      // notify parent if parent is sleeping
      if (parent_id_ != 1
	  && ptable[parent_id_] // valid since we have ptable_lock
	  && ptable[parent_id_]->pstate_ == proc::ps_blocked
	  ) {
	assert(ptable[parent_id_]->blocked_wq_ != -1);
	spinlock_guard sleep_guard(sleep_lock);
	ptable[parent_id_]->child_exited_ = 1;
	sleep_wq_wheel[ptable[parent_id_]->blocked_wq_].notify_all();
      }
    }
    
    // from this point on the proc struct and stack might be obliterated
    yield_noreturn();
    break; // will not be reached
  }

  case SYSCALL_READ:
    return syscall_read(regs);

  case SYSCALL_WRITE:
    return syscall_write(regs);

  case SYSCALL_READDISKFILE:
    return syscall_readdiskfile(regs);

  case SYSCALL_SYNC: {
    int drop = regs->reg_rdi;
    // `drop > 1` asserts that no data blocks are referenced (except
    // possibly superblock and FBB blocks). This can only be ensured on
    // tests that run as the first process.
    if (drop > 1 && strncmp(CHICKADEE_FIRST_PROCESS, "test", 4) != 0) {
      drop = 1;
    }
    return bufcache::get().sync(drop);
  }

  case SYSCALL_MSLEEP: {
    // round up to nearest 0.01 seconds
    unsigned long t_wakeup = ticks + (regs->reg_rdi + 9) / (1000 / HZ);
    // resume_counter_ = 0;
    // unsigned long initial_resumes = resume_counter_;

    spinlock_guard guard(sleep_lock);
    int q = t_wakeup & (WHEEL_QUEUES - 1);
    blocked_wq_ = q;
    child_exited_ = 0;
    
    waiter w;
    w.wait_until(sleep_wq_wheel[q], [&] () {
      return (long(t_wakeup - ticks) <= 0 || child_exited_ == 1);
    }, guard);

    // unsigned long final_resumes = resume_counter_;
    // log_printf("Resumes since started sleeping (process %d): %lu\n", id_, final_resumes - initial_resumes);
    blocked_wq_ = -1;
    return child_exited_ ? E_INTR : 0;
  }

  case SYSCALL_GETPPID: {
    spinlock_guard guard_h(phierarchy_lock);
    return this->parent_id_;
  }

  case SYSCALL_WAITPID: {
    pid_t pid = regs->reg_rdi;
    int options = regs->reg_rsi;
    bool wnohang = options & W_NOHANG;
    if ((options & (~W_NOHANG)) != 0) {
      log_printf("%d: invalid waitpid options %i\n", id_, options);
      return format_waitpid_return(0, E_NOSYS);
    }
    
    spinlock_guard guard_h(phierarchy_lock);      
    if (pid == 0) {
      proc* p = this->children.front();
      if (!p) {
	// log_printf("%d: couldn't find zombies to reap, no children\n", id_);
	return format_waitpid_return(0, E_CHILD);
      }
	
      p = find_zombie_child(); // pstate_ is atomic so this doesn't need ptable lock
      if (!p) {
	if (wnohang) {
	  // log_printf("%d: couldn't find zombies to reap, still alive\n", id_);
	  return format_waitpid_return(0, E_AGAIN);
	} else {
	  // resume_counter_ = 0;
	  // unsigned long initial_resumes = resume_counter_;	  
	  waiter w;
	  w.wait_until(proc_exit_wq, [&] () {
	    return (p = find_zombie_child()); // this is meant to be an assignment
	  }, guard_h);	 
	  // unsigned long final_resumes = resume_counter_;
	  // log_printf("Resumes since started sleeping (process %d): %lu\n", id_, final_resumes - initial_resumes);	  
	}
      }
	
      assert(p);
      pid = p->id_;
      spinlock_guard guard(ptable_lock);
      int exit_status = cleanup_and_return_status(p);
      return format_waitpid_return(exit_status, pid);
    } else {
      spinlock_guard guard(ptable_lock);
      if (!ptable[pid] || ptable[pid]->parent_id_ != this->id_) {
	return format_waitpid_return(0, E_CHILD);
      }
      proc* p = ptable[pid];
      guard.unlock();
      if (p->pstate_ != ps_zombie) {
	if (wnohang) {
	  return format_waitpid_return(0, E_AGAIN);
	} else {
	  // resume_counter_ = 0;
	  // unsigned long initial_resumes = resume_counter_;    
	  waiter w;
	  w.wait_until(proc_exit_wq, [&] () {
	    return (p->pstate_ == ps_zombie);
	  }, guard_h);
	  // unsigned long final_resumes = resume_counter_;
	  // log_printf("Resumes since started sleeping (process %d): %lu\n", id_, final_resumes - initial_resumes);	  
	}
      }
	
      assert(p->pstate_ == ps_zombie);
      guard.lock();
      int exit_status = cleanup_and_return_status(p);
      return format_waitpid_return(exit_status, pid);
    }
  }

  case SYSCALL_GETUSAGE:
    return syscall_getusage(regs);

  case SYSCALL_CORRUPT: {
      char test;
      uintptr_t target = reinterpret_cast<uintptr_t>(&test) - PROCSTACK_SIZE + 0x125;
      for (int i = 0; i < 150; i++) {
          char* ptr = reinterpret_cast<char*>(target + i);
          // log_printf("Wiped %p\n", ptr);
          *ptr = 0;
      }
    return 0;
  }

  case SYSCALL_TESTKALLOC:
      return syscall_testkalloc(regs);

  default:
    // no such system call
    log_printf("%d: no such system call %u\n", id_, regs->reg_rax);
    return E_NOSYS;

  }
}

// simple helper functions

#define OOM_ERROR -1 // idk
#define OOP_ERROR -1 // idk

// CALLER MUST HOLD `ptable_lock`!!!
int find_free_pid() {
  // avoid pid 0
  for (int pid = 1; pid != NPROC; ++pid) {
    if (!ptable[pid]) {
      return pid;
    }
  }
  return OOM_ERROR; // is this the correct error?
}

// void kfree_pagetable(x86_64_pagetable *pagetable)
// {
//   for (ptiter it(pagetable); it.va() < MEMSIZE_VIRTUAL; it.next())
//     {
//       kfree(reinterpret_cast<void *>(it.pa()));
//     }
//   kfree(pagetable);
// }

void cleanup_process_memory(x86_64_pagetable *pagetable, uintptr_t max_addr) {
    size_t cleaned_pages = 0;
    vmiter it = vmiter(pagetable, 0);
    while (it.va() < max_addr) {
        // !!! Is there a potential issue here with iterating by pagesize if allocations are larger than a page? Or does .user() get updated?
        if (it.user() && it.va() != CONSOLE_ADDR) {
            assert(it.writable());
            // void* ptr = pa2kptr<void*>(it.pa());
            // log_printf("trying to free %p\n", it.va());
            it.kfree_page();
            // log_printf("succesfully freed %p\n", it.va());
            // assert(!it.user());
            // assert(it.pa() == (uintptr_t) -1);
            cleaned_pages++;
        // } else {
            // // log_printf("skipping %p\n", addr);
        }
        it.next();
    }

    // log_printf("max_addr: %p\n", max_addr);
    // log_printf("memsize_virtual: %p\n", MEMSIZE_VIRTUAL);    
    // log_printf("%zu pages freed \n", cleaned_pages);
}    

// cleanup_pagetable(x86_64_pagetable *pagetable, uintptr_t max_addr)
//    Frees all process memory from 0 up to max_addr,
//    in `pagetable`, EXCLUDING max_addr, and then frees `pagetable` itself

void cleanup_pagetable(x86_64_pagetable *pagetable, uintptr_t max_addr) {
    cleanup_process_memory(pagetable, max_addr);

    for (ptiter pit(pagetable); pit.low(); pit.next()) {
      pit.kfree_ptp();
    }
    delete pagetable;
}

// proc::syscall_fork(regs)
//    Handle fork system call.

// !!! need to fix all the stupid ahh formatting from my old pset

int proc::syscall_fork(regstate* regs) {
  // initialize process page table
  x86_64_pagetable *child_pagetable = knew_pagetable();
  if (!child_pagetable) {
    return OOM_ERROR;
  }

  // copy process code and data
  uintptr_t addr = 0;
  for (; addr < MEMSIZE_VIRTUAL; addr += PAGESIZE)
      {
          vmiter it(this, addr);
          if (it.writable() && addr != CONSOLE_ADDR)
              {
                  assert(it.user());
                  // alloc new physical memory for copy of parent process data
                  void* pa = kalloc(PAGESIZE);
                  if (!pa)
                      {
                          cleanup_pagetable(child_pagetable, addr);
                          return OOM_ERROR;
                      }
                  int r = vmiter(child_pagetable, it.va()).try_map(pa, it.perm());
                  if (r != 0)
                      {
                          kfree(pa);
                          cleanup_pagetable(child_pagetable, addr);
                          return OOM_ERROR;
                      }
                  memcpy(pa, reinterpret_cast<void *>(addr), PAGESIZE);
              }
          else if (it.user()) {
              // copy read-only segments
              int r = vmiter(child_pagetable, it.va()).try_map(it.pa(), it.perm());
              if (r != 0)
                  {
                      cleanup_pagetable(child_pagetable, addr);
                      return OOM_ERROR;
                  }
              // increment ref count (this helps preserve the read-only data when
              // one process that uses it frees)
	  // int pageno = it.pa() / PAGESIZE;
	  // physpages[pageno].refcount++;
        }
    }
  // init new ptable entry
  int pid = -2;
  proc* p;
  { 
      spinlock_guard guard_h(phierarchy_lock);
      spinlock_guard guard(ptable_lock);
      pid = find_free_pid();
      if (pid == -1) {
          cleanup_pagetable(child_pagetable, addr);
          // log_printf("failed to find a ptable slot\n");
          return OOP_ERROR; // technically not out of memory but similar
      }
      p = knew<proc>();
      if (!p) {
          cleanup_pagetable(child_pagetable, addr);
          return OOM_ERROR;
      }
      p->id_ = pid;
      p->parent_id_ = this->id_;
      p->init_user(child_pagetable);
      *(p->regs_) = *regs;
      // memcpy(p->regs_, regs, sizeof(regstate));
      p->regs_->reg_rax = 0;    
      ptable[pid] = p;
      
      // log_printf("Parenting %ld\n", p->id_);
      this->children.push_front(p);
      // log_printf("Done parenting %ld\n", p->id_);
  }
  assert(pid >= 0);
  // add to run queue
  cpus[pid % ncpu].enqueue(p);
  // return child pid to parent
  // log_printf("Successfully forked process with pid %i\n", pid);
  return pid;
}

// int get_ptable_index(proc* p)


// proc::syscall_exit(regs)
//    Exit current process.

// void proc::syscall_exit(regstate* regs) {  
//   // ??? could not putting a lock here could lead to weird things if multiple threads were on this CPU
// }

// proc::syscall_read(regs), proc::syscall_write(regs),
// proc::syscall_readdiskfile(regs)
//    Handle read and write system calls.

uintptr_t proc::syscall_read(regstate* regs) {
  // This is a slow system call, so allow interrupts by default
  sti();

  uintptr_t addr = regs->reg_rsi;
  size_t sz = regs->reg_rdx;

  // Low-canonical read of size 0 always valid
  if (sz == 0 && addr < VA_LOWEND) {
    return 0;
  }

  // Validate the read buffer.
  if (VA_LOWEND - sz < addr) {
    return E_FAULT;
  }
  vmiter it(this, addr);
  if (!(it.range_perm(sz) & (PTE_P | PTE_W | PTE_U))) {
    return E_FAULT;
  }

  // !!! TBA fd -> file table -> vfs helper function call
  return 0;
}

uintptr_t proc::syscall_write(regstate* regs) {
  // This is a slow system call, so allow interrupts by default
  sti();

  uintptr_t addr = regs->reg_rsi;
  size_t sz = regs->reg_rdx;
  // log_printf("addr: %zu\n", addr);
  // log_printf("sz: %zu\n", sz);
  // log_printf("memsize virt: %zu\n", MEMSIZE_VIRTUAL);

  // Low-canonical write of size 0 always valid
  if (sz == 0 && addr < VA_LOWEND) {
    return 0;
  }
  
  // Your code here!
  // * Write to open file `fd` (reg_rdi), rather than `consolestate`.
  // * Validate the write buffer.
  if (VA_LOWEND - sz < addr) {
    log_printf("nah bruh\n");
    return E_FAULT;
  }
  vmiter it(this, addr);
  if (!(it.range_perm(sz) & (PTE_P | PTE_U))) {
    return E_FAULT;
  }

  // !!! TBA fd -> file table -> vfs helper function call
  return 0;
}

uintptr_t proc::syscall_readdiskfile(regstate* regs) {
  // This is a slow system call, so allow interrupts by default
  sti();

  const char* filename = reinterpret_cast<const char*>(regs->reg_rdi);
  unsigned char* buf = reinterpret_cast<unsigned char*>(regs->reg_rsi);
  size_t sz = regs->reg_rdx;
  off_t off = regs->reg_r10;

  if (!sata_disk) {
    return E_IO;
  }

  // read root directory to find file inode number
  auto ino = chkfsstate::get().lookup_inode(filename);
  if (!ino) {
    return E_NOENT;
  }

  // read file inode
  ino->lock_read();
  chkfs_fileiter it(ino.get());

  size_t nread = 0;
  while (nread < sz) {
    // copy data from current block
    if (auto e = it.find(off).load()) {
      unsigned b = it.block_relative_offset();
      size_t ncopy = min(
			 size_t(ino->size - it.offset()),   // bytes left in file
			 chkfs::blocksize - b,              // bytes left in block
			 sz - nread                         // bytes left in request
			 );
      memcpy(buf + nread, e->buf_ + b, ncopy);

      nread += ncopy;
      off += ncopy;
      if (ncopy == 0) {
	break;
      }
    } else {
      break;
    }
  }

  ino->unlock_read();
  return nread;
}


// memshow()
//    Draw a picture of memory (physical and virtual) on the CGA console.
//    Switches to a new process's virtual memory map every 0.25 sec.
//    Uses `console_memviewer()`, a function defined in `k-memviewer.cc`.

static void memshow() {
  static unsigned long last_redisplay = 0;
  static unsigned long last_switch = 0;
  static int showing = 1;

  // redisplay every 0.04 sec
  if (last_redisplay != 0 && ticks - last_redisplay < HZ / 25) {
    return;
  }
  last_redisplay = ticks;

  // switch to a new process every 0.5 sec
  if (ticks - last_switch >= HZ / 2) {
    showing = (showing + 1) % NPROC;
    last_switch = ticks;
  }

  spinlock_guard guard(ptable_lock);

  int search = 0;
  while ((!ptable[showing]
	  || !ptable[showing]->pagetable_
	  || ptable[showing]->pagetable_ == early_pagetable
          || !(ptable[showing]->pstate_ == proc::ps_runnable
	      || ptable[showing]->pstate_ == proc::ps_blocked
	      || ptable[showing]->pstate_ == proc::ps_faulted)
	  )
	 && search < NPROC) {
    showing = (showing + 1) % NPROC;
    ++search;
  }

  console_memviewer(ptable[showing]);
  if (!ptable[showing]) {
    console_printf(CPOS(10, 26), CS_WHITE "   VIRTUAL ADDRESS SPACE\n"
		   "                          [All processes have exited]\n"
		   "\n\n\n\n\n\n\n\n\n\n\n");
  }
}

// proc::syscall_getusage(regs)
//  Get system usage stats.
int proc::syscall_getusage(regstate* regs) {
  // extract and validate pointer
  uintptr_t addr = regs->reg_rdi;
  uintptr_t pg_addr = (addr / PAGESIZE) * PAGESIZE;
  if (pg_addr >= VA_LOWEND || (pg_addr & 0xFFF) != 0) {
    return E_FAULT;
  }
  if (addr % alignof(usage) != 0) { // right way to check for this?
    return E_FAULT;
  }
  vmiter it(this, pg_addr);
  if (!it.present() || !it.writable() || !it.user()) {
    return E_FAULT; // correct error here?
  }
  usage* u = reinterpret_cast<usage*>(addr); // trust the user to not give us a garbage pointer and ruin their own memory
  // call some stuff from k-alloc.cc to get stats
  // put them in the struct
  u->time = ticks;
  u->free_pages = kget_total_physpages() - kget_allocated_physpages();
  u->allocated_pages = kget_allocated_physpages();
  return 0;
}

// proc::syscall_testkalloc(regs)
//  Run a bunch of memory calls and then check that the memory state is valid.
int proc::syscall_testkalloc(regstate* regs) {
    // log_printf("pt entry\n");
    proc* p = knew<proc>();
    void* ptrs[12];
    for (int i = 0; i < 6; i++) {
        ptrs[i] = kalloc(0x1000u << i);
    }
    for (int i = 5; i >= 2; i--) {
        kfree(ptrs[i]);
    }
    
    validate_all_pages();
    
    for (int i = 6; i < 12; i++) {
        ptrs[i] = kalloc(0x100u << i);
    }
    for (int i = 2; i >= 0; i--) {
        kfree(ptrs[i]);
    }
    for (int i = 6; i < 12; i++) {
        kfree(ptrs[i]);
    }
    delete p;
    
    validate_all_pages();
    
    // log_printf("pt final\n");
    return 0;
}

// tick()
//    Called once every tick (0.01 sec, 1/HZ) by CPU 0. Updates the `ticks`
//    counter and performs other periodic maintenance tasks.

void tick() {
  // Update current time
  ++ticks;

  // Update display
  if (consoletype == CONSOLE_MEMVIEWER) {
    memshow();
  }
}
