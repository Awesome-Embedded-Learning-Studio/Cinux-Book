#include <api/syscall.hpp>

#include "kernel/syscall/syscall.hpp"

namespace {

constexpr unsigned long long kTcgets      = 0x5401;
constexpr unsigned long long kTcsets      = 0x5402;
constexpr unsigned long      kRecordBytes = 36;
constexpr unsigned long      kLflagAt     = 12;
constexpr unsigned int       kEchoBit     = 0x0008;

void put(const char* text) {
    unsigned long long length = 0;
    while (text[length] != '\0') {
        length++;
    }
    user::Write(1, text, length);
}

long long fetch_flags(unsigned char* record) {  // NOLINT(misc-const-correctness)
    return user::Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kIoctl), 0,
                          kTcgets, reinterpret_cast<unsigned long long>(record));
}

long long push_flags(const unsigned char* record) {
    return user::Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kIoctl), 0,
                          kTcsets, reinterpret_cast<unsigned long long>(record));
}

unsigned int word_at(const unsigned char* record, unsigned long seat) {
    unsigned int word = 0;
    for (unsigned long index = 0; index < 4; ++index) {
        word |= static_cast<unsigned int>(record[seat + index]) << (8U * index);
    }
    return word;
}

void put_word_at(unsigned char* record, unsigned long seat, unsigned int word) {
    for (unsigned long index = 0; index < 4; ++index) {
        record[seat + index] = static_cast<unsigned char>(word >> (8U * index));
    }
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" void _start() {
    unsigned char record[kRecordBytes] = {};
    if (fetch_flags(record) != 0) {
        put("ttydemo: TCGETS refused\n");
        user::Exit(1);
    }
    put("ttydemo: echo is on, turning it off for the next line\n");

    unsigned char quiet[kRecordBytes] = {};
    for (unsigned long index = 0; index < kRecordBytes; ++index) {
        quiet[index] = record[index];
    }
    put_word_at(quiet, kLflagAt, word_at(quiet, kLflagAt) & ~kEchoBit);
    if (push_flags(quiet) != 0) {
        put("ttydemo: TCSETS refused\n");
        user::Exit(1);
    }

    put("ttydemo: type a secret line, it will not echo: ");
    unsigned char   heard[128] = {};
    const long long kGot       = user::Read(0, heard, sizeof(heard) - 1);
    put("\n");
    put("ttydemo: heard: ");
    if (kGot > 0) {
        user::Write(1, heard, static_cast<unsigned long long>(kGot));
    }
    put("ttydemo: echo restored\n");
    push_flags(record);
    user::Exit(0);
}
