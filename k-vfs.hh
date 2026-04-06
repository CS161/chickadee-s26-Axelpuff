#include "kernel.hh" // holy crap this WILL NOT COMPILE with anything less than this
#include "k-chkfs.hh"
#include "k-chkfsiter.hh"

// k-vfs.cc
//
//    Virtual file system

#define N_FILE 128 /* global file system file table count */
#define KC_FILE_NUM 0

#define FD_EMPTY 255  /* "this fd slot is empty" */

#define	FTYPE_NONE	0	/* not yet initialized */
#define	FTYPE_VNODE	1	/* file */
#define	FTYPE_PIPE	2	/* pipe */

#define	FREAD		0x0001
#define	FWRITE		0x0002

struct vnode;
struct bbuffer;
struct file_ops;
struct vnode_ops;

struct file {
  spinlock file_lock; // should be held when doing any file operation
  int type;
  int refcount_;
  int flags;
  off_t off_; // This might be greater than the length of the file
  vnode* vnode_;
  bbuffer* pipe_;

  file_ops* ops;
};

struct uio { /* "user input output"? maybe? */
  off_t off;
  char* buf;
  size_t sz;
};

struct vnode {
  int refcount;
  spinlock refcount_lock;

  int mindex = -1;
  
  const vnode_ops* ops;
  vnode(vnode_ops* vn_ops): ops(vn_ops) {
  }
};

struct bbuffer {
  static constexpr size_t bcapacity = 128;
  char bbuf_[bcapacity];
  size_t bpos_ = 0;
  size_t blen_ = 0;
  bool write_closed_ = false;
  bool read_closed_ = false;
  spinlock lock_;
  wait_queue nonfull_;
  wait_queue nonempty_;

  ssize_t read(char* buf, size_t sz);
  ssize_t write(const char* buf, size_t sz);
  inline int is_empty() {
    return blen_ == 0;
  }
  // void shutdown_write();
};

// ops methods SHOULD NOT BE CALLED by external code
// external code should only use the helper methods below

struct file_ops {
  virtual ~file_ops() = default;
  inline int fo_incref(file* f) const {
    return ++f->refcount_;
  };
  virtual int fo_decref(file* f) const = 0;
  virtual int fo_read(file* f, char* buf, size_t sz, irqstate &irqs) const = 0;
  virtual int fo_write(file* f, char* buf, size_t sz, irqstate &irqs) const = 0;
};

struct vnode_ops {
  virtual ~vnode_ops() = default;
  inline int vop_incref(vnode* vn) const {
    return ++vn->refcount;
  };
  virtual int vop_decref(vnode* vn) const = 0;
  virtual int vop_read(vnode* vn, uio* uio, irqstate &irqs) const = 0;
  virtual int vop_write(vnode* vn, uio* uio, irqstate &irqs) const = 0;
};

extern file file_table[N_FILE];
extern spinlock file_table_lock;

struct vnode_fops : public file_ops { // i.e. as opposed to pipe_fops
  int fo_decref(file* f) const override;
  int fo_read(file* f, char* buf, size_t sz, irqstate &irqs) const override;
  int fo_write(file* f, char* buf, size_t sz, irqstate &irqs) const override;
};

struct pipe_fops : public file_ops {
  int fo_decref(file* f) const override;
  int fo_read(file* f, char* buf, size_t sz, irqstate &irqs) const override;
  int fo_write(file* f, char* buf, size_t sz, irqstate &irqs) const override;
};

struct kcfs_vops : public vnode_ops { // "keyboard-console file system"
  int vop_decref(vnode* vn) const override;
  int vop_read(vnode* vn, uio* uio, irqstate &irqs) const override;  
  int vop_write(vnode* vn, uio* uio, irqstate &irqs) const override;
};

struct memf_vops : public vnode_ops { // memfile
  int vop_decref(vnode* vn) const override;
  int vop_read(vnode* vn, uio* uio, irqstate &irqs) const override;  
  int vop_write(vnode* vn, uio* uio, irqstate &irqs) const override;
};

extern vnode_fops vn_fops;
extern pipe_fops p_fops;
extern kcfs_vops kc_vops;
extern memf_vops mf_vops;

int file_incref(file* f);
int file_decref(file* f);
int file_read(file* f, char* buf, size_t sz, irqstate &irqs);
int file_write(file* f, char* buf, size_t sz, irqstate &irqs);

int vnode_incref(vnode* vn);
int vnode_decref(vnode* vn);

void init_kc_file(file* kc_file);
void init_pipe_files(file* read_file, file* write_file);
int init_memfile_entry(file* file_slot, const char* pathname, int flags);

// diskfile::loader: loads a `proc` from a `memfile` (??? idk where else to put this)

struct diskfile_loader : public proc_loader {
  chkfs_iref ino_;
  inline diskfile_loader(chkfs_iref ino, x86_64_pagetable* pt)
    : proc_loader(pt), ino_(std::move(ino)) {
  }
  get_page_type get_page(size_t off) override;
  void put_page(buffer) override;
};
