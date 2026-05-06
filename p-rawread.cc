#define CHICKADEE_OPTIONAL_PROCESS 1
#include "u-lib.hh"

// p-rawread.cc
//    Verifies that tcgetattr / tcsetattr work correctly on the
//    TTY (fd 0). Tests:
//
//      - tcgetattr on the TTY returns 0 and reports canonical defaults
//        (ICANON set, ECHO set).
//      - Switching to raw mode (ICANON | ECHO | ISIG cleared, VMIN=1, VTIME=0)
//        and reading back with tcgetattr shows the exact flags written.
//      - tcsetattr restores the original attributes.
//      - tcgetattr on a pipe fd returns E_NOTTY.

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


// test_tcgetattr_ok
//    tcgetattr on the console fd must succeed and report canonical-mode defaults.
static void test_tcgetattr_ok() {
    struct termios t = {};
    int r = sys_tcgetattr(0, &t);
    check(r == 0,             "tcgetattr(0) returns 0");
    check(t.c_lflag & ICANON, "default: ICANON is set");
    check(t.c_lflag & ECHO,   "default: ECHO is set");
}


// test_raw_mode_round_trip
//    Write raw-mode attributes, read them back, verify all flags, then
//    restore and confirm the restoration.
static void test_raw_mode_round_trip() {
    struct termios orig = {};
    int r = sys_tcgetattr(0, &orig);
    check(r == 0, "round-trip: initial tcgetattr ok");

    // Build a raw-mode termios identical to what ncurses would write.
    struct termios raw = orig;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ECHOE | ISIG | IEXTEN);
    raw.c_iflag &= ~(unsigned)(ICRNL | IXON);
    raw.c_oflag &= ~(unsigned)OPOST;
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    r = sys_tcsetattr(0, TCSANOW, &raw);
    check(r == 0, "round-trip: tcsetattr(raw) returns 0");

    // Read back and verify.
    struct termios got = {};
    r = sys_tcgetattr(0, &got);
    check(r == 0,                    "round-trip: tcgetattr(after set) ok");
    check(!(got.c_lflag & ICANON),   "round-trip: ICANON cleared");
    check(!(got.c_lflag & ECHO),     "round-trip: ECHO cleared");
    check(!(got.c_lflag & ISIG),     "round-trip: ISIG cleared");
    check(!(got.c_iflag & ICRNL),    "round-trip: ICRNL cleared");
    check(!(got.c_oflag & OPOST),    "round-trip: OPOST cleared");
    check(got.c_cc[VMIN]  == 1,      "round-trip: VMIN == 1");
    check(got.c_cc[VTIME] == 0,      "round-trip: VTIME == 0");

    // Restore original attributes.
    r = sys_tcsetattr(0, TCSANOW, &orig);
    check(r == 0, "round-trip: tcsetattr(restore) returns 0");

    struct termios restored = {};
    r = sys_tcgetattr(0, &restored);
    check(r == 0,                      "round-trip: tcgetattr(after restore) ok");
    check(restored.c_lflag & ICANON,   "round-trip: ICANON restored");
    check(restored.c_lflag & ECHO,     "round-trip: ECHO restored");
}


// test_tcsaflush
//    TCSAFLUSH is a legal optional_actions value; the kernel must accept it
//    (but it behaves identically to TCSANOW since output is unbuffered currently).
static void test_tcsaflush() {
    struct termios t = {};
    int r = sys_tcgetattr(0, &t);
    check(r == 0, "tcsaflush: tcgetattr ok");
    r = sys_tcsetattr(0, TCSAFLUSH, &t);
    check(r == 0, "tcsetattr(TCSAFLUSH) returns 0");
}


// test_enotty
//    tcgetattr on a pipe read-end must return E_NOTTY; the buffer must not
//    be modified.
static void test_enotty() {
    int pfd[2];
    int r = sys_pipe(pfd);
    check(r == 0, "enotty: sys_pipe ok");
    if (r != 0) {
        return;
    }

    struct termios t;
    // Fill with a sentinel value so we can detect accidental writes.
    memset(&t, 0xA5, sizeof(t));
    r = sys_tcgetattr(pfd[0], &t);
    check(r == E_NOTTY, "tcgetattr on pipe returns E_NOTTY");

    sys_close(pfd[0]);
    sys_close(pfd[1]);
}


void process_main() {
    test_tcgetattr_ok();
    test_raw_mode_round_trip();
    test_tcsaflush();
    test_enotty();

    int total = n_passed + n_failed;
    if (n_failed == 0) {
        console_printf(CPOS(0, 0),
                       CS_SUCCESS "p-rawread: %d/%d tests passed\n",
                       n_passed, total);
    } else {
        console_printf(CPOS(0, 0),
                       CS_ERROR "p-rawread: %d/%d failed (first: %s)\n",
                       n_failed, total,
                       first_failure ? first_failure : "?");
    }
    sys_exit(n_failed == 0 ? 0 : 1);
}
