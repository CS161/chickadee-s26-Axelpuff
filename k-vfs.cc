#include "k-vfs.hh"
#include "k-devices.hh"

// k-vfs.cc
//
//    Virtual file system

int vnode_fops::fo_read(file* f, char* buf, size_t sz) const {
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

int vnode_fops::fo_write(file* f, char* buf, size_t sz) const {
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
  
int kcfs_vops::vop_read(vnode* vn, uio* uio) const {
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
  
int kcfs_vops::vop_write(vnode* vn, uio* uio) const {
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

file file_table[N_FILE];
spinlock file_table_lock;

vnode_fops vn_fops;
kcfs_vops kc_vops;

int fileread(file* f, char* buf, size_t sz) {
  return -1;  
}
