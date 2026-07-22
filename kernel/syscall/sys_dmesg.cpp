/**
 * @file kernel/syscall/sys_dmesg.cpp
 * @brief sys_dmesg implementation -- format KernelLog history into text
 *
 * Formatting is done by hand (append helpers) rather than vkprintf_impl,
 * because vkprintf_impl takes a va_list -- these fields are fixed values,
 * not a variadic list.
 */

#include "kernel/syscall/sys_dmesg.hpp"

#include <stddef.h>
#include <stdint.h>

#include <cinux/logger.hpp>

#include "kernel/errno.hpp"
#include "kernel/lib/klog.hpp"

namespace cinux::syscall {

using cinux::lib::KernelLog;
using cinux::lib::LogEntry;
using cinux::lib::log_level_string;

namespace {

/// Append one char to @p buf at @p pos (capped at @p len). Returns false if full.
bool append_char(char* buf, uint64_t& pos, uint64_t len, char c) {
    if (pos >= len) {
        return false;
    }
    buf[pos++] = c;
    return true;
}

/// Append a NUL-terminated string.
void append_str(char* buf, uint64_t& pos, uint64_t len, const char* s) {
    for (size_t i = 0; s[i] != '\0'; i++) {
        if (!append_char(buf, pos, len, s[i])) {
            return;
        }
    }
}

/// Append an unsigned 64-bit value in decimal.
void append_u64(char* buf, uint64_t& pos, uint64_t len, uint64_t v) {
    char tmp[24];
    int  n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v != 0) {
            tmp[n++] = '0' + static_cast<char>(v % 10);
            v /= 10;
        }
    }
    for (int i = n - 1; i >= 0; i--) {
        if (!append_char(buf, pos, len, tmp[i])) {
            return;
        }
    }
}

}  // namespace

int64_t sys_dmesg(uint64_t buf_virt, uint64_t len, uint64_t, uint64_t, uint64_t, uint64_t) {
    // Validate the user buffer address (canonical check, same as sys_read).
    if (buf_virt == 0) {
        return -kEfault;
    }
    uint64_t bit47 = (buf_virt >> 47) & 1;
    uint64_t upper = buf_virt >> 48;
    if (bit47 == 0 && upper != 0) {
        return -kEfault;
    }
    if (bit47 == 1 && upper != 0xFFFF) {
        return -kEfault;
    }
    if (len == 0) {
        return 0;
    }

    char* buf = reinterpret_cast<char*>(buf_virt);

    // Drain up to 16 entries per call.  Heap-allocated: LogEntry is 272 B, so
    // 16 entries = ~4.4 KB -- too large for the 16 KB kernel stack + IRQ
    // nesting (DEBT-015).  operator new[] routes through kmalloc (F2-M7b slab);
    // freed before the single return after the loop.
    constexpr std::size_t kMaxEntries = 16;
    LogEntry*    entries = new LogEntry[kMaxEntries];
    if (entries == nullptr) {
        return -kEnomem;
    }
    std::size_t n = KernelLog::instance().read(entries, kMaxEntries);

    uint64_t pos = 0;
    for (std::size_t i = 0; i < n; i++) {
        // Format "[LEVEL] tick: message\n" directly into the user buffer.
        if (!append_char(buf, pos, len, '[')) {
            break;
        }
        append_str(buf, pos, len, log_level_string(entries[i].level));
        if (!append_char(buf, pos, len, ']') || !append_char(buf, pos, len, ' ')) {
            break;
        }
        append_u64(buf, pos, len, entries[i].timestamp);
        if (!append_char(buf, pos, len, ':') || !append_char(buf, pos, len, ' ')) {
            break;
        }
        append_str(buf, pos, len, entries[i].message);
        append_char(buf, pos, len, '\n');
    }

    delete[] entries;
    return static_cast<int64_t>(pos);
}

}  // namespace cinux::syscall
