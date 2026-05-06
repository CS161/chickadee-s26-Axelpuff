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
    init_kc_file(&file_table[KC_FILE_NUM]);
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
  task_group* g = knew<task_group>();
  g->parent_id_ = 1;
  g->live_task_count_ = 1;
  g->tasks_.push_front(p);

  p->id_ = pid;
  p->pid_ = pid;
  p->group_ = g;
  p->init_user(pt);
  p->regs_->reg_rip = ld.entry_rip_;

  // initialize stack
  void* stkpg = kalloc(PAGESIZE);
  assert(stkpg);
  vmiter(pt, MEMSIZE_VIRTUAL - PAGESIZE).map(stkpg, PTE_PWU);
  p->regs_->reg_rsp = MEMSIZE_VIRTUAL;

  // initialize fd table
  // (do I need to get a lock here even though nothing else should be touching this?)
  for (int i = 0; i < 3; i++) {
    g->fd_table_[i] = KC_FILE_NUM;
    file_incref(&file_table[KC_FILE_NUM]);
  }
  for (int i = 3; i < N_FILEDESC; i++) {
    g->fd_table_[i] = FD_EMPTY;
  }

  // map console
  vmiter(pt, ktext2pa(console)).map(console, PTE_PWU);

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
      ptable[1]->group_->children_.push_front(g);
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
task_group* proc::find_zombie_child() {
  task_group* g = this->group_->children_.front();
  while (g != nullptr) {
    // log_printf("parent id is %i\n", g->parent_id_);
    assert(g->parent_id_ == this->pid_);
    if (g->live_task_count_ == 0) {
      break;
    } else {
      g = this->group_->children_.next(g);
    }
  }
  return g;
}

// // cleans up (euphemism) zombie child process and returns exit status
// // MUST BE CALLED holding both `phierarchy_lock` and `ptable_lock`
// int proc::cleanup_and_return_status(proc* p) {
//   // log_printf("checking process %d...\n", p->id_);
//   assert(p != this);
//   pid_t pid = p->id_;
//   int exit_status = p->group_->exit_status_;

//   p->pstate_ = ps_collected; // pointless?
//   ptable[pid] = nullptr;
//   p->group_->child_links_.erase();
	    
//   // log_printf("Trying to clean up process %d\n", pid);
//   delete p;
//   // log_printf("Cleaned up process %d\n", pid);
//   return exit_status;
// }

// cleans up (euphemism) zombie child task group and returns exit status
// MUST BE CALLED holding both `phierarchy_lock` and `ptable_lock`
int proc::cleanup_and_return_status(task_group* g) {
  // log_printf("checking process %d...\n", p->id_);
  int exit_status = g->exit_status_;
  
  spinlock_guard guard(group_->tasks_lock_);
  for (proc* p = g->tasks_.pop_front(); p != nullptr; p = g->tasks_.pop_front()) {
    assert(p->id_ == p->pid_);
    assert(g->tasks_.pop_front() == nullptr); // should only be one task in a process, but just in case...
    pid_t pid = p->id_;
    assert(p->pstate_ == ps_zombie);
    // p->pstate_ = ps_collected;
    // p->group_ = nullptr; // avoid double free
    // log_printf("Tried to null group and pid for task %d\n", pid);
    ptable[pid] = nullptr;
        
    delete p;
  }
  g->child_links_.erase();
  delete g;
  // log_printf("Cleaned up process %d\n", pid);
  return exit_status;
}

// caller should validate that fd is in range and not empty
// caller should hold fd_table_lock (must be acquired before file_table_lock in all cases)
int close_fd(int fd, unsigned int* fd_table) {
  {
    spinlock_guard guard(file_table_lock);
    file_decref(&file_table[fd_table[fd]]);
  }
  fd_table[fd] = FD_EMPTY;
  return 0;
}

// returns either sz or -1
int valid_user_buffer(x86_64_pagetable *pagetable, uintptr_t addr, size_t sz, int flags) {
  if (VA_LOWEND - sz < addr) {
    return -1;
  }
  vmiter it(pagetable, addr);
  if (!(it.range_perm(sz) & flags)) {
    return -1;
  }
  return sz;
}

const unsigned int max_pathname_len = 0xFFFF; // arbitrary
// validate a null-terminated string
// returns string length (WITHOUT the null terminator), or -1 
int valid_user_buffer(x86_64_pagetable *pagetable, uintptr_t start_addr, int flags) {
  uintptr_t addr = start_addr;
  // log_printf("addr from perspective 2: %zu\n", addr);
  while (addr - start_addr < max_pathname_len && addr < VA_LOWEND) {
    // log_printf("difference is now %zu...\n", addr - start_addr);
    vmiter it(pagetable, addr);
    if (!(it.perm() & flags)) {
      return -1;
    }
    if (*reinterpret_cast<char *>(addr) == '\0') {
      return addr - start_addr;
    }
    addr++;
  }
  return -1;
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

  // if the process group is exiting, this thread must exit before returning to user space
  // safe to check here because we haven't yet modified any shared state for this syscall
  if (group_->exiting) {
    syscall_texit(regs);
  }

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

  case SYSCALL_GETPID: {
    log_printf("process %d is getpidding\n", id_);
    return pid_;
  }

  case SYSCALL_GETTID:
    return id_;

  case SYSCALL_YIELD: {
    log_printf("process %d is yielding\n", id_);
    yield();
    log_printf("process %d is between yielding and returning\n", id_);
    log_printf("rsp is %p\n", (void*)regs->reg_rsp);
    return 0;
  }

  case SYSCALL_PAGE_ALLOC: {
    uintptr_t addr = regs->reg_rdi;
    if (addr >= VA_LOWEND || (addr & 0xFFF) != 0) {
      return E_FAULT;
    }
    // Allocate before taking the lock so we don't hold a spinlock during kalloc
    void* pg = kalloc(PAGESIZE);
    if (!pg) {
      return E_NOMEM;
    }
    // The handout code does not allow allocating a page over an existing
    // page. If you do allow this, beware of memory leaks and, especially,
    // TLB invalidation (see the memory iterator documentation).
    spinlock_guard guard_pt(group_->pagetable_lock_);
    vmiter it(this, addr);
    if (it.present()) {
      kfree(pg);
      return E_BUSY;
    }
    if (it.try_map(ka2pa(pg), PTE_PWU) < 0) {
      kfree(pg);
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

  case SYSCALL_CLONE:
    return syscall_clone(regs);

  case SYSCALL_FORK:
    return syscall_fork(regs);

  case SYSCALL_EXIT: {
    syscall_exit(regs); // calls yield_noreturn();
    break; // will not be reached
  }

  case SYSCALL_READ:
    return syscall_read(regs);

  case SYSCALL_WRITE:
    return syscall_write(regs);

  case SYSCALL_LSEEK:
    return syscall_lseek(regs);

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

  case SYSCALL_CLOSE: {
    spinlock_guard guard(group_->fd_table_lock);
    int fd = regs->reg_rdi;
    if (fd < 0 || fd >= N_FILEDESC || group_->fd_table_[fd] == FD_EMPTY) {
      return E_BADF;
    }
    return close_fd(fd, group_->fd_table_); // this should just be 0 since right now close_fd always returns 0
  }

  case SYSCALL_PIPE: {
    // Find fd table slots
    int read_fd = -1, write_fd = -1;
    spinlock_guard guard(group_->fd_table_lock);
    for (int i = 0; i < N_FILEDESC; i++) {
      if (group_->fd_table_[i] == FD_EMPTY) {
	if (read_fd != -1) {
	  write_fd = i;
	  break;
	}
	read_fd = i;
      }
    }
    if (write_fd == -1) {
      return E_MFILE;
    }

    // Find file table slots
    int read_fileid = -1, write_fileid = -1;
    spinlock_guard guard_f(file_table_lock);
    for (int i = 0; i < N_FILE; i++) {
      if (file_table[i].type == FTYPE_NONE) {
	if (read_fileid != -1) {
	  write_fileid = i;
	  break;
	}
	read_fileid = i;
      }
    }
    if (write_fileid == -1) {
      return E_NFILE; // I think this is the right one
    }

    // Initialize pipe
    // (If we want really fine grained locking, we can drag the individual file locks out of
    //  this function and then release the table locks after that)
    init_pipe_files(&file_table[read_fileid], &file_table[write_fileid]);
    assert(file_table[read_fileid].flags == FREAD && file_table[write_fileid].flags == FWRITE);

    group_->fd_table_[read_fd] = read_fileid;
    group_->fd_table_[write_fd] = write_fileid;
    file_incref(&file_table[read_fileid]);
    file_incref(&file_table[write_fileid]);

    long rfd = read_fd;
    long wfd = write_fd;
    return rfd | (wfd << 32);
  }
    
  case SYSCALL_DUP2: {
    spinlock_guard guard(group_->fd_table_lock);
    int oldfd = regs->reg_rdi;
    int newfd = regs->reg_rsi;
    // Validate arguments
    if (oldfd < 0
	|| oldfd >= N_FILEDESC
	|| group_->fd_table_[oldfd] == FD_EMPTY
	|| newfd < 0
	|| newfd >= N_FILEDESC) {
      return E_BADF;
    }
    if (oldfd == newfd) {
      return newfd;
    }
    // Atomically close newfd (silently, error is ignored) and replace
    if (group_->fd_table_[newfd] != FD_EMPTY) {
      close_fd(newfd, group_->fd_table_);
    }
    group_->fd_table_[newfd] = group_->fd_table_[oldfd];
    // Increment ref count of file
    spinlock_guard guard_f(file_table_lock);
    file_incref(&file_table[group_->fd_table_[newfd]]);
    return newfd;
  }

  case SYSCALL_OPEN: {
    uintptr_t addr = regs->reg_rdi;
    int flags = regs->reg_rsi;
    // assume a filename needs to be at least one character long
    if (valid_user_buffer(group_->pagetable_, addr, PTE_P | PTE_U) <= 0) {
      return E_FAULT;
    }
    const char* pathname = reinterpret_cast<const char*>(addr);

    // read root directory to find file inode number
    // !!! IMPLIES FILE CREATION NOT SUPPORTED (true rn)
    auto ino = chkfsstate::get().lookup_inode(pathname);
    if (!ino) {
      if (flags & OF_CREATE) {
	int err = init_regular_inode(pathname);
	if (err == 0) {
	  ino = chkfsstate::get().lookup_inode(pathname);
	}
      }
      if (!ino) {
	return E_NOENT;
      }
    }

    // Make vnode
    vnode* vn = init_diskfile_vnode(std::move(ino), flags);
    
    // Find fd
    int fd = -1;
    spinlock_guard guard(group_->fd_table_lock);
    for (int i = 0; i < N_FILEDESC; i++) {
      if (group_->fd_table_[i] == FD_EMPTY) {
	fd = i;
	break;
      }
    }
    if (fd == -1) {
      delete vn;
      return E_MFILE;
    }

    // find free file table entry
    int fileid = -1;
    spinlock_guard guard_file(file_table_lock);
    for (int i = 0; i < N_FILE; i++) {
      if (file_table[i].type == FTYPE_NONE) {
	fileid = i;
	break;
      }
    }
    if (fileid == -1) {
      delete vn;
      return E_NFILE;
    }

    init_diskfile_entry(&file_table[fileid], vn, flags);
    
    // ??? maybe: abstract this into a function since I keep forgetting to do incref
    group_->fd_table_[fd] = fileid;
    file_incref(&file_table[fileid]);
    return fd;
  }
    
  case SYSCALL_EXECV: {
    uintptr_t pathname_addr = regs->reg_rdi;
    uintptr_t argv_addr = regs->reg_rsi;
    int argc = regs->reg_rdx;
    
    if (!sata_disk) {
      return E_IO;
    }
    
    // validate pathname
    if (valid_user_buffer(group_->pagetable_, pathname_addr, PTE_P | PTE_U) <= 0) {
      return E_FAULT;
    }
    const char* pathname = reinterpret_cast<const char*>(pathname_addr);

    // validate argv
    size_t argv_sz = argc * sizeof(const char*);
    if (valid_user_buffer(group_->pagetable_, argv_addr, argv_sz, PTE_P | PTE_U) == -1) {
      log_printf("argv not valid\n");
      return E_FAULT;
    }
    const char* const* argv = reinterpret_cast<const char* const*>(argv_addr);
    size_t argv_char_lens[argc]; // COUNTING NULL TERMINATORS
    size_t argv_total_chars = 0; // COUNTING NULL TERMINATORS
    for (int i = 0; i < argc; i++) {
      uintptr_t addr = reinterpret_cast<uintptr_t>(argv[i]);
      int len = valid_user_buffer(group_->pagetable_, addr, PTE_P | PTE_U);
      // log_printf("on argument %i\n", i);
      assert(len != 0); // not sure if this can/should happen
      if (len <= 0) { // should this be < ?
	log_printf("argv entry %i not valid\n", i);
	return E_FAULT;
      }
      argv_char_lens[i] = len + 1;
      argv_total_chars += len + 1;
    }
    if (argv[argc] != nullptr) {
      log_printf("argv not nullterminated\n");
      return E_FAULT;
    }
        
    // // look up memfile
    // int mindex = memfile::initfs_lookup(pathname, memfile::optional);
    // if (mindex < 0) {
    //   log_printf("memfile not found\n");
    //   return mindex;
    // }
    x86_64_pagetable* pt = knew_pagetable();
    if (!pt) {
      return E_NOMEM;
    }

    // read root directory to find file inode number
    auto ino = chkfsstate::get().lookup_inode(pathname);
    if (!ino) {
      return E_NOENT;
    }
    
    // load code and data into pagetable
    diskfile_loader ld(std::move(ino), pt);
    // memfile_loader ld(mindex, pt);
    int r = proc::load(ld);
    if (r < 0) {
      cleanup_pagetable(pt, MEMSIZE_VIRTUAL);
      return r;
    }

    // allocate new stack page
    void* stkpg = kalloc(PAGESIZE);
    if (!stkpg) {
      cleanup_pagetable(pt, MEMSIZE_VIRTUAL);
      return E_NOMEM;
    }

    // copy strings in argv into the stack page back to back, one character at a time
    // at the same time, rebuild argv, pointing to the beginnings of the strings
    uintptr_t pos = reinterpret_cast<uintptr_t>(stkpg) + PAGESIZE - argv_total_chars;
    uintptr_t argv_start = ((pos - 8 * (argc + 1)) / 8) * 8;
    assert(argv_start > reinterpret_cast<uintptr_t>(stkpg));
    char** argv_array = reinterpret_cast<char**>(argv_start);
    for (int i = 0; i < argc; i++) {
      uintptr_t new_pt_pos = pos - reinterpret_cast<uintptr_t>(stkpg) + MEMSIZE_VIRTUAL - PAGESIZE;
      argv_array[i] = reinterpret_cast<char*>(new_pt_pos);
      memcpy(reinterpret_cast<char*>(pos), argv[i], argv_char_lens[i]);
      pos += argv_char_lens[i];
      assert(*reinterpret_cast<char*>(pos - 1) == '\0');
    }
    argv_array[argc] = nullptr;
    
    // argv addr in new pagetable
    uintptr_t argv_start_new = argv_start - reinterpret_cast<uintptr_t>(stkpg) + MEMSIZE_VIRTUAL - PAGESIZE;
    
    // map new stack page
    int m = vmiter(pt, MEMSIZE_VIRTUAL - PAGESIZE).try_map(stkpg, PTE_PWU);
    if (m < 0) {
      kfree(stkpg);
      cleanup_pagetable(pt, MEMSIZE_VIRTUAL);
      return r;
    }
    // map console
    m = vmiter(pt, ktext2pa(console)).try_map(console, PTE_PWU);
    if (m < 0) { // this could be freshened up with a goto statement!
      kfree(stkpg);
      cleanup_pagetable(pt, MEMSIZE_VIRTUAL);
      return r;
    }
    
    // there's no going back now...
    // install new page table and initialize fresh registers
    x86_64_pagetable* old_pagetable = group_->pagetable_;
    init_user(pt);
    // setup rip, rsp, rdi, rsi
    regs_->reg_rip = ld.entry_rip_;
    regs_->reg_rsp = argv_start_new - 0x100; // -0x100 is arbitrary
    regs_->reg_rdi = argc; // is this still in a safe place on the stack? what about other locals?
    regs_->reg_rsi = argv_start_new;
    // clean up old pagetable
    cleanup_pagetable(old_pagetable, MEMSIZE_VIRTUAL);
    // everything has faded away into white. the process turns around, looks back one last time, but there's nothing left to see; not even the stack is reachable. it's time to move on.
    // "i'll see you on the other side..."
    yield_noreturn();
    break; // will not be reached
  }

  case SYSCALL_MSLEEP: {
    // round up to nearest 0.01 seconds
    unsigned long t_wakeup = ticks + (regs->reg_rdi + 9) / (1000 / HZ);
    // resume_counter_ = 0;
    // unsigned long initial_resumes = resume_counter_;

    spinlock_guard guard(sleep_lock);
    int q = t_wakeup & (WHEEL_QUEUES - 1);
    group_->blocked_wq_ = q;
    group_->child_exited_ = 0;

    waiter w;
    w.wait_until(sleep_wq_wheel[q], [&] () {
      return (long(t_wakeup - ticks) <= 0 || group_->child_exited_ == 1);
    }, guard);

    // unsigned long final_resumes = resume_counter_;
    // log_printf("Resumes since started sleeping (process %d): %lu\n", id_, final_resumes - initial_resumes);
    group_->blocked_wq_ = -1;
    return group_->child_exited_ ? E_INTR : 0;
  }

  case SYSCALL_GETPPID: {
    spinlock_guard guard_h(phierarchy_lock);
    return this->group_->parent_id_;
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
      task_group* g = group_->children_.front();
      if (!g) {
	// log_printf("%d: couldn't find zombies to reap, no children\n", id_);
	return format_waitpid_return(0, E_CHILD);
      }
	
      g = find_zombie_child(); // pstate_ is atomic so this doesn't need ptable lock
      if (!g) {
	if (wnohang) {
	  // log_printf("%d: couldn't find zombies to reap, still alive\n", id_);
	  return format_waitpid_return(0, E_AGAIN);
	} else {
	  // resume_counter_ = 0;
	  // unsigned long initial_resumes = resume_counter_;	  
	  waiter w;
	  w.wait_until(proc_exit_wq, [&] () {
	    return (g = find_zombie_child()); // this is intentionally an assignment
	  }, guard_h);	 
	  // unsigned long final_resumes = resume_counter_;
	  // log_printf("Resumes since started sleeping (process %d): %lu\n", id_, final_resumes - initial_resumes);	  
	}
      }
	
      assert(g);
      {
	spinlock_guard guard(g->tasks_lock_);
	assert(g->tasks_.front());
	pid = g->tasks_.front()->pid_;
      }
      spinlock_guard guard(ptable_lock);
      int exit_status = cleanup_and_return_status(g);
      return format_waitpid_return(exit_status, pid);
    } else {
      spinlock_guard guard(ptable_lock);
      if (!ptable[pid] || ptable[pid]->group_->parent_id_ != this->id_) {
	return format_waitpid_return(0, E_CHILD);
      }
      proc* p = ptable[pid];
      if (p->id_ != p->pid_) { // the id needs to be for a group leader
	return format_waitpid_return(0, E_INVAL);
      }
      guard.unlock();
      if (p->group_->live_task_count_ != 0) {
	if (wnohang) {
	  return format_waitpid_return(0, E_AGAIN);
	} else {
	  // resume_counter_ = 0;
	  // unsigned long initial_resumes = resume_counter_;    
	  waiter w;
	  w.wait_until(proc_exit_wq, [&] () {
	    return (p->group_->live_task_count_ == 0);
	  }, guard_h);
	  // unsigned long final_resumes = resume_counter_;
	  // log_printf("Resumes since started sleeping (process %d): %lu\n", id_, final_resumes - initial_resumes);	  
	}
      }
	
      assert(p->pstate_ == ps_zombie);
      guard.lock();
      int exit_status = cleanup_and_return_status(p->group_);
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

  case SYSCALL_TEXIT:
    syscall_texit(regs);
    break;  // not reached

  default:
    // no such system call
    log_printf("%d: no such system call %u\n", id_, regs->reg_rax);
    return E_NOSYS;

  }
}

// simple helper functions

#define E_NOPROC -67 // couldn't find an out of process error in lib.hh

// CALLER MUST HOLD `ptable_lock`!!!
// returns -1 on failure
int find_free_tid() {
  // avoid tid 0
  for (int pid = 1; pid != NPROC; ++pid) {
    if (!ptable[pid]) {
      return pid;
    }
  }
  return -1;
}


// CALLER MUST HOLD `ptable_lock`!!!
// returns -1 on failure to find free pid
// int find_free_pid() {
//   // avoid pid 0
//   for (int pid = 1; pid != NPROC; ++pid) {
//     if (!ptable[pid]) {
//       return pid;
//     }
//   }
//   return -1;
// }

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

// proc::syscall_clone(regs)
//    Handle clone system call.
//    Right now just a slightly modified version of fork

int proc::syscall_clone(regstate* regs) {
  // init new ptable entry
  int tid;
  proc* p;
  { 
    spinlock_guard guard_h(phierarchy_lock);
    spinlock_guard guard(ptable_lock);
    tid = find_free_tid();
    if (tid < 0) {
      // log_printf("failed to find a ptable slot\n");
      return E_NOPROC;
    }
    p = knew<proc>();
    if (!p) {
      return E_NOMEM;
    }
    p->id_ = tid;
    // !!! difference is here
    p->pid_ = pid_;
    p->group_ = this->group_;
    spinlock_guard guard_pt(group_->pagetable_lock_);
    p->init_user(group_->pagetable_); // or just set it? idk

    assert(!group_->exiting);
    group_->live_task_count_++;
    {
      spinlock_guard guard_task(group_->tasks_lock_);
      group_->tasks_.push_front(p);
    }

    *(p->regs_) = *regs;
    p->regs_->reg_rax = 0;
    ptable[tid] = p;
  }

  // copy fd_table, increment refcounts
  // spinlock_guard guard(fd_table_lock);
  // for (int i = 0; i < N_FILEDESC; i++) {
  //   p->fd_table[i] = fd_table[i];
  //   if (p->fd_table[i] != FD_EMPTY) {
  //     spinlock_guard guard_f(file_table_lock);
  //     file_incref(&file_table[fd_table[i]]);
  //     // log_printf("ok: %i\n", i);
  //   }
  // }
  
  assert(tid > 0);
  // add to run queue
  cpus[tid % ncpu].enqueue(p);
  // return new task id to caller
  // log_printf("Successfully cloned with tid %i\n", pid);
  return tid;
}

// proc::syscall_fork(regs)
//    Handle fork system call.

int proc::syscall_fork(regstate* regs) {
  // initialize process page table
  x86_64_pagetable *child_pagetable = knew_pagetable();
  if (!child_pagetable) {
    return E_NOMEM;
  }

  // copy process code and data

  // Hold pagetable_lock_ for the entire walk to get a consistent snapshot of
  // the parent's address space (a concurrent SYSCALL_PAGE_ALLOC could otherwise
  // add a mapping mid-fork)
  uintptr_t addr = 0;
  {
    spinlock_guard guard_pt(group_->pagetable_lock_);
    for (; addr < MEMSIZE_VIRTUAL; addr += PAGESIZE) {
      vmiter it(this, addr);
      if (it.writable() && addr != CONSOLE_ADDR) {
	assert(it.user());
	// alloc new physical memory for copy of parent process data
	void* pa = kalloc(PAGESIZE);
	if (!pa) {
	  cleanup_pagetable(child_pagetable, addr);
	  return E_NOMEM;
	}
	int r = vmiter(child_pagetable, it.va()).try_map(pa, it.perm());
	if (r != 0) {
	  kfree(pa);
	  cleanup_pagetable(child_pagetable, addr);
	  return E_NOMEM;
	}
	memcpy(pa, reinterpret_cast<void *>(addr), PAGESIZE);
      }
      else if (it.user()) {
	// copy read-only segments
	int r = vmiter(child_pagetable, it.va()).try_map(it.pa(), it.perm());
	if (r != 0) {
	  cleanup_pagetable(child_pagetable, addr);
	  return E_NOMEM;
	}
      }
    }
  }
  
  // init new ptable entry
  int tid;
  proc* p;
  { 
    spinlock_guard guard_h(phierarchy_lock);
    spinlock_guard guard(ptable_lock);
    tid = find_free_tid();
    if (tid < 0) {
      cleanup_pagetable(child_pagetable, addr);
      // log_printf("failed to find a ptable slot\n");
      return E_NOPROC;
    }
    p = knew<proc>();
    if (!p) {
      cleanup_pagetable(child_pagetable, addr);
      return E_NOMEM;
    }
    task_group* g = knew<task_group>();
    if (!g) {
      delete p;
      cleanup_pagetable(child_pagetable, addr);
      return E_NOMEM;
    }
    p->group_ = g;
    p->id_ = tid;
    p->pid_ = tid;

    g->parent_id_ = pid_;
    g->live_task_count_ = 1;
    g->tasks_.push_front(p);

    spinlock_guard guard_pt(g->pagetable_lock_);
    p->init_user(child_pagetable);
    *(p->regs_) = *regs;
    p->regs_->reg_rax = 0;
    ptable[tid] = p;
    // log_printf("Parenting %ld\n", p->id_);
    group_->children_.push_front(g);
    // log_printf("Done parenting %ld\n", p->id_);
  }

  // copy fd_table, increment refcounts
  spinlock_guard guard(this->group_->fd_table_lock);
  for (int i = 0; i < N_FILEDESC; i++) {
    p->group_->fd_table_[i] = this->group_->fd_table_[i];
    if (p->group_->fd_table_[i] != FD_EMPTY) {
      spinlock_guard guard_f(file_table_lock);
      file_incref(&file_table[this->group_->fd_table_[i]]);
      // log_printf("ok: %i\n", i);
    }
  }
  
  assert(tid > 0);
  // add to run queue
  cpus[tid % ncpu].enqueue(p);
  // return child pid to parent
  // log_printf("Successfully forked process with pid %i\n", pid);
  return tid;
}

// int get_ptable_index(proc* p)

// proc::syscall_texit(regs)
//    Exit current task.

void proc::syscall_texit(regstate* regs) {
  log_printf("process %d is exiting\n", id_);
  x86_64_pagetable* pt_to_free = nullptr;
  {
    spinlock_guard guard_h(phierarchy_lock);
    spinlock_guard guard(ptable_lock);
    pstate_ = ps_zombie;
    pid_t ppid = group_->parent_id_;
    if (--group_->live_task_count_ == 0) {
      log_printf("finishing exit process...\n");

      {
        spinlock_guard guard_pt(group_->pagetable_lock_);
        pt_to_free = group_->pagetable_;
        group_->pagetable_ = nullptr;
      }
      // Switch off the process page table before it is freed
      set_pagetable(early_pagetable);

      // wake up waiters (won't activate until lock is released)
      proc_exit_wq.notify_all();
      // notify parent if parent is sleeping
      if (ppid != 1
          && ptable[ppid] // valid since we have ptable_lock
          && ptable[ppid]->pstate_ == proc::ps_blocked
          ) {
        assert(ptable[ppid]->group_->blocked_wq_ != -1);
        spinlock_guard sleep_guard(sleep_lock);
        ptable[ppid]->group_->child_exited_ = 1;
        sleep_wq_wheel[ptable[ppid]->group_->blocked_wq_].notify_all();
      }
    } else {
      int bruh = group_->live_task_count_;
      log_printf("live task count: %i\n", bruh);
      {
        spinlock_guard tasks_guard(group_->tasks_lock_);
        task_links_.erase();
      }

      ptable[id_] = nullptr;
      group_ = nullptr; // avoid double free
      log_printf("Tried to null group and pid for task %d\n", id_);
      pstate_ = ps_collected;
    }
  }

  // Free old page table after releasing all locks
  if (pt_to_free) {
    cleanup_pagetable(pt_to_free, MEMSIZE_VIRTUAL);
    // regs_ = regs;
  }

  // from this point on the proc struct and stack might be obliterated
  yield_noreturn();
}


// proc::syscall_exit(regs)
//    Exit current process.

void proc::syscall_exit(regstate* regs) {  
  // log_printf("Process %ld is exiting...\n", this->id_);

  if (this->id_ == 1) {
    process_halt();
  }
    
  {
    spinlock_guard guard_h(phierarchy_lock);
    spinlock_guard guard(ptable_lock);
    // reparent kids
    for (task_group* g = group_->children_.pop_front();
	 g != nullptr;
	 g = group_->children_.pop_front()) {
      // log_printf("Reparenting %d to init\n", p->id_);
      g->parent_id_ = 1;
      ptable[1]->group_->children_.push_front(g);
      // log_printf("Done reparenting %d\n", p->id_);
    }
  }

  {
    // decrement fds
    spinlock_guard guard_f(group_->fd_table_lock);
    for (int i = 0; i < N_FILEDESC; i++) {
      if (group_->fd_table_[i] != FD_EMPTY) {
	close_fd(i, group_->fd_table_);
      }
    }
  }

  {
    // Mark the group as exiting
    spinlock_guard guard_h(phierarchy_lock);
    spinlock_guard guard(ptable_lock);
    // set exit status
    group_->exit_status_ = regs->reg_rdi;
    // mark group as exiting. However, the real sign of death is live_task_count == 0.
    group_->exiting = 1;
  }
    
  syscall_texit(regs);
}


// proc::syscall_read(regs), proc::syscall_write(regs),
// proc::syscall_readdiskfile(regs)
//    Handle read and write system calls.

uintptr_t proc::syscall_read(regstate* regs) {
  // This is a slow system call, so allow interrupts by default
  sti();

  int fd = regs->reg_rdi;
  uintptr_t addr = regs->reg_rsi;
  size_t sz = regs->reg_rdx;

  // Low-canonical read of size 0 always valid
  if (sz == 0 && addr < VA_LOWEND) {
    return 0;
  }

  // Validate the read buffer.
  if (valid_user_buffer(group_->pagetable_, addr, sz, PTE_P | PTE_W | PTE_U) == -1) {
    return E_FAULT;
  }

  file* f;
  irqstate irqs;
  {
    spinlock_guard guard(group_->fd_table_lock);
    // Check that fd is valid
    if (fd < 0 || fd >= N_FILEDESC || group_->fd_table_[fd] == FD_EMPTY) {
      return E_BADF;
    }

    spinlock_guard guard_file(file_table_lock);
    f = &(file_table[group_->fd_table_[fd]]);
    irqs = f->file_lock.lock();
    if (f->type == FTYPE_NONE) {
      f->file_lock.unlock(irqs);
      return E_BADF;
    }
    // file_read() MUST unlock file_lock once it has obtained its next lock
  }
  // even though file_lock.lock() (with irq), we need to manually disable interrupts
  // since the guards going out of scope above re-enable interrupts 
  cli();
  int n_read = file_read(f, reinterpret_cast<char*>(addr), sz, irqs);
  return n_read;
}

uintptr_t proc::syscall_write(regstate* regs) {
  // This is a slow system call, so allow interrupts by default
  sti();

  int fd = regs->reg_rdi;
  uintptr_t addr = regs->reg_rsi;
  size_t sz = regs->reg_rdx;
  // log_printf("addr: %zu\n", addr);
  // log_printf("sz: %zu\n", sz);
  // log_printf("memsize virt: %zu\n", MEMSIZE_VIRTUAL);

  // Low-canonical write of size 0 always valid
  if (sz == 0 && addr < VA_LOWEND) {
    return 0;
  }

  // Validate write buffer
  if (valid_user_buffer(group_->pagetable_, addr, sz, PTE_P | PTE_U) == -1) {
    return E_FAULT;
  }

  file* f;
  irqstate irqs;
  {
    spinlock_guard guard(group_->fd_table_lock);
    // Check that fd is valid
    if (fd < 0 || fd >= N_FILEDESC || group_->fd_table_[fd] == FD_EMPTY) {
      return E_BADF;
    }

    spinlock_guard guard_file(file_table_lock);
    f = &(file_table[group_->fd_table_[fd]]);
    irqs = f->file_lock.lock();
    if (f->type == FTYPE_NONE) {
      f->file_lock.unlock(irqs);
      return E_BADF;
    }
    // handoff: file_write MUST unlock file_lock, using irqs (this might be very sketchy)
  }
  cli(); // !!! not sure how to avoid scheduling while spinlocked without doing this
  return file_write(f, reinterpret_cast<char*>(addr), sz, irqs);
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

ssize_t proc::syscall_lseek(regstate* regs) {
  int fd = regs->reg_rdi;
  off_t off = regs->reg_rsi;
  int whence = regs->reg_rdx;

  // file-getting boilerplate
  file* f;
  irqstate irqs;
  spinlock_guard guard_fdtable(group_->fd_table_lock);
  // Check that fd is valid
  if (fd < 0 || fd >= N_FILEDESC || group_->fd_table_[fd] == FD_EMPTY) {
    return E_BADF;
  }

  spinlock_guard guard_ftable(file_table_lock);
  f = &(file_table[group_->fd_table_[fd]]);
  spinlock_guard guard_file(f->file_lock);    
  if (f->type == FTYPE_NONE) {
    return E_BADF;
  }
  guard_fdtable.unlock();
  guard_ftable.unlock();
  
  off_t sz = f->size_;
  off_t cur = f->off_;
  
  if (sz < 0) {
    return sz; // an error code
  }
  
  switch (whence) {
  case LSEEK_SET: {
    break;
  }
  case LSEEK_CUR: {
    if (MAX_OFF_T - cur < off) {
      return E_RANGE;
    }
    off += cur;
    break;
  }
  case LSEEK_END: {
    if (MAX_OFF_T - sz < off) {
      return E_RANGE;
    }
    off += sz;
    break;
  }
  case LSEEK_SIZE: { 
    return sz;
  }
  default:{
    return E_INVAL;    
  }
  }
  
  if (off < 0) {
    return E_INVAL;
  }
  
  f->off_ = off;
  return static_cast<ssize_t>(off);
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
	  || !ptable[showing]->group_
	  || !ptable[showing]->group_->pagetable_
	  || ptable[showing]->group_->pagetable_ == early_pagetable
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
