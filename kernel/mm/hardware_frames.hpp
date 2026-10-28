/**
 * @file    hardware_frames.hpp
 * @brief   The registry of physical frames owned by hardware, never by
 *          the allocator.
 *
 * A memory device whose mmap faults straight to MMIO lends the kernel
 * nothing: those frames belong to the device, and every retirement path
 * that would hand one back to the frame allocator consults this registry
 * first. Bring-up reserves one span per device before any mapping goes
 * live; the spans never move and the table never shrinks.
 *
 * @author  Charliechen114514
 * @date    2026-10-26
 * @version 0.1
 * @since   0.9.0
 * @ingroup kernel_mm
 * @copyright Copyright (c) 2026
 */

#pragma once

namespace cinux::mm {

/**
 * @brief         Reserves one span of hardware-owned frames.
 *
 * @param[in]     physical_base   First frame's physical address,
 *                                page-aligned.
 * @param[in]     page_count      Frames in the span.
 * @return        True when the span joined the registry; false when the
 *                table is full.
 * @note          Bring-up wiring, single-threaded, before the device's
 *                node serves mappings. Overlapping spans are refused the
 *                same way a full table is: one owner per frame.
 * @since         0.9.0
 * @ingroup       kernel_mm
 */
[[nodiscard]] bool ReserveHardwarePages(unsigned long physical_base, unsigned long page_count);

/**
 * @brief         Whether one physical frame belongs to hardware.
 *
 * @param[in]     physical   The frame's physical address.
 * @return        True when a reserved span covers the frame.
 * @since         0.9.0
 * @ingroup       kernel_mm
 */
[[nodiscard]] bool IsHardwarePage(unsigned long physical);

}  // namespace cinux::mm
