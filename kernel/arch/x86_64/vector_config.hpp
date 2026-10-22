/**
 * @file vector_config.hpp
 * @brief Shared vector assignments for gates and interrupt controllers.
 * @author Charliechen114514
 * @date 2026-10-21
 * @version 0.1
 * @since 0.3.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */
#pragma once

namespace cinux::arch::irq {
inline constexpr unsigned int kFirstIrqVector = 0x20;
inline constexpr unsigned int kSpuriousVector = 0xFF;

/// First vector of the message pool. Chosen clear of every resident
/// assignment: line stubs 0x20-0x2F, local timer 0x30, sigreturn 0x80,
/// reschedule IPI 0xE0, spurious 0xFF.
inline constexpr unsigned int kFirstMessageVector = 0x40;
}
