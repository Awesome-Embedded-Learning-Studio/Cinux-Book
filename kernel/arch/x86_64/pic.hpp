/**
 * @file    pic.hpp
 * @brief   The two 8259 PICs as the interrupt chip behind the service.
 *
 * A singleton whose four initialization words per chip move their
 * default answers (BIOS vectors 8 and 0x70, colliding with CPU
 * exceptions) to 0x20 and 0x28, the range this kernel's IDT arms for
 * IRQs. Satisfies the interrupt service's IrqChip contract — open a
 * line, acknowledge a line — without knowing that contract exists.
 * Ports and command bytes are device facts living in the .cpp.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.2
 * @since   0.1.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "cinux/singleton.hpp"
#include "kernel/interrupt/irq.hpp"

namespace cinux::arch {

/**
 * @brief         Programmable Interrupt Controller pair.
 * @note          Meyers-singleton shape; the chips keep every bit of
 *                state, so the instance carries none and the
 *                zero-construction rule holds.
 * @since         0.2.0
 * @ingroup       kernel_arch
 */
class Pic : public cinux::base::Singleton<Pic> {
    friend class cinux::base::Singleton<Pic>;

public:
    /**
     * @brief         Remaps both chips onto vectors 0x20/0x28, all lines off.
     *
     * @return        None
     * @note          After this call every device may knock, but no
     *                door is open until unmask says so. Bring-up
     *                territory, called by the composition root.
     * @since         0.1.0
     * @ingroup       kernel_arch
     */
    void remap();

    /**
     * @brief         Opens one IRQ line.
     *
     * @param[in]     line   0..7 for the master chip, 8..15 for the slave.
     * @return        None
     * @note          Slave lines also unmask the cascade line 2 on the
     *                master — a slave's knock travels through it.
     * @since         0.1.0
     * @ingroup       kernel_arch
     */
    void unmask(cinux::interrupt::IrqLine line);

    /**
     * @brief         Acknowledges the line currently being serviced.
     *
     * @param[in]     line   Device line that fired.
     * @return        None
     * @note          Slave lines thank both chips. A missing
     *                acknowledge silences the line after its first
     *                interrupt — the classic one-shot failure the tick
     *                test catches by demanding three heartbeats.
     * @since         0.1.0
     * @ingroup       kernel_arch
     */
    void ack(cinux::interrupt::IrqLine line);

private:
    Pic() = default;
};

}  // namespace cinux::arch
