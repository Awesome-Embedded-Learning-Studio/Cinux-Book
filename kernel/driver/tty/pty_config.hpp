/**
 * @file    pty_config.hpp
 * @brief   Facts the PTY pair and its devfs seats must agree on.
 *
 * How many pairs the slot table holds, each ring's budget, where the
 * pair inode numbers begin, and the device identities stat reports.
 *
 * @author  Charliechen114514
 * @date    2026-10-19
 * @version 0.1
 * @since   0.5.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::driver {

/// How many PTY pairs the slot table holds at once.
inline constexpr unsigned long kPtyPairs = 8;

/// One pair's raw-key budget: bytes a master wrote that the slave's
/// discipline has not taken yet.
inline constexpr unsigned long kPtyRawBytes = 256;

/// One pair's master-read budget: the slave's output and its echo,
/// waiting for whoever holds the master end.
inline constexpr unsigned long kPtyMasterBytes = 4096;

/// Where the pair inode numbers begin, above devfs's own table rows.
inline constexpr unsigned long kPtyInoBase = 0x1000;

/// The ptmx device's identity, Linux's 5:2.
inline constexpr unsigned long kPtmxDeviceId = (5UL << 8U) | 2UL;

/// The master side's identity word base, Linux's major 128.
inline constexpr unsigned long kPtyMasterMajor = 128;

/// The slave side's identity word base, Linux's major 136.
inline constexpr unsigned long kPtySlaveMajor = 136;

}  // namespace cinux::driver
