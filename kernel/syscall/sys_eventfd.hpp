/** @file sys_eventfd.hpp
 * @brief Event counters used to wake native process event loops.
 */
#pragma once
#include "kernel/syscall/syscall.hpp"
namespace cinux::fs {
struct OpenFile;
}
namespace cinux::syscall {
/**
 * @brief Create an event counter with descriptor and semaphore flags.
 * @return The result or a negative Linux errno.
 */
long long HandleEventfd2(const SyscallFrame& frame);
/**
 * @brief Answer readiness when the open belongs to an event counter.
 * @return The result or a negative Linux errno.
 */
bool      PollEventCounter(const fs::OpenFile* file, unsigned int events, unsigned short& ready);
}
