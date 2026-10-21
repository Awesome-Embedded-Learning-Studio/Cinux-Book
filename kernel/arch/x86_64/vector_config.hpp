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
}
