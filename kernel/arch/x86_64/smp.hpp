/**
 * @file smp.hpp
 * @brief Secondary processor startup and mechanism readback.
 * @author Charliechen114514
 * @date 2026-10-20
 * @version 0.1
 * @since 0.3.0
 * @ingroup kernel_arch
 * @copyright Copyright (c) 2026
 */
#pragma once

namespace cinux::arch::smp {

/**
 * @brief Hardware mechanism snapshot captured on its owning processor.
 * @since 0.3.0
 * @ingroup kernel_arch
 */
struct CpuMechanisms {
    unsigned long long cr4;
    unsigned long long efer;
    unsigned long long lstar;
    unsigned long long star;
    unsigned long long sfmask;
    unsigned long long gdt_base;
    unsigned long long tss_base;
    unsigned long long owned_tss;
    unsigned int       task_register;
    unsigned int       timer_interrupts;
    unsigned int       cpu_id;
    unsigned int       apic_id;
    unsigned int       magic;
};

/**
 * @brief Boot firmware processors and wait for their mechanism readback.
 * @return True when all discovered secondary processors reported online.
 * @since 0.3.0
 * @ingroup kernel_arch
 */
[[nodiscard]] bool BootSecondaryCpus();

/**
 * @brief Count processors that completed initialization.
 * @return Online count including the BSP.
 * @since 0.3.0
 * @ingroup kernel_arch
 */
[[nodiscard]] unsigned int OnlineCpuCount();

/**
 * @brief Read the published mechanism snapshot for one online CPU.
 * @param[in] cpu Logical CPU index.
 * @return Its readback, captured before online publication.
 * @since 0.3.0
 * @ingroup kernel_arch
 */
[[nodiscard]] const CpuMechanisms& Mechanisms(unsigned int cpu);

/**
 * @brief Wake other online processors after runnable work is published.
 * @since 0.3.0
 * @ingroup kernel_arch
 */
void NotifyRunnable();

/**
 * @brief Publish the initialized scheduler to secondary idle loops.
 * @since 0.3.0
 * @ingroup kernel_arch
 */
void EnableSecondaryDispatch();

}
