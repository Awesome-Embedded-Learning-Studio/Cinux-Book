#pragma once

namespace cinux::mini::lib {
// ============================================================
// Kernel Print Function (kprintf)
// ============================================================
// Format specifier support:
//   %% - Percent sign
//   %c - Character (int)
//   %s - String (char*, nullptr prints as "(null)")
//   %d - Signed decimal (int64_t)
//   %u - Unsigned decimal (uint64_t)
//   %x - Lowercase hexadecimal (uint64_t, no prefix)
//   %X - Uppercase hexadecimal (uint64_t, no prefix)
//   %p - Pointer (uint64_t, with "0x" prefix)
//   %b - Binary (uint64_t, no prefix)
//
// Width/precision modifiers (simplified):
//   %Nd - Minimum width N, right-aligned with spaces
//   %0Nd - Minimum width N, right-aligned with zeros
//
// Examples:
//   kprintf("Value: %d\n", 42);
//   kprintf("Ptr: %p\n", ptr);      // Prints "0x..."
//   kprintf("Hex: %04x\n", 0xAB);   // Prints "00ab"
void kprintf(const char* format, ...);

// ============================================================
// Kernel Print Function (kdebugf)
// ============================================================
// This is using for the kernel debug printf, when debugcon is available
// we shell write something to prove we are alive
// Format specifier support:
//   %% - Percent sign
//   %c - Character (int)
//   %s - String (char*, nullptr prints as "(null)")
//   %d - Signed decimal (int64_t)
//   %u - Unsigned decimal (uint64_t)
//   %x - Lowercase hexadecimal (uint64_t, no prefix)
//   %X - Uppercase hexadecimal (uint64_t, no prefix)
//   %p - Pointer (uint64_t, with "0x" prefix)
//   %b - Binary (uint64_t, no prefix)
//
// Width/precision modifiers (simplified):
//   %Nd - Minimum width N, right-aligned with spaces
//   %0Nd - Minimum width N, right-aligned with zeros
//
// Examples:
//   kprintf("Value: %d\n", 42);
//   kprintf("Ptr: %p\n", ptr);      // Prints "0x..."
//   kprintf("Hex: %04x\n", 0xAB);   // Prints "00ab"
void kdebugf(const char* format, ...);

}  // namespace cinux::mini::lib
