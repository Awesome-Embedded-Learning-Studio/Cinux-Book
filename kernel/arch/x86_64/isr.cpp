#include "kernel/arch/x86_64/isr.hpp"

#include <utility>

#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"

namespace {

using cinux::arch::gdt::kSelectorCode;
using cinux::arch::idt::EncodeGate;
using cinux::arch::idt::InstallGate;
using cinux::arch::isr::InterruptFrame;

constexpr unsigned kExceptionCount = 32;

constexpr bool kVectorPushesErrorCode[kExceptionCount] = {
    false, false, false, false, false, false, false, false,  // 0-7
    true,  false, true,  true,  true,  true,  true,  false,  // 8-15
    false, true,  false, false, false, true,  false, false,  // 16-23
    false, false, false, false, false, true,  true,  false,  // 24-31
};

template <unsigned Vector>
__attribute__((interrupt)) void exception_no_code(InterruptFrame* frame) {
    cinux::arch::isr::ReportFault(Vector, *frame, 0);
}

template <unsigned Vector>
__attribute__((interrupt)) void exception_with_code(InterruptFrame*    frame,
                                                    unsigned long long error_code) {
    cinux::arch::isr::ReportFault(Vector, *frame, error_code);
}

void install(unsigned vector, void (*handler)(InterruptFrame*)) {
    InstallGate(vector, EncodeGate(reinterpret_cast<unsigned long long>(handler), kSelectorCode, 0,
                                   cinux::arch::idt::kTypeInterruptGate));
}

void install(unsigned vector, void (*handler)(InterruptFrame*, unsigned long long)) {
    InstallGate(vector, EncodeGate(reinterpret_cast<unsigned long long>(handler), kSelectorCode, 0,
                                   cinux::arch::idt::kTypeInterruptGate));
}

template <unsigned Vector>
void install_one() {
    if constexpr (kVectorPushesErrorCode[Vector]) {
        install(Vector, &exception_with_code<Vector>);
    } else {
        install(Vector, &exception_no_code<Vector>);
    }
}

template <unsigned... Vectors>
void install_all([[maybe_unused]] std::integer_sequence<unsigned, Vectors...> sequence) {
    (install_one<Vectors>(), ...);
}

}  // namespace

namespace cinux::arch::isr {

void InstallExceptionStubs() {
    install_all(std::make_integer_sequence<unsigned, kExceptionCount>{});
}

}  // namespace cinux::arch::isr
