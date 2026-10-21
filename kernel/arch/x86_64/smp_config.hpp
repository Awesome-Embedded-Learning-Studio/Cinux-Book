/**
 * @file smp_config.hpp
 * @brief Local interrupt vectors and startup geometry.
 * @author Charliechen114514
 * @date 2026-10-20
 * @version 0.1
 * @since 0.3.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */
#pragma once

namespace cinux::arch::smp {
inline constexpr unsigned int  kTimerVector        = 0x30;
inline constexpr unsigned int  kRescheduleVector   = 0xE0;
inline constexpr unsigned long kTrampolinePhysical = 0x8000;
}
