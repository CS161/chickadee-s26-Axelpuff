#include "k-vfs.hh"
#include "k-devices.hh"

// k-vfs.cc
//
//    Virtual file system

ssize_t bbuffer::write(const char* buf, size_t sz) {
  spinlock_guard guard(lock_);
  assert(!this->write_closed_);
  //   waiter w;
  // w.wait_until(this->wq_, [&] () {
  //   return (this->blen_ < bcapacity);
  // }, guard);  
  size_t pos = 0;
  while (pos < sz && this->blen_ < bcapacity) {
    size_t bindex = (this->bpos_ + this->blen_) % bcapacity;
    size_t bspace = min(bcapacity - bindex, bcapacity - this->blen_);
    size_t n = min(sz - pos, bspace);
    memcpy(&this->bbuf_[bindex], &buf[pos], n);
    this->blen_ += n;
    pos += n;
  }
  if (pos == 0 && sz > 0) {
    return -1;  // try again
  } else {
    return pos;
  }
}

ssize_t bbuffer::read(char* buf, size_t sz) {
  spinlock_guard guard(lock_);
  size_t pos = 0;
  while (pos < sz && this->blen_ > 0) {
    size_t bspace = min(this->blen_, bcapacity - this->bpos_);
    size_t n = min(sz - pos, bspace);
    memcpy(&buf[pos], &this->bbuf_[this->bpos_], n);
    this->bpos_ = (this->bpos_ + n) % bcapacity;
    this->blen_ -= n;
    pos += n;
  }
  if (pos == 0 && sz > 0 && !this->write_closed_) {
    return -1;  // try again
  } else {
    return pos;
  }
}

int vnode_fops::fo_decref(file* f) const {
  // lock here?
  assert(f->refcount_ > 0);
  if (--f->refcount_ == 0) {
    if (f->vnode_->ops->vop_decref(f->vnode_) == 0) {
      kfree(f->vnode_);
    }
    f->type = FTYPE_NONE;
    // caller doesn't really have to do anything
  }
  return f->refcount_; 
}

int vnode_fops::fo_read(file* f, char* buf, size_t sz) const {
  // add argument verification?
  uio arg;
  {
    spinlock_guard guard(f->file_lock);
    arg.off = f->off_;
    f->off_ += sz;
  }
  // !!! EOF handling, off overflow?
  arg.buf = buf;
  arg.sz = sz;
  return f->vnode_->ops->vop_read(f->vnode_, &arg); // is this sus
}

int vnode_fops::fo_write(file* f, char* buf, size_t sz) const {
  // add argument verification?
  uio arg;
  {
    spinlock_guard guard(f->file_lock);
    arg.off = f->off_;
    f->off_ += sz;
  }
  // !!! EOF handling, off overflow?
  arg.buf = buf;
  arg.sz = sz;
  return f->vnode_->ops->vop_write(f->vnode_, &arg);
}

int pipe_fops::fo_decref(file* f) const {
  assert(f->refcount_ > 0);
  if (--f->refcount_ == 0) {
    spinlock_guard guard(f->pipe_->lock_);
    if (f->type == FREAD) {
      f->pipe_->read_closed_ = true;
    } else {
      assert(f->type == FWRITE);
      f->pipe_->write_closed_ = true;
    }
    f->type = FTYPE_NONE;
    // f->pipe_->wq_.notify_all();
    if (f->pipe_->read_closed_ && f->pipe_->write_closed_) {
      // ??? let go of the lock temporarily to let any blocked processes respond
      //     before blowing up the pipe (does this work? is this needed?)
      guard.unlock();
      guard.lock();
      kfree(f->pipe_);
    }
  }
  return f->refcount_; 
}

int pipe_fops::fo_read(file* f, char* buf, size_t sz) const {
  if (f->flags == FWRITE) {
    assert(f->flags == FWRITE);
    log_printf("trying to read from write end\n");
    return E_BADF;
  }
  if (f->pipe_->write_closed_ && f->pipe_->is_empty()) {
    return 0; // EOF
  }
  return f->pipe_->read(buf, sz);
}

int pipe_fops::fo_write(file* f, char* buf, size_t sz) const {
  if (f->flags == FREAD) {
    assert(f->flags == FREAD);
    return E_BADF;
  }
  if (f->pipe_->read_closed_) {
    return E_PIPE;
  }
  return f->pipe_->write(buf, sz);
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
pipe_fops p_fops;
kcfs_vops kc_vops;

int file_incref(file* f) {
  spinlock_guard guard(f->file_lock);
  return f->ops->fo_incref(f);
}

int file_decref(file* f) {
  spinlock_guard guard(f->file_lock);
  int bluh = f->ops->fo_decref(f);
  log_printf("da file is at %i refs\n", bluh);
  return bluh;
  // return f->ops->fo_decref(f);
}

int file_read(file* f, char* buf, size_t sz) {
  // spinlock_guard guard(f->file_lock);
  return f->ops->fo_read(f, buf, sz);  
}

int file_write(file* f, char* buf, size_t sz) {
  // spinlock_guard guard(f->file_lock);
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
// initialize the two given files as pipe read and write ends
void init_pipe_files(file* read_file, file* write_file) {
  bbuffer* pipe = knew<bbuffer>();
  {
    spinlock_guard guard_file(read_file->file_lock);
    read_file->type = FTYPE_PIPE;
    read_file->refcount_ = 0; 
    read_file->flags = FREAD;
    read_file->pipe_ = pipe;
    read_file->ops = &p_fops;
  }
  {
    spinlock_guard guard_file(write_file->file_lock);
    write_file->type = FTYPE_PIPE;
    write_file->refcount_ = 0; 
    write_file->flags = FWRITE;
    write_file->pipe_ = pipe;
    write_file->ops = &p_fops;
  }
}
