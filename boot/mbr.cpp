#include "early/boot_console.hpp"
#include "layout.hpp"
#include "mbr/disk_address_packet.hpp"


namespace {
cinux::boot::Dap g_dap = {.size     = 16,
                          .reserved = 0,
                          .count    = cinux::boot::kStage2Spot.sectors,
                          .offset   = cinux::boot::kStage2Spot.offset,
                          .segment  = cinux::boot::kStage2Spot.segments,
                          .lba      = 1};
}
/* Seems wired, right? */
extern "C" unsigned char g_boot_drive;      // Linkers dont know the g_boot_drive, export it
unsigned char            g_boot_drive = 0;  // and then we sadly init it

/**
 * @brief This is our OS bootstrap, which, we are
 *
 */
asm(".section .text.boot, \"ax\"\n"
    ".global mbr_start\n"
    "mbr_start:\n"
    "   ljmp $0x0000, $mbr_real_start\n"  // Clean and force boot at concreate place
    "mbr_real_start:\n"
    "   xor %ax, %ax\n"  // clean registers
    "   mov %ax, %ds\n"
    "   mov %ax, %es\n"
    "   mov %ax, %ss\n"
    "   movw $0x7c00, %sp\n"       // Set the stack
    "   movb %dl, g_boot_drive\n"  // import the cld to load the second stage
    "   cld\n"
    "   call MbrMain\n"
    "0: hlt\n"
    "   jmp 0b\n"  // if we die, jump back, machine hlts
    ".text\n");

namespace {
void boot_failed(unsigned long status) {
    using namespace cinux::boot::serial;
    char const* hex = "0123456789ABCDEF";
    PutString("Error: ");
    PutChar(hex[(status >> 4) & 0xF]);
    PutChar(hex[status & 0xF]);
    PutChar('\n');
    for (;;) {
        asm volatile("hlt");
    }
}

void returned_from_stage2() {
    cinux::boot::serial::PutString("unexpected: returned from stage2\n");
    for (;;) {
        asm volatile("hlt");
    }
}
}  // namespace

extern "C" [[noreturn]] void MbrMain() {
    unsigned short       reg_ax = 0x4200;  // NOLINT(misc-const-correctness)
    const unsigned short kDrive = g_boot_drive;

    cinux::boot::serial::PutString("Ready to call bios\n");
    // Call Bios
    asm volatile("int $0x13" : "+a"(reg_ax) : "S"(&g_dap), "d"(kDrive) : "memory");
    if ((reg_ax >> 8) != 0) {
        boot_failed(reg_ax >> 8);
    }

    cinux::boot::serial::PutString("Jump to Stage 2\n");

    asm volatile("ljmp %0, %1"
                 :
                 : "i"(cinux::boot::kStage2Spot.segments), "i"(cinux::boot::kStage2Spot.offset)
                 : "memory");
    // And if, we failed, runs into the unreachable
    returned_from_stage2();

    __builtin_unreachable();
}
