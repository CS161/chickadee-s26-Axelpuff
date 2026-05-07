/* usr/include/fcntl.h
 * File-control flags and open() prototype for Chickadee userspace.
 *
 * O_* flag values deliberately match the OF_* constants in lib.hh so that
 * the POSIX open() wrapper can forward the flags to SYSCALL_OPEN without
 * any translation:
 *
 *   O_RDONLY = OF_READ  = 1
 *   O_WRONLY = OF_WRITE = 2
 *   O_RDWR   = OF_READ|OF_WRITE = 3
 *   O_CREAT  = OF_CREATE = 4
 *   O_TRUNC  = OF_TRUNC  = 8
 *
 * O_APPEND and O_NONBLOCK are accepted by the kernel but currently ignored.
 *
 * This header is pure C so that ncurses can include it during cross-compile.
 */
#ifndef CHICKADEE_FCNTL_H
#define CHICKADEE_FCNTL_H

#define O_RDONLY    1     /* open for reading only   (= OF_READ)   */
#define O_WRONLY    2     /* open for writing only   (= OF_WRITE)  */
#define O_RDWR      3     /* open for reading and writing           */
#define O_CREAT     4     /* create file if it does not exist       */
#define O_TRUNC     8     /* truncate to zero length on open        */
#define O_APPEND    16    /* writes always append (accepted, ignored) */
#define O_NONBLOCK  32    /* non-blocking I/O       (accepted, ignored) */

#ifndef __cplusplus
/* In C, size_t comes from stddef.h; pull in a minimal definition here. */
#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef __SIZE_TYPE__ size_t;
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* open(path, flags[, mode])
 *    Open or create a file.  The optional mode argument is accepted for
 *    POSIX compatibility when O_CREAT is specified but is ignored by the
 *    Chickadee kernel (permissions are not implemented).
 */
int open(const char* path, int flags, ...);

/* creat(path, mode):  equivalent to open(path, O_WRONLY|O_CREAT|O_TRUNC, mode) */
int creat(const char* path, int mode);

#ifdef __cplusplus
}
#endif

#endif /* CHICKADEE_FCNTL_H */
