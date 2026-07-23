/**
 * @file kernel/gui/terminal.cpp
 * @brief Terminal window implementation
 */

#include "terminal.hpp"

#include "kernel/drivers/tty/pty_device.hpp"  // pty_release (F-ECO busybox PTY)
#include "kernel/drivers/video/font.hpp"
#include "kernel/fs/inode.hpp"  // Inode::ops->read/write on the PTY master
#include "kernel/ipc/pipe.hpp"  // legacy pipe (test path)
#include "kernel/lib/echo_trace.hpp"
#include "kernel/proc/pid.hpp"
#include "kernel/proc/process.hpp"

namespace cinux::gui {

// ============================================================
// Construction
// ============================================================

Terminal::Terminal(uint32_t x, uint32_t y, const char* title)
    : Window(title, static_cast<int32_t>(x), static_cast<int32_t>(y),
             COLS * 8,   // 80 chars * 8px per char (approximate)
             ROWS * 16)  // 25 rows * 16px per char (approximate)
{
    // Initialise all cells to default (space, white on black)
    for (uint32_t r = 0; r < ROWS; r++) {
        for (uint32_t c = 0; c < COLS; c++) {
            screen_[r][c] = TerminalCell{};
        }
    }
}

// ============================================================
// Destruction
// ============================================================

Terminal::~Terminal() {
    // Release the PTY slot so a later terminal can reuse it.  The shell child
    // is reaped below (zombie prevention); the PTY's master/slave inodes are
    // owned by the slot table (pty_device.cpp), not this Terminal.
    if (pty_index_ >= 0) {
        cinux::drivers::pty_release(pty_index_);
        pty_index_ = -1;
    }
    master_inode_ = nullptr;
    // Legacy pipe cleanup (test path): close the ends this Terminal owns so the
    // peer detects EOF / write failure.
    if (stdin_pipe_ != nullptr) {
        stdin_pipe_->close_writer();
    }
    if (stdout_pipe_ != nullptr) {
        stdout_pipe_->close_reader();
    }
    stdin_pipe_  = nullptr;
    stdout_pipe_ = nullptr;

    // Reap the shell child process to prevent zombies.
    // Try up to a bounded number of iterations -- if the shell has already
    // exited it will be a zombie ready to reap; if still running we give up.
    if (shell_pid_ > 0) {
        for (uint32_t attempt = 0; attempt < 1000; attempt++) {
            int  status = 0;
            auto result = cinux::proc::waitpid(shell_pid_, &status, cinux::proc::kWaitNoHang,
                                               cinux::proc::g_pid_alloc);
            if (result == cinux::proc::WaitpidResult::Ok) {
                cinux::lib::kprintf("[TERM] Reaped shell pid=%d status=%d\n", shell_pid_, status);
                break;
            }
            if (result == cinux::proc::WaitpidResult::NoChildren ||
                result == cinux::proc::WaitpidResult::NotFound) {
                break;
            }
            // NotExited -- spin briefly and retry
        }
        shell_pid_ = 0;
    }
}

// ============================================================
// Window virtual overrides
// ============================================================

void Terminal::on_key(KeyEvent& ev) {
    // Only handle key press events, not releases
    if (!ev.pressed) {
        return;
    }

    // Only process printable characters
    if (ev.ascii == 0) {
        return;
    }

    // PTY master connected: type into the terminal.  master_write feeds the
    // slave's TTY line discipline, which echoes the char back onto the master
    // read side (poll_output picks it up next pump) + cooks the line for the
    // shell.  No local echo here -- the PTY's TTY does the echo (a real
    // terminal model), so busybox ash sees isatty(slave)==true + interactive mode
    // + musl line-buffers stdout (output flushes per line, not stuck in stdio).
    if (master_inode_ != nullptr) {
        char ch = ev.ascii;
        if (ch == '\r') {
            ch = '\n';
        }
        cinux::debug::trace_char("terminal.on_key before master_write", ch);
        auto wr = master_inode_->ops->write(master_inode_, 0, &ch, 1);
        if (cinux::debug::kEchoTrace) {
            cinux::lib::kprintf("[ECHO_TRACE] terminal.on_key master_write ok=%d n=%d\n",
                                wr.ok() ? 1 : 0, wr.ok() ? static_cast<int>(*wr) : -1);
        }
        (void)wr;  // echo arrives via poll_output (master_read); no local display
        return;
    }

    // Legacy pipe path (test only): forward to stdin pipe.  No local echo --
    // the test expects the original behavior (forward-only; display happens via
    // stdout pipe + poll_output).  Production uses the PTY master branch above.
    if (stdin_pipe_ != nullptr) {
        char ch = ev.ascii;
        if (ch == '\r') {
            ch = '\n';
        }
        stdin_pipe_->try_write(&ch, 1);
        return;
    }

    // No pipe connected -- write directly to the screen buffer
    process_char(ev.ascii);
    content_dirty_ = true; /* not seen by poll_output; pump consumes this (§4c) */
}

void Terminal::on_paint(cinux::drivers::Canvas& /*canvas*/) {
    if (font_ != nullptr) {
        render_to_canvas();
    }
}

// ============================================================
// External write interface
// ============================================================

void Terminal::write(const char* str, uint64_t len) {
    uint64_t pos = 0;

    while (pos < len) {
        char ch = str[pos];

        // Check for ANSI escape sequence
        if (is_escape(ch)) {
            handle_ansi(str, len, pos);
            continue;
        }

        // Process the character
        switch (ch) {
        case '\n':
            newline();
            break;
        case '\r':
            cursor_x_ = 0;
            break;
        case '\b':
            backspace();
            break;
        case '\t':
            tab();
            break;
        default:
            process_char(ch);
            break;
        }

        pos++;
    }
}

// ============================================================
// Query
// ============================================================

const TerminalCell& Terminal::cell(uint32_t row, uint32_t col) const {
    return screen_[row][col];
}

uint32_t Terminal::cursor_x() const {
    return cursor_x_;
}

uint32_t Terminal::cursor_y() const {
    return cursor_y_;
}

// ============================================================
// Stdout pipe polling
// ============================================================

void Terminal::set_master(cinux::fs::Inode* master, int pty_index) {
    master_inode_ = master;
    pty_index_    = pty_index;
}

void Terminal::set_stdin_pipe(cinux::ipc::Pipe* pipe) {
    stdin_pipe_ = pipe;
}

void Terminal::set_stdout_pipe(cinux::ipc::Pipe* pipe) {
    stdout_pipe_ = pipe;
}

void Terminal::set_shell_pid(int pid) {
    shell_pid_ = pid;
}

int Terminal::shell_pid() const {
    return shell_pid_;
}

bool Terminal::poll_output() {
    // Legacy pipe path (test): drain stdout pipe.
    if (master_inode_ == nullptr && stdout_pipe_ != nullptr) {
        char buf[256];
        bool got_any = false;
        while (true) {
            int64_t n = stdout_pipe_->try_read(buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            write(buf, static_cast<uint64_t>(n));
            got_any = true;
        }
        return got_any;
    }

    if (master_inode_ == nullptr) {
        return false;
    }

    // Drain the PTY master: shell output (stdout, which musl line-buffered
    // because the slave is a tty) + the slave TTY's echo of typed chars.
    char buf[256];
    bool got_any = false;
    while (true) {
        auto    r = master_inode_->ops->read(master_inode_, 0, buf, sizeof(buf));
        int64_t n = r.ok() ? *r : 0;
        if (n <= 0) {
            break;
        }
        cinux::debug::trace_bytes("terminal.poll_output master_read", buf, n);
        write(buf, static_cast<uint64_t>(n));
        got_any = true;
    }
    return got_any;
}

bool Terminal::consume_content_dirty() {
    bool was       = content_dirty_;
    content_dirty_ = false;
    return was;
}

// ============================================================
// Clear
// ============================================================

void Terminal::clear() {
    for (uint32_t r = 0; r < ROWS; r++) {
        for (uint32_t c = 0; c < COLS; c++) {
            screen_[r][c] = TerminalCell{};
        }
    }
    cursor_x_ = 0;
    cursor_y_ = 0;
}

// ============================================================
// Internal helpers
// ============================================================

void Terminal::process_char(char ch) {
    // Only process printable ASCII characters
    if (static_cast<uint8_t>(ch) < 0x20 || static_cast<uint8_t>(ch) > 0x7E) {
        return;
    }

    // Write character at current cursor position
    screen_[cursor_y_][cursor_x_].ch = ch;
    screen_[cursor_y_][cursor_x_].fg = fg_;
    screen_[cursor_y_][cursor_x_].bg = bg_;

    // Advance cursor
    cursor_x_++;

    // Wrap at end of line
    if (cursor_x_ >= COLS) {
        cursor_x_ = 0;
        newline();
    }
}

void Terminal::scroll_up() {
    // Move rows 1..ROWS-1 up by one row
    for (uint32_t r = 0; r < ROWS - 1; r++) {
        for (uint32_t c = 0; c < COLS; c++) {
            screen_[r][c] = screen_[r + 1][c];
        }
    }

    // Clear the last row
    for (uint32_t c = 0; c < COLS; c++) {
        screen_[ROWS - 1][c] = TerminalCell{};
    }
}

void Terminal::newline() {
    cursor_x_ = 0;
    cursor_y_++;

    // Scroll if at the bottom
    if (cursor_y_ >= ROWS) {
        cursor_y_ = ROWS - 1;
        scroll_up();
    }
}

void Terminal::backspace() {
    if (cursor_x_ > 0) {
        cursor_x_--;
        screen_[cursor_y_][cursor_x_] = TerminalCell{};
    } else if (cursor_y_ > 0) {
        // Move to end of previous line
        cursor_y_--;
        cursor_x_                     = COLS - 1;
        screen_[cursor_y_][cursor_x_] = TerminalCell{};
    }
}

void Terminal::tab() {
    // Advance to next 8-column tab stop
    uint32_t next_tab = (cursor_x_ / 8 + 1) * 8;
    if (next_tab >= COLS) {
        cursor_x_ = COLS - 1;
    } else {
        cursor_x_ = next_tab;
    }
}

// ANSI escape handling (is_escape / handle_ansi) lives in terminal_ansi.cpp --
// split off to keep this TU under the 500-line CI line-limit.

// ============================================================
// Font / rendering
// ============================================================

void Terminal::set_font(cinux::drivers::PSFFont* font) {
    font_ = font;
}

void Terminal::render_to_canvas() {
    if (font_ == nullptr) {
        return;
    }

    auto&    cvs = canvas();
    uint32_t gw  = font_->width();
    uint32_t gh  = font_->height();

    for (uint32_t row = 0; row < ROWS; row++) {
        for (uint32_t col = 0; col < COLS; col++) {
            const TerminalCell& cell = screen_[row][col];
            uint32_t            px   = col * gw;
            uint32_t            py   = TITLE_BAR_HEIGHT + row * gh;

            cvs.draw_rect(px, py, gw, gh, cell.bg);

            if (cell.ch > ' ') {
                const uint8_t* g = font_->glyph(static_cast<uint8_t>(cell.ch));
                if (g != nullptr) {
                    for (uint32_t gr = 0; gr < gh; gr++) {
                        uint8_t bits = g[gr];
                        for (uint32_t gc = 0; gc < gw; gc++) {
                            if ((bits >> (7 - gc)) & 1) {
                                cvs.draw_pixel(px + gc, py + gr, cell.fg);
                            }
                        }
                    }
                }
            }
        }
    }

    if (cursor_visible_) {
        uint32_t            cx = cursor_x_ * gw;
        uint32_t            cy = TITLE_BAR_HEIGHT + cursor_y_ * gh;
        const TerminalCell& cc = screen_[cursor_y_][cursor_x_];
        cvs.draw_rect(cx, cy, gw, gh, cc.fg);

        if (cc.ch > ' ') {
            const uint8_t* g = font_->glyph(static_cast<uint8_t>(cc.ch));
            if (g != nullptr) {
                for (uint32_t gr = 0; gr < gh; gr++) {
                    uint8_t bits = g[gr];
                    for (uint32_t gc = 0; gc < gw; gc++) {
                        if ((bits >> (7 - gc)) & 1) {
                            cvs.draw_pixel(cx + gc, cy + gr, cc.bg);
                        }
                    }
                }
            }
        }
    }
}

}  // namespace cinux::gui
