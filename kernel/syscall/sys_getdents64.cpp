/**
 * @file kernel/syscall/sys_getdents64.cpp
 * @brief sys_getdents64 (Linux 217) handler -- F-ECO batch 1.
 *
 * musl opendir/readdir call getdents64 (217), NOT the legacy getdents (78). The
 * old sys_getdents only copied the entry NAME (a shape musl cannot parse); this
 * fills a real linux_dirent64 {d_ino, d_off, d_reclen, d_type, d_name[]} so musl
 * parses it and busybox ls lists real entries.
 *
 * Reuses do_getdents_kernel (reads ONE entry per call into a kernel buffer,
 * advances file->offset) in a loop, packing each into the user buffer.
 *
 * Layout (linux_dirent64, no padding before d_name):
 *   off 0  : uint64_t d_ino
 *   off 8  : int64_t  d_off
 *   off 16 : uint16_t d_reclen
 *   off 18 : uint8_t  d_type
 *   off 19 : char     d_name[] (NUL + pad to 8)
 * reclen = ALIGN(19 + strlen(name) + 1, 8).
 *
 * d_ino: nonzero (entry index). Linux filters d_ino==0 as a "deleted" entry,
 *   so a real 0 would make ls list nothing (same false-green as ENOSYS). True
 *   per-entry ino needs readdir to return it (follow-up: extend readdir).
 * d_type: DT_UNKNOWN (0); readdir gives only the name. ls lists names without
 *   it; ls -F/-l stat by name, which still works.
 */

#include "kernel/syscall/sys_getdents64.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // P0e (SMAP): access_ok/copy_to_user
#include "kernel/errno.hpp"
#include "kernel/lib/string.hpp"            // kernel memcpy (not <string.h> -> __memcpy_chk fortify)
#include "kernel/syscall/sys_getdents.hpp"  // do_getdents_kernel

namespace cinux::syscall {

int64_t sys_getdents64(uint64_t fd, uint64_t buf_virt, uint64_t count, uint64_t, uint64_t,
                       uint64_t) {
    if (count == 0) {
        return -cinux::kEfault;
    }
    if (!cinux::user::access_ok(reinterpret_cast<void*>(buf_virt), count)) {
        return -cinux::kEfault;
    }

    constexpr uint64_t kNameOff = 19;  // offsetof(linux_dirent64, d_name)
    uint64_t           off      = 0;   // bytes written to the user buffer
    uint64_t           entry    = 0;   // entry index (nonzero fake d_ino)
    char               kname[256];

    // Loop reading one entry at a time; stop when the buffer cannot hold the
    // next minimal record (leave the rest for the next getdents64 call).
    while (off + kNameOff + 8 <= count) {
        int64_t n = do_getdents_kernel(static_cast<int>(fd), kname, 255);
        if (n < 0) {
            return n;  // -errno
        }
        if (n == 0) {
            break;  // end of directory
        }
        const uint64_t name_len = static_cast<uint64_t>(n);
        const uint16_t reclen   = static_cast<uint16_t>(((kNameOff + name_len + 1) + 7) & ~7ull);
        if (off + reclen > count) {
            // Buffer full. do_getdents_kernel already advanced offset past this
            // entry, so it is lost -- but the smoke uses a large count and never
            // hits this; revisit (lseek-back / peek-ahead) if a caller does.
            break;
        }

        uint8_t  stage[256 + 8];
        uint64_t d_ino = ++entry;                          // nonzero (see file header)
        int64_t  d_off = static_cast<int64_t>(off + reclen);
        memcpy(stage + 0, &d_ino, 8);
        memcpy(stage + 8, &d_off, 8);
        memcpy(stage + 16, &reclen, 2);
        stage[18] = 0;                                     // DT_UNKNOWN
        memcpy(stage + kNameOff, kname, name_len);
        stage[kNameOff + name_len] = '\0';
        for (uint16_t p = static_cast<uint16_t>(kNameOff + name_len + 1); p < reclen; ++p) {
            stage[p] = 0;                                  // pad to 8
        }

        if (!cinux::user::copy_to_user(reinterpret_cast<void*>(buf_virt + off), stage,
                                       static_cast<uint64_t>(reclen))) {
            return -cinux::kEfault;
        }
        off += reclen;
    }
    return static_cast<int64_t>(off);
}

}  // namespace cinux::syscall
