/**
 * @file kernel/syscall/sys_uname.cpp
 * @brief sys_uname handler (F-ECO busybox sh smoke)
 *
 * See sys_uname.hpp.  Static identity; do_uname_kernel fills a kernel utsname,
 * sys_uname stages it out.
 *
 * Namespace: cinux::syscall
 */

#include "kernel/syscall/sys_uname.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/user_access.hpp"  // copy_to_user (SMAP/extable)
#include "kernel/errno.hpp"
#include "kernel/lib/string.hpp"  // memset / strncpy-style copy
#include "kernel/version.hpp"     // kOsName / kOsVersion / kOsRelease (single source of truth)

namespace cinux::syscall {

namespace {
/// Copy a NUL-terminated C string into a fixed 65-byte utsname field, zero-pad
/// the tail (Linux semantics: the whole field is cleared first).
void set_field(char* dst, const char* src) {
    uint32_t i = 0;
    for (; i < 64 && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    for (; i < 65; ++i) {
        dst[i] = '\0';
    }
}
}  // namespace

int64_t do_uname_kernel(kutsname* out) {
    if (out == nullptr) {
        return -cinux::kEinval;
    }
    memset(out, 0, sizeof(*out));
    set_field(out->sysname, cinux::kOsName);
    set_field(out->nodename, "cinux");
    set_field(out->release, cinux::kOsVersion);
    set_field(out->version, cinux::kOsRelease);
    set_field(out->machine, "x86_64");
    set_field(out->domainname, "(none)");
    return 0;
}

int64_t sys_uname(uint64_t ts_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    if (ts_virt == 0) {
        return -cinux::kEfault;
    }
    kutsname ku;
    int64_t  rc = do_uname_kernel(&ku);
    if (rc < 0) {
        return rc;
    }
    if (!cinux::user::copy_to_user(reinterpret_cast<void*>(ts_virt), &ku, sizeof(ku))) {
        return -cinux::kEfault;
    }
    return 0;
}

}  // namespace cinux::syscall
