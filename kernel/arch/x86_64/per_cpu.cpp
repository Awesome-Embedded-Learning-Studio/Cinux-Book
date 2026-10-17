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

void StoreTaskAddressRegs(unsigned long long& fs_base, unsigned long long& gs_base) {
    fs_base = ReadMsr(msr::kFsBase);
    gs_base = ReadMsr(msr::kKernelGsBase);
}

void LoadTaskAddressRegs(unsigned long long fs_base, unsigned long long gs_base) {
    WriteMsr(msr::kFsBase, fs_base);
    WriteMsr(msr::kKernelGsBase, gs_base);
}

void WriteFsBase(unsigned long long base) {
    WriteMsr(msr::kFsBase, base);
}

void WriteKernelGsBase(unsigned long long base) {
    WriteMsr(msr::kKernelGsBase, base);
}

}  // namespace cinux::arch::per_cpu
