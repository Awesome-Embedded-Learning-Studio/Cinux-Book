#include "kernel/arch/x86_64/per_cpu.hpp"

#include "cinux/assert.hpp"
#include "cinux/ptr.hpp"
#include "kernel/arch/x86_64/instructions.hpp"
#include "kernel/arch/x86_64/msr.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/per_cpu_config.hpp"

namespace cinux::arch::per_cpu {

namespace {
[[gnu::section(".data")]] alignas(page::kSize) constinit PerCpu g_percpu_blocks[kMaxCpus] = {};
}

PerCpu& PerCpu::self() {
    return *base::PtrAt<PerCpu>(ReadMsr(msr::kGsBase));
}

void InstallKernelGs(unsigned int cpu_id) {
    base::safety::Check(cpu_id < kMaxCpus, "CPU index exceeds static per-CPU storage");
    auto& block         = g_percpu_blocks[cpu_id];
    block.cpu_id        = cpu_id;
    auto const kAddress = reinterpret_cast<unsigned long long>(&block);
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
