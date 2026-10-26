#include "kernel/loader/exec_arguments.hpp"

#include "cinux/math.hpp"
#include "cinux/result.hpp"
#include "kernel/proc/proc_config.hpp"

namespace cinux::loader {
base::Result<unsigned long> ArgumentStringBudget(unsigned long long stack_limit, unsigned long argc,
                                                 unsigned long envc) {
    if (argc > kMaxArgumentStrings || envc > kMaxArgumentStrings) {
        return base::KernelError::kInvalidArgument;
    }
    const auto kLimit =
        base::math::Max(base::math::Min(proc::kDefaultStackCeiling / 4 * 3, stack_limit / 4),
                        static_cast<unsigned long long>(kMaxArgumentStringBytes));
    const auto kPointers = (base::math::Max(argc, 1UL) + envc) * sizeof(unsigned long);
    if (kLimit <= kPointers) {
        return base::KernelError::kInvalidArgument;
    }
    return static_cast<unsigned long>(kLimit - kPointers);
}
}
