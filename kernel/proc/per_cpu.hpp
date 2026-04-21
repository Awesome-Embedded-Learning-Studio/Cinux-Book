#pragma once

#include <stdint.h>

namespace cinux::proc {

struct Task;

struct PerCPU {
    Task* current;
    uint64_t kernel_stack;
};

extern PerCPU g_per_cpu;

}  // namespace cinux::proc
