#include "kernel/arch/x86_64/idt.hpp"

namespace cinux::arch::idt {

namespace {

constexpr unsigned int kVectorCount = 256;

GateEntry g_table[kVectorCount];

}  // namespace

void InstallGate(unsigned int vector, GateEntry entry) {
    g_table[vector] = entry;
}

void LoadIdt() {
    TablePointer const kPointer = {.limit = sizeof(g_table) - 1,
                                   .base  = reinterpret_cast<unsigned long long>(&g_table)};
    asm volatile("lidtq %0" : : "m"(kPointer) : "memory");
}

}  // namespace cinux::arch::idt
