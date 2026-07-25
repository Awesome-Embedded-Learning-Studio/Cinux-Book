/**
 * @file kernel/arch/x86_64/ap_trampoline.S
 * @brief AP (Application Processor) startup trampoline (F4-M3 P2-2)
 *
 * The BSP copies this blob (ap_trampoline_start..ap_trampoline_end) to physical
 * 0x8000, injects the AP's parameters into the data area, then issues the
 * INIT-SIPI-SIPI sequence.  The AP wakes in 16-bit real mode at 0x0000:0x8000
 * and this code transitions it to 64-bit long mode, then jumps (via
 * ap_entry_long) to ap_main().
 *
 * Addressing: the blob is LINKED in the higher-half kernel image but EXECUTED
 * at physical 0x8000.  A label's runtime physical address is therefore
 * (label - ap_trampoline_start + 0x8000).  GAS folds the (label - start) part
 * (both symbols are in this section) into a constant, so each such expression
 * is an absolute immediate -- no runtime fixups or linker-script changes.
 * Memory accesses use register-indirect addressing with that constant loaded
 * into a register (lgdt/mov cannot take it directly).
 *
 * GAS is invoked directly (no cpp) on .S here, so # is a comment char and GAS
 * macros only expand at statement start -- hence the inline expressions instead
 * of a macro.
 *
 * Reference: boot/common/long_mode.S (enter_long_mode) + boot/stage2.S.
 *
 * Debug: each stage writes a marker to debugcon (port 0xE9 -> debug.log) so a
 * triple fault shows how far the AP got.
 */

.section .text

.set AP_PHYS,    0x8000     # physical address the BSP copies this blob to
.set DEBUGCON,   0xE9       # qemu debugcon port (-> debug.log)

# ============================================================
# 16-bit real-mode entry -- AP starts here at 0x0000:0x8000
# ============================================================
.code16
.global ap_trampoline_start
ap_trampoline_start:
    cli
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss

    movb    $0x31, %al              # '1': real-mode entry reached
    outb    %al, $DEBUGCON

    # Fast A20 enable via port 0x92 (insurance; QEMU keeps A20 on already).
    inb     $0x92, %al
    orb     $0x02, %al              # set A20 bit
    andb    $0xFE, %al              # clear bit 0 (would trigger reset!)
    outb    %al, $0x92

    # Load the temporary GDT.  Its 6-byte descriptor is at the runtime physical
    # address (tgdt_desc - start + 0x8000); that fits in 16 bits, so load it
    # into BX and lgdt via DS:BX (DS=0).
    movl    $(tgdt_desc - ap_trampoline_start + AP_PHYS), %eax
    movw    %ax, %bx
    lgdt    (%bx)

    # Enable protected mode (CR0.PE).
    movl    %cr0, %eax
    orl     $0x1, %eax
    movl    %eax, %cr0

    # Far jump to 32-bit protected mode (selector 0x08 = 32-bit code).
    ljmp    $0x08, $(ap_trampoline_pmode - ap_trampoline_start + AP_PHYS)

# ============================================================
# 32-bit protected mode
# ============================================================
.code32
.global ap_trampoline_pmode
ap_trampoline_pmode:
    movw    $0x10, %ax              # 32-bit data selector
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss

    movb    $0x32, %al              # '2': protected mode reached
    outb    %al, $DEBUGCON

    # Enable PAE + OSFXSR + OSXMMEXCPT.  The scheduler fxsave/fxrstor's FPU
    # state on every context switch; without CR4.OSFXSR those instructions
    # raise #UD on the AP (the BSP sets the same bits in boot.S, so its FPU
    # switches are fine -- the AP booted straight to a PAE-only CR4).
    movl    %cr4, %eax
    orl     $0x620, %eax          # PAE(0x20) | OSFXSR(0x200) | OSXMMEXCPT(0x400)
    movl    %eax, %cr4

    # Load CR3 with the BSP-prepared temporary PML4 (identity-maps low memory
    # + the higher-half kernel image).  Paging is still off, so linear=physical
    # and the param address reads the injected value directly.
    movl    $(ap_cr3 - ap_trampoline_start + AP_PHYS), %ebx
    movl    (%ebx), %eax
    movl    %eax, %cr3

    # Enable long mode (EFER.LME).
    movl    $0xC0000080, %ecx       # MSR_EFER
    rdmsr
    orl     $0x100, %eax            # LME (bit 8)
    wrmsr

    # Reload the GDT (carries the 64-bit code selector at 0x18).
    movl    $(tgdt_desc - ap_trampoline_start + AP_PHYS), %ebx
    lgdt    (%ebx)

    # Enable paging (CR0.PG | PE) -> long mode activates.
    movl    %cr0, %eax
    orl     $0x80000001, %eax
    movl    %eax, %cr0

    # Far jump to 64-bit long mode (selector 0x18 = 64-bit code).
    ljmp    $0x18, $(ap_trampoline_lmode - ap_trampoline_start + AP_PHYS)

# ============================================================
# 64-bit long mode
# ============================================================
.code64
.global ap_trampoline_lmode
ap_trampoline_lmode:
    movw    $0x20, %ax              # 64-bit data selector (mostly ignored)
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss

    movb    $0x33, %al              # '3': long mode reached
    outb    %al, $DEBUGCON

    # Read injected params (still at 0x8xxx, identity-mapped in the temp
    # tables) into callee-saved registers, then jump to ap_entry_long in the
    # higher-half kernel image.  ap_entry_long switches CR3 to the real kernel
    # PML4 and sets the stack -- it cannot be done here because 0x8000 is no
    # longer mapped after the CR3 switch.
    movabsq $(ap_stack - ap_trampoline_start + AP_PHYS), %rax
    movq    (%rax), %rbx            # rbx = AP kernel stack (higher-half virt)
    movabsq $(ap_kernel_cr3 - ap_trampoline_start + AP_PHYS), %rax
    movq    (%rax), %r12            # r12 = kernel PML4 physical
    movabsq $(ap_cpu_id - ap_trampoline_start + AP_PHYS), %rax
    movl    (%rax), %edi            # rdi = cpu_id (arg0 to ap_main)
    movabsq $(ap_entry - ap_trampoline_start + AP_PHYS), %rax
    movq    (%rax), %rax
    jmp     *%rax                   # -> ap_entry_long (higher-half)

    cli
1:  hlt
    jmp 1b

# ============================================================
# ap_entry_long -- higher-half C-callable entry (kernel image, NOT in the blob).
# The trampoline jumps here with rbx=stack, r12=kernel PML4, rdi=cpu_id.  Both
# this code and the stack are mapped in the temp tables AND the kernel PML4.
# ============================================================
.code64
.global ap_entry_long
ap_entry_long:
    movq    %r12, %cr3              # switch to the real kernel page tables
    movq    %rbx, %rsp              # set up the AP's kernel stack
    call    ap_main                 # ap_main(cpu_id); does not return
    cli
2:  hlt
    jmp 2b

# ============================================================
# Temporary GDT for the mode transition (selectors are throwaway; ap_main loads
# the real per-CPU GDT afterwards).
#   0x00 null | 0x08 32-bit code | 0x10 32-bit data
#   0x18 64-bit code (L=1) | 0x20 data
# ============================================================
.align 8
tgdt:
    .quad 0x0000000000000000        # 0x00 null
    .quad 0x00CF9A000000FFFF        # 0x08 32-bit code (ring0, exec/read, 4K)
    .quad 0x00CF92000000FFFF        # 0x10 32-bit data (ring0, rw, 4K)
    .quad 0x00AF9A000000FFFF        # 0x18 64-bit code (L=1)
    .quad 0x00CF92000000FFFF        # 0x20 data
tgdt_end:

# 6-byte pseudo-descriptor: 16-bit limit + 32-bit base.  The base is the
# runtime PHYSICAL address of tgdt = (tgdt - start + 0x8000), a GAS-folded
# constant, so no BSP patching of the descriptor is needed.
.global ap_trampoline_gdt_desc
ap_trampoline_gdt_desc:
tgdt_desc:
    .word   tgdt_end - tgdt - 1
    .long   (tgdt - ap_trampoline_start + AP_PHYS)

# ============================================================
# Injected parameters (BSP writes these in the copy at 0x8000 before SIPI).
# ============================================================
.align 8
.global ap_stack
ap_stack:
    .quad   0                       # AP kernel stack top (higher-half virt)
.global ap_entry
ap_entry:
    .quad   0                       # ap_entry_long virtual address
.global ap_cr3
ap_cr3:
    .quad   0                       # temporary PML4 physical (identity + hh kernel)
.global ap_kernel_cr3
ap_kernel_cr3:
    .quad   0                       # real kernel PML4 physical (ap_entry_long)
.global ap_cpu_id
ap_cpu_id:
    .quad   0                       # logical CPU index (percpu_blocks index)

.global ap_trampoline_end
ap_trampoline_end:
