```c++
#define N_FILEDESC 16
#define N_FILE 128
#define FD_EMPTY = 255  /* "this fd slot is empty" */

#define	FTYPE_NONE	0	/* not yet initialized */
#define	FTYPE_VNODE	1	/* file */
#define	FTYPE_PIPE	2	/* pipe */


struct file {
	int type;
	int refcount_;
	int flags;
	off_t off_;
	vnode* vnode_;

	const file_ops* ops;
};

struct uio {
  off_t off;
  char* buf;
  int len;
};

struct vnode {
	int refcount;
	spinlock refcount_lock;

	const vnode_ops* ops;
};

class file_ops {
  virtual int fo_read(file* f, char* buf, int len);
  virtual int fo_write(file* f, char* buf, int len);
};

class vnode_ops {
  virtual int vop_read(vnode* vn, uio* uio);
  virtual int vop_write(vnode* vn, uio* uio);
};

// in struct proc
unsigned int fd_table[N_FILEDESC];

// wherever ptable is defined
file file_table[N_FILE];
spinlock file_table_lock;
```

## Functionality
Each `proc` has an `fd_table`, which is an array of indexes into the global `file_table`. `fd_table` is of size `N_FILEDESC`.

The `file_table` is declared similarly to `ptable` and has fixed size `N_FILE`.

`vnode_ops` is a parent class where inheriting subclasses implement specific file systems. These filesystem-specific function tables are instantiated as globals. The same applies to `file_ops`.

A `file` should be instantiated with `ops` pointing to an instantiated subclass of `file_ops`. Similarly, a `vnode` should be instantiated with `ops` pointing to an instantiated subclass of `vnode_ops`.

`vnode_ops->vop_read` reads the section of `vn` specified by `uio` and reads it into the corresponding buffer. `vop_write` is the corresponding write implementation.

`file_ops->fo_read` reads the section of `f` of length `len` at the current offset and reads it into `buf`. `fo_write` is the corresponding write implementation. Both internally access `off`.

`uio` is instantiated locally and then used as an argument to vnode operations.

## Synchronization
`file_table` is protected by `file_table_lock`. Any threads accessing `file_table` or any of its individual `file` objects should hold the lock.

A `vnode`'s refcount should only be changed when holding `refcount_lock`

## Concerns
The `_ops` pattern is based on os161 and FreeBSD, but I don't fully understand it, so having it for both files and vnodes might be overkill.

Confusion: ref count (who calls to update it? what function do they call? if it's `open`, does each child need to call `open` again on fork?)
