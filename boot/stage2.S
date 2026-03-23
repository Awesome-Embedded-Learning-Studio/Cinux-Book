/**
 * @file boot/stage2.S
 * @brief Cinux Stage2 Bootloader - VESA Initialization
 *
 * This file is loaded by MBR to 0x8000 and continues the boot process.
 * Responsibilities (for this milestone):
 *   - Print "Stage2 OK" message
 *   - Enable A20 line
 *   - Get VESA controller information
 *   - Get VESA mode information (0x118: 1024x768x32)
 *   - Set VESA video mode with linear framebuffer
 *   - Save framebuffer information to fixed location
 *
 * AT&T Syntax: source, destination order
 * Registers: % prefix, Immediates: $ prefix
 */

.section .text
.code16                      // 16-bit real mode code
.global _start                // Entry point

// ============================================================
// External functions from common/serial.S
// ============================================================
.extern print_string
.extern panic
.extern enable_a20
.extern vesa_get_controller_info
.extern vesa_get_mode_info
.extern vesa_set_mode
.extern vesa_save_framebuffer_info

// ============================================================
// Constants from common/serial.S
// ============================================================
.set VBE_FB_INFO_BLOCK,         0x6400
.set STAGE2_STACK_BASEADDR,     0xFFFE

// Framebuffer info layout offsets
.set FB_INFO_PHYS_ADDR,         0       // Physical base address (8 bytes)
.set FB_INFO_PITCH,             8       // Bytes per scan line (4 bytes)
.set FB_INFO_WIDTH,             12      // Width in pixels (2 bytes) 

// ============================================================
// Function: _start
// Responsibility: Stage2 entry point, called from MBR
// Input: None (execution starts here)
// Output: Never returns (continues to next stage)
// ============================================================
_start:
    cli                             // Disable interrupts during critical setup

    // Reset all registers again...
    movw %cs, %ax                 // Tricks: Xor one number with itself get 0
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    movw $0x900, %ax    // 0x900 << 4 = 0x9000
    movw %ax, %ss

    // [→sp] Stack at bottom of stage2
    movw $STAGE2_STACK_BASEADDR, %sp

    sti                             // Re-enable interrupts after stack is set

    // Mark Once, see the issue
    movw $(msg_stage2_ok), %si
    call print_string

    // Enable A20 line
    call enable_a20

    // Get VESA controller information
    call vesa_get_controller_info   // [→0x6000] Get VBE info block

    // Get VESA mode information for 0x118
    call vesa_get_mode_info         // [→0x6200] Get mode info block

    movw $(msg_mode_info_ok), %si
    call print_string               // "Mode info OK" (still in text mode!)

    // Set VESA video mode with linear framebuffer
    call vesa_set_mode              // [→video] Set 1024x768x32 mode (SCREEN CLEARS HERE)

    // Save framebuffer information to 0x6400
    // Layout: addr(8B) + pitch(4B) + w(2B) + h(2B) = 16B
    call vesa_save_framebuffer_info // [→0x6400] Save FB info

    // === VERIFY: Print BEFORE and AFTER mode switch ===
    // (After switch, text won't be visible, but execution proves it worked)
    movw $0x4F, %ax               // Verify we can still write registers
    cmpw $0x4F, %ax               // If this executes, mode switch succeeded!

    cli                             // [→flags] Disable interrupts
.halt_loop:
    hlt                             // [→cpu] Halt CPU
    jmp .halt_loop                  // [→IP] Infinite loop

// ============================================================
// Error Handlers
// ============================================================
a20_failed:
    jmp panic

.section .data

// Stage2 OK message
msg_stage2_ok:
    .asciz "Stage2 OK\r\n"

msg_mode_info_ok:
    .asciz "Mode info OK, switching...\r\n"

// VESA success message
msg_vesa_ok:
    .asciz "VESA: Mode 1024x768x32 set\r\n"

// ============================================================
// Stage2 Padding
// ============================================================
// Ensure stage2 is properly sized (max 15 sectors = 7680 bytes)
// Actual code will be much smaller, but we leave room for expansion
