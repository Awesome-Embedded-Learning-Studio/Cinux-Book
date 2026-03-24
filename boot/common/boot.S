/**
 * @file boot/common/boot.S
 * @brief Real mode memory enumeration and kernel loading functions
 *
 * This file provides two critical operations that must complete before
 * entering protected mode:
 *   1. E820 memory enumeration: Query system memory layout, save to 0x5000 buffer
 *   2. Disk read kernel: Read kernel ELF from LBA=16 to 0x10000
 *
 * Call timing: In stage2.S, after VESA and before entering protected mode
 *
 * AT&T syntax: operand order is "source, destination"
 * Registers: % prefix, immediates: $ prefix, memory: offset(base, index, scale)
 */

.section .text
.code16                          // 16-bit real mode code

// ============================================================
// Common definitions
// ============================================================
// Note: GNU as cannot include .h files directly, constants redefined via .set

// ============================================================
// Constant definitions (consistent with boot_info.h)
// ============================================================

// E820 memory layout
.set E820_BUFFER_ADDR,          0x5000
.set E820_BUFFER_COUNT_ADDR,    0x5000
.set E820_BUFFER_ENTRIES_ADDR,  0x5004
.set E820_MAX_ENTRIES,          32
.set E820_ENTRY_SIZE,           24

// Pre-calculated segment/offset values
// Real mode: physical = segment << 4 + offset
// For physical 0x5000: segment = 0x0500 (0x0500 << 4 = 0x5000)
.set E820_COUNT_ADDR,           0x5000
.set E820_ENTRIES_ADDR,          0x5004
.set E820_COUNT_SEG,             0x0500      // 0x0500 << 4 = 0x5000
.set E820_COUNT_OFF,             0x0000
.set E820_ENTRIES_SEG,           0x0500      // 0x0500 << 4 = 0x5000
.set E820_ENTRIES_OFF,           0x0004      // 0x5000 + 4 = 0x5004

// MemoryMapEntry structure offsets (24 bytes/entry)
.set MEM_MAP_BASE,            0
.set MEM_MAP_LENGTH,          8
.set MEM_MAP_TYPE,            16
.set MEM_MAP_SIZE,            24

// E820 BIOS call constants
.set E820_SIGNATURE,          0x534D4150  // 'SMAP'
.set E820_CMD,                0x0000E820  // E820 function ID (full 32-bit)
.set E820_MAX_BIOS_SIZE,      20          // Max entry size BIOS returns

// DAP (Disk Address Packet) structure offsets (16 bytes)
.set DAP_SIZE,                0           // Packet size (bytes)
.set DAP_RESERVED,            1           // Reserved (always 0)
.set DAP_COUNT,               2           // Number of sectors to transfer
.set DAP_BUFFER_OFFSET,       4           // Transfer buffer offset (16-bit)
.set DAP_BUFFER_SEGMENT,      6           // Transfer buffer segment (16-bit)
.set DAP_LBA,                 8           // Starting LBA (64-bit)

// DAP fixed location in low memory (MBR DAP area, safe to reuse after MBR)
.set DAP_PHYS_ADDR,           0x7B00      // Fixed DAP location (physical address)
.set DAP_SEGMENT,             0x07B0      // DAP segment = 0x7B00 >> 4
.set DAP_OFFSET,              0x0000      // DAP offset

// ============================================================
// Mini kernel loading configuration
// ============================================================
// Real mode can only access low 1MB directly, so we load to 0x10000
// The mini kernel (512KB) fits in 0x10000 ~ 0x90000
// After entering long mode, the disk driver will relocate the large kernel

// Mini kernel disk location
.set MINI_KERNEL_LBA,         16          // Mini kernel start LBA (sector 16)
.set MINI_KERNEL_SECTORS,     832         // Total sectors (416KB) - 32KB gap before protected mode stack at 0x90000

// Mini kernel load address (real mode accessible: < 1MB)
// Real mode stack:    SS=0x0900, SP=0xFFFE (physical: 0x9000~0x19000)
// Protected mode stack: ESP=0x90000 (stage2.S line 168)
// Kernel loads to 0x20000~0x90000 to avoid both stacks
.set MINI_KERNEL_LOAD_PHYS,   0x20000     // Physical address where kernel is loaded
.set MINI_KERNEL_LOAD_SEG,    0x2000      // Segment: 0x2000 (0x2000 << 4 = 0x20000)
.set MINI_KERNEL_LOAD_OFF,    0x0000      // Offset

// BIOS disk read limits
.set DISK_READ_CMD,           0x42        // INT 0x13 AH=0x42 extended read
.set DISK_MAX_SECTORS_PER_CALL, 127       // Max sectors per BIOS call (int13 limit)

// Error messages (for panic use)
.extern msg_e820_failed
.extern msg_disk_read_failed
.extern msg_disk_too_large
.extern panic

// ============================================================
// Function: query_memory_map
// Responsibility: Execute E820 memory enumeration, store results in 0x5000 buffer
// Input: None
// Output: %cx = number of memory map entries
// Clobbers: %ax, %bx, %cx, %dx, %si, %di, %es (saved/restored)
// ============================================================
.global query_memory_map
.type query_memory_map, @function
query_memory_map:
    pushaw
    pushw %es
    pushw %ds

    movw $0x0, %ax
    movw $E820_COUNT_SEG, %dx
    movw %dx, %ds
    movw %ax, E820_COUNT_OFF     // count = 0

    movw $E820_ENTRIES_SEG, %ax
    movw %ax, %es
    movw $E820_ENTRIES_OFF, %di

    // EBX = 0 (first call)
    xorl %ebx, %ebx

.e820_loop:
    // with debugcon output, see how many entry do we find :)
    // movb $'E', %al       // [imm->al] Load 'L' (0x4C)
    // outb %al, $0xe9        // [al->port] Output 'L' to debugcon

    movl E820_COUNT_OFF, %eax
    cmpl $E820_MAX_ENTRIES, %eax
    jae .e820_done

    // EAX=0x0000E820, EBX=continuation, ECX=bufsize, EDX='SMAP', ES:DI=buffer
    movl $E820_SIGNATURE, %edx       // EDX = 'SMAP'
    movl $E820_CMD, %eax             // EAX = 0x0000E820
    movl $E820_ENTRY_SIZE, %ecx      // ECX = 24 (buffer size)


    int $0x15
    jc .e820_failed

    cmpl $E820_SIGNATURE, %eax
    jne .e820_failed

    // BIOS 可能返回 <24，至少要有 20
    cmpl $20, %ecx
    jb .e820_failed

    addl $E820_ENTRY_SIZE, %edi

    movl E820_COUNT_OFF, %eax
    incl %eax
    movl %eax, E820_COUNT_OFF

    testl %ebx, %ebx
    jnz .e820_loop

.e820_done:
    movl E820_COUNT_OFF, %eax
    movl %eax, %ecx

    popw %ds
    popw %es
    popaw
    ret

.e820_failed:
    popw %ds
    popw %es
    popaw

    movw $msg_e820_failed, %si
    jmp panic

// ============================================================
// Function: load_kernel_from_disk
// Responsibility: Read complete mini kernel ELF (512KB) from disk LBA=16
//                 directly to MINI_KERNEL_LOAD_PHYS (0x10000)
//
// Strategy: Read in chunks (max 127 sectors per BIOS call)
//   - Each iteration updates DAP buffer address for next chunk
//   - No intermediate copy needed: direct read to final location
//
// Memory layout after load:
//   0x10000 ~ 0x90000: Mini kernel (512KB)
//
// Input: None
// Output: %ax = number of sectors read (1024), 0 means failure
// Clobbers: %ax, %bx, %cx, %dx, %si, %di, %es (saved/restored)
// ============================================================
.global load_kernel_from_disk
.type load_kernel_from_disk, @function
load_kernel_from_disk:
    pusha                                // [regs->stack] save all 16-bit registers
    pushw %es                            // [es->stack] save ES
    pushw %ds                            // [ds->stack] save DS

    // Initialize: BX = sectors read so far, SI = current buffer offset
    xorw %bx, %bx                        // [0->bx] BX = 0 (no sectors read yet)
    xorw %si, %si                        // [0->si] SI = 0 (buffer offset, stays 0)

    // Set ES to DAP segment for DAP access
    movw $DAP_SEGMENT, %dx               // [imm->dx] DX = 0x07B0
    movw %dx, %es

.read_loop:
    // Check if we've read all sectors
    cmpw $MINI_KERNEL_SECTORS, %bx       // [imm->bx] Compare with total
    jae .read_done                       // [cf->] Done if BX >= total

    // Calculate sectors to read this iteration (min(127, remaining))
    movw $MINI_KERNEL_SECTORS, %ax       // [imm->ax] AX = total sectors
    subw %bx, %ax                        // [bx->ax] AX = remaining sectors
    cmpw $DISK_MAX_SECTORS_PER_CALL, %ax // [imm->ax] Compare with max
    jbe .read_count_ok                   // [cf->] Use AX if <= 127
    movw $DISK_MAX_SECTORS_PER_CALL, %ax // [imm->ax] Cap at 127

.read_count_ok:
    // Save sector count in BP
    movw %ax, %bp                        // [ax->bp] BP = sectors to read

    // CRITICAL: Re-set ES before building DAP (BIOS may clobber it)
    movw $DAP_SEGMENT, %dx               // [imm->dx] DX = 0x07B0
    movw %dx, %es                        // [dx->es] ES = DAP segment

    // Build DAP at fixed location 0x7B00
    movw $DAP_OFFSET, %di                // [imm->di] DI = 0x0000

    // Fill DAP.size = 16
    movb $16, %es:(%di)                  // [imm->mem] set DAP size

    // Fill DAP.reserved = 0 (important for some BIOS)
    movb $0, %es:1(%di)                  // [imm->mem] clear reserved byte

    // Fill DAP.count = BP
    movw %bp, %es:2(%di)                 // [bp->mem] set sector count

    // Fill DAP.buffer = MINI_KERNEL_LOAD_PHYS + (BX * 512)
    // Each sector = 512 bytes, so offset = BX sectors * 512
    // In real mode: segment = (base >> 4), offset = (base & 0xF)
    // Base = 0x10000 + BX * 512
    //      = 0x10000 + (BX << 9)
    // Segment = (0x10000 + (BX << 9)) >> 4
    //        = 0x1000 + (BX << 5)
    //        = 0x1000 + (BX * 32)
    movw %bx, %ax                        // [bx->ax] AX = sector offset
    shlw $5, %ax                         // [ax->ax] AX *= 32
    addw $MINI_KERNEL_LOAD_SEG, %ax      // [imm->ax] AX = target segment
    movw %ax, %es:6(%di)                 // [ax->mem] buffer segment
    movw $MINI_KERNEL_LOAD_OFF, %ax      // [imm->ax] AX = 0 (offset always 0)
    movw %ax, %es:4(%di)                 // [ax->mem] buffer offset

    // Fill DAP.lba = MINI_KERNEL_LBA + BX (full 8 bytes, must be zero-initialized)
    movw $MINI_KERNEL_LBA, %ax           // [imm->ax] AX = base LBA (16)
    addw %bx, %ax                        // [bx->ax] AX = current LBA
    movw %ax, %es:8(%di)                 // [ax->mem] LBA low 16 bits
    xorw %ax, %ax                        // [0->ax] AX = 0
    movw %ax, %es:10(%di)                // [ax->mem] LBA high 16 bits
    movw %ax, %es:12(%di)                // [ax->mem] LBA upper 16 bits
    movw %ax, %es:14(%di)                // [ax->mem] LBA top 16 bits (clear garbage)

    // CRITICAL: Save BX and BP across BIOS call (BIOS clobbers both)
    pushw %bx                            // [bx->stack] save sector counter
    pushw %bp                            // [bp->stack] save sector count

    // Call INT 0x13 AH=0x42
    // CRITICAL: INT 13h AH=42h requires DS:SI to point to DAP, NOT ES:SI!
    //            DS was saved on stack at function entry, so we can modify it.
    movw $DAP_SEGMENT, %dx               // [imm->dx] DX = 0x07B0
    movw %dx, %ds                        // [dx->ds] DS = DAP segment (REQUIRED!)
    movw $DAP_OFFSET, %si                // [imm->si] SI = 0x0000
    movb $0x80, %dl                      // [imm->dl] DL = 0x80
    movb $DISK_READ_CMD, %ah             // [imm->ah] AH=0x42
    
    int $0x13                            // [->regs] execute disk read

    jc .disk_error_restore_bp            // [cf->] CF=1 means failure
    cmpb $0, %ah                         // [ah->] AH should be 0
    jne .disk_error_restore_bp           // [zf->] AH!=0 means error

    // CRITICAL: Restore BP and BX after BIOS call (success path)
    popw %bp                             // [stack->bp] restore sector count
    popw %bx                             // [stack->bx] restore sector counter

    // Update sector counter
    addw %bp, %bx                        // [bp->bx] BX += sectors read

    jmp .read_loop                       // [->] Continue reading

.disk_error_restore_bp:
    // Restore BP and BX before handling error (stack: BP then BX)
    popw %bp                             // [stack->bp] restore sector count
.disk_error_restore:
    popw %bx                             // [stack->bx] restore sector counter
    jmp disk_read_failed                 // [->] handle error

.read_done:
    popw %ds                             // [stack->ds] restore DS
    popw %es                             // [stack->es] restore ES
    popa   
    movb $'O', %al                       // [imm->al] Load 'L' (0x4C)
    outb %al, $0xe9                      // [al->port] Output 'L' to debugcon

    movw $MINI_KERNEL_SECTORS, %ax       // [imm->ax] return sectors read
    ret                                  // [return] return

disk_read_failed:
    movb $'F', %al                       // [imm->al] Load 'L' (0x4C)
    outb %al, $0xe9                      // [al->port] Output 'L' to debugcon
    popw %ds                             // [stack->ds] restore DS
    popw %es                             // [stack->es] restore ES
    popa                                 // [stack->regs] restore general registers
    movw $(msg_disk_read_failed), %si    // [msg->si] load error message
    jmp panic                            // [->never] call panic

disk_too_large:
    movb $'T', %al       // [imm->al] Load 'L' (0x4C)
    outb %al, $0xe9        // [al->port] Output 'L' to debugcon
    popw %ds                             // [stack->ds] restore DS
    popw %es                             // [stack->es] restore ES
    popa                                 // [stack->regs] restore general registers
    movw $(msg_disk_too_large), %si      // [msg->si] load error message
    jmp panic                            // [->never] call panic

// ============================================================
// Data section: Error messages
// ============================================================
.section .rodata

msg_e820_failed:
    .asciz "E820: Memory query failed!\r\n"

msg_disk_read_failed:
    .asciz "DISK: Failed to read kernel!\r\n"

msg_disk_too_large:
    .asciz "DISK: Kernel too large (>448KB)!\r\n"
