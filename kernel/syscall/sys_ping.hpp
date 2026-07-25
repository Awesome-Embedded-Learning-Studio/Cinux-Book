/**
 * @file kernel/syscall/sys_ping.hpp
 * @brief sys_ping handler declaration -- ICMP echo from user space.
 *
 * A Cinux-custom shortcut (no socket layer yet): the kernel does the whole
 * ARP resolve + ICMP echo + wait on the caller's behalf.  The syscall IS the
 * poll driver for the duration of the echo (no resident net thread needed).
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Send one ICMP echo request and wait for the reply.
 *
 * @param ip_packed  Destination IPv4, octets packed MSB-first:
 *                   (a<<24)|(b<<16)|(c<<8)|d (e.g. 10.0.2.2 -> 0x0A000202).
 * @param id         Echo identifier (host order).
 * @param seq        Echo sequence (host order).
 * @return 0 if an echo reply was received; -errno otherwise
 *         (ETIMEDOUT = no reply in the budget, ENOSYS = stack not up).
 */
int64_t sys_ping(uint64_t ip_packed, uint64_t id, uint64_t seq, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
