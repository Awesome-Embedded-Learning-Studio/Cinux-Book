/**
 * @file kernel/syscall/sys_ping.cpp
 * @brief sys_ping handler -- delegate to cinux::net::ping().
 *
 * Unpacks the destination IPv4, calls the production ping (which sends the echo
 * + drives the sti/hlt+poll loop), and translates the result to 0 / -errno.
 */

#include "kernel/syscall/sys_ping.hpp"

#include <stdint.h>

#include "kernel/errno.hpp"
#include "kernel/net/net_init.hpp"
#include "kernel/net/net_types.hpp"  // Ipv4Addr

namespace cinux::syscall {

int64_t sys_ping(uint64_t ip_packed, uint64_t id, uint64_t seq, uint64_t, uint64_t, uint64_t) {
    // Unpack a.b.c.d (MSB-first) into the 4 octets.
    const uint32_t       ip = static_cast<uint32_t>(ip_packed);
    cinux::net::Ipv4Addr dst{};
    dst.oct[0] = static_cast<uint8_t>((ip >> 24) & 0xFF);
    dst.oct[1] = static_cast<uint8_t>((ip >> 16) & 0xFF);
    dst.oct[2] = static_cast<uint8_t>((ip >> 8) & 0xFF);
    dst.oct[3] = static_cast<uint8_t>(ip & 0xFF);

    auto r = cinux::net::ping(dst, static_cast<uint16_t>(id), static_cast<uint16_t>(seq));
    if (!r.ok()) {
        return -to_errno(r.error());  // NotImplemented -> ENOSYS (stack not up)
    }
    if (!r.value().got_reply) {
        return -kEtimedout;  // request sent, no reply within the budget
    }
    return 0;  // echo reply received
}

}  // namespace cinux::syscall
