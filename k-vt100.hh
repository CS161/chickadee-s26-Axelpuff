#pragma once
#include "kernel.hh"

// line_discipline
//    Bridges raw keycodes from `keyboardstate` to bytes delivered by
//    `read()` on the TTY. Behavior controlled by `tty_state::ktermios_` flags
//    (ICANON, ECHO, ISIG).
struct line_discipline {
    static constexpr size_t ldbuf_cap = 256;

    // Points to the owning tty_state::ktermios_, set by tty_state's constructor
    const struct termios* ktermios_ = nullptr;

    char   canon_buf_[ldbuf_cap]; // current line being edited (ICANON)
    size_t canon_len_ = 0;

    char   ring_buf_[ldbuf_cap];  // ready-to-read ring buffer
    size_t ring_pos_ = 0;
    size_t ring_len_ = 0;

    spinlock  lock_;
    wait_queue wq_;

    // Feed one raw character from the keyboard interrupt handler
    // Caller must hold keyboardstate::lock_
    void push_byte(int ch);

    // Consume up to sz bytes into buf, blocks until data is available
    // Caller must hold lock_ via guard, holds lock_ on return
    ssize_t read_locked(char* buf, size_t sz, spinlock_guard& guard);

 private:
    void ring_push(char c);
    void echo_char(char c);
};

// default_ktermios
//    Construct the canonical mode initial termios that TCGETATTR returns.
//    Called once as a default member initializer in tty_state.
static inline struct termios default_ktermios() {
    struct termios t = {};
    t.c_iflag = ICRNL | IXON;
    t.c_oflag = OPOST | ONLCR;
    t.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
    t.c_cc[VINTR]  = 3;    // ^C
    t.c_cc[VQUIT]  = 28;   // ^backslash
    t.c_cc[VERASE] = 127;  // DEL
    t.c_cc[VKILL]  = 21;   // ^U
    t.c_cc[VEOF]   = 4;    // ^D
    t.c_cc[VTIME]  = 0;
    t.c_cc[VMIN]   = 1;
    return t;
}

// tty_state
//    All emulated-terminal state for a single TTY: logical cursor row
//    and column (independent of the CGA hardware cursor), saved cursor
//    position (for ESC 7 / ESC 8 / DECSC / DECRC), current SGR
//    attributes (foreground color, background color, bold, reverse,
//    underline-as-color), the active scroll region (top and bottom
//    rows, used by ncurses' status bar and by `setscrreg`), DEC private
//    modes (autowrap, application keypad, cursor-key mode), and the
//    advertised window size in rows x columns.
//
//    Each `tty_state` is paired with one CGA framebuffer region. For
//    the foreseeable future Chickadee will have exactly one of these
//    (the boot console), but the design admits more.
struct tty_state {
    int rows_ = CONSOLE_ROWS;        // window height in rows
    int cols_ = CONSOLE_COLUMNS;     // window width in columns
    int row_ = 0;                    // logical cursor row (0-based)
    int col_ = 0;                    // logical cursor column (0-based)
    int saved_row_ = 0;              // DECSC saved row
    int saved_col_ = 0;              // DECSC saved column
    int color_ = COLOR_GRAY;         // current SGR attribute (CGA format, in high byte)
    int saved_color_ = COLOR_GRAY;   // DECSC saved color
    int scroll_top_ = 0;             // scroll region top row (0-based, inclusive)
    int scroll_bot_ = CONSOLE_ROWS - 1; // scroll region bottom row (0-based, inclusive)
    bool autowrap_ = true;           // DEC autowrap mode

    // POSIX termios block
    struct termios ktermios_ = default_ktermios();

    line_discipline ldisc_;          // input line discipline

    tty_state() { ldisc_.ktermios_ = &ktermios_; }

    void reset();
    void put_char(char c);
    void fill_cells(int from_cpos, int to_cpos);
    void scroll_up_region();
    void scroll_down_region();
    void update_hw_cursor();
};

// vt_parser
//    Streaming parser for the subset of ECMA-48 / VT100 / ANSI X3.64
//    escape sequences that ncurses emits when driven by a terminfo entry
//    of type `vt100` or `ansi`. The parser is a byte-at-a-time state
//    machine with three states: GROUND (printable bytes go straight to
//    the screen), ESCAPE (saw 0x1B), and CSI_PARAM (saw `ESC [`,
//    collecting numeric parameters). On reaching a final byte the parser
//    dispatches to an action method on the bound `tty_state`.
//
//    The parser owns no screen state of its own, it is purely a lexer
//    over the byte stream. All cursor, color, and scroll-region state
//    lives on `tty_state` so that multiple consoles can share the parser
//    logic.
//
//    Threading: the parser is not internally locked. Callers (the
//    console write path) must hold the owning `tty_state`'s lock for
//    the duration of `feed()`.
class vt_parser {
public:
    static constexpr int max_params = 16;

    explicit vt_parser(tty_state& tty);

    // Feed one byte into the parser.
    void feed(char byte);

    // Feed a buffer of bytes.
    void feed(const char* buf, size_t n);

    // Reset the parser state machine (leaves tty_state untouched).
    void reset_parser();

private:
    enum class pstate : uint8_t { ground, escape, csi_param };

    tty_state& tty_;
    pstate state_ = pstate::ground;
    char private_char_ = 0;            // leading '?' or '>' in CSI sequence
    int params_[max_params] = {};
    int nparams_ = 0;
    int cur_param_ = 0;
    bool have_digit_ = false;

    // Helpers
    int param(int i, int def) const;
    void commit_param();
    void commit_param_if_needed();
    void clear_params();

    // State handlers
    void handle_ground(char c);
    void handle_escape(char c);
    void handle_csi(char final_byte);

    // Action methods
    void action_newline();
    void action_cursor_move(int row, int col);
    void action_cursor_up(int n);
    void action_cursor_down(int n);
    void action_cursor_right(int n);
    void action_cursor_left(int n);
    void action_erase_display(int n);
    void action_erase_line(int n);
    void action_sgr();
    void action_set_scroll_region(int top, int bot);
    void action_index();
    void action_reverse_index();
    void action_decsc();
    void action_decrc();
};
