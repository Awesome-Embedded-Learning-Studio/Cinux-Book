/**
 * @file user/programs/shell/cmd_ping.cpp
 * @brief 'ping' command -- ICMP echo via sys_ping.
 *
 * Parses "a.b.c.d" + optional count (default 4), sends one ICMP echo per seq
 * through sys_ping (the kernel does ARP resolve + echo + wait), and prints
 * "reply from <ip>: seq=N" / "no reply (timeout)" per probe + a summary.
 */

#include <stdint.h>

#include "libc/printf.hpp"
#include "libc/syscall.h"
#include "shell.hpp"

using cinux::user::printf;

namespace {

// errno values mirrored from kernel/errno.hpp (the user libc has no errno.h yet).
constexpr int64_t kEtimedout = 110;  ///< ETIMEDOUT -- no reply
constexpr int64_t kEnosys    = 38;   ///< ENOSYS -- stack not up

/// @brief Parse "a.b.c.d" into a packed uint32 (MSB-first oct[0]).
bool parse_ipv4(const char* s, uint32_t& out) {
    uint32_t oct[4] = {0, 0, 0, 0};
    int      idx    = 0;
    bool     any    = false;
    for (int i = 0; s[i] != '\0'; ++i) {
        char c = s[i];
        if (c == '.') {
            if (!any || idx == 3) {
                return false;  // leading dot / too many dots
            }
            ++idx;
            any = false;
            continue;
        }
        if (c < '0' || c > '9') {
            return false;
        }
        oct[idx] = oct[idx] * 10 + static_cast<uint32_t>(c - '0');
        if (oct[idx] > 255) {
            return false;
        }
        any = true;
    }
    if (idx != 3 || !any) {
        return false;
    }
    out = (oct[0] << 24) | (oct[1] << 16) | (oct[2] << 8) | oct[3];
    return true;
}

}  // namespace

void cmd_ping(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: ping <a.b.c.d> [count]\n");
        return;
    }
    uint32_t ip = 0;
    if (!parse_ipv4(argv[1], ip)) {
        printf("ping: invalid address '%s'\n", argv[1]);
        return;
    }

    uint32_t count = 4;
    if (argc >= 3) {
        uint32_t c = 0;
        for (int i = 0; argv[2][i] != '\0'; ++i) {
            if (argv[2][i] < '0' || argv[2][i] > '9') {
                c = 0;
                break;
            }
            c = c * 10 + static_cast<uint32_t>(argv[2][i] - '0');
        }
        if (c > 0 && c < 100) {
            count = c;
        }
    }

    const uint16_t id = 0xC1A0;  // fixed echo id (Cinux shell)
    printf("PING %s (count=%u)\n", argv[1], count);
    uint32_t got = 0;
    for (uint32_t seq = 1; seq <= count; ++seq) {
        int64_t r = sys_ping(ip, id, static_cast<uint16_t>(seq));
        if (r == 0) {
            printf("reply from %s: seq=%u\n", argv[1], seq);
            ++got;
        } else if (r == -kEnosys) {
            printf("ping: network not available\n");
            return;  // no point retrying without a stack
        } else {
            printf("no reply from %s: seq=%u (timeout)\n", argv[1], seq);
        }
    }
    printf("--- %s: %u/%u replies ---\n", argv[1], got, count);
}
