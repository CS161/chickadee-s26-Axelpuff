#include "kernel.hh" // holy crap this WILL NOT COMPILE with anything less than this

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
struct file_ops;
struct vnode_ops;

struct file {
  spinlock file_lock; // should be held when doing any file operation
  int type;
  int refcount_;
  int flags;
  off_t off_;
  vnode* vnode_;

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

  const vnode_ops* ops;
  vnode(vnode_ops* vn_ops): ops(vn_ops) {
  }
};

struct file_ops {
  virtual ~file_ops() = default;
  inline void fo_incref(file* f) const {
    ++f->refcount_;
  };
  inline void fo_decref(file* f) const {
    --f->refcount_;
  };
  virtual int fo_read(file* f, char* buf, size_t sz) const = 0;
  virtual int fo_write(file* f, char* buf, size_t sz) const = 0;
};

struct vnode_ops {
  virtual ~vnode_ops() = default;
  inline void vop_incref(vnode* vn) const {
    ++vn->refcount;
  };
  inline void vop_decref(vnode* vn) const {
    --vn->refcount;
  };
  virtual int vop_read(vnode* vn, uio* uio) const = 0;
  virtual int vop_write(vnode* vn, uio* uio) const = 0;
};

extern file file_table[N_FILE];
extern spinlock file_table_lock;

struct vnode_fops : public file_ops { // i.e. as opposed to pipe_fops
  // void fo_incref(file* f) const override;
  // void fo_decref(file* f) const override;
  int fo_read(file* f, char* buf, size_t sz) const override;
  int fo_write(file* f, char* buf, size_t sz) const override;
};
  
struct kcfs_vops : public vnode_ops { // "keyboard-console file system"
  // void vop_incref(vnode* vn) const override;
  // void vop_decref(vnode* vn) const override;
  int vop_read(vnode* vn, uio* uio) const override;  
  int vop_write(vnode* vn, uio* uio) const override;
};

extern vnode_fops vn_fops;
extern kcfs_vops kc_vops;

void file_incref(file* f);
void file_decref(file* f);
int file_read(file* f, char* buf, size_t sz);
int file_write(file* f, char* buf, size_t sz);

void vnode_incref(vnode* vn);
void vnode_decref(vnode* vn);
