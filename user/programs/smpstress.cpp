#include <api/syscall.hpp>
#include <array>
#include <initializer_list>

#include "cinux/bit_ops/bitmask.hpp"
#include "cinux/format.hpp"
#include "cinux/literal_types.hpp"
#include "kernel/syscall/syscall.hpp"

namespace {
using cinux::base::operator""_KiB;
constexpr auto kCowPageBytes = 4_KiB;
alignas(kCowPageBytes)
    std::array<volatile unsigned long, kCowPageBytes / sizeof(unsigned long)> g_cow_page{};
constexpr cinux::base::bit::BitRange kExitStatus{.low = 8, .width = 8};
constexpr cinux::base::bit::BitRange kWaitSignal{.low = 0, .width = 8};
constexpr auto                       kBothCpus =
    cinux::base::bit::MaskBit<unsigned int>(0) | cinux::base::bit::MaskBit<unsigned int>(1);

void write_cow_page(unsigned long value) {
    for (unsigned long index = 0; index < g_cow_page.size(); ++index) {
        g_cow_page[index] = value + index;
    }
}

bool cow_page_matches(unsigned long value) {
    for (unsigned long index = 0; index < g_cow_page.size(); ++index) {
        if (g_cow_page[index] != value + index) {
            return false;
        }
    }
    return true;
}

unsigned int cpu_id() {
    unsigned int eax;
    unsigned int ebx;
    unsigned int ecx;
    unsigned int edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    return ebx >> 24U;
}

[[noreturn]] void fail(const char* stage, unsigned long round, long long status) {
    char                           message[160]{};
    const cinux::base::format::Arg kArgs[] = {
        cinux::base::format::Str(stage),
        cinux::base::format::NumU(round),
        cinux::base::format::NumS(status),
    };
    cinux::base::format::FormatToBuf(message, sizeof(message),
                                     "SMP_FORK_FAIL stage=%s round=%llu status=%lld\n",
                                     cinux::base::format::ArgsView::of(kArgs));
    for (const char& glyph : message) {
        if (glyph == '\0') {
            break;
        }
        user::Write(1, &glyph, 1);
    }
    user::Exit(1);
}

void check_exit_eof() {
    unsigned int descriptors[2]{};
    if (user::Syscall3(static_cast<unsigned long long>(cinux::syscall::SyscallNr::kPipe2),
                       reinterpret_cast<unsigned long long>(descriptors), 0, 0) != 0) {
        fail("pipe", 0, 0);
    }
    const auto kWriter = user::Fork();
    if (kWriter < 0) {
        fail("pipe-writer", 0, kWriter);
    }
    if (kWriter == 0) {
        user::Close(descriptors[0]);
        constexpr char kPayload[] = "ok";
        if (user::Write(descriptors[1], kPayload, 2) != 2) {
            user::Exit(1);
        }
        user::Exit(0);
    }
    const auto kReader = user::Fork();
    if (kReader < 0) {
        fail("pipe-reader", 0, kReader);
    }
    if (kReader == 0) {
        user::Close(descriptors[1]);
        char          heard[3]{};
        unsigned long used = 0;
        while (used < 2) {
            const auto kTaken = user::Read(descriptors[0], heard + used, 2 - used);
            if (kTaken <= 0) {
                user::Exit(2);
            }
            used += static_cast<unsigned long>(kTaken);
        }
        if (heard[0] != 'o' || heard[1] != 'k' || user::Read(descriptors[0], heard + 2, 1) != 0) {
            user::Exit(3);
        }
        user::Exit(0);
    }
    user::Close(descriptors[0]);
    user::Close(descriptors[1]);
    for (const auto kPid : {kReader, kWriter}) {
        int status = 0;
        if (user::Wait4(kPid, &status) != kPid || status != 0) {
            fail("pipe-wait-reader-first", 0, status);
        }
    }
    constexpr char kSuccess[] = "SMP_PIPE_EOF_PASS\n";
    user::Write(1, kSuccess, sizeof(kSuccess) - 1);
}
}

extern "C" void UserMain() {
    check_exit_eof();
    unsigned int seen = 0;
    for (unsigned long round = 1; round <= 96; ++round) {
        write_cow_page(round);
        const long long kChild = user::Fork();
        if (kChild < 0) {
            fail("fork", round, kChild);
        }
        if (kChild == 0) {
            if (!cow_page_matches(round)) {
                user::Exit(100);
            }
            write_cow_page(round + 1000);
            for (unsigned int turn = 0; turn < 32; ++turn) {
                user::Yield();
            }
            if (!cow_page_matches(round + 1000)) {
                user::Exit(101);
            }
            user::Exit(static_cast<int>(cpu_id() + 1));
        }
        write_cow_page(round + 2000);
        int                                           status  = 0;
        const auto                                    kWaited = user::Wait4(kChild, &status);
        const cinux::base::bit::BitMask<unsigned int> kStatus{static_cast<unsigned int>(status)};
        const unsigned int                            kCode = kStatus.extract(kExitStatus);
        if (kWaited != kChild || kStatus.extract(kWaitSignal) != 0 || kCode < 1 || kCode > 8) {
            fail("wait-child", round, status);
        }
        seen |= cinux::base::bit::MaskBit<unsigned int>(static_cast<unsigned char>(kCode - 1)).raw;
        const unsigned int kCpu = cpu_id();
        if (kCpu >= 8) {
            fail("cpu-id", round, kCpu);
        }
        seen |= cinux::base::bit::MaskBit<unsigned int>(static_cast<unsigned char>(kCpu)).raw;
        if (!cow_page_matches(round + 2000)) {
            fail("parent-cow", round, static_cast<long long>(g_cow_page[0]));
        }
    }
    if ((cinux::base::bit::BitMask<unsigned int>{seen} & kBothCpus).raw != kBothCpus.raw) {
        fail("cpu-mask", 96, seen);
    }
    constexpr char kSuccess[] = "SMP_FORK_PASS cpus=2 rounds=96\n";
    user::Write(1, kSuccess, sizeof(kSuccess) - 1);
    user::Exit(0);
}
