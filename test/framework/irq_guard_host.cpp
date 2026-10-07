#include "kernel/arch/x86_64/irq_guard.hpp"

namespace cinux::arch {

unsigned long long SaveAndDisableIrq() {
    return 0x200ULL;
}

void RestoreIrq(unsigned long long /*snapshot*/) {}

}  // namespace cinux::arch
