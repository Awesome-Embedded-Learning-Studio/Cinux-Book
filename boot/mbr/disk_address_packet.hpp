/**
 * @file    disk_address_packet.hpp
 * @author  CharlieChen114514 (725610365@qq.com)
 * @brief   Disk Address Packet — the argument block for BIOS extended reads.
 *
 * Classical CHS reads through INT 13h AH=02h top out at the 528 MB
 * geometry barrier, so the boot path uses the extended services instead:
 * AH=42h (Extended Read) takes a pointer to this packet rather than a
 * pile of registers, and addresses sectors by absolute LBA number. The
 * field order and widths below are the ones firmware expects, which makes
 * [[gnu::packed]] load-bearing — any padding the compiler slips in
 * shifts the BIOS's view of the packet and the read lands wrong.
 *
 * @version 0.1
 * @date    2026-09-27
 * @since   0.1.0
 * @ingroup boot_mbr
 * @copyright Copyright (c) 2026
 */
#pragma once

namespace cinux::boot {
/**
 * @brief   Disk Address Packet handed to INT 13h AH=42h (Extended Read).
 *
 * Size is 16 bytes for the six fields below; a 24-byte variant with an
 * extra 64-bit flat buffer address also exists. On a failed transfer
 * some BIOSes rewrite count with the number of sectors actually moved.
 */
struct [[gnu::packed]] Dap {
    unsigned char      size;      ///< Packet size in bytes, 16 for this layout.
    unsigned char      reserved;  ///< Must be zero on entry.
    unsigned short     count;     ///< Number of sectors to transfer.
    unsigned short     offset;    ///< Destination buffer offset, half of a segment:offset pair.
    unsigned short     segment;   ///< Destination buffer segment, half of a segment:offset pair.
    unsigned long long lba;       ///< Starting sector as a zero-based absolute block address.
};
}  // namespace cinux::boot
