/**
 * @file    ex_table.cpp
 * @brief   The boot half of the exception table.
 *
 * @author  Charliechen114514
 * @date    2026-10-23
 * @version 0.3.0
 * @since   0.3.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */

#include "kernel/arch/x86_64/ex_table.hpp"

namespace cinux::arch::ex_table {

namespace {

/// The linker's table bounds, named by the kernel script.
extern "C" unsigned char g_ex_table_start[];
extern "C" unsigned char g_ex_table_end[];

/// How many entries the linker gathered.
unsigned long linked_count() {
    const unsigned long long kSpan =
        static_cast<unsigned long long>(g_ex_table_end - g_ex_table_start);
    return static_cast<unsigned long>(kSpan / sizeof(Entry));
}

}  // namespace

void ArmTable() {
    auto* const kTable = reinterpret_cast<Entry*>(g_ex_table_start);
    SortEntries(kTable, linked_count());
}

unsigned long long FindFixupFor(unsigned long long fault_from) {
    auto* const kTable = reinterpret_cast<Entry*>(g_ex_table_start);
    return FindFixup(kTable, linked_count(), fault_from);
}

}  // namespace cinux::arch::ex_table
