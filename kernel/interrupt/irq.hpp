/**
 * @file    irq.hpp
 * @brief   The interrupt service: one table, every device line through it.
 *
 * The neutral face of interrupt delivery, Meyers-singleton shape after
 * Pmm, Tick and Pit. The chip behind it arrives by injection, the
 * TickSource pattern one subsystem over: any type satisfying IrqChip is
 * type-erased at init, so this file names no chip and a port swaps the
 * backend without touching a consumer. Device drivers seat a handler
 * and open their line here; acknowledge is fully internal — dispatch
 * runs the handler, then thanks the chip, and no driver or stub ever
 * sends one. Lines travel as the IrqLine vocabulary, never as a
 * unit-less number.
 *
 * @author  Charliechen114514
 * @date    2026-10-02
 * @version 0.4
 * @since   0.1.0
 * @ingroup kernel_interrupt
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <concepts>

#include "cinux/singleton.hpp"
#include "kernel/interrupt/irq_config.hpp"

namespace cinux::interrupt {

/**
 * @brief         A device interrupt line, as its own vocabulary.
 * @note          The number's meaning lives in the type: on this
 *                platform a line 0..15 on the chip pair, on another an
 *                interrupt id — callers never hand a unit-less number
 *                across this face.
 * @since         0.3.0
 * @ingroup       kernel_interrupt
 */
struct IrqLine {
    unsigned int value;  ///< Line number, 0 on the first chip upward.
};

/// What a device line hands the service: a plain entry, no frame.
using IrqHandler = void (*)();

/**
 * @brief         What an interrupt chip must offer the service.
 * @tparam        Chip   The device type being checked.
 * @note          Open a line, acknowledge a line — nothing else. The
 *                chip's bring-up (remap and friends) stays between the
 *                chip and the composition root.
 * @since         0.4.0
 * @ingroup       kernel_interrupt
 */
template <typename Chip>
concept IrqChip = requires(Chip& chip, IrqLine line) {
    { chip.unmask(line) } -> std::same_as<void>;
    { chip.ack(line) } -> std::same_as<void>;
};

/**
 * @brief         The interrupt service: seats lines, runs handlers.
 * @note          The dispatch table is member state sized by
 *                kIrqLineCount, constant-initialized, so the
 *                zero-construction rule holds.
 * @since         0.1.0
 * @ingroup       kernel_interrupt
 */
class Irq : public cinux::base::Singleton<Irq> {
    friend class cinux::base::Singleton<Irq>;

public:
    /**
     * @brief         Injects the interrupt chip backing the service.
     *
     * @param[in]     chip   Device to drive, any IrqChip type.
     * @return        None
     * @note          Type erasure after the Tick init pattern: two
     *                captureless lambdas become the stored unmask and
     *                acknowledge thunks, the chip pointer rides along
     *                as their context. Called once during interrupt
     *                bring-up, from the arch layer that knows the chip.
     * @since         0.4.0
     * @ingroup       kernel_interrupt
     */
    template <IrqChip Chip>
    void init(Chip& chip) {
        unmask_ = [](void* chip_ptr, IrqLine line) { static_cast<Chip*>(chip_ptr)->unmask(line); };
        ack_    = [](void* chip_ptr, IrqLine line) { static_cast<Chip*>(chip_ptr)->ack(line); };
        chip_   = &chip;
    }

    /**
     * @brief         Seats one handler on a device line.
     *
     * @param[in]     line     The line the handler takes.
     * @param[in]     handler   Entry to call when the line fires; nullptr
     *                          clears the seat.
     * @return        None
     * @note          Seating does not open the line — enable_line does.
     *                Seat and gate move independently, so neither can
     *                fire before both are set.
     * @since         0.1.0
     * @ingroup       kernel_interrupt
     */
    void register_handler(IrqLine line, IrqHandler handler);

    /**
     * @brief         Opens one device line at the injected chip.
     *
     * @param[in]     line   Device line to unmask.
     * @return        None
     * @since         0.1.0
     * @ingroup       kernel_interrupt
     */
    void enable_line(IrqLine line);

    /**
     * @brief         Runs the seated handler, then thanks the chip.
     *
     * @param[in]     line   Device line that fired.
     * @return        None
     * @note          Called by the arch stub layer; acknowledge happens
     *                here, inside the service, so no driver or stub
     *                ever sends one. An unseated line is still
     *                acknowledged and dropped, which is today's
     *                spurious-and-cascade behavior kept.
     * @since         0.1.0
     * @ingroup       kernel_interrupt
     */
    void dispatch(IrqLine line);

private:
    Irq() = default;

    IrqHandler seats_[kIrqLineCount] = {};

    void (*unmask_)(void*, IrqLine) = nullptr;
    void (*ack_)(void*, IrqLine)    = nullptr;
    void* chip_                     = nullptr;
};

}  // namespace cinux::interrupt
