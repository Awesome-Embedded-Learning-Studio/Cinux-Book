/**
 * @file exec_arguments.hpp
 * @brief Linux argument-size accounting before an executable is committed.
 * @author Charliechen114514
 * @date 2026-10-25
 * @version 0.1
 * @since 0.8.0
 * @ingroup kernel_loader
 * @copyright Copyright (c) 2026
 */
#pragma once
#include "cinux/result.hpp"
#include "kernel/arch/x86_64/page.hpp"
namespace cinux::loader {
/** @brief Linux MAX_ARG_STRLEN, including the terminating NUL. */
inline constexpr unsigned long kMaxArgumentStringBytes = 32 * arch::page::kSize;
/** @brief Linux MAX_ARG_STRINGS for each pointer vector. */
inline constexpr unsigned long kMaxArgumentStrings     = 0x7FFF'FFFF;
/**
 * @brief Computes Linux bprm_stack_limits after reserving argv/envp pointers.
 * @param[in] stack_limit The calling task's soft RLIMIT_STACK.
 * @param[in] argc Number of supplied argument pointers.
 * @param[in] envc Number of supplied environment pointers.
 * @return The string budget, or kInvalidArgument when pointer space exhausts it.
 * @since 0.8.0
 * @ingroup kernel_loader
 */
base::Result<unsigned long> ArgumentStringBudget(unsigned long long stack_limit, unsigned long argc,
                                                 unsigned long envc);
}
