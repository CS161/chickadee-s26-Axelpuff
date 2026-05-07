#define CHICKADEE_OPTIONAL_PROCESS 1
// p-curses.cc
//    Acceptance test for the VT100/ncurses stack.
//    Draws 8 colored panels (4x2 grid), a status bar, and echoes
//    key names at the screen center.  Exits cleanly on 'q'.
//
//    If this renders correctly and responds to input, the VT100
//    emulator, line discipline, termios, ioctl, signal, and ncurses
//    glue are all verified end-to-end.

#include <curses.h>
#include <string.h>
#include <stdlib.h>

// Override u-lib.cc's weak `environ` default so getenv("TERM") returns
// "chickadee" and ncurses can find the right terminfo entry via initscr().
static const char* const curses_environ_[] = {
    "TERM=chickadee",
    "HOME=/",
    "PATH=/",
    nullptr
};
const char* const* environ = curses_environ_;

static const char* const COLOR_NAMES[8] = {
    "BLACK", "RED", "GREEN", "YELLOW",
    "BLUE",  "MAGENTA", "CYAN", "WHITE"
};

// draw_panels
//    Fills the screen (minus the bottom status row) with 8 equal
//    panels arranged in a 4-column x 2-row grid, each filled with
//    its color pair and labeled with the color name.
static void draw_panels() {
    int ph = (LINES - 1) / 2;
    int pw = COLS / 4;

    for (int i = 0; i < 8; ++i) {
        int row0 = (i / 4) * ph;
        int col0 = (i % 4) * pw;

        attron(COLOR_PAIR(i + 1));
        for (int r = row0; r < row0 + ph && r < LINES - 1; ++r) {
            move(r, col0);
            for (int c = 0; c < pw; ++c)
                addch(' ');
        }
        int name_len = (int)strlen(COLOR_NAMES[i]);
        move(row0 + ph / 2, col0 + (pw - name_len) / 2);
        addstr(COLOR_NAMES[i]);
        attroff(COLOR_PAIR(i + 1));
    }
}

static void draw_keyname(const char* kn) {
    if (!kn)
        kn = "?";
    int klen = (int)strlen(kn);
    move((LINES - 1) / 2, (COLS - klen) / 2);
    attron(A_BOLD | A_REVERSE);
    addstr(kn);
    attroff(A_BOLD | A_REVERSE);
}

static void draw_status(const char* msg) {
    move(LINES - 1, 0);
    attron(A_REVERSE);
    addstr(msg);
    clrtoeol();
    attroff(A_REVERSE);
}

void process_main() {
    initscr();
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    start_color();

    // Pair i+1: XOR-complementary foreground on color-i background.
    for (int i = 0; i < 8; ++i)
        init_pair((short)(i + 1), (short)(i ^ 7), (short)i);

    draw_panels();
    draw_status("Press any key (q to quit)");
    refresh();

    int key;
    while ((key = getch()) != 'q') {
        draw_panels();
        draw_keyname(keyname(key));
        draw_status("Press any key (q to quit)");
        refresh();
    }

    endwin();
    exit(0);
}
