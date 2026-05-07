/* usr/include/errno.h
 * POSIX error codes and the global errno variable for Chickadee userspace.
 *
 * Chickadee syscalls return negative errno values (e.g. -2 for ENOENT).
 * The POSIX wrappers in u-lib.cc negate the return value and store it in
 * `errno`, then return -1, matching the libc ABI that ncurses expects.
 *
 * The numeric values match Linux x86-64 / glibc so that unmodified C code
 * cross-compiled for Chickadee uses the same constants.
 *
 * This header is pure C so that ncurses can include it during cross-compile.
 */
#ifndef CHICKADEE_ERRNO_H
#define CHICKADEE_ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif

extern int errno;

#define EPERM           1   /* Operation not permitted */
#define ENOENT          2   /* No such file or directory */
#define ESRCH           3   /* No such process */
#define EINTR           4   /* Interrupted system call */
#define EIO             5   /* I/O error */
#define ENXIO           6   /* No such device or address */
#define E2BIG           7   /* Argument list too long */
#define ENOEXEC         8   /* Exec format error */
#define EBADF           9   /* Bad file number */
#define ECHILD          10  /* No child processes */
#define EAGAIN          11  /* Try again */
#define ENOMEM          12  /* Out of memory */
#define EFAULT          14  /* Bad address */
#define EBUSY           16  /* Resource busy */
#define EINVAL          22  /* Invalid argument */
#define EMFILE          24  /* Too many open files */
#define ENOTTY          25  /* Not a typewriter (fd is not a TTY) */
#define ETXTBSY         26  /* Text file busy */
#define EFBIG           27  /* File too large */
#define ENOSPC          28  /* No space left on device */
#define ESPIPE          29  /* Illegal seek */
#define EPIPE           32  /* Broken pipe */
#define ERANGE          34  /* Out of range */
#define ENAMETOOLONG    36  /* File name too long */
#define ENOSYS          38  /* Invalid system call number */
#define ENFILE          23  /* File table overflow */
#define EOVERFLOW       75  /* Value too large for data type */

#ifdef __cplusplus
}
#endif

#endif /* CHICKADEE_ERRNO_H */
