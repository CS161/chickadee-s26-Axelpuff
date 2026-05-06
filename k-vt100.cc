// k-vt100.cc
//    VT100/ANSI terminal emulator

#include "k-devices.hh"

// Global singleton tty_state and vt_parser.
// Initialized by init_constructors() before any userspace process runs.
static tty_state the_tty;
static vt_parser the_parser(the_tty);

// consolestate accessors, defined here to keep the circular-include
// boundary clean (k-devices.hh -> k-vt100.hh; k-vt100.cc -> k-devices.hh)
tty_state& consolestate::tty() { return the_tty; }
vt_parser& consolestate::parser() { return the_parser; }


// line_discipline methods

void line_discipline::ring_push(char c) {
    if (ring_len_ < ldbuf_cap) {
        size_t slot = (ring_pos_ + ring_len_) % ldbuf_cap;
        ring_buf_[slot] = c;
        ++ring_len_;
    }
    // Drop silently when full.
}

void line_discipline::echo_char(char c) {
    auto& csl = consolestate::get();
    csl.lock_.lock_noirq();
    if (c == '\b' || c == char(0x08)) {
        if (csl.tty().col_ > 0) {
            csl.parser().feed("\b \b", 3);
        }
    } else if (c != char(0x04)) {
        csl.parser().feed(&c, 1);
    }
    csl.lock_.unlock_noirq();
}

// push_byte
//    Feed one character from the keyboard interrupt handler.
//    Called while keyboardstate::lock_ is held; acquires ldisc lock_
//    internally (lock order: kbd -> ldisc -> csl).
void line_discipline::push_byte(int ch) {
    spinlock_guard guard(lock_);

    bool icanon = ktermios_->c_lflag & ICANON;
    bool echo   = ktermios_->c_lflag & ECHO;

    if (icanon) {
        if (ch == '\b' || ch == 0x7F) { // backspace
            if (canon_len_ > 0) {
                --canon_len_;
                if (echo) {
                    echo_char('\b');
                }
            }
            return;
        }
        if (ch == '\r') {
            ch = '\n';
        }
        if (canon_len_ < ldbuf_cap) {
            canon_buf_[canon_len_++] = (char) ch;
            if (echo && ch != 0x04) {
                echo_char((char) ch);
            }
        }
        // Flush canonical buffer to ring on newline or Ctrl-D.
        if (ch == '\n' || ch == 0x04) {
            for (size_t i = 0; i < canon_len_; ++i) {
                ring_push(canon_buf_[i]);
            }
            canon_len_ = 0;
            wq_.notify_all();
        }
    } else {
        // Raw mode: every byte goes directly to the ring.
        if (ch == '\r') {
            ch = '\n';
        }
        ring_push((char) ch);
        if (echo) {
            echo_char((char) ch);
        }
        wq_.notify_all();
    }
}

// read_locked
//    Consume up to sz bytes from the ring into buf.
//    Caller holds lock_ via guard on entry and on return.
//    Blocks until at least one byte is available.
//    In ICANON mode, stops at end of line (\n or Ctrl-D).
ssize_t line_discipline::read_locked(char* buf, size_t sz,
                                      spinlock_guard& guard) {
    if (sz == 0) {
        return 0;
    }
    waiter w;
    w.wait_until(wq_, [&] { return ring_len_ > 0; }, guard);

    size_t n = 0;
    while (ring_len_ > 0 && n < sz) {
        char c = ring_buf_[ring_pos_];
        if (c == char(0x04)) {
            // Ctrl-D: consume only when no bytes precede it in this read
            if (n == 0) {
                ring_pos_ = (ring_pos_ + 1) % ldbuf_cap;
                --ring_len_;
            }
            break;
        }
        buf[n++] = c;
        ring_pos_ = (ring_pos_ + 1) % ldbuf_cap;
        --ring_len_;
        if ((ktermios_->c_lflag & ICANON) && c == '\n') {
            break;  // deliver at most one canonical line per read
        }
    }
    return (ssize_t) n;
}


// tty_state methods

void tty_state::reset() {
    row_ = 0;
    col_ = 0;
    saved_row_ = 0;
    saved_col_ = 0;
    color_ = COLOR_GRAY;
    saved_color_ = COLOR_GRAY;
    scroll_top_ = 0;
    scroll_bot_ = rows_ - 1;
    autowrap_ = true;
    update_hw_cursor();
}

void tty_state::update_hw_cursor() {
    cursorpos = row_ * cols_ + col_;
    consolestate::get().cursor();
}

// fill_cells: fill CGA cells [from_cpos, to_cpos) with space at the
// current background color.
void tty_state::fill_cells(int from_cpos, int to_cpos) {
    uint16_t fill = (uint16_t)(' ' | (color_ & 0xFF00));
    for (int i = from_cpos; i < to_cpos; ++i) {
        console[i] = fill;
    }
}

// put_char: write character c at the current cursor position, advance
// the cursor, and wrap/scroll when the cursor reaches the right edge.
void tty_state::put_char(char c) {
    if (row_ >= 0 && row_ < rows_ && col_ >= 0 && col_ < cols_) {
        console[row_ * cols_ + col_] =
            (unsigned char)c | (uint16_t)(color_ & 0xFF00);
        ++col_;
        if (col_ >= cols_) {
            if (autowrap_) {
                col_ = 0;
                if (row_ == scroll_bot_) {
                    scroll_up_region();
                } else if (row_ < rows_ - 1) {
                    ++row_;
                }
            } else {
                col_ = cols_ - 1;
            }
        }
    }
    update_hw_cursor();
}

// scroll_up_region: scroll the active scroll region [scroll_top_,
// scroll_bot_] up by one line.  The vacated bottom row is cleared.
void tty_state::scroll_up_region() {
    for (int r = scroll_top_; r < scroll_bot_; ++r) {
        for (int c = 0; c < cols_; ++c) {
            console[r * cols_ + c] = console[(r + 1) * cols_ + c];
        }
    }
    uint16_t fill = (uint16_t)(' ' | (color_ & 0xFF00));
    for (int c = 0; c < cols_; ++c) {
        console[scroll_bot_ * cols_ + c] = fill;
    }
}

// scroll_down_region: scroll the active scroll region [scroll_top_,
// scroll_bot_] down by one line.  The vacated top row is cleared.
void tty_state::scroll_down_region() {
    for (int r = scroll_bot_; r > scroll_top_; --r) {
        for (int c = 0; c < cols_; ++c) {
            console[r * cols_ + c] = console[(r - 1) * cols_ + c];
        }
    }
    uint16_t fill = (uint16_t)(' ' | (color_ & 0xFF00));
    for (int c = 0; c < cols_; ++c) {
        console[scroll_top_ * cols_ + c] = fill;
    }
}


// vt_parser: constructor and reset

vt_parser::vt_parser(tty_state& tty) : tty_(tty) {
    reset_parser();
}

void vt_parser::reset_parser() {
    state_ = pstate::ground;
    clear_params();
}

void vt_parser::clear_params() {
    for (int& p : params_) {
        p = 0;
    }
    nparams_ = 0;
    cur_param_ = 0;
    have_digit_ = false;
    private_char_ = 0;
}

void vt_parser::commit_param() {
    if (nparams_ < max_params) {
        params_[nparams_++] = have_digit_ ? cur_param_ : 0;
    }
    cur_param_ = 0;
    have_digit_ = false;
}

// Commit the current parameter only when something was actually parsed
// (at least one digit, or a ';' was seen that already incremented nparams_).
void vt_parser::commit_param_if_needed() {
    if (have_digit_ || nparams_ > 0) {
        commit_param();
    }
}

// Return parameter i, substituting def when the parameter is absent or zero.
int vt_parser::param(int i, int def) const {
    if (i >= nparams_) {
        return def;
    }
    return params_[i] != 0 ? params_[i] : def;
}


// vt_parser: main feed loop

void vt_parser::feed(char byte) {
    switch (state_) {
    case pstate::ground:
        handle_ground(byte);
        break;

    case pstate::escape:
        handle_escape(byte);
        break;

    case pstate::csi_param: {
        unsigned char ub = (unsigned char) byte;
        if (ub >= '0' && ub <= '9') {
            have_digit_ = true;
            cur_param_ = cur_param_ * 10 + (ub - '0');
            if (cur_param_ > 9999) {
                cur_param_ = 9999;
            }
        } else if (byte == ';') {
            commit_param();
        } else if (byte == '?' || byte == '>') {
            private_char_ = byte;
        } else if (ub >= 0x40 && ub <= 0x7E) {
            commit_param_if_needed();
            handle_csi(byte);
            state_ = pstate::ground;
        }
        // Intermediate bytes (0x20–0x2F) are ignored.
        break;
    }
    }
}

void vt_parser::feed(const char* buf, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        feed(buf[i]);
    }
}


// vt_parser: state handlers

void vt_parser::handle_ground(char c) {
    if (c == '\x1B') {
        state_ = pstate::escape;
        return;
    }
    // Line feed, vertical tab, form feed -> CR+LF (ONLCR; termios will
    // gate this on the ONLCR flag instead).
    if (c == '\n' || c == '\v' || c == '\f') {
        tty_.col_ = 0;
        action_newline();
        return;
    }
    if (c == '\r') {
        tty_.col_ = 0;
        tty_.update_hw_cursor();
        return;
    }
    if (c == '\b') {
        if (tty_.col_ > 0) {
            --tty_.col_;
        }
        tty_.update_hw_cursor();
        return;
    }
    if ((unsigned char) c < 0x20 || c == 0x7F) {
        return; // ignore other C0/DEL control characters
    }
    tty_.put_char(c);
}

void vt_parser::handle_escape(char c) {
    state_ = pstate::ground;
    switch (c) {
    case '[':
        state_ = pstate::csi_param;
        clear_params();
        return;
    case '7':
        action_decsc();
        return;
    case '8':
        action_decrc();
        return;
    case 'D':
        action_index();
        return;
    case 'M':
        action_reverse_index();
        return;
    default:
        break; // unrecognised ESC sequence, stay in ground
    }
}

void vt_parser::handle_csi(char final_byte) {
    switch (final_byte) {
    // CUP / HVP: cursor position (1-based row ; col)
    case 'H':
    case 'f': {
        int row = param(0, 1) - 1;
        int col = param(1, 1) - 1;
        action_cursor_move(row, col);
        break;
    }
    // CUU: cursor up
    case 'A':
        action_cursor_up(param(0, 1));
        break;
    // CUD: cursor down
    case 'B':
        action_cursor_down(param(0, 1));
        break;
    // CUF: cursor right
    case 'C':
        action_cursor_right(param(0, 1));
        break;
    // CUB: cursor left
    case 'D':
        action_cursor_left(param(0, 1));
        break;
    // ED: erase display
    case 'J':
        action_erase_display(nparams_ > 0 ? params_[0] : 0);
        break;
    // EL: erase line
    case 'K':
        action_erase_line(nparams_ > 0 ? params_[0] : 0);
        break;
    // SGR: select graphic rendition
    case 'm':
        action_sgr();
        break;
    // DECSTBM: set top and bottom margins (scroll region)
    case 'r': {
        int top = (nparams_ >= 1 && params_[0] > 0) ? params_[0] - 1 : 0;
        int bot = (nparams_ >= 2 && params_[1] > 0) ? params_[1] - 1
                                                     : tty_.rows_ - 1;
        action_set_scroll_region(top, bot);
        break;
    }
    // DEC private modes (h/l): accept but ignore for now
    case 'h':
    case 'l':
        break;
    default:
        break;
    }
}


// vt_parser: action implementations

namespace {
inline int vt_clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
} // anonymous namespace

// LF/IND: move down one line in the scroll region, scrolling if needed.
void vt_parser::action_newline() {
    action_index();
}

// CUP / HVP: absolute cursor positioning (0-based row/col after conversion).
void vt_parser::action_cursor_move(int row, int col) {
    tty_.row_ = vt_clamp(row, 0, tty_.rows_ - 1);
    tty_.col_ = vt_clamp(col, 0, tty_.cols_ - 1);
    tty_.update_hw_cursor();
}

// CUU: cursor up N rows, clipped at row 0.
void vt_parser::action_cursor_up(int n) {
    tty_.row_ = vt_clamp(tty_.row_ - n, 0, tty_.rows_ - 1);
    tty_.update_hw_cursor();
}

// CUD: cursor down N rows, clipped at last row.
void vt_parser::action_cursor_down(int n) {
    tty_.row_ = vt_clamp(tty_.row_ + n, 0, tty_.rows_ - 1);
    tty_.update_hw_cursor();
}

// CUF: cursor right N columns, clipped at last column.
void vt_parser::action_cursor_right(int n) {
    tty_.col_ = vt_clamp(tty_.col_ + n, 0, tty_.cols_ - 1);
    tty_.update_hw_cursor();
}

// CUB: cursor left N columns, clipped at column 0.
void vt_parser::action_cursor_left(int n) {
    tty_.col_ = vt_clamp(tty_.col_ - n, 0, tty_.cols_ - 1);
    tty_.update_hw_cursor();
}

// ED: erase display.
//   n=0: cursor to end of screen
//   n=1: start of screen to cursor (inclusive)
//   n=2: entire screen
void vt_parser::action_erase_display(int n) {
    int cursor = tty_.row_ * tty_.cols_ + tty_.col_;
    int total  = tty_.rows_ * tty_.cols_;
    if (n == 0) {
        tty_.fill_cells(cursor, total);
    } else if (n == 1) {
        tty_.fill_cells(0, cursor + 1);
    } else { // n == 2 or n == 3
        tty_.fill_cells(0, total);
    }
}

// EL: erase line.
//   n=0: cursor to end of line
//   n=1: start of line to cursor (inclusive)
//   n=2: entire line
void vt_parser::action_erase_line(int n) {
    int line_start  = tty_.row_ * tty_.cols_;
    int cursor_pos  = line_start + tty_.col_;
    int line_end    = line_start + tty_.cols_;
    if (n == 0) {
        tty_.fill_cells(cursor_pos, line_end);
    } else if (n == 1) {
        tty_.fill_cells(line_start, cursor_pos + 1);
    } else { // n == 2
        tty_.fill_cells(line_start, line_end);
    }
}

// SGR: select graphic rendition (colors and stuff)
// Maps ANSI 8-color palette to the CGA 16-color palette.
void vt_parser::action_sgr() {
    // ANSI color index -> CGA color nibble
    static const uint8_t ansi2cga[8] = {0, 4, 2, 6, 1, 5, 3, 7};

    if (nparams_ == 0) {
        tty_.color_ = COLOR_GRAY;
        return;
    }
    for (int i = 0; i < nparams_; ++i) {
        int p = params_[i];
        if (p == 0) {
            tty_.color_ = COLOR_GRAY;
        } else if (p == 1) {
            // Bold -> high-intensity foreground
            tty_.color_ |= 0x0800;
        } else if (p == 4) {
            // Underline -> blue foreground (CGA cannot underline)
            tty_.color_ = (tty_.color_ & 0xF000) | (ansi2cga[4] << 8);
        } else if (p == 7) {
            // Reverse -> swap FG and BG nibbles
            int fg = (tty_.color_ >> 8) & 0x0F;
            int bg = (tty_.color_ >> 12) & 0x0F;
            tty_.color_ = (fg << 12) | (bg << 8);
        } else if (p >= 30 && p <= 37) {
            // Set foreground color
            tty_.color_ = (tty_.color_ & 0xF000) | (ansi2cga[p - 30] << 8);
        } else if (p == 39) {
            // Reset foreground to default (gray = CGA 7)
            tty_.color_ = (tty_.color_ & 0xF000) | 0x0700;
        } else if (p >= 40 && p <= 47) {
            // Set background color
            tty_.color_ = (tty_.color_ & 0x0F00) | (ansi2cga[p - 40] << 12);
        } else if (p == 49) {
            // Reset background to default (black = CGA 0)
            tty_.color_ = tty_.color_ & 0x0F00;
        }
    }
}

// DECSTBM: set scrolling region
// Homes the cursor to (0,0) after setting the region (VT100 behaviour).
void vt_parser::action_set_scroll_region(int top, int bot) {
    if (top < 0) {
        top = 0;
    }
    if (bot >= tty_.rows_) {
        bot = tty_.rows_ - 1;
    }
    if (top >= bot) {
        return; // degenerate region, ignore this
    }
    tty_.scroll_top_ = top;
    tty_.scroll_bot_ = bot;
    tty_.row_ = 0;
    tty_.col_ = 0;
    tty_.update_hw_cursor();
}

// IND: move cursor down one row in the scroll region; scroll if at bottom.
void vt_parser::action_index() {
    if (tty_.row_ == tty_.scroll_bot_) {
        tty_.scroll_up_region();
        // cursor stays at scroll_bot_ after region scrolls
    } else if (tty_.row_ < tty_.rows_ - 1) {
        ++tty_.row_;
    }
    tty_.update_hw_cursor();
}

// RI: move cursor up one row in the scroll region; scroll if at top.
void vt_parser::action_reverse_index() {
    if (tty_.row_ == tty_.scroll_top_) {
        tty_.scroll_down_region();
        // cursor stays at scroll_top_ after region scrolls
    } else if (tty_.row_ > 0) {
        --tty_.row_;
    }
    tty_.update_hw_cursor();
}

// DECSC: save cursor position and SGR attributes.
void vt_parser::action_decsc() {
    tty_.saved_row_   = tty_.row_;
    tty_.saved_col_   = tty_.col_;
    tty_.saved_color_ = tty_.color_;
}

// DECRC: restore cursor position and SGR attributes.
void vt_parser::action_decrc() {
    tty_.row_   = tty_.saved_row_;
    tty_.col_   = tty_.saved_col_;
    tty_.color_ = tty_.saved_color_;
    tty_.update_hw_cursor();
}
