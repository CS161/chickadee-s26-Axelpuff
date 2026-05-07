#define CHICKADEE_OPTIONAL_PROCESS 1
#include "u-lib.hh"

// p-testsysroot.cc
//    Tests the sysroot headers and POSIX C wrappers added to usr/include/
//    and u-lib.cc so that ncurses configure can find them.  Verifies:
//
//      - <errno.h>:   errno is set (to ENOENT) when open() fails on a
//                     non-existent path; the global variable is accessible.
//      - <unistd.h>:  STDIN_FILENO / STDOUT_FILENO / STDERR_FILENO constants;
//                     POSIX open/close/read/write return -1 + set errno on
//                     error rather than returning a negative errno value.
//      - <fcntl.h>:   O_RDONLY / O_WRONLY / O_RDWR / O_CREAT / O_TRUNC
//                     constants match the OF_* values the kernel expects.
//      - <string.h>:  strlen, strcmp, strchr, strdup, strerror all compile
//                     and return sensible values.
//      - <stdlib.h>:  atoi / strtol parse digits correctly; getenv returns
//                     NULL for an unknown variable.

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

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


// test_errno
//    open() on a non-existent file must return -1 and set errno to ENOENT.
static void test_errno() {
    errno = 0;
    int fd = open("/no/such/file", O_RDONLY);
    check(fd == -1,        "errno: open() returns -1 on missing file");
    check(errno == ENOENT, "errno: errno == ENOENT after failed open");
}


// test_unistd_constants
//    The three standard I/O file-descriptor numbers must have the values
//    POSIX mandates.
static void test_unistd_constants() {
    check(STDIN_FILENO  == 0, "unistd: STDIN_FILENO  == 0");
    check(STDOUT_FILENO == 1, "unistd: STDOUT_FILENO == 1");
    check(STDERR_FILENO == 2, "unistd: STDERR_FILENO == 2");
}


// test_fcntl_constants
//    O_RDONLY / O_WRONLY / O_RDWR must be 0 / 1 / 2 (matching OF_READ=1
//    etc. in lib.hh:  the POSIX open() wrapper translates them).
static void test_fcntl_constants() {
    check((O_RDONLY | O_WRONLY) == O_RDWR, "fcntl: O_RDONLY|O_WRONLY == O_RDWR");
    check(O_CREAT != 0,                    "fcntl: O_CREAT is non-zero");
    check(O_TRUNC != 0,                    "fcntl: O_TRUNC is non-zero");
    // O_RDONLY, O_WRONLY, O_RDWR must be mutually distinct
    check(O_RDONLY != O_WRONLY, "fcntl: O_RDONLY != O_WRONLY");
    check(O_RDONLY != O_RDWR,   "fcntl: O_RDONLY != O_RDWR");
    check(O_WRONLY != O_RDWR,   "fcntl: O_WRONLY != O_RDWR");
}


// test_string_h
//    Basic smoke-test for the string functions ncurses hits most.
static void test_string_h() {
    const char* s = "hello";
    check(strlen(s) == 5,              "string: strlen(\"hello\") == 5");
    check(strcmp(s, "hello") == 0,     "string: strcmp equal");
    check(strcmp(s, "world") < 0,      "string: strcmp less-than");
    check(strchr(s, 'e') == s + 1,     "string: strchr finds 'e'");
    check(strchr(s, 'z') == nullptr,   "string: strchr returns null for missing");

    char* dup = strdup(s);
    check(dup != nullptr,              "string: strdup returns non-null");
    check(dup != s,                    "string: strdup returns fresh allocation");
    if (dup) {
        check(strcmp(dup, s) == 0,     "string: strdup content matches");
        free(dup);
    }

    const char* err = strerror(ENOENT);
    check(err != nullptr,              "string: strerror(ENOENT) non-null");
    check(strlen(err) > 0,            "string: strerror(ENOENT) non-empty");
}


// test_stdlib_h
//    atoi and strtol must parse decimal strings; getenv must return NULL
//    for a name that was never set.
static void test_stdlib_h() {
    check(atoi("42")   == 42,  "stdlib: atoi(\"42\")");
    check(atoi("-7")   == -7,  "stdlib: atoi(\"-7\")");
    check(atoi("0")    == 0,   "stdlib: atoi(\"0\")");

    char* end = nullptr;
    check(strtol("255", &end, 10) == 255, "stdlib: strtol base-10");
    check(strtol("ff",  &end, 16) == 255, "stdlib: strtol base-16");

    // getenv for an unknown name must return NULL, not crash.
    check(getenv("DEFINITELY_NOT_SET_XYZ") == nullptr,
          "stdlib: getenv unknown returns null");
}


void process_main() {
    test_errno();
    test_unistd_constants();
    test_fcntl_constants();
    test_string_h();
    test_stdlib_h();

    int total = n_passed + n_failed;
    if (n_failed == 0) {
        console_printf(CPOS(0, 0),
                       CS_SUCCESS "p-testsysroot: %d/%d tests passed\n",
                       n_passed, total);
    } else {
        console_printf(CPOS(0, 0),
                       CS_ERROR "p-testsysroot: %d/%d failed (first: %s)\n",
                       n_failed, total,
                       first_failure ? first_failure : "?");
    }
    sys_exit(n_failed == 0 ? 0 : 1);
}
