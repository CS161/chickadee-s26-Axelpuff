/* usr/include/sys/ioctl.h
 * Generic device-control interface for Chickadee userspace.
 *
 * Provides the TIOC* window-size request constants, struct winsize, and the
 * ioctl() wrapper around SYSCALL_IOCTL.  The struct winsize layout matches
 * lib.hh so that the kernel and userspace agree on the size and field order.
 *
 * This header is pure C so that ncurses can include it during cross-compile.
 */
#ifndef CHICKADEE_SYS_IOCTL_H
#define CHICKADEE_SYS_IOCTL_H

/* struct winsize:  layout must match lib.hh */
struct winsize {
    unsigned short ws_row;    /* terminal rows */
    unsigned short ws_col;    /* terminal columns */
    unsigned short ws_xpixel; /* pixel width  (unused) */
    unsigned short ws_ypixel; /* pixel height (unused) */
};

/* TTY window-size ioctl request numbers (Linux x86-64 values) */
#define TIOCGWINSZ  0x5413U   /* read  window size into struct winsize * */
#define TIOCSWINSZ  0x5414U   /* write window size from struct winsize * */

/* Additional TTY ioctls recognised on Linux:  stubs on Chickadee */
#define TCGETS      0x5401U
#define TCSETS      0x5402U
#define TCSETSW     0x5403U
#define TCSETSF     0x5404U
#define TIOCGPGRP   0x540FU
#define TIOCSPGRP   0x5410U
#define TIOCGPTN    0x80045430U
#define TIOCEXCL    0x540CU
#define TIOCNXCL    0x540DU

#ifdef __cplusplus
extern "C" {
#endif

/* ioctl(fd, request, ...)
 *    Generic device control.  The third argument (if present) is a pointer
 *    to a device-specific structure.  On Chickadee, delegates to the vnode's
 *    ioctl implementation via SYSCALL_IOCTL.  Returns E_NOTTY for non-TTY fds.
 */
int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif /* CHICKADEE_SYS_IOCTL_H */
