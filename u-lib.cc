#include "u-lib.hh"

// dprintf
//    Construct a string from `format` and pass it to `sys_write(fd)`.
//    Returns the number of characters printed, or E_2BIG if the string
//    could not be constructed.

int dprintf(int fd, const char* format, ...) {
    char buf[513];
    va_list val;
    va_start(val, format);
    size_t n = vsnprintf(buf, sizeof(buf), format, val);
    if (n < sizeof(buf)) {
        return sys_write(fd, buf, n);
    } else {
        return E_2BIG;
    }
}


// printf
//    Like `dprintf(1, ...)`.

int printf(const char* format, ...) {
    char buf[513];
    va_list val;
    va_start(val, format);
    size_t n = vsnprintf(buf, sizeof(buf), format, val);
    if (n < sizeof(buf)) {
        return sys_write(1, buf, n);
    } else {
        return E_2BIG;
    }
}


// panic, assert_fail
//     Call the SYSCALL_PANIC system call so the kernel loops until Control-C.

void panic(const char* format, ...) {
    va_list val;
    va_start(val, format);
    char buf[160];
    memcpy(buf, "PANIC: ", 7);
    int len = vsnprintf(&buf[7], sizeof(buf) - 7, format, val) + 7;
    va_end(val);
    if (len > 0 && buf[len - 1] != '\n') {
        strcpy(buf + len - (len == (int) sizeof(buf) - 1), "\n");
    }
    error_printf(CS_ERROR "%s", buf);
    sys_panic(nullptr);
}

void error_vprintf(const char* format, va_list val) {
    int scroll_mode = console_printer::scroll_on;
    if (consoletype != CONSOLE_NORMAL) {
        scroll_mode = console_printer::scroll_blank;
    }
    console_printer pr(-1, scroll_mode);
    if (consoletype != CONSOLE_NORMAL
        && pr.cell_ < console + END_CPOS - CONSOLE_COLUMNS) {
        pr.cell_ = console + END_CPOS;
    }
    pr.vprintf(format, val);
    pr.move_cursor();
}

void assert_fail(const char* file, int line, const char* msg,
                 const char* description) {
    if (description) {
        error_printf("%s:%d: %s\n", file, line, description);
    }
    error_printf("%s:%d: user assertion '%s' failed\n", file, line, msg);
    sys_panic(nullptr);
}


// POSIX C wrappers (extern "C" so ncurses can link against them by name)
//
// These functions expose the Chickadee sys_* inline helpers under the
// standard POSIX names that ncurses (and eventually other C programs) call.

// errno:  set to the positive error code by every failing POSIX wrapper below.
// Chickadee syscalls return negative errno values; the wrappers negate them.
int errno = 0;

// Forward-declare malloc so strdup can call it; the definition is in u-malloc.cc.
extern "C" void* malloc(size_t);

extern "C" {

// __errno_location:  glibc ABI for accessing errno via a pointer.
// ncurses is compiled against glibc headers which define errno as
// (*__errno_location()), so it calls this function instead of referencing
// errno directly.  Return the address of our global so both paths alias
// the same int.
int* __errno_location() {
    return &errno;
}


// I/O wrappers

ssize_t read(int fd, void* buf, size_t sz) {
    ssize_t r = sys_read(fd, reinterpret_cast<char*>(buf), sz);
    if (r < 0) { errno = (int)(-r); return -1; }
    return r;
}

ssize_t write(int fd, const void* buf, size_t sz) {
    ssize_t r = sys_write(fd, reinterpret_cast<const char*>(buf), sz);
    if (r < 0) { errno = (int)(-r); return -1; }
    return r;
}

int open(const char* path, int flags, ...) {
    int r = sys_open(path, flags);
    if (r < 0) { errno = (int)(-r); return -1; }
    return r;
}

int creat(const char* path, int /* mode */) {
    // OF_WRITE | OF_CREATE | OF_TRUNC = 2 | 4 | 8
    return open(path, OF_WRITE | OF_CREATE | OF_TRUNC);
}

int close(int fd) {
    int r = sys_close(fd);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

off_t lseek(int fd, off_t offset, int whence) {
    ssize_t r = sys_lseek(fd, offset, whence);
    if (r < 0) { errno = (int)(-r); return (off_t)-1; }
    return (off_t)r;
}

int ftruncate(int fd, off_t length) {
    int r = sys_ftruncate(fd, length);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

int unlink(const char* pathname) {
    int r = sys_unlink(pathname);
    if (r < 0) { errno = (int)(-r); return -1; }
    return 0;
}

pid_t getpid(void) {
    return sys_getpid();
}

pid_t getppid(void) {
    return sys_getppid();
}

[[noreturn]] void _exit(int status) {
    sys_exit(status);
}

[[noreturn]] void exit(int status) {
    sys_exit(status);
}

[[noreturn]] void abort(void) {
    sys_exit(134); // 128 + SIGABRT(6)
}


// getenv
//    Walk the null-terminated environ array looking for "NAME=value".
//    Returns a pointer to the value portion on match, nullptr otherwise.
//
//    The weak default covers processes that don't seed environ themselves
//    (e.g. test programs).  p-sh.cc overrides this with a real definition.

static const char* const empty_environ_[] = {nullptr};
__attribute__((weak)) const char* const* environ = empty_environ_;

char* getenv(const char* name) {
    if (!environ || !name) {
        return nullptr;
    }
    size_t namelen = strlen(name);
    for (size_t i = 0; environ[i]; ++i) {
        const char* e = environ[i];
        if (strncmp(e, name, namelen) == 0 && e[namelen] == '=') {
            return const_cast<char*>(e + namelen + 1);
        }
    }
    return nullptr;
}


// strerror
//    Map a positive POSIX errno value to a human-readable string.

const char* strerror(int errnum) {
    switch (errnum) {
    case 1:  return "Operation not permitted";
    case 2:  return "No such file or directory";
    case 3:  return "No such process";
    case 4:  return "Interrupted system call";
    case 5:  return "I/O error";
    case 6:  return "No such device or address";
    case 7:  return "Argument list too long";
    case 8:  return "Exec format error";
    case 9:  return "Bad file descriptor";
    case 10: return "No child processes";
    case 11: return "Try again";
    case 12: return "Out of memory";
    case 14: return "Bad address";
    case 16: return "Device or resource busy";
    case 22: return "Invalid argument";
    case 23: return "File table overflow";
    case 24: return "Too many open files";
    case 25: return "Not a typewriter";
    case 26: return "Text file busy";
    case 27: return "File too large";
    case 28: return "No space left on device";
    case 29: return "Illegal seek";
    case 32: return "Broken pipe";
    case 34: return "Numerical result out of range";
    case 36: return "File name too long";
    case 38: return "Function not implemented";
    case 75: return "Value too large for defined data type";
    default: return "Unknown error";
    }
}


// strdup
//    Allocate a fresh copy of s via malloc() (implemented in u-malloc.cc).

char* strdup(const char* s) {
    if (!s) {
        return nullptr;
    }
    size_t n = strlen(s) + 1;
    char* p = reinterpret_cast<char*>(malloc(n));
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}


// termios functions

int tcgetattr(int fd, struct termios* t) {
    return sys_tcgetattr(fd, t);
}

int tcsetattr(int fd, int optional_actions, const struct termios* t) {
    return sys_tcsetattr(fd, optional_actions, t);
}

void cfmakeraw(struct termios* t) {
    t->c_iflag &= ~(unsigned)(0000001  /* IGNBRK  */
                             | 0000002  /* BRKINT  */
                             | 0000010  /* PARMRK  */
                             | 0000040  /* ISTRIP  */
                             | 0000100  /* INLCR   */
                             | 0000200  /* IGNCR   */
                             | ICRNL
                             | IXON);
    t->c_oflag &= ~(unsigned)OPOST;
    t->c_lflag &= ~(unsigned)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(unsigned)(0000060   /* CSIZE  */
                             | 0000400); /* PARENB */
    t->c_cflag |= 0000060;              /* CS8    */
    t->c_cc[VMIN]  = 1;
    t->c_cc[VTIME] = 0;
}

int cfsetispeed(struct termios* /* t */, unsigned /* speed */) { return 0; }
int cfsetospeed(struct termios* /* t */, unsigned /* speed */) { return 0; }
unsigned cfgetispeed(const struct termios* /* t */) { return 0; }
unsigned cfgetospeed(const struct termios* /* t */) { return 0; }

int tcdrain(int /* fd */)                 { return 0; }
int tcflush(int /* fd */, int /* sel */)  { return 0; }
int tcsendbreak(int /* fd */, int /* d */) { return 0; }
int tcflow(int /* fd */, int /* act */)   { return 0; }


// ioctl

int ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    uintptr_t arg = va_arg(ap, uintptr_t);
    va_end(ap);
    return sys_ioctl(fd, request, arg);
}


// signal functions

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
int sigaction(int sig, const struct sigaction* act, struct sigaction* oldact) {
    return sys_sigaction(sig, act, oldact);
}
#pragma GCC diagnostic pop

void (*signal(int sig, void (*handler)(int)))(int) {
    struct sigaction act = {}, old = {};
    act.sa_handler = handler;
    if (sys_sigaction(sig, &act, &old) < 0) {
        return SIG_ERR;
    }
    return old.sa_handler;
}

int kill(pid_t pid, int sig) {
    return sys_kill(pid, sig);
}

int raise(int sig) {
    return sys_kill(sys_getpid(), sig);
}

// stdio stubs
//    Chickadee has no buffered FILE* layer.  We represent the three standard
//    streams as small structs carrying only a file descriptor, and implement
//    fprintf / fputc etc. on top of our write()-based vsnprintf.

struct _CHICKADEE_FILE { int fd; };
typedef struct _CHICKADEE_FILE FILE;
static struct _CHICKADEE_FILE stdin_f  = {0};
static struct _CHICKADEE_FILE stdout_f = {1};
static struct _CHICKADEE_FILE stderr_f = {2};

FILE* stdin  = &stdin_f;
FILE* stdout = &stdout_f;
FILE* stderr = &stderr_f;

int vfprintf(FILE* stream, const char* format, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), format, ap);
    if (n <= 0) {
        return 0;
    }
    if ((size_t)n >= sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    sys_write(stream ? stream->fd : 2, buf, n);
    return n;
}

int fprintf(FILE* stream, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int r = vfprintf(stream, format, ap);
    va_end(ap);
    return r;
}

int vsprintf(char* buf, const char* format, va_list ap) {
    return vsnprintf(buf, 65536, format, ap);
}

int sprintf(char* buf, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int r = vsnprintf(buf, SIZE_MAX, format, ap);
    va_end(ap);
    return r;
}

// Restore __sprintf_chk to its non-logging form below.

int fputc(int c, FILE* stream) {
    char ch = (char)c;
    sys_write(stream ? stream->fd : 1, &ch, 1);
    return c;
}

int fputs(const char* s, FILE* stream) {
    size_t n = strlen(s);
    sys_write(stream ? stream->fd : 1, s, n);
    return (int)n;
}

int putchar(int c) {
    return fputc(c, stdout);
}

int puts(const char* s) {
    fputs(s, stdout);
    fputc('\n', stdout);
    return 0;
}

int fflush(FILE* /* stream */) {
    return 0; // unbuffered
}

void perror(const char* s) {
    if (s && *s) {
        fputs(s, stderr);
        fputs(": ", stderr);
    }
    fputs(strerror(errno), stderr);
    fputc('\n', stderr);
}

int sscanf(const char* /* str */, const char* /* format */, ...) {
    return -1; // not implemented; ncurses uses this only for termcap parsing
}

} // extern "C"


// ─────────────────────────────────────────────────────────────────────────
// glibc compatibility stubs for libncurses.a
//
// libncurses.a was compiled against glibc headers and calls several
// glibc-internal / POSIX symbols that Chickadee does not otherwise
// provide.  The implementations below are sufficient for the ncurses
// functions used by p-curses.cc (and any future ncurses program).
// ─────────────────────────────────────────────────────────────────────────

// ── __ctype_b_loc:  glibc character-classification table ─────────────────
//
// ncurses calls (*__ctype_b_loc())[c] to classify single bytes.
// We provide a minimal C-locale table for the ASCII range.
//
// The table has 384 entries; the returned pointer addresses element 128
// so that signed-char indices in [-128, 255] all hit valid storage.
//
// Bit values match glibc's _IS* constants for x86-64 little-endian:
//   0x0001 upper   0x0002 lower   0x0004 alpha   0x0008 digit
//   0x0010 xdigit  0x0020 space   0x0040 print   0x0080 graph
//   0x0100 blank   0x0200 cntrl   0x0400 punct   0x0800 alnum

static unsigned short ctype_b_table_[384]; // BSS -> zero-initialized
static const unsigned short* ctype_b_ptr_; // BSS -> nullptr until lazy init

static void init_ctype_b() {
    unsigned short* T = ctype_b_table_;
    // T[128 + c] holds classification bits for char value c.
    for (int c = 0;   c <= 8;   ++c) T[128+c] = 0x0200; // NUL..BS: cntrl
    T[128+9]  = 0x0320;                                   // HT: cntrl+space+blank
    for (int c = 10;  c <= 13;  ++c) T[128+c] = 0x0220; // LF..CR: cntrl+space
    for (int c = 14;  c <= 31;  ++c) T[128+c] = 0x0200; // SO..US: cntrl
    T[128+32] = 0x0160;                                   // SP: space+blank+print
    for (int c = 33;  c <= 47;  ++c) T[128+c] = 0x04C0; // !"#../ punct
    for (int c = 48;  c <= 57;  ++c) T[128+c] = 0x08D8; // 0–9 digit+xdigit+alnum
    for (int c = 58;  c <= 64;  ++c) T[128+c] = 0x04C0; // :;<=>?@ punct
    for (int c = 65;  c <= 70;  ++c) T[128+c] = 0x08D5; // A–F upper+xdigit+alnum
    for (int c = 71;  c <= 90;  ++c) T[128+c] = 0x08C5; // G–Z upper+alnum
    for (int c = 91;  c <= 96;  ++c) T[128+c] = 0x04C0; // [\]^_` punct
    for (int c = 97;  c <= 102; ++c) T[128+c] = 0x08D6; // a–f lower+xdigit+alnum
    for (int c = 103; c <= 122; ++c) T[128+c] = 0x08C6; // g–z lower+alnum
    for (int c = 123; c <= 126; ++c) T[128+c] = 0x04C0; // {|}~ punct
    T[128+127] = 0x0200;                                  // DEL: cntrl
    // 128–255: 0 (non-ASCII; C locale leaves unclassified)
    ctype_b_ptr_ = &T[128];
}

// sigset_t compatible with glibc on x86-64: 16 x unsigned long = 128 bytes.
typedef struct { unsigned long __val[16]; } __chk_sigset_t;

extern "C" {

const unsigned short** __ctype_b_loc(void) {
    if (!ctype_b_ptr_) init_ctype_b();
    return &ctype_b_ptr_;
}

// ── fileno / isatty ──────────────────────────────────────────────────────

int fileno(FILE* stream) {
    return stream ? stream->fd : -1;
}

int isatty(int fd) {
    return (fd >= 0 && fd <= 2) ? 1 : 0;
}

// ── process group stubs ──────────────────────────────────────────────────
// ncurses lib_tstp.c needs these for SIGTSTP handling (job control).
// Chickadee has no job control; return the PID as the group ID.

pid_t getpgrp(void) { return sys_getpid(); }
pid_t tcgetpgrp(int /* fd */) { return sys_getpid(); }

// ── POSIX signal-set functions ───────────────────────────────────────────
// ncurses blocks signals around terminal state changes via sigprocmask.
// We stub sigprocmask as a no-op; Chickadee ignores signal masks.

int sigemptyset(__chk_sigset_t* set) {
    memset(set, 0, sizeof(*set)); return 0;
}
int sigfillset(__chk_sigset_t* set) {
    memset(set, 0xFF, sizeof(*set)); return 0;
}
int sigaddset(__chk_sigset_t* set, int sig) {
    if ((unsigned)sig - 1 < 64)
        set->__val[(sig-1)/64] |= 1UL << ((sig-1) & 63);
    return 0;
}
int sigdelset(__chk_sigset_t* set, int sig) {
    if ((unsigned)sig - 1 < 64)
        set->__val[(sig-1)/64] &= ~(1UL << ((sig-1) & 63));
    return 0;
}
int sigismember(const __chk_sigset_t* set, int sig) {
    if ((unsigned)sig - 1 >= 64) return 0;
    return (set->__val[(sig-1)/64] >> ((sig-1) & 63)) & 1;
}
int sigprocmask(int /* how */, const __chk_sigset_t* /* set */,
                __chk_sigset_t* oldset) {
    if (oldset) memset(oldset, 0, sizeof(*oldset));
    return 0;
}

// atexit: ncurses registers cleanup handlers; no-op for now
int atexit(void (* /* func */)(void)) { return 0; }

// ── glibc fortified I/O stubs ────────────────────────────────────────────
// glibc's -D_FORTIFY_SOURCE replaces printf/read/sscanf with these
// checked variants.  Forward them to our ordinary implementations.

int __fprintf_chk(FILE* stream, int /* flag */, const char* format, ...) {
    va_list ap; va_start(ap, format);
    int r = vfprintf(stream, format, ap); va_end(ap); return r;
}
int __vfprintf_chk(FILE* stream, int /* flag */, const char* format,
                   va_list ap) {
    return vfprintf(stream, format, ap);
}
int __printf_chk(int /* flag */, const char* format, ...) {
    va_list ap; va_start(ap, format);
    int r = vfprintf(stdout, format, ap); va_end(ap); return r;
}
ssize_t __read_chk(int fd, void* buf, size_t count, size_t /* bufsz */) {
    return read(fd, buf, count);
}
int __isoc99_sscanf(const char* /* str */, const char* /* fmt */, ...) {
    return -1; // not implemented; ncurses only uses this in termcap paths
}

// ── setjmp / longjmp ─────────────────────────────────────────────────────
// ncurses (lib_initscr.c) calls _setjmp for allocation-failure recovery.
// glibc's setjmp(env) macro expands to _setjmp(env), so ncurses.a
// references _setjmp by name.
//
// jmp_buf layout (first 64 bytes of glibc's __jmp_buf_tag on x86-64):
//   [0]  RBX  [8]  RBP  [16] R12  [24] R13
//   [32] R14  [40] R15  [48] RSP  [56] RIP
//

__attribute__((naked)) int _setjmp(void* /* buf */) {
    asm(
        "movq %rbx, 0(%rdi) \n\t" "movq %rbp, 8(%rdi) \n\t"
        "movq %r12, 16(%rdi)\n\t" "movq %r13, 24(%rdi)\n\t"
        "movq %r14, 32(%rdi)\n\t" "movq %r15, 40(%rdi)\n\t"
        "leaq 8(%rsp), %rax \n\t" "movq %rax, 48(%rdi)\n\t"
        "movq (%rsp), %rax  \n\t" "movq %rax, 56(%rdi)\n\t"
        "xorl %eax, %eax    \n\t" "ret"
    );
}
__attribute__((naked)) int setjmp(void* /* buf */) {
    asm(
        "movq %rbx, 0(%rdi) \n\t" "movq %rbp, 8(%rdi) \n\t"
        "movq %r12, 16(%rdi)\n\t" "movq %r13, 24(%rdi)\n\t"
        "movq %r14, 32(%rdi)\n\t" "movq %r15, 40(%rdi)\n\t"
        "leaq 8(%rsp), %rax \n\t" "movq %rax, 48(%rdi)\n\t"
        "movq (%rsp), %rax  \n\t" "movq %rax, 56(%rdi)\n\t"
        "xorl %eax, %eax    \n\t" "ret"
    );
}
__attribute__((naked)) int sigsetjmp(void* /* buf */, int /* savemask */) {
    asm(
        "movq %rbx, 0(%rdi) \n\t" "movq %rbp, 8(%rdi) \n\t"
        "movq %r12, 16(%rdi)\n\t" "movq %r13, 24(%rdi)\n\t"
        "movq %r14, 32(%rdi)\n\t" "movq %r15, 40(%rdi)\n\t"
        "leaq 8(%rsp), %rax \n\t" "movq %rax, 48(%rdi)\n\t"
        "movq (%rsp), %rax  \n\t" "movq %rax, 56(%rdi)\n\t"
        "xorl %eax, %eax    \n\t" "ret"
    );
}

__attribute__((naked, noreturn)) void _longjmp(void* /* buf */, int /* val */) {
    asm(
        "movq 0(%rdi),%rbx  \n\t" "movq 8(%rdi),%rbp  \n\t"
        "movq 16(%rdi),%r12 \n\t" "movq 24(%rdi),%r13 \n\t"
        "movq 32(%rdi),%r14 \n\t" "movq 40(%rdi),%r15 \n\t"
        "movq 48(%rdi),%rsp \n\t"
        "movl %esi,%eax     \n\t" "testl %eax,%eax    \n\t"
        "jnz 1f             \n\t" "movl $1,%eax       \n\t"
        "1: jmpq *56(%rdi)"
    );
}
__attribute__((naked, noreturn)) void longjmp(void* /* buf */, int /* val */) {
    asm(
        "movq 0(%rdi),%rbx  \n\t" "movq 8(%rdi),%rbp  \n\t"
        "movq 16(%rdi),%r12 \n\t" "movq 24(%rdi),%r13 \n\t"
        "movq 32(%rdi),%r14 \n\t" "movq 40(%rdi),%r15 \n\t"
        "movq 48(%rdi),%rsp \n\t"
        "movl %esi,%eax     \n\t" "testl %eax,%eax    \n\t"
        "jnz 1f             \n\t" "movl $1,%eax       \n\t"
        "1: jmpq *56(%rdi)"
    );
}
__attribute__((naked, noreturn)) void siglongjmp(void* /* buf */, int /* val */) {
    asm(
        "movq 0(%rdi),%rbx  \n\t" "movq 8(%rdi),%rbp  \n\t"
        "movq 16(%rdi),%r12 \n\t" "movq 24(%rdi),%r13 \n\t"
        "movq 32(%rdi),%r14 \n\t" "movq 40(%rdi),%r15 \n\t"
        "movq 48(%rdi),%rsp \n\t"
        "movl %esi,%eax     \n\t" "testl %eax,%eax    \n\t"
        "jnz 1f             \n\t" "movl $1,%eax       \n\t"
        "1: jmpq *56(%rdi)"
    );
}

} // extern "C" glibc compatibility stubs


// ── Additional glibc / POSIX stubs (second wave) ─────────────────────────
// Forward-declare free since u-lib.cc only forward-declares malloc.
extern "C" void free(void*);

// Types needed by the stubs below.
typedef long time_t;
typedef int  clockid_t;
struct timespec { time_t tv_sec; long tv_nsec; };
struct pollfd   { int fd; short events; short revents; };

// Tolower table (same layout as ctype_b: 384 ints, ptr at [128]).
static int tolower_table_[384]; // BSS
static const int* tolower_ptr_; // BSS

static void init_tolower() {
    int* T = tolower_table_;
    for (int i = 0; i < 384; ++i) T[i] = i - 128; // identity by default
    for (int c = 'A'; c <= 'Z'; ++c) T[128+c] = c + 32;
    tolower_ptr_ = &T[128];
}

// POSIX binary search tree operations (used by ncurses lib_tparm cache).
typedef struct _tnode { const void* key; struct _tnode *left, *right; } _tnode;
typedef enum { preorder, postorder, endorder, leaf } VISIT;

extern "C" {

const int** __ctype_tolower_loc(void) {
    if (!tolower_ptr_) init_tolower();
    return &tolower_ptr_;
}

// ── strncat ──────────────────────────────────────────────────────────────

char* strncat(char* dest, const char* src, size_t n) {
    char* d = dest;
    while (*d) ++d;
    while (n && *src) { *d++ = *src++; --n; }
    *d = '\0';
    return dest;
}

// ── poll / clock_gettime / nanosleep / time ──────────────────────────────

// poll: ncurses calls this to wait for keyboard input.  Always report fd 0
// as ready so ncurses immediately calls read() (which blocks in the kernel).
int poll(struct pollfd* fds, unsigned int nfds, int /* timeout */) {
    for (unsigned int i = 0; i < nfds; ++i) {
        fds[i].revents = (fds[i].fd == 0) ? fds[i].events : 0;
    }
    return 1;
}

int clock_gettime(clockid_t /* clk_id */, struct timespec* tp) {
    if (tp) { tp->tv_sec = 0; tp->tv_nsec = 0; }
    return 0;
}

int nanosleep(const struct timespec* /* req */, struct timespec* rem) {
    if (rem) { rem->tv_sec = 0; rem->tv_nsec = 0; }
    // Yield a few times to approximate a short sleep.
    for (int i = 0; i < 4; ++i) sys_yield();
    return 0;
}

time_t time(time_t* t) {
    if (t) *t = 0;
    return 0;
}

// ── Locale / environment stubs ────────────────────────────────────────────
// Built with --disable-locale but some references remain.

char* setlocale(int /* cat */, const char* /* loc */) { return (char*)"C"; }
char* nl_langinfo(int /* item */) { return (char*)""; }
long  sysconf(int /* name */) { return -1; }
int   setenv(const char* /* n */, const char* /* v */, int /* ow */) { return 0; }

// ── File access stubs ─────────────────────────────────────────────────────
// read_entry.c tries to open terminfo files; since ncurses was built with
// --with-fallbacks=chickadee the static fallback is used instead, but the
// fopen/fread/fclose and access/stat symbols must still resolve.

FILE* fopen(const char* path, const char* mode) {
    int flags = (mode && mode[0] == 'r') ? OF_READ
                                         : (OF_WRITE | OF_CREATE | OF_TRUNC);
    int fd = sys_open(path, flags);
    if (fd < 0) { errno = -fd; return NULL; }
    FILE* f = (FILE*)malloc(sizeof(FILE));
    if (!f) { sys_close(fd); errno = 12; return NULL; }
    f->fd = fd;
    return f;
}

size_t fread(void* ptr, size_t size, size_t count, FILE* stream) {
    if (!stream || !ptr || !size) return 0;
    ssize_t r = sys_read(stream->fd, reinterpret_cast<char*>(ptr), size * count);
    return (r <= 0) ? 0 : (size_t)r / size;
}

int fclose(FILE* stream) {
    if (!stream) return -1;
    sys_close(stream->fd);
    free(stream);
    return 0;
}

int access(const char* path, int /* mode */) {
    int fd = sys_open(path, OF_READ);
    if (fd < 0) { errno = 2; return -1; }
    sys_close(fd);
    return 0;
}

// stat: always report "not found" (terminfo is embedded via fallback).
int stat(const char* /* path */, void* /* buf */) {
    errno = 2; // ENOENT
    return -1;
}

// ── Fortified string / memory stubs ──────────────────────────────────────

int __sprintf_chk(char* buf, int /* flag */, size_t buflen,
                  const char* format, ...) {
    va_list ap; va_start(ap, format);
    int r = vsnprintf(buf, buflen ? buflen : SIZE_MAX, format, ap); va_end(ap);
    return r;
}

int __vsnprintf_chk(char* buf, size_t size, int /* flag */, size_t /* blen */,
                    const char* format, va_list ap) {
    return vsnprintf(buf, size, format, ap);
}

char* __strcpy_chk(char* dest, const char* src, size_t /* destlen */) {
    return strcpy(dest, src);
}

void* __memcpy_chk(void* dest, const void* src, size_t n, size_t /* dlen */) {
    return memcpy(dest, src, n);
}

// ── POSIX binary search tree (used by ncurses lib_tparm) ─────────────────

void* tsearch(const void* key, void** rootp,
              int (*compar)(const void*, const void*)) {
    if (!rootp) return NULL;
    _tnode** np = (_tnode**)rootp;
    while (*np) {
        int c = compar(key, (*np)->key);
        if (c == 0) return (void*)&(*np)->key;
        np = (c < 0) ? &(*np)->left : &(*np)->right;
    }
    _tnode* n = (_tnode*)malloc(sizeof(_tnode));
    if (!n) return NULL;
    n->key = key; n->left = n->right = NULL;
    *np = n;
    return (void*)&n->key;
}

void* tfind(const void* key, void* const* rootp,
            int (*compar)(const void*, const void*)) {
    if (!rootp) return NULL;
    _tnode* const* np = (_tnode* const*)rootp;
    while (*np) {
        int c = compar(key, (*np)->key);
        if (c == 0) return (void*)&(*np)->key;
        np = (c < 0) ? &(*np)->left : &(*np)->right;
    }
    return NULL;
}

void* tdelete(const void* key, void** rootp,
              int (*compar)(const void*, const void*)) {
    if (!rootp || !*rootp) return NULL;
    _tnode** np = (_tnode**)rootp;
    _tnode* parent = NULL;
    while (*np) {
        int c = compar(key, (*np)->key);
        if (c == 0) {
            _tnode* node = *np;
            void* ret = parent ? (void*)&parent->key : (void*)rootp;
            if      (!node->right) *np = node->left;
            else if (!node->left)  *np = node->right;
            else {
                _tnode** sp = &node->right;
                while ((*sp)->left) sp = &(*sp)->left;
                node->key = (*sp)->key;
                _tnode* old = *sp; *sp = old->right; free(old);
                return ret;
            }
            free(node);
            return ret;
        }
        parent = *np;
        np = (c < 0) ? &(*np)->left : &(*np)->right;
    }
    return NULL;
}

static void twalk_r(const _tnode* n, void (*action)(const void*, VISIT, int),
                    int depth) {
    if (!n) return;
    if (!n->left && !n->right) { action(&n->key, leaf, depth); return; }
    action(&n->key, preorder, depth);
    twalk_r(n->left,  action, depth + 1);
    action(&n->key, postorder, depth);
    twalk_r(n->right, action, depth + 1);
    action(&n->key, endorder, depth);
}

void twalk(const void* root, void (*action)(const void*, VISIT, int)) {
    twalk_r((const _tnode*)root, action, 0);
}

} // extern "C" additional stubs


// Because a C++ function isn't thin enough for `many_threads` :(

extern "C" [[noreturn]] void thread_texit_entry();

__attribute__((naked, noreturn))
void thread_texit_entry() {
    asm volatile (
        "movq %[num], %%rax\n\t"
        "syscall\n\t"
        "1: jmp 1b\n\t"
        :
        : [num] "i" (SYSCALL_TEXIT)
        : "rax", "rcx", "r11", "memory"
    );
}

// sys_clone
//    Create a new thread.


pid_t sys_clone(void (*function)(void*), void* arg, char* stack_top) {
  register uintptr_t fn asm("r12") =
    reinterpret_cast<uintptr_t>(function);
  register uintptr_t fn_arg asm("r13") =
    reinterpret_cast<uintptr_t>(arg);
  register uintptr_t st asm("r14") =
    reinterpret_cast<uintptr_t>(stack_top);

  long tid = make_syscall(SYSCALL_CLONE);

  if (tid == 0) {
    asm volatile (
		  "movq %[stack], %%rsp\n\t"
		  "pushq %[texit]\n\t"
		  "movq %[arg], %%rdi\n\t"
		  "jmp *%[fn]\n\t"
		  :
		  : [stack] "r" (st),
		    [texit] "r" (thread_texit_entry),
		    [arg] "r" (fn_arg),
		    [fn] "r" (fn)
		  : "memory", "cc", "rdi"
		  );
    __builtin_unreachable();
  }

  return tid;
}
