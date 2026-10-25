#include <api/syscall.hpp>

#include "kernel/arch/x86_64/context.hpp"
#include "kernel/syscall/syscall.hpp"

namespace {

constexpr unsigned long long kPageSize     = 4096;
constexpr unsigned long long kAnonPages    = 3;
constexpr unsigned long long kFirstXmmWord = 20;
constexpr unsigned long long kProbeWords   = 16;

cinux::arch::FpuImage probe_image() {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    cinux::arch::FpuImage image;
    cinux::arch::SaveFpu(image);
    for (unsigned long long index = 0; index < kProbeWords; ++index) {
        image.words[kFirstXmmWord + index] = 0xA11CE000F00D0000ULL + index;
    }
    return image;
}

bool probe_matches(const cinux::arch::FpuImage& expected, const cinux::arch::FpuImage& actual) {
    for (unsigned long long index = 0; index < kProbeWords; ++index) {
        if (expected.words[kFirstXmmWord + index] != actual.words[kFirstXmmWord + index]) {
            return false;
        }
    }
    return true;
}

bool probe_file_fault(const unsigned char* address) {
    const cinux::arch::FpuImage kExpected = probe_image();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init,misc-const-correctness)
    cinux::arch::FpuImage       actual;
    asm volatile("fxrstor64 %[expected]\n\tmovb (%[address]), %%al\n\tfxsave64 %[actual]"
                 : [actual] "=m"(actual)
                 : [expected] "m"(kExpected), [address] "r"(address)
                 : "rax", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8",
                   "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory");
    return probe_matches(kExpected, actual);
}

struct SyscallProbe {
    long long result;
    bool      preserved;
};

SyscallProbe probe_syscall(cinux::syscall::SyscallNr number) {
    const cinux::arch::FpuImage kExpected = probe_image();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init,misc-const-correctness)
    cinux::arch::FpuImage       actual;
    long long                   result;
    asm volatile("fxrstor64 %[expected]\n\tsyscall\n\tfxsave64 %[actual]"
                 : [actual] "=m"(actual), "=a"(result)
                 : [expected] "m"(kExpected), "a"(static_cast<unsigned long long>(number)),
                   "D"(0ULL), "S"(0ULL), "d"(0ULL)
                 : "rcx", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
                   "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory");
    return {.result = result, .preserved = probe_matches(kExpected, actual)};
}

unsigned long long text_len(const char* text) {
    unsigned long long length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

void put(const char* text) {
    static_cast<void>(user::Write(1, text, text_len(text)));
}

void report(const char* step, bool passed) {
    put(step);
    put(passed ? ": ok\n" : ": FAIL\n");
    if (!passed) {
        user::Exit(1);
    }
}

unsigned long long descend(unsigned long long depth, unsigned long long seed) {
    volatile unsigned long long frame[64];
    frame[0] = seed;
    for (unsigned long long index = 1; index < 64; ++index) {
        frame[index] = ((index - 1) * 3ULL) + 1ULL;
    }
    if (depth == 0) {
        return frame[63];
    }
    return descend(depth - 1, frame[63]);
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" void UserMain() {
    const long long kSpot = user::Mmap(0, kAnonPages * kPageSize,
                                       cinux::syscall::kProtRead | cinux::syscall::kProtWrite,
                                       cinux::syscall::kMapAnonymous, 0, 0);
    report("anon mmap", kSpot > 0);
    // NOLINTNEXTLINE(performance-no-int-to-ptr) the kernel hands back addresses as numbers
    auto* mem = reinterpret_cast<unsigned char*>(kSpot);
    for (unsigned long long page = 0; page < kAnonPages; ++page) {
        mem[(page * kPageSize) + 123] = static_cast<unsigned char>('A' + page);
    }
    bool anon_roundtrip = true;
    for (unsigned long long page = 0; page < kAnonPages; ++page) {
        anon_roundtrip = anon_roundtrip &&
                         mem[(page * kPageSize) + 123] == static_cast<unsigned char>('A' + page);
    }
    report("anon write+read back", anon_roundtrip);
    report("mprotect read-only",
           user::Mprotect(static_cast<unsigned long long>(kSpot), kAnonPages * kPageSize,
                          cinux::syscall::kProtRead) == 0);
    bool anon_readable = true;
    for (unsigned long long page = 0; page < kAnonPages; ++page) {
        anon_readable = anon_readable &&
                        mem[(page * kPageSize) + 123] == static_cast<unsigned char>('A' + page);
    }
    report("read after mprotect", anon_readable);
    report("munmap",
           user::Munmap(static_cast<unsigned long long>(kSpot), kAnonPages * kPageSize) == 0);

    const long long kBreakNow = user::Brk(0);
    report("brk ask", kBreakNow > 0);
    const long long kBreakWant = kBreakNow + (5 * static_cast<long long>(kPageSize));
    report("brk stretch", user::Brk(static_cast<unsigned long long>(kBreakWant)) == kBreakWant);
    // NOLINTNEXTLINE(performance-no-int-to-ptr) the break speaks in addresses, not pointers
    auto* heap = reinterpret_cast<unsigned char*>(kBreakNow);
    for (unsigned long long index = 0; index < (5 * kPageSize); index += kPageSize / 2) {
        heap[index] = static_cast<unsigned char>(index & 0xFFULL);
    }
    bool heap_roundtrip = true;
    for (unsigned long long index = 0; index < (5 * kPageSize); index += kPageSize / 2) {
        heap_roundtrip =
            heap_roundtrip && heap[index] == static_cast<unsigned char>(index & 0xFFULL);
    }
    report("heap pages on demand", heap_roundtrip);
    report("brk restore", user::Brk(static_cast<unsigned long long>(kBreakNow)) == kBreakNow);

    const long long kFd = user::Open("/etc/motd", 0);
    report("open motd", kFd > 0);
    const long long kFileSpot = user::Mmap(0, kPageSize, cinux::syscall::kProtRead, 0,
                                           static_cast<unsigned long long>(kFd), 0);
    report("file mmap", kFileSpot > 0);
    // NOLINTNEXTLINE(performance-no-int-to-ptr) the kernel hands back addresses as numbers
    const auto* mapped = reinterpret_cast<const unsigned char*>(kFileSpot);
    report("xmm0-xmm7 survive file fault", probe_file_fault(mapped));
    unsigned char   via_read[64] = {};
    const long long kGot         = user::Read(static_cast<unsigned long long>(kFd), via_read, 63);
    report("read motd", kGot > 0);
    bool file_same = true;
    for (unsigned long long index = 0; index < 40; ++index) {
        file_same = file_same && mapped[index] == via_read[index];
    }
    report("mapping agrees with read", file_same);
    report("close motd", user::Close(static_cast<unsigned long long>(kFd)) == 0);

    const SyscallProbe kYield = probe_syscall(cinux::syscall::SyscallNr::kYield);
    report("xmm0-xmm7 survive syscall", kYield.result == 0 && kYield.preserved);
    const SyscallProbe kFork = probe_syscall(cinux::syscall::SyscallNr::kFork);
    if (kFork.result == 0) {
        report("child inherits xmm0-xmm7", kFork.preserved);
        user::Exit(0);
    }
    report("parent retains xmm0-xmm7", kFork.result > 0 && kFork.preserved);
    int status = -1;
    report("wait for xmm child", user::Wait4(kFork.result, &status) == kFork.result && status == 0);

    const unsigned long long kDeep = descend(256, 1);
    put("deep stack press: ok\n");
    static_cast<void>(kDeep);

    put("mapper: all green\n");
    user::Exit(7);
}
