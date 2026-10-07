#include "kernel/arch/x86_64/per_cpu.hpp"

#include "kernel/arch/x86_64/instructions.hpp"
#include "kernel/arch/x86_64/msr.hpp"

namespace cinux::arch::per_cpu {

void InstallKernelGs() {
    auto const kAddress = reinterpret_cast<unsigned long long>(&PerCpu::self());
    WriteMsr(msr::kGsBase, kAddress);
    WriteMsr(msr::kKernelGsBase, kAddress);
}

unsigned long long ReadGsBase() {
    return ReadMsr(msr::kGsBase);
}

unsigned long long ReadKernelGsBase() {
    return ReadMsr(msr::kKernelGsBase);
}

}  // namespace cinux::arch::per_cpu
