/**
 * @file wait_time.hpp
 * @brief Validated timeout records and deadlines shared by blocking syscalls.
 * @ingroup kernel_syscall
 */
#pragma once
namespace cinux::syscall {
/// Linux x86-64 timespec used by clock and wait syscalls.
struct Timespec {
    long long seconds     = 0;
    long long nanoseconds = 0;
};
static_assert(sizeof(Timespec) == 16);

/**
 * @brief Read and validate a user timespec without changing caller state on failure.
 * @param[in] address User record address.
 * @param[out] record Validated seconds and subsecond field.
 * @return Zero, or a negative errno.
 */
long long ReadTimespec(unsigned long long address, Timespec& record);

/**
 * @brief Decode an optional relative timeout into a saturated monotonic deadline.
 * @param[in] address User record address; zero means no deadline.
 * @param[out] deadline Monotonic expiry or the maximum word for an unbounded wait.
 * @return Zero, or a negative errno.
 */
long long ReadWaitDeadline(unsigned long long address, unsigned long long& deadline);

/**
 * @brief Decode poll's signed millisecond timeout.
 * @param[in] milliseconds Negative means an unbounded wait.
 * @return Saturated monotonic deadline.
 */
unsigned long long MillisecondDeadline(int milliseconds);

}
