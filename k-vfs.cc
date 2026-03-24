#include "k-vfs.hh"
#include "k-devices.hh"

// k-vfs.cc
//
//    Virtual file system

int vnode_fops::fo_decref(file* f) const {
  // lock here?
  assert(f->refcount_ > 0);
  if (--f->refcount_ == 0) {
    if (vnode_decref(f->vnode_) == 0) {
      kfree(f->vnode_); // caller should then free this file slot
    }
  }
  return f->refcount_; 
}

int vnode_fops::fo_read(file* f, char* buf, size_t sz) const {
  // add argument verification?
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
  uio arg;
  arg.off = f->off_;
  f->off_ += sz;
  // !!! EOF handling
  arg.buf = buf;
  arg.sz = sz;
  return f->vnode_->ops->vop_write(f->vnode_, &arg);
}

int kcfs_vops::vop_decref(vnode* vn) const {
  assert(vn->refcount > 0);
  return --vn->refcount; // caller should then free this vnode
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

int file_incref(file* f) {
  spinlock_guard guard(f->file_lock);
  return f->ops->fo_incref(f);
}

int file_deccref(file* f) {
  spinlock_guard guard(f->file_lock);
  return f->ops->fo_decref(f);
}

int file_read(file* f, char* buf, size_t sz) {
  spinlock_guard guard(f->file_lock);
  return f->ops->fo_read(f, buf, sz);  
}

int file_write(file* f, char* buf, size_t sz) {
  spinlock_guard guard(f->file_lock);
  return f->ops->fo_write(f, buf, sz);  
}

int vnode_incref(vnode* vn) {
  spinlock_guard guard(vn->refcount_lock);
  return vn->ops->vop_incref(vn);
}

int vnode_decref(vnode* vn) {
  spinlock_guard guard(vn->refcount_lock);
  return vn->ops->vop_decref(vn);
}

void init_kc_file(file* kc_file) {
  // set up keyboard/console vnode
  vnode* kcvn = knew<vnode>(&kc_vops);
  {
    spinlock_guard guard(kcvn->refcount_lock);
    kcvn->refcount = 1; // ??? is this incremented per file pointing to this vnode or what
  }
  {
    spinlock_guard guard_file(kc_file->file_lock);
    kc_file->type = FTYPE_VNODE;
    kc_file->refcount_ = 0; 
    kc_file->flags = FREAD | FWRITE;
    kc_file->off_ = 0;
    kc_file->vnode_ = kcvn;
    kc_file->ops = &vn_fops;
  }
}
