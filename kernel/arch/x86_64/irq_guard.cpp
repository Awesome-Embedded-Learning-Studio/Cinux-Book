#include "kernel/arch/x86_64/irq_guard.hpp"

namespace cinux::arch {

namespace {

constexpr unsigned long long kIfBit = 0x200ULL;

}  // namespace

unsigned long long SaveAndDisableIrq() {
    unsigned long long snapshot = 0;  // NOLINT(misc-const-correctness) written by the asm output
    __asm__ volatile("pushfq\n\tpopq %0\n\tcli" : "=r"(snapshot) : : "memory");
    return snapshot;
}

void RestoreIrq(unsigned long long snapshot) {
    if ((snapshot & kIfBit) != 0) {
        __asm__ volatile("sti" : : : "memory");
    }
}

void EnableIrqAndHalt() {
    __asm__ volatile("sti; hlt" : : : "memory");
}

}  // namespace cinux::arch
