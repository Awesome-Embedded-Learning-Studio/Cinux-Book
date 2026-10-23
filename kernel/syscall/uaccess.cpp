/**
 * @file    uaccess.cpp
 * @brief   The door's body: range first, then the annotated move.
 *
 * @author  Charliechen114514
 * @date    2026-10-23
 * @version 0.3.0
 * @since   0.3.0
 * @ingroup kernel_syscall
 * @copyright Copyright (c) 2026
 */

#include "kernel/syscall/uaccess.hpp"

#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall::uaccess {

namespace {

/// The door's own state, one home for the arm bit; Meyers singleton,
/// no bare globals. stac and clac fault with #UD on a processor whose
/// CPUID answer lacks SMAP, so the copies reach for the window only
/// when this bit says the machine has one.
class DoorState {
public:
    static DoorState& self() {
        static DoorState state;
        return state;
    }

    /// The boot's CPUID answer for SMAP, remembered so no copy pays
    /// for asking twice.
    bool windows_armed = false;

private:
    DoorState() = default;
};

/**
 * @brief         Runs one door-to-door byte move, annotated.
 *
 * @param[in,out] destination   Kernel-side address, as a raw word.
 * @param[in,out] source        User-side address, as a raw word.
 * @param[in,out] bytes         Width of the move; the hardware counts
 *                              it down as the move proceeds.
 * @return        True when the whole move ran; false when the page
 *                fault path bounced it through the exception table.
 * @note          The move itself is rep movsb wrapped in one asm block
 *                per arm state, each planting an exception-table entry:
 *                a fault mid-move lands on the fixup word, which shuts
 *                the window when one was open, clears the verdict, and
 *                rejoins the normal exit. The output operands reload
 *                from the very registers the hardware walked, so the
 *                caller sees exactly how far the move got — though
 *                every caller treats any bounce as a failed copy.
 * @since         0.3.0
 * @ingroup       kernel_syscall
 */
bool move_through_door(unsigned long long destination, unsigned long long source,
                       unsigned long long bytes) {
    unsigned long long verdict = 1;  // NOLINT(misc-const-correctness) written by the asm output
    if (DoorState::self().windows_armed) {
        asm volatile(
            "stac\n"
            "1: rep movsb\n"
            "2: clac\n"
            ".pushsection .ex_table,\"a\"\n"
            ".balign 16\n"
            ".quad 1b, 3f\n"
            ".popsection\n"
            "jmp 4f\n"
            "3: clac\n"
            "xorq %q0, %q0\n"
            "4:\n"
            : "+r"(verdict), "+D"(destination), "+S"(source), "+c"(bytes)
            :
            : "memory", "cc");
        return verdict != 0;
    }
    asm volatile(
        "1: rep movsb\n"
        ".pushsection .ex_table,\"a\"\n"
        ".balign 16\n"
        ".quad 1b, 2f\n"
        ".popsection\n"
        "jmp 3f\n"
        "2: xorq %q0, %q0\n"
        "3:\n"
        : "+r"(verdict), "+D"(destination), "+S"(source), "+c"(bytes)
        :
        : "memory", "cc");
    return verdict != 0;
}

}  // namespace

void ArmWindows(bool supported) {
    DoorState::self().windows_armed = supported;
}

// NOLINTNEXTLINE(misc-const-correctness) destination is written through the annotated asm copy
bool CopyFromUser(void* destination, Word source, Word bytes) {
    if (bytes == 0) {
        return true;
    }
    if (!UserRangeReadable(source, bytes)) {
        return false;
    }
    return move_through_door(reinterpret_cast<unsigned long long>(destination), source, bytes);
}

bool CopyToUser(Word destination, const void* source, Word bytes) {
    if (bytes == 0) {
        return true;
    }
    if (!UserRangeWritable(destination, bytes)) {
        return false;
    }
    return move_through_door(destination, reinterpret_cast<unsigned long long>(source), bytes);
}

}  // namespace cinux::syscall::uaccess
