#define CHICKADEE_OPTIONAL_PROCESS 1
#include "u-lib.hh"

// p-sigwinch.cc
//    Verifies signals and ioctl window size operations.
//    Tests:
//
//      - TIOCGWINSZ reads the initial TTY window size (25 rows x 80 cols).
//      - sigaction installs a SIGWINCH handler.
//      - TIOCSWINSZ with a new size updates tty_state and raises SIGWINCH.
//      - SIGWINCH is delivered to the handler; the handler can call
//        TIOCGWINSZ and observes the updated size.
//      - kill(getpid(), SIGUSR1) delivers SIGUSR1 to a self-installed handler.
//      - sigaction with SIG_IGN causes the signal to be swallowed silently.

static int n_passed = 0;
static int n_failed = 0;
static const char* first_failure = nullptr;

static void check(bool ok, const char* name) {
    if (ok) {
        ++n_passed;
    } else {
        ++n_failed;
        if (!first_failure) {
            first_failure = name;
        }
    }
}


// SIGWINCH handler state

static volatile int sigwinch_count = 0;
static volatile unsigned short handler_rows = 0;
static volatile unsigned short handler_cols = 0;

static void sigwinch_handler(int sig) {
    (void) sig;
    sigwinch_count = sigwinch_count + 1;
    // Read the current window size from inside the handler to verify that
    // TIOCSWINSZ updated tty_state before the signal was delivered.
    struct winsize ws = {};
    sys_ioctl(0, TIOCGWINSZ, reinterpret_cast<uintptr_t>(&ws));
    handler_rows = ws.ws_row;
    handler_cols = ws.ws_col;
}


// SIGUSR1 handler state

static volatile int sigusr1_count = 0;

static void sigusr1_handler(int sig) {
    (void) sig;
    sigusr1_count = sigusr1_count + 1;
}


// test_tiocgwinsz_initial
//    The TTY must report the hardware geometry (25 x 80) at startup.
static void test_tiocgwinsz_initial() {
    struct winsize ws = {};
    int r = sys_ioctl(0, TIOCGWINSZ, reinterpret_cast<uintptr_t>(&ws));
    check(r == 0,          "TIOCGWINSZ returns 0");
    check(ws.ws_row == 25, "initial ws_row == 25");
    check(ws.ws_col == 80, "initial ws_col == 80");
}


// test_sigwinch_delivery
//    Install a SIGWINCH handler, call TIOCSWINSZ, then verify the handler
//    ran exactly once and observed the new dimensions.
static void test_sigwinch_delivery() {
    struct sigaction sa = {};
    sa.sa_handler = sigwinch_handler;
    int r = sys_sigaction(SIGWINCH, &sa, nullptr);
    check(r == 0, "sigaction(SIGWINCH) returns 0");

    // Resize the window, the kernel must update tty_state before marking
    // the signal pending so the handler sees the new size via TIOCGWINSZ.
    struct winsize new_ws = { 30, 100, 0, 0 };
    r = sys_ioctl(0, TIOCSWINSZ, reinterpret_cast<uintptr_t>(&new_ws));
    check(r == 0, "TIOCSWINSZ returns 0");

    // Yield to guarantee delivery if the kernel has not yet delivered on
    // the ioctl return path.
    sys_yield();

    check(sigwinch_count == 1,   "SIGWINCH delivered exactly once");
    check(handler_rows  == 30,   "handler sees ws_row == 30");
    check(handler_cols  == 100,  "handler sees ws_col == 100");

    // Verify the size is readable from outside the handler.
    struct winsize got = {};
    r = sys_ioctl(0, TIOCGWINSZ, reinterpret_cast<uintptr_t>(&got));
    check(r == 0,         "TIOCGWINSZ after resize returns 0");
    check(got.ws_row == 30,  "TIOCGWINSZ reports updated ws_row");
    check(got.ws_col == 100, "TIOCGWINSZ reports updated ws_col");

    // Restore original size and consume the resulting SIGWINCH.
    struct winsize orig_ws = { 25, 80, 0, 0 };
    sys_ioctl(0, TIOCSWINSZ, reinterpret_cast<uintptr_t>(&orig_ws));
    sys_yield();
}


// test_kill_self
//    kill(getpid(), SIGUSR1) must deliver SIGUSR1 to the installed handler.
static void test_kill_self() {
    struct sigaction sa = {};
    sa.sa_handler = sigusr1_handler;
    int r = sys_sigaction(SIGUSR1, &sa, nullptr);
    check(r == 0, "sigaction(SIGUSR1) returns 0");

    r = sys_kill(sys_getpid(), SIGUSR1);
    check(r == 0, "kill(getpid(), SIGUSR1) returns 0");

    // Yield so delivery can happen if not already done on kill() return.
    sys_yield();
    check(sigusr1_count == 1, "SIGUSR1 delivered exactly once");
}


// test_sig_ign
//    sigaction with SIG_IGN must silence subsequent deliveries of the signal.
static void test_sig_ign() {
    // Reset the SIGUSR1 count from test_kill_self.
    int before = sigusr1_count;

    struct sigaction sa_ign = {};
    sa_ign.sa_handler = SIG_IGN;
    int r = sys_sigaction(SIGUSR1, &sa_ign, nullptr);
    check(r == 0, "sigaction(SIGUSR1, SIG_IGN) returns 0");

    r = sys_kill(sys_getpid(), SIGUSR1);
    check(r == 0, "kill(self, SIGUSR1) with SIG_IGN returns 0");
    sys_yield();
    check(sigusr1_count == before, "SIG_IGN: handler not called");

    // Restore SIG_DFL so later tests are unaffected.
    struct sigaction sa_dfl = {};
    sa_dfl.sa_handler = SIG_DFL;
    sys_sigaction(SIGUSR1, &sa_dfl, nullptr);
}


// test_sigaction_oldact
//    The `oldact` out-parameter must receive the previously installed handler.
static void test_sigaction_oldact() {
    struct sigaction sa_new = {};
    sa_new.sa_handler = sigusr1_handler;
    sys_sigaction(SIGUSR1, &sa_new, nullptr);

    struct sigaction sa_old = {};
    struct sigaction sa_rep = {};
    sa_rep.sa_handler = SIG_DFL;
    int r = sys_sigaction(SIGUSR1, &sa_rep, &sa_old);
    check(r == 0, "sigaction with oldact returns 0");
    check(sa_old.sa_handler == sigusr1_handler,
          "oldact.sa_handler is the previously installed handler");
}


void process_main() {
    test_tiocgwinsz_initial();
    test_sigwinch_delivery();
    test_kill_self();
    test_sig_ign();
    test_sigaction_oldact();

    int total = n_passed + n_failed;
    if (n_failed == 0) {
        console_printf(CPOS(0, 0),
                       CS_SUCCESS "p-sigwinch: %d/%d tests passed\n",
                       n_passed, total);
    } else {
        console_printf(CPOS(0, 0),
                       CS_ERROR "p-sigwinch: %d/%d failed (first: %s)\n",
                       n_failed, total,
                       first_failure ? first_failure : "?");
    }
    sys_exit(n_failed == 0 ? 0 : 1);
}
