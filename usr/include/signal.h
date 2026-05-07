/* usr/include/signal.h
 * POSIX signal interface for Chickadee userspace.
 *
 * Signal numbers, SIG_DFL/SIG_IGN sentinels, struct sigaction, and wrappers
 * around SYSCALL_SIGACTION and SYSCALL_KILL.  Layout matches lib.hh so that
 * kernel and userspace share the same sigaction ABI.
 *
 * This header is pure C so that ncurses can include it during cross-compile.
 */
#ifndef CHICKADEE_SIGNAL_H
#define CHICKADEE_SIGNAL_H

#ifndef _PID_T_DEFINED
#define _PID_T_DEFINED
typedef int pid_t;
#endif

/* sig_atomic_t: type that can be read/written atomically */
typedef volatile int sig_atomic_t;

/* Signal numbers (POSIX subset) */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGSTKFLT 16
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGWINCH 28
#define SIGIO    29
#define SIGPWR   30
#define SIGSYS   31
#define NSIG     32

/* Handler sentinels:  must match lib.hh */
#define SIG_DFL  ((void (*)(int)) 0)  /* default disposition */
#define SIG_IGN  ((void (*)(int)) 1)  /* ignore signal */
#define SIG_ERR  ((void (*)(int))-1)  /* error return from signal() */

/* struct sigaction:  layout must match lib.hh */
struct sigaction {
    void (*sa_handler)(int);   /* handler, SIG_DFL, or SIG_IGN */
    unsigned long sa_mask;     /* blocked signals during handler (reserved) */
    unsigned long sa_flags;    /* flags (reserved; stored but not consulted) */
    void (*sa_restorer)(void); /* not used; set to NULL */
};

/* sa_flags bits (stored but not yet consulted) */
#define SA_NOCLDSTOP  1
#define SA_NOCLDWAIT  2
#define SA_SIGINFO    4
#define SA_ONSTACK    0x08000000
#define SA_RESTART    0x10000000
#define SA_NODEFER    0x40000000
#define SA_RESETHAND  0x80000000

#ifdef __cplusplus
extern "C" {
#endif

/* sigaction(sig, act, oldact)
 *    Install a handler for signal sig from *act.
 *    If oldact != NULL, stores the previous disposition there.
 *    Returns 0 on success, -EINVAL for an uncatchable or out-of-range signal.
 */
int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);

/* signal(sig, handler)
 *    BSD-style signal installation.  Wraps sigaction().
 *    Returns the previous handler or SIG_ERR on error.
 */
void (*signal(int sig, void (*handler)(int)))(int);

/* kill(pid, sig)
 *    Send signal sig to process pid.
 *    Returns 0 on success, -ESRCH if the process does not exist.
 */
int kill(pid_t pid, int sig);

/* raise(sig):  send signal to the calling process */
int raise(int sig);

#ifdef __cplusplus
}
#endif

#endif /* CHICKADEE_SIGNAL_H */
