#include "kernel/boot/console.hpp"
#include "kernel/boot/print.hpp"

using cinux::print::Println;
using namespace cinux::console;

extern "C" void ReportException(unsigned long long vector, unsigned long long rip) {
    Println("[kern] exc #%u @ rip=%X", vector, rip);
    cinux::console::Halt();
}
