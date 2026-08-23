/**
 * @file logger.hpp
 * @brief Lightweight logging framework — level filtering + formatted output + sink dispatch.
 */

#ifndef CINUX_LOGGER_HPP
#define CINUX_LOGGER_HPP

#include <cstdarg>
#include <cstddef>
#include <cstdint>

namespace cinux::lib {

/** @brief Log severity levels. */
enum class LogLevel : uint8_t {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERROR = 3,
};

/** @brief Convert LogLevel to a short string tag. */
constexpr const char* log_level_string(LogLevel level) {
    switch (level) {
    case LogLevel::DEBUG:
        return "DEBUG";
    case LogLevel::INFO:
        return "INFO";
    case LogLevel::WARN:
        return "WARN";
    case LogLevel::ERROR:
        return "ERROR";
    }
    return "UNKNOWN";
}

/** @brief Sink callback type: receives level, formatted message, and user context. */
using LogSink = void (*)(LogLevel level, const char* message, void* ctx);

/**
 * @brief Lightweight logger with level filtering and multiple sinks.
 *
 * Singleton pattern. No heap allocation. Thread safety is the caller's responsibility.
 */
class Logger {
public:
    /** @brief Get the singleton instance. */
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    static constexpr int kMaxSinks = 8;

    /** @brief Set the minimum log level. Messages below this are dropped. */
    void set_level(LogLevel level) { threshold_ = level; }

    /** @brief Get the current threshold level. */
    LogLevel level() const { return threshold_; }

    /** @brief Register a sink callback. Returns false if at max capacity. */
    bool register_sink(LogSink sink, void* ctx = nullptr) {
        if (sink_count_ >= kMaxSinks || !sink) {
            return false;
        }
        sinks_[sink_count_++] = {sink, ctx};
        return true;
    }

    /** @brief Remove all registered sinks. */
    void clear_sinks() { sink_count_ = 0; }

    /** @brief Log a message at the given level (printf-style format). */
    void log(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

    /** @brief Convenience: log at DEBUG level. */
    void debug(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    /** @brief Convenience: log at INFO level. */
    void info(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    /** @brief Convenience: log at WARN level. */
    void warn(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    /** @brief Convenience: log at ERROR level. */
    void error(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    /** @brief Number of messages dropped due to level filtering. */
    size_t dropped_count() const { return dropped_; }

    /** @brief Reset the dropped counter. */
    void reset_dropped_count() { dropped_ = 0; }

private:
    Logger() = default;

    void emit(LogLevel level, const char* fmt, va_list args);

    struct SinkEntry {
        LogSink fn  = nullptr;
        void*   ctx = nullptr;
    };

    LogLevel  threshold_ = LogLevel::INFO;
    SinkEntry sinks_[kMaxSinks]{};
    int       sink_count_ = 0;
    size_t    dropped_    = 0;
};

}  // namespace cinux::lib

#endif  // CINUX_LOGGER_HPP
