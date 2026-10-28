/**
 * @file    tty_config.hpp
 * @brief   Fact the terminal discipline and its callers must agree on.
 *
 * The line budget, the pool budget, the termios record's shape as the
 * hosted world spells it, and the window record's values. Knobs private
 * to the discipline's own algorithms live beside its code in tty.cpp;
 * everything two files must share lives here.
 *
 * @author  Charliechen114514
 * @date    2026-10-19
 * @version 0.1
 * @since   0.5.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstdint>

namespace cinux::driver {

/// One terminal line's editing budget, NUL included.
inline constexpr unsigned long kTtyLineMax = 128;

/// How many finished lines the terminal keeps before a reader drains it.
inline constexpr unsigned long kTtyPoolBytes = kTtyLineMax * 8;

/// A termios record as the wire sizes it: four flag words, one line
/// byte, nineteen control characters — thirty-six bytes, the kernel
/// termios the hosted TCGETS/TCSETS carry. The libc struct termios is
/// larger (thirty-two control characters, two speed words) and glibc
/// converts between the two shapes in tcgetattr/tcsetattr; the syscall
/// boundary itself never carries more than these thirty-six. The
/// sixty-byte reading of 2026-10-19 measured the libc struct and was
/// the wrong layer: it wrote twenty-four bytes past the record into
/// whatever the caller kept beside it.
inline constexpr unsigned long kTermiosBytes = 36;

/** @brief Extended Linux termios2 record with speed words. */
inline constexpr unsigned long      kTermios2Bytes = 44;
/** @brief Extended terminal attribute read. */
inline constexpr unsigned long long kTtyTcgets2    = 0x802C542A;
/** @brief Extended terminal attribute write. */
inline constexpr unsigned long long kTtyTcsets2    = 0x402C542B;
/** @brief Extended attribute write after drain. */
inline constexpr unsigned long long kTtyTcsetsw2   = 0x402C542C;
/** @brief Extended attribute write with input flush. */
inline constexpr unsigned long long kTtyTcsetsf2   = 0x402C542D;

/// The window-size ioctl record's bytes on the wire.
inline constexpr unsigned long kTtyWinsizeBytes = 8;

/// The process-group ioctl word's bytes on the wire.
inline constexpr unsigned long kTtyPgrpBytes = 4;

/// The lflag word's seat inside a termios record.
inline constexpr unsigned long kTermiosLflagAt = 12;

/// The control-character array's byte offset in the hosted record.
inline constexpr unsigned long kTermiosCcAt = 17;

/// Control-character indices in the x86-64 termios ABI.
inline constexpr unsigned long kTermiosVintr  = 0;
inline constexpr unsigned long kTermiosVquit  = 1;
inline constexpr unsigned long kTermiosVerase = 2;
inline constexpr unsigned long kTermiosVkill  = 3;
inline constexpr unsigned long kTermiosVeof   = 4;
inline constexpr unsigned long kTermiosVmin   = 6;
inline constexpr unsigned long kTermiosVsusp  = 10;

/// The terminal questions' words, one home for every asker.
inline constexpr unsigned long long kTtyTcgets     = 0x5401;
inline constexpr unsigned long long kTtyTcsets     = 0x5402;
inline constexpr unsigned long long kTtyTcsetsw    = 0x5403;
inline constexpr unsigned long long kTtyTcsetsf    = 0x5404;
inline constexpr unsigned long long kTtyTiocsctty  = 0x540E;
inline constexpr unsigned long long kTtyTiocgpgrp  = 0x540F;
inline constexpr unsigned long long kTtyTiocspgrp  = 0x5410;
inline constexpr unsigned long long kTtyTiocgwinsz = 0x5413;
inline constexpr unsigned long long kTtyTiocswinsz = 0x5414;
inline constexpr unsigned long long kTtyTiocgptn   = 0x80045430;
inline constexpr unsigned long long kTtyTiocsptlck = 0x40045431;

/// Keep queued input when a signal character arrives.
inline constexpr std::uint32_t kTermiosNoflsh = 0x0080;

/// The ISIG bit: signal characters act on the line.
inline constexpr std::uint32_t kTermiosIsig = 0x0001;

/// The ICANON bit: line editing on, raw pass-through off.
inline constexpr std::uint32_t kTermiosIcanon = 0x0002;

/// The ECHO bit: the discipline echoes what it takes.
inline constexpr std::uint32_t kTermiosEcho = 0x0008;

/// The INTR character's value (^C), which ISIG turns into signal 2.
constexpr char kTtyIntrChar = 0x03;

/// The QUIT character's value (^\), which ISIG turns into signal 3.
constexpr char kTtyQuitChar = 0x1C;

/// The SUSP character's value (^Z), which ISIG turns into signal 20.
constexpr char kTtySuspChar = 0x1A;

/**
 * @brief   The window shape a terminal reports: cells, then pixels.
 * @note    The ioctl record's own shape is four shorts in this order;
 *          this type only names the values, not the wire layout.
 * @since   0.5.0
 * @ingroup kernel_driver
 */
struct TtyWindow {
    /// Rows of cells.
    unsigned short rows = 0;

    /// Columns of cells.
    unsigned short columns = 0;

    /// Pixel width, zero when the terminal has no pixel shape.
    unsigned short pixels_x = 0;

    /// Pixel height, zero when the terminal has no pixel shape.
    unsigned short pixels_y = 0;
};

}  // namespace cinux::driver
