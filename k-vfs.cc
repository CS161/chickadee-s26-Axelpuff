#include "k-vfs.hh"
#include "k-devices.hh"
#include <limits>

// k-vfs.cc
//
//    Virtual file system

ssize_t bbuffer::write(const char* buf, size_t sz) {
  assert(this->lock_.is_locked());
  assert(!this->write_closed_);
  assert(this->blen_ < bcapacity);
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
    return -1; // ??? what should this be, if anything
  } else {
    this->nonempty_.notify_all();
    return pos;
  }
}

ssize_t bbuffer::read(char* buf, size_t sz) {
  assert(this->lock_.is_locked());
  assert(!this->read_closed_);
  assert(this->blen_ > 0);
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
    log_printf("L\n");    return -1;  // ??? what should this be, if anything
  } else {
    this->nonfull_.notify_all();
    log_printf("I am successfully reading from a pipe\n");
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

const off_t MAX_OFF_T = std::numeric_limits<off_t>::max();
const off_t MAX_SZ_T = std::numeric_limits<size_t>::max();

int vnode_fops::fo_read(file* f, char* buf, size_t sz, irqstate &irqs) const {
  assert(f->file_lock.is_locked());
  // Lock handoff
  auto vn_irqs = f->vnode_->refcount_lock.lock();
  f->file_lock.unlock(irqs);
  
  if (!(f->flags | FREAD)) {
    f->vnode_->refcount_lock.unlock(vn_irqs);
    return E_BADF;
  }
  assert(MAX_OFF_T - static_cast<off_t>(sz) > f->off_);
  
  uio arg;
  arg.off = f->off_;
  f->off_ += sz;
  arg.buf = buf;
  arg.sz = sz;
  return f->vnode_->ops->vop_read(f->vnode_, &arg, vn_irqs);
}

int vnode_fops::fo_write(file* f, char* buf, size_t sz, irqstate &irqs) const {
  assert(f->file_lock.is_locked());
  // Lock handoff
  auto vn_irqs = f->vnode_->refcount_lock.lock();
  f->file_lock.unlock(irqs);
  
  if (!(f->flags | FREAD)) {
    f->vnode_->refcount_lock.unlock(vn_irqs);
    return E_BADF;
  }
  assert(MAX_OFF_T - static_cast<off_t>(sz) > f->off_);

  uio arg;
  arg.off = f->off_;
  f->off_ += sz;
  arg.buf = buf;
  arg.sz = sz;
  return f->vnode_->ops->vop_write(f->vnode_, &arg, vn_irqs);
}

int pipe_fops::fo_decref(file* f) const {
  assert(f->refcount_ > 0);
  if (--f->refcount_ == 0) {
  spinlock_guard guard(f->pipe_->lock_);
    f->type = FTYPE_NONE;
    if (f->flags == FREAD) {
      f->pipe_->read_closed_ = true;
      f->pipe_->nonfull_.notify_all();
    } else {
      assert(f->flags == FWRITE);
      f->pipe_->write_closed_ = true;
      log_printf("TERMINATED\n");
      f->pipe_->nonempty_.notify_all();      
    }
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

int pipe_fops::fo_read(file* f, char* buf, size_t sz, irqstate &irqs) const {
  // assert(!file_table_lock.is_locked());
  assert(f->file_lock.is_locked());
  // Lock handoff
  spinlock_guard guard(f->pipe_->lock_);
  f->file_lock.unlock(irqs);
  if (f->flags == FWRITE) {
    assert(f->flags == FWRITE);
    return E_BADF;
  }
  if (f->pipe_->write_closed_ && f->pipe_->is_empty()) {
    return 0; // EOF
  }
  // blocking logic is here since I don't know how to perform lock handoff otherwise
  waiter w;
  log_printf("Reader might block\n");
  w.wait_until(f->pipe_->nonempty_, [&] () {
    return (f->pipe_->blen_ > 0 || f->pipe_->write_closed_);
  }, guard);
  if (f->pipe_->write_closed_ && f->pipe_->is_empty()) {
    log_printf("Never got to try to read\n");
    return 0;
  }
  return f->pipe_->read(buf, sz);
}

int pipe_fops::fo_write(file* f, char* buf, size_t sz, irqstate &irqs) const {
  // assert(!file_table_lock.is_locked());
  assert(f->file_lock.is_locked());
  // Lock handoff
  spinlock_guard guard(f->pipe_->lock_);
  f->file_lock.unlock(irqs);
  if (f->flags == FREAD) {
    assert(f->flags == FREAD);
    return E_BADF;
  }
  if (f->pipe_->read_closed_) {
    return E_PIPE;
  }  
  // blocking logic is here since I don't know how to perform lock handoff otherwise
  waiter w;
  log_printf("Writer might block\n");
  w.wait_until(f->pipe_->nonfull_, [&] () {
    return (f->pipe_->blen_ < f->pipe_->bcapacity || f->pipe_->read_closed_);
  }, guard);
  if (f->pipe_->read_closed_) {
    // I think 0 is appropriate rather than E_BADF because
    // if we got past the initial check in the caller, the read
    // end got closed after we tried to start writing, which isn't
    // the caller's fault
    return 0;
  }
  return f->pipe_->write(buf, sz);
}

// kcfs_vops (keyboard-console "file system" vnode functions)

int kcfs_vops::vop_decref(vnode* vn) const {
  assert(vn->refcount > 0);
  return --vn->refcount; // caller should then free this vnode
}

int kcfs_vops::vop_read(vnode* vn, uio* uio, irqstate &irqs) const {
  auto& kbd = keyboardstate::get();
  // Lock handoff
  spinlock_guard guard(kbd.lock_);
  vn->refcount_lock.unlock(irqs);
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
  
int kcfs_vops::vop_write(vnode* vn, uio* uio, irqstate &irqs) const {
  uintptr_t addr = reinterpret_cast<uintptr_t>(uio->buf);
  auto& csl = consolestate::get();
  // Lock handoff
  spinlock_guard guard(csl.lock_);
  vn->refcount_lock.unlock(irqs);  
  size_t n = 0;
  while (n < uio->sz) {
    int ch = *reinterpret_cast<const char*>(addr);
    ++addr;
    ++n;
    console_printf(CS_WHITE "%c", ch);
  }
  return n;
}

// memf_vops (memfile vnode functions)

int memf_vops::vop_decref(vnode* vn) const { // may be worth inlining if all vop_decrefs look like this
  assert(vn->refcount > 0);
  return --vn->refcount; // caller should then free this vnode
}

int memf_vops::vop_read(vnode* vn, uio* uio, irqstate &irqs) const {
  log_printf("I'm getting a read of size %zu\n", uio->sz);
  log_printf("Offset is %lu\n", uio->off);
  // lock
  auto init_irqs = memfile::initfs_lock.lock();
  // find memfile
  assert(vn->mindex >= 0);
  memfile* m = &(memfile::initfs[vn->mindex]);
  // get memfile lock
  spinlock_guard guard(m->lock_);
  log_printf("File len is %lu\n", m->len_);
  // let go of initfs lock and vnode lock
  memfile::initfs_lock.unlock(init_irqs);
  vn->refcount_lock.unlock(irqs);
  // read from relevant location in file (return 0 if past end, also log printf/assert that)
  if (static_cast<size_t>(uio->off) >= m->len_) {
    return 0;
  }
  uintptr_t start_copy = reinterpret_cast<uintptr_t>(m->data_) + uio->off;
  size_t read_sz = min(m->len_ - static_cast<size_t>(uio->off), uio->sz);
  memcpy(uio->buf, reinterpret_cast<char *>(start_copy), read_sz);
  // return amount read
  // (this will not get reflected in the file struct `off` if it is at the end of the
  // file and reads less than the intended amount, but this has no functional effect)
  log_printf("Returned size: %zu\n", read_sz);
  return read_sz;
}
  
int memf_vops::vop_write(vnode* vn, uio* uio, irqstate &irqs) const {
  log_printf("I'm getting a write of size %zu\n", uio->sz);
  // lock
  auto init_irqs = memfile::initfs_lock.lock();
  // find memfile
  assert(vn->mindex >= 0);
  memfile* m = &(memfile::initfs[vn->mindex]);
  // get memfile lock
  spinlock_guard guard(m->lock_);
  // let go of initfs lock and vnode lock
  memfile::initfs_lock.unlock(init_irqs);
  vn->refcount_lock.unlock(irqs);
  if (MAX_SZ_T - uio->sz < static_cast<size_t>(uio->off)) {
  log_printf("Capacity is  %zu\n", m->capacity_);
    return E_NOSPC;
  }
  // expand file if needed
  size_t needed_file_sz = uio->sz + static_cast<size_t>(uio->off);
  if (needed_file_sz > m->capacity_) {
    int s = m->set_length(needed_file_sz);
    if (s < 0) {
      return s;
    }
  }
  // write to relevant location in file
  uintptr_t start_copy = reinterpret_cast<uintptr_t>(m->data_) + uio->off;
  memcpy(reinterpret_cast<char *>(start_copy), uio->buf, uio->sz);
  return uio->sz;
}

// chkfs_vops (chickadee disk file system vnode functions)

int chkfs_vops::vop_decref(vnode* vn) const { // may be worth inlining if all vop_decrefs look like this
  assert(vn->refcount > 0);
  return --vn->refcount; // caller should then free this vnode
}

int chkfs_vops::vop_read(vnode* vn, uio* uio, irqstate &irqs) const {
  log_printf("I'm getting a read of size %zu\n", uio->sz);
  log_printf("Offset is %lu\n", uio->off);
  // lock
  vn->ino_->lock_read();
  size_t sz = vn->ino_->size; // ??? is vn->ino_->size the right thing?
  // bcslot* slot = vn->ino_->slot(); 
  log_printf("File size is %lu\n", sz);
  // let go of vnode lock
  vn->refcount_lock.unlock(irqs);
  // read from relevant location in file (return 0 if past end)
  if (static_cast<size_t>(uio->off) >= sz) {
    return 0;
  }

  chkfs_fileiter it(vn->ino_.get());
  
  size_t nread = 0;
  off_t off = uio->off;
  while (nread < uio->sz) {
    // copy data from current block
    if (auto e = it.find(off).load()) {
      unsigned b = it.block_relative_offset();
      size_t ncopy = min(
			 size_t(vn->ino_->size - it.offset()),   // bytes left in file
			 chkfs::blocksize - b,              // bytes left in block
			 uio->sz - nread                         // bytes left in request
			 );
      memcpy(uio->buf + nread, e->buf_ + b, ncopy);

      nread += ncopy;
      off += ncopy;
      if (ncopy == 0) {
	break;
      }
    } else {
      break;
    }
  }
  // assert(slot->state_ == bcslot::s_clean || slot->state_ == bcslot::s_dirty);
  // ??? I was trying to copy directly from the slot cache. Was I insane? Does chkfs_fileiter already handle this?
  // uintptr_t start_copy = reinterpret_cast<uintptr_t>(slot->buf_) + uio->off;
  // size_t read_sz = min(sz - static_cast<size_t>(uio->off), uio->sz);
  // memcpy(uio->buf, reinterpret_cast<char *>(start_copy), read_sz);
  // unlock
  vn->ino_->unlock_read();
  // return amount read
  // (this will not get reflected in the file struct `off` if it is at the end of the
  // file and reads less than the intended amount, but this has no functional effect)
  log_printf("Returned size: %zu\n", nread);
  return nread;
}
  
int chkfs_vops::vop_write(vnode* vn, uio* uio, irqstate &irqs) const {
  // NOT AT ALL DONE YET!!!
  // AWFUL IMPLEMENTATION RN (DIRECT WRITING TO SLOT)
  log_printf("I'm getting a write of size %zu\n", uio->sz);
  // lock
  vn->ino_->lock_write();
  bcslot* slot = vn->ino_->slot();
  slot->lock_buffer();
  // let go of vnode lock
  vn->refcount_lock.unlock(irqs);
  if (MAX_SZ_T - uio->sz < static_cast<size_t>(uio->off)) {
    return E_NOSPC;
  }
  // expand file if needed
  // size_t needed_file_sz = uio->sz + static_cast<size_t>(uio->off);
  // if (needed_file_sz > m->capacity_) {
  //   int s = m->set_length(needed_file_sz);
  //   if (s < 0) {
  //     return s;
  //   }
  // }
  // write to relevant location in file
  chkfs_fileiter it(vn->ino_.get());
  
  size_t nwrite = 0;
  off_t off = uio->off;
  while (nwrite < uio->sz) {
    // copy data from current block
    if (auto e = it.find(off).load()) {
      e->lock_buffer(); // no deadlock risk I think?
      unsigned b = it.block_relative_offset();
      size_t ncopy = min(
			 size_t(vn->ino_->size - it.offset()),   // bytes left in file
			 chkfs::blocksize - b,              // bytes left in block
			 uio->sz - nwrite                         // bytes left in request
			 );
      memcpy(e->buf_ + b, uio->buf + nwrite, ncopy);
      e->unlock_buffer();
      
      nwrite += ncopy;
      off += ncopy;
      // not implemented yet (write past end of file, write past end of block)
      assert(size_t(vn->ino_->size - it.offset()) != 0);
      assert(chkfs::blocksize - b != 0);
      if (ncopy == 0) { // ??? does this do the intended behavior here? why did this work for writes in the first place?
	break;
      }
    } else {
      break;
    }
  }
  //   uintptr_t start_copy = reinterpret_cast<uintptr_t>(slot->buf_) + uio->off;
  //   memcpy(reinterpret_cast<char *>(start_copy), uio->buf, uio->sz);
  // slot->unlock_buffer();
 
  vn->ino_->unlock_write();
  // !!! have to do flush stuff (writeback) eventually
  return uio->sz;
}

file file_table[N_FILE];
spinlock file_table_lock;

vnode_fops vn_fops;
pipe_fops p_fops;
kcfs_vops kc_vops;
memf_vops mf_vops;
chkfs_vops chk_vops;

int file_incref(file* f) {
  spinlock_guard guard(f->file_lock);
  return f->ops->fo_incref(f);
}

int file_decref(file* f) {
  spinlock_guard guard(f->file_lock);
  return f->ops->fo_decref(f);
}

int file_read(file* f, char* buf, size_t sz, irqstate &irqs) {
  return f->ops->fo_read(f, buf, sz, irqs);  
}

int file_write(file* f, char* buf, size_t sz, irqstate &irqs) {
  return f->ops->fo_write(f, buf, sz, irqs);  
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
// caller should possess file_table_lock (?)
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

// caller should possess file_table_lock
int init_memfile_entry(file* file_slot, const char* pathname, int flags) {
  // exception to lock acquisiton here, where vnode comes last, because it doesn't make sense to allocate and
  // deallocate it (also nothing will contend for it)
  spinlock_guard guard_file(file_slot->file_lock);

  // Find memfile, or return error
  int mindex;
  spinlock_guard guard_i(memfile::initfs_lock);
  mindex = memfile::initfs_lookup(pathname, (flags & OF_CREATE)
				  ? memfile::create : memfile::optional);
  if (mindex < 0) { // error
    return mindex;
  }
    
  memfile* m = &(memfile::initfs[mindex]);
  spinlock_guard guard_memfile(m->lock_);
  if (flags & OF_TRUNC) {
    m->set_length(0);
  }

  // Make vnode
  vnode* mfvn = knew<vnode>(&mf_vops);
  int file_flags = ((flags | OF_READ) ? FREAD : 0) | ((flags | OF_WRITE) ? FWRITE : 0);
  {
    spinlock_guard guard(mfvn->refcount_lock);
    mfvn->refcount = 1;
    mfvn->mindex = mindex;
  }
  
  file_slot->type = FTYPE_VNODE;
  file_slot->refcount_ = 0; 
  file_slot->flags = file_flags;
  file_slot->off_ = 0;
  file_slot->vnode_ = mfvn;
  file_slot->ops = &vn_fops;
  return 0;
}

// caller should possess file_table_lock
int init_diskfile_entry(file* file_slot, chkfs_iref ino, int flags) {
  // exception to lock acquisiton here, where vnode comes last, because it doesn't make sense to allocate and
  // deallocate it (also nothing will contend for it)
  spinlock_guard guard_file(file_slot->file_lock);
  
  // Make vnode
  vnode* chkvn = knew<vnode>(&chk_vops, std::move(ino));
  int file_flags = ((flags | OF_READ) ? FREAD : 0) | ((flags | OF_WRITE) ? FWRITE : 0);
  {
    spinlock_guard guard(chkvn->refcount_lock);
    chkvn->refcount = 1;
  }

  ino->unlock_write();
  
  file_slot->type = FTYPE_VNODE;
  file_slot->refcount_ = 0; 
  file_slot->flags = file_flags;
  file_slot->off_ = 0;
  file_slot->vnode_ = chkvn;
  file_slot->ops = &vn_fops;
  return 0;
}

