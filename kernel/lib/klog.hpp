/**
 * @file kernel/lib/klog.hpp
 * @brief Kernel log buffer (dmesg-style) with level filtering
 *
 * KernelLog is a singleton holding a fixed-size ring buffer of LogEntry
 * (timestamp + level + message).  It complements kprintf: kprintf remains
 * the real-time character sink (serial/framebuffer), while KernelLog keeps
 * a structured history readable via sys_dmesg (M2-3).
 *
 * LogLevel is reused from Cinux-Base (cinux::lib::LogLevel); the history
 * storage uses ConcurrentRingBuffer (IRQ-safe), so log() is safe to call
 * from interrupt / panic context as well as from multiple tasks.
 *
 * Namespace: cinux::lib
 */

#pragma once

#include <cinux/logger.hpp>  // LogLevel
#include <cstddef>
#include <cstdint>

#include "kernel/lib/concurrent_ring_buffer.hpp"

namespace cinux::lib {

/// Maximum formatted message length per entry (including trailing NUL).
constexpr std::size_t kKlogMsgMax = 256;

/// Number of entries kept in the history ring.  Memory-bound: the todo
/// plan's 4096 would be ~1 MiB of global storage; 128 (~33 KiB) is the
/// chosen trade-off, enough for boot + recent activity.
constexpr std::size_t kKlogRingSize = 128;

/// A single buffered log entry.
struct LogEntry {
    uint64_t timestamp;             ///< PIT tick count at log time
    LogLevel level;                 ///< Severity
    char     message[kKlogMsgMax];  ///< NUL-terminated formatted message
};

// F-INFRA I-4 (R11): LogEntry is the klog ring element (array of these). Pinned
// so a field/type change forces a conscious update, not a silent ABI drift.
static_assert(sizeof(LogEntry) == 272, "LogEntry layout (u64 + u8 level + 256-byte msg)");

/**
 * @brief Kernel log history (dmesg backing store)
 *
 * Singleton.  log() formats a printf-style message and pushes a LogEntry
 * into an IRQ-safe ring buffer.  read() drains entries (consumes them).
 * kprintf output is mirrored in via klog_init()'s sink (line accumulation).
 */
class KernelLog {
public:
    /** @brief Singleton accessor (first use constructs the instance). */
    static KernelLog& instance();

    KernelLog(const KernelLog&)            = delete;
    KernelLog& operator=(const KernelLog&) = delete;

    /**
     * @brief Log a printf-style message at @p level
     *
     * Dropped without formatting if below g_klog_threshold.
     */
    void log(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

    /** @brief Log an already-formatted NUL-terminated message at @p level. */
    void log_raw(LogLevel level, const char* msg);

    /**
     * @brief Drain up to @p max_entries into @p out (consumes them)
     * @return Number of entries written
     *
     * @note Drain semantics: read entries leave the buffer.  A non-consuming
     *       snapshot read can be added later if dmesg needs repeatable reads.
     */
    std::size_t read(LogEntry* out, std::size_t max_entries);

    /** @brief Entries dropped because the ring was full. */
    std::size_t dropped() const { return dropped_; }

    /** @brief Entries currently buffered. */
    std::size_t size() const { return ring_.size(); }

    /** @brief Clear all entries and reset the dropped counter. */
    void clear();

private:
    KernelLog() = default;

    void push_entry(LogLevel level, const char* msg, std::size_t len);

    ConcurrentRingBuffer<LogEntry, kKlogRingSize> ring_;
    std::size_t dropped_ = 0;  // single-core safe; SMP would need atomics
};

/// Global level threshold; messages below this are dropped at log time.
/// Default DEBUG (keep all); raise to INFO/WARN to reduce volume.
extern LogLevel g_klog_threshold;

/// Set the global log threshold.
void set_klog_level(LogLevel level);

/**
 * @brief Register the kprintf -> KernelLog sink (line accumulation)
 *
 * After this, every kprintf line is also appended to the log history at
 * INFO level.  Call once after kprintf_init() and PIT init().  The sink
 * accumulates characters and flushes a LogEntry on '\n'.
 */
void klog_init();

}  // namespace cinux::lib

// ============================================================
// Convenience macros -- level-checked inside log()
// ============================================================
#define klog_debug(fmt, ...)                                                                       \
    ::cinux::lib::KernelLog::instance().log(::cinux::lib::LogLevel::DEBUG, fmt, ##__VA_ARGS__)
#define klog_info(fmt, ...)                                                                        \
    ::cinux::lib::KernelLog::instance().log(::cinux::lib::LogLevel::INFO, fmt, ##__VA_ARGS__)
#define klog_warn(fmt, ...)                                                                        \
    ::cinux::lib::KernelLog::instance().log(::cinux::lib::LogLevel::WARN, fmt, ##__VA_ARGS__)
#define klog_error(fmt, ...)                                                                       \
    ::cinux::lib::KernelLog::instance().log(::cinux::lib::LogLevel::ERROR, fmt, ##__VA_ARGS__)
