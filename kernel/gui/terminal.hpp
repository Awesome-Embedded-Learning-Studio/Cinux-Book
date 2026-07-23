/**
 * @file kernel/gui/terminal.hpp
 * @brief Terminal window with 80x25 character buffer, cursor, and text rendering
 *
 * A GUI Terminal that inherits from Window and provides an 80x25 text
 * buffer with per-cell foreground/background colours, a blinking cursor,
 * and basic ANSI escape sequence support (clear screen, cursor home,
 * clear to end of line).  Used as the primary text output surface for
 * the Cinux shell and other console programs.
 *
 * This header is only compiled when CINUX_GUI is defined.
 *
 * Namespace: cinux::gui
 */

#pragma once

#include <stdint.h>

#include "kernel/gui/window.hpp"

namespace cinux::drivers {
class PSFFont;
}
namespace cinux::fs {
struct Inode;
}
namespace cinux::ipc {
class Pipe;
}

namespace cinux::gui {

// ============================================================
// TerminalCell -- one character cell in the terminal buffer
// ============================================================

/**
 * @brief Represents a single character cell in the terminal buffer
 */
struct TerminalCell {
    char     ch = ' ';
    uint32_t fg = 0x00FFFFFF;  // white
    uint32_t bg = 0x00000000;  // black
};

// ============================================================
// Terminal class
// ============================================================

/**
 * @brief A terminal window with 80x25 character buffer and cursor management
 *
 * Provides text rendering via a screen buffer, keyboard input handling
 * through on_key(), and paint output via on_paint().  Supports basic
 * ANSI escape sequences for clear screen, cursor home, and clear-to-EOL.
 *
 * The write() method is intended as the stdout callback from user-space
 * programs (e.g. the shell), allowing their output to be displayed in
 * the terminal window.
 */
class Terminal : public Window {
public:
    // ============================================================
    // Constants
    // ============================================================

    static constexpr uint32_t COLS = 80;
    static constexpr uint32_t ROWS = 25;

    // ============================================================
    // Construction / destruction
    // ============================================================

    /**
     * @brief Construct a terminal window
     *
     * @param x      Screen X position (pixels)
     * @param y      Screen Y position (pixels)
     * @param title  Window title
     */
    Terminal(uint32_t x, uint32_t y, const char* title = "Cinux Terminal");
    /**
     * @brief Destroy the terminal and clean up all resources
     *
     * Closes stdin pipe write end (notifies shell: EOF on stdin) and
     * stdout pipe read end (shell writes will return -1).  Then calls
     * waitpid to reap the shell child process and prevent zombies.
     */
    ~Terminal() override;

    Terminal(const Terminal&)            = delete;
    Terminal& operator=(const Terminal&) = delete;

    // ============================================================
    // Window virtual overrides
    // ============================================================

    /**
     * @brief Handle a keyboard event
     *
     * Writes the key's ASCII character into the screen buffer.
     * Also forwards the character to the pipe write fd if set
     * (for Phase 5 shell integration).
     *
     * @param ev  The keyboard event
     */
    void on_key(KeyEvent& ev) override;

    /**
     * @brief Paint the terminal content onto a canvas
     *
     * Draws the title bar, fills the content background, renders
     * each non-space character via canvas.draw_text(), and draws
     * the cursor as a reverse-colour block.
     *
     * @param canvas  The canvas to paint onto (the screen canvas)
     */
    void on_paint(cinux::drivers::Canvas& canvas) override;

    /**
     * @brief Query whether this window is a terminal
     * @return true (Terminal is always a terminal window)
     */
    bool is_terminal() const override { return true; }

    // ============================================================
    // External write interface (shell stdout callback)
    // ============================================================

    /**
     * @brief Write a string into the terminal buffer
     *
     * Processes each character, handling printable chars, newlines,
     * carriage returns, backspaces, tabs, and ANSI escape sequences.
     *
     * @param str  String data
     * @param len  Number of bytes to write
     */
    void write(const char* str, uint64_t len);

    // ============================================================
    // Query
    // ============================================================

    /**
     * @brief Access a single cell in the terminal buffer
     *
     * @param row  Row index (0..ROWS-1)
     * @param col  Column index (0..COLS-1)
     * @return     Reference to the cell
     */
    const TerminalCell& cell(uint32_t row, uint32_t col) const;

    /**
     * @brief Get the current cursor column
     * @return cursor X position (0..COLS-1)
     */
    uint32_t cursor_x() const;

    /**
     * @brief Get the current cursor row
     * @return cursor Y position (0..ROWS-1)
     */
    uint32_t cursor_y() const;

    // ============================================================
    // Stdout pipe polling (Phase 5 shell output forwarding)
    // ============================================================

    /**
     * @brief Bind a PTY master as this terminal's shell channel (F-ECO busybox)
     *
     * The shell runs on the PTY slave (a real tty: isatty true, so musl line-
     * buffers stdout + ash is interactive).  The terminal holds the master:
     * on_key() writes typed chars to the master (the PTY's slave TTY echoes +
     * cooks them), and poll_output() drains the master (shell output + the TTY
     * echo).  @p pty_index is released in the destructor.
     *
     * @param master     The PTY master inode (read = shell output + echo,
     *                   write = keyboard into the slave TTY).
     * @param pty_index  The PTY slot index (for pty_release on destroy).
     */
    void set_master(cinux::fs::Inode* master, int pty_index);

    // Legacy pipe interface (used by the QEMU terminal-shell test; production
    // GUI uses the PTY master above).  on_key forwards to stdin_pipe, poll_output
    // drains stdout_pipe.
    void set_stdin_pipe(class cinux::ipc::Pipe* pipe);
    void set_stdout_pipe(class cinux::ipc::Pipe* pipe);

    /**
     * @brief Set the PID of the shell child process bound to this terminal
     *
     * Used by the destructor to waitpid() and reap the shell child,
     * preventing zombie processes when the terminal window is closed.
     *
     * @param pid  Shell child process PID, or 0 if no shell is attached
     */
    void set_shell_pid(int pid);

    /**
     * @brief Get the PID of the shell child process
     * @return Shell child PID, or 0 if none is attached
     */
    int shell_pid() const;

    /**
     * @brief Non-blocking poll of the stdout pipe for shell output
     *
     * Reads available bytes from the stdout pipe (if set) via try_read() and
     * writes them into the terminal buffer via write(). Should be called from
     * the GUI pump each iteration.
     *
     * @return true if at least one byte was read (the terminal buffer changed
     *              and the screen region should be re-flushed this frame),
     *         false if no output was pending
     */
    bool poll_output();

    /**
     * @brief Consume the "content changed outside the stdout pipe" flag
     *
     * on_key() with no stdin pipe mutates the screen buffer directly (process_char)
     * instead of through the stdout pipe, so poll_output() does not signal it. This
     * returns (and clears) a flag set on that path so the pump can invalidate the
     * frame -- otherwise a direct key echo would never reach the screen under the
     * §4c dirty-gated composite (the live desktop always wires pipes, so this only
     * matters for a pipe-less / direct-echo terminal).
     *
     * @return true if content changed via a non-stdout path since the last call
     */
    bool consume_content_dirty();

    // ============================================================
    // Rendering
    // ============================================================

    void set_font(cinux::drivers::PSFFont* font);
    void render_to_canvas();

    // ============================================================
    // Clear
    // ============================================================

    /**
     * @brief Clear the entire screen buffer and reset cursor to (0, 0)
     */
    void clear();

private:
    // ============================================================
    // Internal helpers
    // ============================================================

    /**
     * @brief Process a single character into the screen buffer
     * @param ch  The character to process
     */
    void process_char(char ch);

    /**
     * @brief Scroll the screen buffer up by one line
     *
     * Moves rows 1..ROWS-1 to rows 0..ROWS-2 and clears the last row.
     */
    void scroll_up();

    /**
     * @brief Advance to the beginning of the next line
     *
     * Wraps to the next row; scrolls up if at the bottom.
     */
    void newline();

    /**
     * @brief Handle backspace: erase previous character
     */
    void backspace();

    /**
     * @brief Handle tab: advance cursor to next 8-column tab stop
     */
    void tab();

    // ============================================================
    // ANSI escape sequence state
    // ============================================================

    /**
     * @brief Check if the character starts an ANSI escape sequence
     * @return true if ch == '\033'
     */
    static bool is_escape(char ch);

    /**
     * @brief Try to parse and handle an ANSI CSI sequence
     *
     * Reads from str[pos] until a complete sequence is found or
     * parsing fails.  Advances pos past the consumed bytes.
     *
     * @param str  The source string
     * @param len  Total length of the string
     * @param pos  Current position (updated on return)
     */
    void handle_ansi(const char* str, uint64_t len, uint64_t& pos);

    // ============================================================
    // Members
    // ============================================================

    TerminalCell screen_[ROWS][COLS];
    uint32_t     cursor_x_       = 0;
    uint32_t     cursor_y_       = 0;
    uint32_t     fg_             = 0x00FFFFFF;
    uint32_t     bg_             = 0x00000000;
    bool         cursor_visible_ = true;

    /// Set when content mutates via a non-stdout path (on_key with no stdin pipe).
    /// The pump consumes it to invalidate the frame (§4c dirty-gating).
    bool                     content_dirty_ = false;
    cinux::drivers::PSFFont* font_          = nullptr;
    cinux::fs::Inode*        master_inode_  = nullptr;  ///< PTY master (shell channel)
    int                      pty_index_     = -1;       ///< PTY slot (for pty_release)
    ipc::Pipe*               stdin_pipe_    = nullptr;  ///< legacy pipe (test path)
    ipc::Pipe*               stdout_pipe_   = nullptr;  ///< legacy pipe (test path)
    int                      shell_pid_     = 0;  ///< PID of the shell child process (0 = none)
};

}  // namespace cinux::gui
