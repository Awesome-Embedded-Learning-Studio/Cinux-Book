/**
 * @file boot/mbr.S
 * @brief Cinux MBR (Master Boot Record) - Real Mode Bootloader
 *
 * This is the first code executed by BIOS after loading the MBR to 0x7C00.
 * Responsibilities:
 *   - Initialize segment registers
 *   - Save boot drive number
 *   - Print boot message via BIOS INT 0x10
 *   - Load Stage2 from disk using BIOS INT 0x13 AH=0x42 (extended read)
 *   - Jump to Stage2
 *
 * AT&T Syntax: source, destination order
 * Registers: % prefix, Immediates: $ prefix
 */

// ============================================================
// Constants and Memory Layout
// ============================================================
.set STAGE2_LBA,          1       // LBA of stage2 (right after MBR)
.set STAGE2_SECTORS,      15      // Maximum sectors for stage2 (7.5KB)
.set STAGE2_LOAD_ADDR,    0x8000  // Physical address to load stage2 (0x0000:0x8000)

.set STACK_BASE_ADDR,     0x7000  // Stack Base Address

.set DAP_STORE_ADDR,      0x7B00  // DAP Structure storage
.set DAP_SIZE,            0x10

.section .text
.code16                      // Generate 16-bit code
.global _start                // Make _start visible to linker

.extern print_string_mbr

// ============================================================
// Function: _start
// Responsibility: MBR entry point, BIOS loads to 0x7C00
// Input: CS:IP = 0x0000:0x7C00 (BIOS loads MBR here)
//        %dl = boot drive number (saved by BIOS)
// Output: Never returns (jumps to stage2)
// ============================================================
_start:
    // Types Of BIOS might set variants of address, lets take the
    // standard actions
    
    // Boot From Real Mode
    ljmp $0, $real_start    // Jumps to the code 16bits start

// ============================================================
// Label: real_start
// Responsibility: Main MBR code after segment normalization
// ============================================================
real_start:
    cli                             // Disable interrupts during setup

    // Clean the Register Actions
    xorw %ax, %ax                 // Tricks: Xor one number with itself get 0
    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    cld

    // Setup the stacks, for the convs op
    movw $STACK_BASE_ADDR, %sp

    sti                             // Re-enable interrupts after stack is set

    // save the boot drives, BIOS Puts the driver num here
    movb %dl, boot_drive

    // load the disk into memory for stage 2 booting
    call load_stage2

    // call a string printer for debugging session
    movw $msg_booting, %si
    call print_string_mbr
    ljmp $STAGE2_LOAD_ADDR >> 4, $0

// Should not be back, if so, shut it off.
die:
    cli
    hlt
    jmp die

print_string_mbr:
    cld
._loop:
    lodsb
    test %al, %al
    jz ._done
    mov $0x0E, %ah
    int $0x10
    jmp ._loop
._done:
    ret

// ============================================================
// Function: load_stage2
// Responsibility: Load Stage2 from disk using BIOS INT 0x13 AH=0x42
// Input: None (uses STAGE2_LBA, STAGE2_SECTORS constants)
// Output: None (stage2 loaded to STAGE2_LOAD_ADDR)
// Modifies: %ax, %si, %dl
// ============================================================
load_stage2:
// ============================================================
// DAP (Disk Address Packet) Template
// ============================================================
// This is used as reference, actual DAP is built on stack
// .set DAP_SIZE,       0x10    // Size of DAP
// .set DAP_SECTORS,    0x02    // Offset to sector count
// .set DAP_OFFSET,     0x04    // Offset to buffer offset
// .set DAP_SEGMENT,    0x06    // Offset to buffer segment
// .set DAP_LBA,        0x08    // Offset to LBA (64-bit)
    movw $DAP_STORE_ADDR, %si

    movb $DAP_SIZE, (%si)           // DAP.size      = DAP_SIZE
    movw $STAGE2_SECTORS, 2(%si)    // DAP.sectors   = STAGE2_SECTORS
    movw $STAGE2_LOAD_ADDR, 4(%si)  // DAP.offset    = STAGE2_LOAD_ADDR & 0xFFFF
    movw $0, 6(%si)                 // DAP.segment   = 0

    movl $STAGE2_LBA, 8(%si)        // DAP.lba.low32 = STAGE2_LBA
    movl $0, 12(%si)                // DAP.lba.hig32 = 0
    movb boot_drive, %dl            // Restore back from real mode storing

    // Call BIOS
    movw $0x4200, %ax               // [→ax] AH=0x42, AL=0 (extended read)
    int $0x13                       // [→disk] Call BIOS disk interrupt

    jc disk_error                   // if carry sets, error sucks

    // else, we should returns
    ret                             // [stack→IP] Return to caller

disk_error:
    // before call panic, print the string first
    movw $(msg_disk_error), %si     // [→si] Load error message
    call print_string_mbr
    jmp die                         // [→never] Call panic (never returns)

// ============================================================
// Data Section (embedded in text for MBR layout)
// ============================================================

msg_booting:
    .asciz "Cinux Booting...\r\n"

msg_disk_error:
    .asciz "Disk: Failed to load stage2!\r\n"

boot_drive:
    .byte 0

// Pad to 510 bytes, then MBR signature
.org 510
.word 0xAA55
