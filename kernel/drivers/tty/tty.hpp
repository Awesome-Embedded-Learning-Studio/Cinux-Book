/**
 * @file kernel/drivers/tty/tty.hpp
 * @brief Terminal line discipline + Linux termios UAPI (F10-M3 Phase 1)
 *
 * Pure logic: the TTY owns a termios state, a canonical line buffer, and a
 * cooked output queue.  Keyboard bytes go in via input_char(); sys_read drains
 * read_cooked() (batch 3).  Signal/echo are decoupled -- signal requests come
 * back as a TtySignal enum and echo goes through an injected callback -- so the
 * unit compiles in the host test harness with no proc/Console dependency.
 *
 * libc-agnostic: the Termios layout + c_lflag/c_cc constants match the Linux
 * x86_64 UAPI (<asm-generic/termbits.h>), so musl and glibc read the same
 * struct via TCGETS/TCSETS (batch 4).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cinux::drivers {

// ============================================================
// Linux x86_64 termios UAPI
// ============================================================

/// NCCS: number of c_cc control-char slots (Linux x86_64).
constexpr size_t kNccs = 19;

/// Linux <asm-generic/termbits.h> struct termios (NOT termios2).  libc reads
/// this exact layout via TCGETS.
struct Termios {
    uint32_t c_iflag;      ///< input modes
    uint32_t c_oflag;      ///< output modes
    uint32_t c_cflag;      ///< control modes
    uint32_t c_lflag;      ///< local modes (ICANON/ECHO/ISIG ...)
    uint8_t  c_line;       ///< line discipline
    uint8_t  c_cc[kNccs];  ///< control chars (VINTR/VERASE/VEOF/VSUSP ...)
};

// c_lflag bits (values match Linux octal definitions).
constexpr uint32_t kIsig   = 0b0000001;  ///< ISIG  (signal chars generate signals)
constexpr uint32_t kIcanon = 0b0000010;  ///< ICANON (canonical / line mode)
constexpr uint32_t kEcho   = 0b0001000;  ///< ECHO
constexpr uint32_t kEchoe  = 0b0010000;  ///< ECHOE (erase: bs space bs)
constexpr uint32_t kEchok  = 0b0100000;  ///< ECHOK (echo KILL char)

// c_cc indices (Linux x86_64 asm-generic order).
constexpr uint8_t kVintr   = 0;
constexpr uint8_t kVquit   = 1;
constexpr uint8_t kVerase  = 2;
constexpr uint8_t kVkill   = 3;
constexpr uint8_t kVeof    = 4;
constexpr uint8_t kVsusp   = 10;
constexpr uint8_t kVwerase = 14;

// Common ASCII control bytes ( ^ X = X - 'A' + 1 ).
constexpr char kCharEof       = 0x04;  ///< ^D (default VEOF)
constexpr char kCharIntr      = 0x03;  ///< ^C (default VINTR)
constexpr char kCharQuit      = 0x1C;  ///< ^\ (default VQUIT)
constexpr char kCharSusp      = 0x1A;  ///< ^Z (default VSUSP)
constexpr char kCharNewline   = '\n';
constexpr char kCharBackspace = 0x7F;  ///< DEL (default VERASE on most terms)

// ============================================================
// Linux x86_64 ioctl requests + window size (F10-M3 batch 4)
// ============================================================

// Terminal ioctl requests (Linux <asm-generic/ioctls.h>, magic 'T' = 0x54).
// libc reaches these via ioctl(2): TCGETS/TCSETS read/write termios,
// TIOCGWINSZ learns row/col geometry -- musl/glibc probe it on the first
// stdout write to pick a buffering mode and a wrap width.
constexpr uint32_t kTcgets     = 0x5401;  ///< TCGETS:    read termios
constexpr uint32_t kTcsets     = 0x5402;  ///< TCSETS:    write termios
constexpr uint32_t kTiocgwinsz = 0x5413;  ///< TIOCGWINSZ: read window size
constexpr uint32_t kTiocgpgrp  = 0x540F;  ///< TIOCGPGRP: get foreground pgid (batch 5)
constexpr uint32_t kTiocspgrp  = 0x5410;  ///< TIOCSPGRP: set foreground pgid (batch 5)

/// Linux <asm-generic/termios.h> struct winsize. libc reads this exact layout
/// via TIOCGWINSZ.
struct Winsize {
    uint16_t ws_row;     ///< rows (text lines)
    uint16_t ws_col;     ///< columns
    uint16_t ws_xpixel;  ///< horizontal pixels (unused, 0)
    uint16_t ws_ypixel;  ///< vertical pixels (unused, 0)
};

/// Build the default termios (ICANON|ECHO|ECHOE|ECHOK|ISIG + ^C/^\/DEL/^D/^Z).
void make_default_termios(Termios& out);

// ============================================================
// Line discipline
// ============================================================

/// Signal requested by a line-discipline char.  Decoupled from proc::Signal so
/// the TTY stays host-testable; the kernel wiring (batch 5) maps these to
/// signal_send.
enum class TtySignal {
    kNone,
    kSigint,   ///< Ctrl+C (VINTR) -> SIGINT
    kSigquit,  ///< ^\ (VQUIT) -> SIGQUIT
    kSigtstp,  ///< ^Z (VSUSP) -> SIGTSTP
};

/// Outcome of feeding one input byte.
enum class InputResult {
    kConsumed,   ///< processed, no line ready (editing/echo/printable)
    kLineReady,  ///< newline or VEOF committed a cooked line; read_cooked() can serve
    kEof,        ///< VEOF on an empty line -> sys_read should return 0
    kSignal,     ///< signal char; see pending_signal()
    kOverflow,   ///< canonical line buffer full, char dropped
};

/// Terminal line discipline: termios state + canonical line + cooked queue.
class TTY {
public:
    static constexpr size_t kLineBufSize   = 256;
    static constexpr size_t kCookedBufSize = kLineBufSize * 2;

    TTY();

    /// Feed one raw input byte.  Drives canonical editing, echo, signal
    /// generation, and line accumulation.
    InputResult input_char(char c);

    /// Copy cooked bytes into @p buf (a committed canonical line, or raw bytes
    /// in ~ICANON).  Returns the count copied; 0 if nothing is ready.
    size_t read_cooked(char* buf, size_t maxlen);

    /// Consume a pending EOF (VEOF/^D on an empty line).  Returns true once per
    /// EOF so the caller (sys_read) can return 0 to the application exactly
    /// once, distinct from "no line yet" (which blocks).
    bool take_eof();

    /// Signal requested by the most recent kSignal input_char() (peek; does
    /// not clear).
    TtySignal pending_signal() const;

    /// Consume and return the pending signal (clears it). Use on the kSignal
    /// branch so a signal char is delivered exactly once.
    TtySignal take_signal();

    const Termios& termios() const;
    void           set_termios(const Termios& t);

    /// Install the echo sink (Console in the kernel; a capture buffer in host
    /// tests).  @p emit is called for each echoed byte.
    void set_echo_sink(void (*emit)(char c, void* ctx), void* ctx);

private:
    void echo(char c) const;
    bool cooked_push(char c);
    bool cooked_pop(char& c);
    void commit_line();  ///< copy line_buf_[0..line_len_) into the cooked queue

    Termios termios_;

    char   line_buf_[kLineBufSize];
    size_t line_len_;

    // cooked output ring (committed lines + raw bytes waiting for read_cooked)
    char   cooked_[kCookedBufSize];
    size_t cooked_head_;
    size_t cooked_tail_;
    bool   cooked_full_;

    TtySignal pending_signal_;
    bool      eof_pending_;  ///< VEOF on an empty line -- next read returns 0

    void (*echo_fn_)(char c, void* ctx);
    void* echo_ctx_;
};

}  // namespace cinux::drivers
