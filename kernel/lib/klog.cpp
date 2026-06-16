/**
 * @file kernel/lib/klog.cpp
 * @brief KernelLog implementation + kprintf line-accumulation sink
 */

#include "kernel/lib/klog.hpp"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/drivers/pit/pit.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/private/vkprintf_impl.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::lib {

using cinux::drivers::PIT;
using cinux::lib::detail::vkprintf_impl;

// ============================================================
// Threshold
// ============================================================

LogLevel g_klog_threshold = LogLevel::DEBUG;

// Reentrancy guard: while KernelLog::log() prints its real-time console
// copy via kprintf, the klog sink skips that line (it is pushed to the
// ring directly below, avoiding double-entry).  Single-core safe.
std::size_t g_klog_emit_depth = 0;

void set_klog_level(LogLevel level) {
    g_klog_threshold = level;
}

// ============================================================
// Singleton
// ============================================================

KernelLog& KernelLog::instance() {
    static KernelLog inst;
    return inst;
}

// ============================================================
// Internal: push one formatted entry
// ============================================================

void KernelLog::push_entry(LogLevel level, const char* msg, std::size_t len) {
    LogEntry e{};
    e.timestamp   = PIT::get_ticks();
    e.level       = level;
    std::size_t n = (len < kKlogMsgMax - 1) ? len : (kKlogMsgMax - 1);
    for (std::size_t i = 0; i < n; i++) {
        e.message[i] = msg[i];
    }
    e.message[n] = '\0';
    if (!ring_.push(e)) {
        dropped_++;
    }
}

// ============================================================
// Public API
// ============================================================

void KernelLog::log(LogLevel level, const char* fmt, ...) {
    if (static_cast<int>(level) < static_cast<int>(g_klog_threshold)) {
        return;
    }
    char        buf[kKlogMsgMax];
    std::size_t len = 0;
    va_list     args;
    va_start(args, fmt);
    vkprintf_impl(
        [&](char c) {
            if (len < kKlogMsgMax - 1) {
                buf[len++] = c;
            }
        },
        fmt, args);
    va_end(args);
    buf[len] = '\0';
    // Real-time console copy.  Guarded so the klog kprintf sink skips
    // this line (it is pushed to the ring right below).
    g_klog_emit_depth++;
    kprintf("[%s] %s\n", log_level_string(level), buf);
    g_klog_emit_depth--;
    push_entry(level, buf, len);
}

void KernelLog::log_raw(LogLevel level, const char* msg) {
    if (static_cast<int>(level) < static_cast<int>(g_klog_threshold)) {
        return;
    }
    std::size_t len = 0;
    while (msg[len] != '\0' && len < kKlogMsgMax) {
        len++;
    }
    push_entry(level, msg, len);
}

std::size_t KernelLog::read(LogEntry* out, std::size_t max_entries) {
    std::size_t got = 0;
    while (got < max_entries) {
        LogEntry e;
        if (!ring_.pop(e)) {
            break;
        }
        out[got++] = e;
    }
    return got;
}

void KernelLog::clear() {
    ring_.clear();
    dropped_ = 0;
}

// ============================================================
// kprintf -> KernelLog sink (line accumulation)
//
// kprintf invokes sinks character-by-character without holding any lock,
// so concurrent callers would interleave.  We accumulate under an IRQ-safe
// Spinlock: each character is appended, and a LogEntry is flushed on '\n'.
// On single-core this is race-free; on SMP a per-CPU line buffer would
// reduce contention (future work).
// ============================================================

namespace {

struct LineBuffer {
    char                  buf[kKlogMsgMax];
    std::size_t           len = 0;
    cinux::proc::Spinlock lock;
};

LineBuffer g_klog_line;

void klog_kprintf_sink(char c, void* /*ctx*/) {
    // Skip lines that KernelLog::log() is itself emitting in real time --
    // they are pushed to the ring directly; capturing here would double.
    if (g_klog_emit_depth > 0) {
        return;
    }
    auto guard = g_klog_line.lock.irq_guard();
    if (c == '\n') {
        g_klog_line.buf[g_klog_line.len] = '\0';
        KernelLog::instance().log_raw(LogLevel::INFO, g_klog_line.buf);
        g_klog_line.len = 0;
    } else if (g_klog_line.len < kKlogMsgMax - 1) {
        g_klog_line.buf[g_klog_line.len++] = c;
    }
    // else: line longer than kKlogMsgMax -- drop chars until newline
}

}  // namespace

void klog_init() {
    kprintf_register_sink(klog_kprintf_sink, nullptr);
}

}  // namespace cinux::lib
