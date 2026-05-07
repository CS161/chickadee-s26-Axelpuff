/* usr/include/unistd.h
 * POSIX I/O interface for Chickadee userspace.
 *
 * Provides thin C-callable wrappers around the SYSCALL_* numbers already
 * present in u-lib.hh.  The wrappers follow the POSIX ABI: they return -1
 * on error and store the positive errno value in the global `errno`.
 *
 * This header is pure C so that ncurses can include it during cross-compile.
 */
#ifndef CHICKADEE_UNISTD_H
#define CHICKADEE_UNISTD_H

/* Pull in errno so callers can check it after a failed call. */
#include <errno.h>

/* Standard file-descriptor numbers */
#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

/* lseek() origin constants:  values match LSEEK_* in lib.hh */
#define SEEK_SET    0   /* seek from beginning of file */
#define SEEK_CUR    1   /* seek from current position  */
#define SEEK_END    2   /* seek from end of file       */

/* Basic integer types used by the I/O prototypes below.
 * Defined only if not already provided by a prior include. */
#ifndef _PID_T_DEFINED
#define _PID_T_DEFINED
typedef int pid_t;
#endif

#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef __SIZE_TYPE__ size_t;
#endif

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long ssize_t;
#endif

#ifndef _OFF_T_DEFINED
#define _OFF_T_DEFINED
typedef long off_t;
#endif

/* Pointer to the process environment (set by the shell on process spawn). */
extern const char* const* environ;

#ifdef __cplusplus
extern "C" {
#endif

/* read(fd, buf, count)
 *    Read up to count bytes from fd into buf.
 *    Returns bytes read (0 at EOF), or -1 and sets errno on error.
 */
ssize_t read(int fd, void* buf, size_t count);

/* write(fd, buf, count)
 *    Write count bytes from buf to fd.
 *    Returns bytes written, or -1 and sets errno on error.
 */
ssize_t write(int fd, const void* buf, size_t count);

/* open(path, flags[, mode]):  declared in <fcntl.h>; also available here */
int open(const char* path, int flags, ...);

/* close(fd)
 *    Close the file descriptor fd.
 *    Returns 0 on success, or -1 and sets errno on error.
 */
int close(int fd);

/* lseek(fd, offset, whence)
 *    Reposition fd's file offset.  whence is one of SEEK_SET/SEEK_CUR/SEEK_END.
 *    Returns the new absolute offset, or -1 and sets errno on error.
 */
off_t lseek(int fd, off_t offset, int whence);

/* ftruncate(fd, length)
 *    Truncate or extend the file referred to by fd to exactly length bytes.
 *    Returns 0 on success, or -1 and sets errno on error.
 */
int ftruncate(int fd, off_t length);

/* unlink(pathname)
 *    Remove the file at pathname.
 *    Returns 0 on success, or -1 and sets errno on error.
 */
int unlink(const char* pathname);

/* getpid():  return the calling process's PID. */
pid_t getpid(void);

/* getppid():  return the parent process's PID. */
pid_t getppid(void);

/* _exit(status):  terminate the process immediately without atexit handlers. */
void _exit(int status) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* CHICKADEE_UNISTD_H */
