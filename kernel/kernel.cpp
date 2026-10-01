#include "kernel/boot/boot_info.hpp"
#include "kernel/boot/console.hpp"
#include "kernel/boot/print.hpp"

namespace kernel {

using namespace cinux::console;
using cinux::print::Println;

// NOLINTNEXTLINE(misc-use-internal-linkage)
void Main(const cinux::boot::BootInfo& info) {
    Println("[kern] 64-bit C++ world alive");
    Println("[kern] bootinfo: %u e820 entries, fb %u*%u*%u", info.e820_count,
            info.framebuffer.width, info.framebuffer.height, info.framebuffer.bpp);
    Println("[kern] kernel at %X size %X entry %X", info.kernel_paddr, info.kernel_mem_size,
            info.kernel_entry);
    cinux::console::Halt();
}

}  // namespace kernel
