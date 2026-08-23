/**
 * @file logger.cpp
 * @brief Logger implementation — uses detail::vformat for formatting.
 */

#include <cinux/detail/vformat.hpp>
#include <cinux/logger.hpp>
#include <cstdarg>
#include <cstring>

namespace cinux::lib {

static constexpr size_t kLogBufSize = 256;

void Logger::emit(LogLevel level, const char* fmt, va_list args) {
    if (level < threshold_) {
        ++dropped_;
        return;
    }

    char buf[kLogBufSize];

    // Write prefix: [LEVEL]
    int pos         = 0;
    buf[pos++]      = '[';
    const char* tag = log_level_string(level);
    while (*tag && pos < static_cast<int>(kLogBufSize) - 2) {
        buf[pos++] = *tag++;
    }
    buf[pos++] = ']';
    buf[pos++] = ' ';

    // Format the message into the remaining buffer space
    va_list args_copy;
    va_copy(args_copy, args);
    detail::vformat_to_buf(buf + pos, kLogBufSize - pos, fmt, args_copy);
    va_end(args_copy);

    // Dispatch to all sinks
    for (int i = 0; i < sink_count_; ++i) {
        if (sinks_[i].fn) {
            sinks_[i].fn(level, buf, sinks_[i].ctx);
        }
    }
}

void Logger::log(LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(level, fmt, args);
    va_end(args);
}

void Logger::debug(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(LogLevel::DEBUG, fmt, args);
    va_end(args);
}

void Logger::info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(LogLevel::INFO, fmt, args);
    va_end(args);
}

void Logger::warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(LogLevel::WARN, fmt, args);
    va_end(args);
}

void Logger::error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(LogLevel::ERROR, fmt, args);
    va_end(args);
}

}  // namespace cinux::lib
