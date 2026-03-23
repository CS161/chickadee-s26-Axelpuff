#include "kernel.hh"

// k-vfs.cc
//
//    Virtual file system

#define N_FILEDESC 16 /* size of per-process file descriptor table */
#define N_FILE 128 /* global file system file table count */

#define FD_EMPTY 255  /* "this fd slot is empty" */

#define	FTYPE_NONE	0	/* not yet initialized */
#define	FTYPE_VNODE	1	/* file */
#define	FTYPE_PIPE	2	/* pipe */

struct vnode;
struct file_ops;
struct vnode_ops;

struct file {
  spinlock file_lock; // should be held when doing any file operation
  int type;
  int refcount_;
  int flags;
  off_t off_;
  vnode* vnode_;

  const file_ops* ops;
};

struct uio { /* "user input output"? maybe? */
  off_t off;
  char* buf;
  size_t sz;
};

struct vnode {
  int refcount;
  spinlock refcount_lock;

  const vnode_ops* ops;
};

struct file_ops {
  virtual ~file_ops() = default;
  virtual int fo_read(file* f, char* buf, size_t sz) const = 0;
  virtual int fo_write(file* f, char* buf, size_t sz) const = 0;
};

struct vnode_ops {
  virtual ~vnode_ops() = default;
  virtual int vop_read(vnode* vn, uio* uio) const = 0;
  virtual int vop_write(vnode* vn, uio* uio) const = 0;
};

extern file file_table[N_FILE];
extern spinlock file_table_lock;

struct vnode_fops : public file_ops { // i.e. as opposed to pipe_fops
  int fo_read(file* f, char* buf, size_t sz) const override {
    // add argument verification?
    spinlock_guard guard(f->file_lock);
    uio arg;
    arg.off = f->off_;
    f->off_ += sz;
    // !!! EOF handling
    arg.buf = buf;
    arg.sz = sz;
    return f->vnode_->ops->vop_read(f->vnode_, &arg); // is this sus
  }
  int fo_write(file* f, char* buf, size_t sz) const override {
    // add argument verification?
    spinlock_guard guard(f->file_lock);
    uio arg;
    arg.off = f->off_;
    f->off_ += sz;
    // !!! EOF handling
    arg.buf = buf;
    arg.sz = sz;
    return f->vnode_->ops->vop_write(f->vnode_, &arg);
  }
};
  
struct kcfs_vops : public vnode_ops { // "keyboard-console file system"
  int vop_read(vnode* vn, uio* uio) const override {
    auto& kbd = keyboardstate::get();
    spinlock_guard guard(kbd.lock_);
    uintptr_t addr = reinterpret_cast<uintptr_t>(uio->buf); // ??? ignore offset for kcfs_vops? should this be reflected in file offset not increasing?
    
    // mark that we are now reading from the keyboard
    // (so `q` should not power off)
    if (kbd.state_ == kbd.boot) {
      kbd.state_ = kbd.input;
    }

    waiter w;
    w.wait_until(kbd.wq_, [&] () {
      return (uio->sz == 0 || kbd.eol_ != 0);
    }, guard);


    // read that line or lines
    size_t n = 0;
    while (kbd.eol_ != 0 && n < uio->sz) {
      if (kbd.buf_[kbd.pos_] == 0x04) {
	// Ctrl-D means EOF
	if (n == 0) {
	  kbd.consume(1);
	}
	break;
      } else {
	*reinterpret_cast<char*>(addr) = kbd.buf_[kbd.pos_];
	++addr;
	++n;
	kbd.consume(1);
      }
    }

    // kbd.lock_.unlock(irqs);
    return n;
  }
  
  int vop_write(vnode* vn, uio* uio) const override {
    uintptr_t addr = reinterpret_cast<uintptr_t>(uio->buf);
    auto& csl = consolestate::get();
    spinlock_guard guard(csl.lock_);
    size_t n = 0;
    while (n < uio->sz) {
      int ch = *reinterpret_cast<const char*>(addr);
      ++addr;
      ++n;
      console_printf(CS_WHITE "%c", ch);
    }
    return n;
  }
};

static vnode_fops vn_fops;
static kcfs_vops kc_vops;
