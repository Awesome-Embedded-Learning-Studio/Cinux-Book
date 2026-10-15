/**
 * @file    vma_diag.hpp
 * @brief   The page-fault verdict spoken from the register's point of view.
 *
 * When the fault resolver declines a page fault, the machine is about to
 * report a segmentation fault; the one question the register can still
 * answer is whether the process had been promised that byte. This face
 * prints exactly that: which range covers the faulting address, or the
 * admission that nothing does. It is a reporting face only — no verdict
 * here changes control flow — and lives apart from vma.cpp so the
 * register itself stays free of console dependencies and host-linkable.
 *
 * @author  Charliechen114514
 * @date    2026-10-14
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_mm
 * @copyright Copyright (c) 2026
 */

#pragma once

#include "kernel/mm/vma.hpp"

namespace cinux::mm {

/**
 * @brief         Prints what the register knows about one faulting address.
 *
 * @param[in]     store        The faulting task's register; null means the
 *                             task keeps none (a kernel-only world).
 * @param[in]     address      The faulting virtual address.
 * @param[in]     error_code   The page-fault error word, read for the
 *                             access type.
 * @return        None.
 * @note          A hit is not innocence and a miss is not automatically
 *                guilt at this station: the resolver alone decides, and
 *                this line is the evidence it leaves behind. One range
 *                keeps its terms in plain sight so the access type can be
 *                compared against the promise without re-walking tables.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_mm
 */
void PrintFaultVerdict(const VmaStore* store, unsigned long address, unsigned long long error_code);

}  // namespace cinux::mm
