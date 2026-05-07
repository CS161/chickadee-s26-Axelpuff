/* usr/include/termios.h
 * POSIX terminal I/O interface for Chickadee userspace.
 *
 * Struct layout and constant values MUST match the kernel-side definitions
 * in lib.hh exactly:  both sides of the kernel/user boundary memcpy through
 * this layout via SYSCALL_TCGETATTR / SYSCALL_TCSETATTR.
 *
 * This header is pure C so that ncurses (written in C) can include it when
 * cross-compiled for Chickadee.
 */
#ifndef CHICKADEE_TERMIOS_H
#define CHICKADEE_TERMIOS_H

/* Basic types needed here */
typedef unsigned int  tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int  speed_t;

#define NCCS 19

/* struct termios:  layout must match lib.hh */
struct termios {
    tcflag_t c_iflag;    /* input flags       */
    tcflag_t c_oflag;    /* output flags      */
    tcflag_t c_cflag;    /* control flags     */
    tcflag_t c_lflag;    /* local flags       */
    cc_t     c_line;     /* line discipline (ignored) */
    cc_t     c_cc[NCCS]; /* control characters */
};

/* c_lflag bits consulted by the line discipline */
#define ISIG    0000001  /* generate signals on INTR/QUIT/SUSP */
#define ICANON  0000002  /* canonical (line-buffered) input */
#define ECHO    0000010  /* echo input characters */
#define ECHOE   0000020  /* echo ERASE as BS-SP-BS */
#define ECHOK   0000040  /* echo KILL */
#define ECHONL  0000100  /* echo NL even when ECHO is cleared */
#define NOFLSH  0000200  /* do not flush after signal */
#define IEXTEN  0100000  /* enable extended input processing */

/* c_iflag bits consulted by the line discipline */
#define IGNBRK  0000001  /* ignore break condition */
#define BRKINT  0000002  /* signal interrupt on break */
#define IGNPAR  0000004  /* ignore framing/parity errors */
#define PARMRK  0000010  /* mark parity and framing errors */
#define INPCK   0000020  /* enable input parity check */
#define ISTRIP  0000040  /* strip 8th bit */
#define INLCR   0000100  /* map NL to CR on input */
#define IGNCR   0000200  /* ignore carriage return */
#define ICRNL   0000400  /* map CR to NL on input (consulted) */
#define IXON    0002000  /* enable XON/XOFF flow control (consulted) */
#define IXOFF   0010000  /* enable sending XON/XOFF */
#define IXANY   0004000  /* any character will restart after stop */

/* c_oflag bits consulted by the output path */
#define OPOST   0000001  /* enable output processing */
#define ONLCR   0000004  /* map NL to CR-NL on output (consulted) */
#define OCRNL   0000010
#define ONOCR   0000020
#define ONLRET  0000040
#define OFILL   0000100

/* c_cflag bits (stored but not consulted:  no real serial line) */
#define CSIZE   0000060  /* character size mask */
#define CS5     0000000
#define CS6     0000020
#define CS7     0000040
#define CS8     0000060  /* 8-bit characters */
#define CSTOPB  0000100  /* 2 stop bits */
#define CREAD   0000200
#define PARENB  0000400  /* enable parity */
#define PARODD  0001000
#define HUPCL   0002000
#define CLOCAL  0004000
#define B0      0000000
#define B9600   0000015
#define B38400  0000017
#define B115200 0010002

/* c_cc indices:  Linux x86-64 layout; must match glibc */
#define VINTR   0   /* interrupt character (SIGINT) */
#define VQUIT   1   /* quit character (SIGQUIT) */
#define VERASE  2   /* erase character */
#define VKILL   3   /* kill-line character */
#define VEOF    4   /* end-of-file (typically ^D) */
#define VTIME   5   /* read timeout in deciseconds (non-canonical) */
#define VMIN    6   /* minimum bytes per read (non-canonical) */
#define VSWTC   7
#define VSTART  8
#define VSTOP   9
#define VSUSP   10
#define VEOL    11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE 14
#define VLNEXT  15
#define VEOL2   16

/* optional_actions argument to tcsetattr */
#define TCSANOW   0   /* apply immediately */
#define TCSADRAIN 1   /* apply after draining output */
#define TCSAFLUSH 2   /* apply after draining; discard pending input */

/* tcflush() queue selectors */
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

/* tcflow() actions */
#define TCOOFF  0
#define TCOON   1
#define TCIOFF  2
#define TCION   3

#ifdef __cplusplus
extern "C" {
#endif

int  tcgetattr(int fd, struct termios *t);
int  tcsetattr(int fd, int optional_actions, const struct termios *t);
void cfmakeraw(struct termios *t);
int  cfsetispeed(struct termios *t, speed_t speed);
int  cfsetospeed(struct termios *t, speed_t speed);
speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);
int  tcdrain(int fd);
int  tcflush(int fd, int queue_selector);
int  tcsendbreak(int fd, int duration);
int  tcflow(int fd, int action);

#ifdef __cplusplus
}
#endif

#endif /* CHICKADEE_TERMIOS_H */
