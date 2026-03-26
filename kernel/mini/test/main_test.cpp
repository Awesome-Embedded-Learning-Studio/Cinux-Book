/* ==============================================================
 * Cinux Mini Kernel - Test Entry Point
 * ============================================================== */

extern "C" {
#include <stdint.h>
}

#include "../../boot/boot_info.h"
#include "../lib/kprintf.h"

using cinux::mini::lib::kprintf;
using cinux::mini::lib::kdebugf;

extern "C" {
extern uint64_t __boot_info_ptr;
void run_cpp_tests();  // C++ runtime tests from test_cpp_basic.cpp
}

extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
    (void)boot_info_addr;
    (void)boot_info;

    // ============================================================
    // kprintf/kdebugf Tests
    // ============================================================
    kprintf("=== kprintf Test ===\n");
    kprintf("String: %s\n", "Hello, Cinux!");
    kprintf("Char: %c\n", 'X');
    kprintf("Decimal: %d\n", -12345);
    kprintf("Unsigned: %u\n", 42);
    kprintf("Hex lower: %x\n", 0xDEADBEEF);
    kprintf("Hex upper: %X\n", 0xDEADBEEF);
    kprintf("Pointer: %p\n", 0xFFFFFFFF80000000ULL);
    kprintf("Binary: %b\n", 0b101010);
    kprintf("Width test: [%4d]\n", 7);
    kprintf("Zero pad: [%04d]\n", 7);
    kprintf("Null string: %s\n", nullptr);
    kprintf("Percent: %%\n");

    kdebugf("=== kdebugf Test ===\n");
    kdebugf("Value: %d, Hex: %x\n", -42, 0xDEADBEEF);
    kdebugf("String: %s, Pointer: %p\n", "Debug", 0xFFFFFFFF80000000ULL);

    // ============================================================
    // C++ Runtime Tests
    // ============================================================
    run_cpp_tests();

    // ============================================================
    // Test Complete - Shutdown
    // ============================================================
    kprintf("\n=== All tests completed ===\n");

    // 使用 QEMU 的 isa-debug-exit 设备安全退出
    // 向端口 0xf4 写入双字，高字节是退出码
    // 0x0 << 24 | 0x0 << 16 | 0x0 << 8 | 0x0 = 退出码 0
    __asm__ volatile("outl %0, $0xf4" : : "a"(0));

    // 如果 QEMU 没有退出（比如在没有 isa-debug-exit 的环境下），则停机
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
