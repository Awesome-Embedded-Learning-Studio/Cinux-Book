/**
 * @file kernel/arch/x86_64/extable.hpp
 * @brief RIP-based exception table for user-memory accessors (Linux uaccess)
 *
 * Lets a user accessor (copy_to_user / copy_from_user) recover from a mid-copy
 * #PF by resuming at a per-instruction fixup that returns failure, instead of
 * relying on the demand-page contract or panicking. Each annotated load/store
 * emits an ExceptionTableEntry (fault_rip, fixup_rip) into the __ex_table
 * section via _ASM_EXTABLE. handle_pf calls search_exception_tables(frame->rip);
 * on a hit it writes fixup_rip into frame->rip and returns, so the accessor's
 * inline-asm fixup path runs (clac + ok = false -> caller returns -EFAULT).
 *
 * The table is sorted once at boot (sort_extable) so search can binary-search.
 *
 * Scope fence: handle_pf only consults the table for kernel-mode faults
 * (cs & 3 == 0); user-mode demand-paging is untouched -- that is the F2
 * lazy-allocation contract and is left to a separate milestone. See the
 * F-EXTABLE design in document/ai.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cinux::arch {

/// One entry in the exception table. fault_rip is the address of the annotated
/// accessor instruction (a rep movsb or single load/store); fixup_rip is where
/// to resume instead of panicking. 16 bytes (two quads); the section is
/// 8-byte aligned and sorted by fault_rip at boot.
struct ExceptionTableEntry {
    uint64_t fault_rip;
    uint64_t fixup_rip;
};
static_assert(sizeof(ExceptionTableEntry) == 16, "exception table entry is two quads");

/// Binary-search a [begin, end) exception-table range for fault_rip == rip.
/// Range must be sorted ascending by fault_rip (see extable_sort). Pure and
/// host-testable; the kernel wrapper adds the linker-symbol bounds.
inline const ExceptionTableEntry* extable_search(const ExceptionTableEntry* begin,
                                                 const ExceptionTableEntry* end, uint64_t rip) {
    while (begin < end) {
        const ExceptionTableEntry* mid = begin + (end - begin) / 2;
        if (mid->fault_rip == rip) {
            return mid;
        }
        if (mid->fault_rip < rip) {
            begin = mid + 1;
        } else {
            end = mid;
        }
    }
    return nullptr;
}

/// Sort a [begin, end) exception-table range by fault_rip so extable_search can
/// binary-search. Insertion sort: the table has only a few dozen entries and a
/// freestanding kernel has no qsort. Pure and host-testable.
inline void extable_sort(ExceptionTableEntry* begin, ExceptionTableEntry* end) {
    for (ExceptionTableEntry* i = begin + 1; i < end; ++i) {
        ExceptionTableEntry  key = *i;
        ExceptionTableEntry* j   = i;
        while (j > begin && (j - 1)->fault_rip > key.fault_rip) {
            *j = *(j - 1);
            --j;
        }
        *j = key;
    }
}

}  // namespace cinux::arch

// Linker-script bounds for the __ex_table section. Mutable because sort_extable
// reorders entries in place at boot (the section lives in writable .data space;
// CinuxOS does not post-init read-only the data segment). extern "C" keeps the
// symbol names identical to the linker-script labels (no C++ mangling).
extern "C" cinux::arch::ExceptionTableEntry __start___ex_table[];
extern "C" cinux::arch::ExceptionTableEntry __stop___ex_table[];

namespace cinux::arch {

/// Kernel wrapper: search the linker-symbol exception table for the accessor
/// instruction that faulted. Called from handle_pf for kernel-mode faults.
inline const ExceptionTableEntry* search_exception_tables(uint64_t rip) {
    return extable_search(__start___ex_table, __stop___ex_table, rip);
}

/// Kernel wrapper: sort the linker-symbol exception table by fault_rip. Called
/// once at boot after the IDT is up and before interrupts are enabled.
inline void sort_extable() {
    extable_sort(__start___ex_table, __stop___ex_table);
}

}  // namespace cinux::arch

/// _ASM_EXTABLE(fault_lbl, fixup_lbl): emit one exception-table entry inside an
/// inline-asm block. Records that a fault at instruction `fault_lbl` (typically
/// `1b`, a rep movsb or single load) should resume at `fixup_lbl` (`2b`, the
/// clac + return-failure path). Numeric labels keep each accessor self-contained
/// without inventing global symbols. The two .quad land in __ex_table.
#define _ASM_EXTABLE(fault_lbl, fixup_lbl)                                                         \
    ".pushsection __ex_table,\"a\"\n"                                                              \
    ".balign 8\n"                                                                                  \
    ".quad " #fault_lbl                                                                            \
    "\n"                                                                                           \
    ".quad " #fixup_lbl                                                                            \
    "\n"                                                                                           \
    ".popsection\n"
