#define CHICKADEE_OPTIONAL_PROCESS 1
#include "u-lib.hh"

// p-vttest
//    VT100 output emulator test.  Walks every escape sequence
//    dispatched by `vt_parser` and verifies the resulting framebuffer
//    state:
//
//      - cursor positioning (CUP, CUU/CUD/CUF/CUB, ESC[H home)
//      - erase display (ED: ESC[J, ESC[1J, ESC[2J)
//      - erase line   (EL: ESC[K, ESC[1K, ESC[2K)
//      - SGR colors and attributes (FG/BG, reset, bold, reverse)
//      - scroll region (DECSTBM) plus IND (ESC D) and RI (ESC M)
//
//    All escapes are sent via `sys_write(1, ...)` so that they flow
//    through the parser.  Verification reads the CGA framebuffer
//    (`console[]`) directly.  The final summary uses `console_printf`,
//    which bypasses the parser, so a parser bug cannot mask the
//    pass/fail message.

static int n_passed = 0;
static int n_failed = 0;
static const char* first_failure = nullptr;

static inline void send(const char* s) {
    sys_write(1, s, strlen(s));
}

static inline char fb_char(int row, int col) {
    return static_cast<char>(console[row * CONSOLE_COLUMNS + col] & 0xFF);
}

static inline uint16_t fb_attr(int row, int col) {
    return console[row * CONSOLE_COLUMNS + col] & 0xFF00;
}

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

// Bring the terminal back to a known starting point: default SGR, no
// scroll region, screen cleared, cursor homed. Called between tests
// so they do not contaminate each other
static void reset_terminal() {
    send("\x1b[m\x1b[r\x1b[2J\x1b[H");
}


// Cursor positioning

// Plain printable bytes land at the cursor position.
static void test_plain_text() {
    reset_terminal();
    send("Hi");
    check(fb_char(0, 0) == 'H' && fb_char(0, 1) == 'i', "plain text");
}

// CUP (ESC[r;cH) uses 1-based row/column.
static void test_cup() {
    reset_terminal();
    send("\x1b[5;10HX");
    check(fb_char(4, 9) == 'X', "CUP positions cursor (1-based)");
}

// ESC[H is shorthand for the home position.
static void test_home() {
    reset_terminal();
    send("\x1b[10;20HA"
         "\x1b[HB");
    check(fb_char(9, 19) == 'A' && fb_char(0, 0) == 'B',
          "ESC[H homes the cursor");
}

// CUU / CUD / CUF / CUB move the cursor relative to its position.
static void test_cursor_arrows() {
    reset_terminal();
    send("\x1b[10;10H");   // start at (9, 9)
    send("*");             // print '*' at (9, 9), cursor -> (9, 10)
    send("\x1b[A");        // up:   cursor -> (8, 10)
    send("U");             // print 'U' at (8, 10), cursor -> (8, 11)
    send("\x1b[3D");       // left 3: cursor -> (8, 8)
    send("L");             // print 'L' at (8, 8), cursor -> (8, 9)
    send("\x1b[2B");       // down 2: cursor -> (10, 9)
    send("D");             // print 'D' at (10, 9), cursor -> (10, 10)
    send("\x1b[5C");       // right 5: cursor -> (10, 15)
    send("R");             // print 'R' at (10, 15)
    bool ok = fb_char(9, 9) == '*'
           && fb_char(8, 10) == 'U'
           && fb_char(8, 8) == 'L'
           && fb_char(10, 9) == 'D'
           && fb_char(10, 15) == 'R';
    check(ok, "CUU/CUD/CUF/CUB cursor arrows");
}

// CUU at row 0 must clip to row 0 rather than wrap.
static void test_cursor_clip_top() {
    reset_terminal();
    send("\x1b[1;1H");
    send("\x1b[10A");      // up 10 from top, should clip
    send("T");
    check(fb_char(0, 0) == 'T', "CUU clips at row 0");
}


// Erase display

// ED 2 (ESC[2J) clears the entire screen.
static void test_ed_all() {
    reset_terminal();
    send("\x1b[1;1HAAAAA");
    send("\x1b[12;1HBBBBB");
    send("\x1b[25;1HCCCCC");
    send("\x1b[2J");
    bool ok = fb_char(0, 0) == ' '
           && fb_char(0, 4) == ' '
           && fb_char(11, 0) == ' '
           && fb_char(11, 4) == ' '
           && fb_char(24, 0) == ' '
           && fb_char(24, 4) == ' ';
    check(ok, "ED 2 clears whole screen");
}

// ED 0 (ESC[J) clears from the cursor to the end of the screen.
static void test_ed_to_end() {
    reset_terminal();
    send("\x1b[1;1HUUUUU");      // top
    send("\x1b[13;1HMMMMM");     // middle (row 12)
    send("\x1b[25;1HBBBBB");     // bottom
    send("\x1b[13;3H\x1b[J");    // cursor at (12, 2), ED 0
    bool ok = fb_char(0, 0) == 'U'      // above cursor untouched
           && fb_char(12, 0) == 'M'     // before cursor on cursor row untouched
           && fb_char(12, 1) == 'M'
           && fb_char(12, 2) == ' '     // cursor cell cleared
           && fb_char(12, 4) == ' '
           && fb_char(24, 0) == ' '     // below cursor cleared
           && fb_char(24, 4) == ' ';
    check(ok, "ED 0 clears cursor-to-end");
}

// ED 1 (ESC[1J) clears from the start of the screen to the cursor.
static void test_ed_to_start() {
    reset_terminal();
    send("\x1b[1;1HUUUUU");
    send("\x1b[13;1HMMMMM");
    send("\x1b[25;1HBBBBB");
    send("\x1b[13;3H\x1b[1J");   // cursor at (12, 2), ED 1
    bool ok = fb_char(0, 0) == ' '      // above cursor cleared
           && fb_char(0, 4) == ' '
           && fb_char(12, 0) == ' '     // before cursor on cursor row cleared
           && fb_char(12, 2) == ' '     // cursor cell cleared
           && fb_char(12, 3) == 'M'     // after cursor on cursor row untouched
           && fb_char(12, 4) == 'M'
           && fb_char(24, 0) == 'B'     // below cursor untouched
           && fb_char(24, 4) == 'B';
    check(ok, "ED 1 clears start-to-cursor");
}


// Erase line

// EL 0 (ESC[K) clears from the cursor to the end of the line.
static void test_el_to_end() {
    reset_terminal();
    send("\x1b[5;1HABCDEFGHIJ");
    send("\x1b[5;5H\x1b[K");     // cursor at (4, 4), EL 0
    bool ok = fb_char(4, 0) == 'A'
           && fb_char(4, 3) == 'D'
           && fb_char(4, 4) == ' '      // cursor cell cleared
           && fb_char(4, 9) == ' ';
    check(ok, "EL 0 clears cursor-to-EOL");
}

// EL 1 (ESC[1K) clears from the start of the line to the cursor.
static void test_el_to_start() {
    reset_terminal();
    send("\x1b[5;1HABCDEFGHIJ");
    send("\x1b[5;5H\x1b[1K");
    bool ok = fb_char(4, 0) == ' '      // start to cursor cleared
           && fb_char(4, 4) == ' '      // cursor cell cleared
           && fb_char(4, 5) == 'F'      // after cursor untouched
           && fb_char(4, 9) == 'J';
    check(ok, "EL 1 clears BOL-to-cursor");
}

// EL 2 (ESC[2K) clears the entire line.
static void test_el_all() {
    reset_terminal();
    send("\x1b[5;1HABCDEFGHIJ");
    send("\x1b[6;1HOTHER");      // unrelated row, must be untouched
    send("\x1b[5;5H\x1b[2K");
    bool ok = fb_char(4, 0) == ' '
           && fb_char(4, 4) == ' '
           && fb_char(4, 9) == ' '
           && fb_char(5, 0) == 'O'      // adjacent row untouched
           && fb_char(5, 4) == 'R';
    check(ok, "EL 2 clears whole line");
}


// SGR

// ESC[31m changes the foreground attribute, and ESC[39m restores it.
static void test_sgr_fg_reset() {
    reset_terminal();
    send("X");                   // default attribute
    send("\x1b[31m" "Y");        // red FG
    send("\x1b[39m" "Z");        // FG default
    uint16_t def = fb_attr(0, 0);
    uint16_t red = fb_attr(0, 1);
    uint16_t restored = fb_attr(0, 2);
    check(def != red && def == restored, "SGR FG change and 39 reset");
}

// ESC[m (no params) resets all SGR state.
static void test_sgr_full_reset() {
    reset_terminal();
    send("X");
    send("\x1b[31m" "Y");
    send("\x1b[m" "Z");
    check(fb_attr(0, 0) == fb_attr(0, 2)
          && fb_attr(0, 0) != fb_attr(0, 1),
          "SGR ESC[m full reset");
}

// ESC[1m (bold) changes the foreground intensity.
static void test_sgr_bold() {
    reset_terminal();
    send("\x1b[31m" "X");
    send("\x1b[1m" "Y");
    check(fb_attr(0, 0) != fb_attr(0, 1), "SGR 1 (bold)");
}

// ESC[7m (reverse) swaps the FG/BG nibbles.
static void test_sgr_reverse() {
    reset_terminal();
    send("\x1b[31;42m" "X");     // some FG on some BG
    send("\x1b[7m" "Y");         // reverse
    uint8_t a = (fb_attr(0, 0) >> 8) & 0xFF;
    uint8_t b = (fb_attr(0, 1) >> 8) & 0xFF;
    uint8_t a_swapped = ((a & 0x0F) << 4) | ((a & 0xF0) >> 4);
    check(b == a_swapped, "SGR 7 (reverse) swaps FG/BG nibbles");
}

// ESC[40m..47m sets the background; ESC[49m restores it.
static void test_sgr_bg_reset() {
    reset_terminal();
    send("X");
    send("\x1b[41m" "Y");
    send("\x1b[49m" "Z");
    check(fb_attr(0, 0) != fb_attr(0, 1)
          && fb_attr(0, 0) == fb_attr(0, 2),
          "SGR BG change and 49 reset");
}

// Multiple SGR parameters in one escape (ESC[1;31;42m) all take effect.
static void test_sgr_multi_param() {
    reset_terminal();
    send("\x1b[1;31;42m" "X");
    send("\x1b[m" "Y");
    check(fb_attr(0, 0) != fb_attr(0, 1),
          "SGR multi-parameter (1;31;42) applies all");
}


// Scroll region, IND, RI

// IND (ESC D) at the bottom of the scroll region scrolls the region up.
static void test_ind_scrolls_region() {
    reset_terminal();
    send("\x1b[5;7r");           // scroll region rows 5..7 (1-based)
    send("\x1b[5;1HAAAAA");      // top of region
    send("\x1b[6;1HBBBBB");      // middle of region
    send("\x1b[7;1HCCCCC");      // bottom of region
    send("\x1b[7;1H");           // cursor at bottom-of-region row 0
    send("\x1b" "D");            // IND
    bool ok = fb_char(4, 0) == 'B'      // top now holds old middle
           && fb_char(5, 0) == 'C'      // middle now holds old bottom
           && fb_char(6, 0) == ' ';     // freshly scrolled-in line is blank
    check(ok, "IND at bottom of scroll region scrolls up");
}

// RI (ESC M) at the top of the scroll region scrolls the region down.
static void test_ri_scrolls_region() {
    reset_terminal();
    send("\x1b[5;7r");
    send("\x1b[5;1HAAAAA");
    send("\x1b[6;1HBBBBB");
    send("\x1b[7;1HCCCCC");
    send("\x1b[5;1H");           // cursor at top-of-region
    send("\x1b" "M");            // RI
    bool ok = fb_char(4, 0) == ' '      // freshly scrolled-in line is blank
           && fb_char(5, 0) == 'A'
           && fb_char(6, 0) == 'B';
    check(ok, "RI at top of scroll region scrolls down");
}

// Lines outside the scroll region are not touched when the region scrolls.
static void test_scroll_region_isolation() {
    reset_terminal();
    send("\x1b[1;1HABOVE");      // outside (above)
    send("\x1b[25;1HBELOW");     // outside (below)
    send("\x1b[5;7r");           // scroll region rows 5..7
    send("\x1b[5;1HAAAAA");
    send("\x1b[6;1HBBBBB");
    send("\x1b[7;1HCCCCC");
    send("\x1b[7;1H\x1b" "D");   // IND from bottom -> region scrolls
    bool ok = fb_char(0, 0) == 'A' && fb_char(0, 4) == 'E'
           && fb_char(24, 0) == 'B' && fb_char(24, 4) == 'W';
    check(ok, "scroll region does not touch lines outside it");
}

// IND in the middle of the scroll region is a plain cursor-down (no scroll).
static void test_ind_middle_no_scroll() {
    reset_terminal();
    send("\x1b[5;7r");
    send("\x1b[5;1HAAAAA");
    send("\x1b[6;1HBBBBB");
    send("\x1b[7;1HCCCCC");
    send("\x1b[5;1H\x1b" "D");   // IND from top of region (not bottom)
    bool ok = fb_char(4, 0) == 'A'      // top untouched
           && fb_char(5, 0) == 'B'      // middle untouched
           && fb_char(6, 0) == 'C';     // bottom untouched
    check(ok, "IND inside region does not scroll");
}


// smoke test: \x1b[2J\x1b[10;20HHello.
static void test_smoke_hello() {
    reset_terminal();
    send("\x1b[2J\x1b[10;20HHello");
    bool ok = fb_char(9, 19) == 'H'
           && fb_char(9, 20) == 'e'
           && fb_char(9, 21) == 'l'
           && fb_char(9, 22) == 'l'
           && fb_char(9, 23) == 'o';
    check(ok, "smoke: ESC[2J ESC[10;20H Hello");
}


void process_main() {
    test_plain_text();
    test_cup();
    test_home();
    test_cursor_arrows();
    test_cursor_clip_top();

    test_ed_all();
    test_ed_to_end();
    test_ed_to_start();

    test_el_to_end();
    test_el_to_start();
    test_el_all();

    test_sgr_fg_reset();
    test_sgr_full_reset();
    test_sgr_bold();
    test_sgr_reverse();
    test_sgr_bg_reset();
    test_sgr_multi_param();

    test_ind_scrolls_region();
    test_ri_scrolls_region();
    test_scroll_region_isolation();
    test_ind_middle_no_scroll();

    test_smoke_hello();

    // Restore default terminal state, then report.  Use
    // `console_printf` so that the result is visible even if the
    // parser is broken.
    reset_terminal();
    int total = n_passed + n_failed;
    if (n_failed == 0) {
        console_printf(CPOS(0, 0),
                       CS_SUCCESS "p-vttest: %d/%d tests passed\n",
                       n_passed, total);
    } else {
        console_printf(CPOS(0, 0),
                       CS_ERROR "p-vttest: %d/%d failed (first: %s)\n",
                       n_failed, total,
                       first_failure ? first_failure : "?");
    }
    sys_exit(n_failed == 0 ? 0 : 1);
}
