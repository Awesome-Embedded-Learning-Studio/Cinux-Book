/**
 * @file kernel/lib/random.cpp
 * @brief KRandom implementation — entropy mix + xoshiro256** (F9 batch 7)
 */

#include "kernel/lib/random.hpp"

#include "kernel/drivers/pit/pit.hpp"  // PIT::get_ticks entropy
#include "kernel/lib/kprintf.hpp"

namespace cinux::lib {

namespace {

// xoshiro256** (Blackman & Vigna): fast, high-quality, 2^256 period.
inline uint64_t rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

uint64_t xoshiro256starstar(uint64_t s[4]) {
    const uint64_t result = rotl(s[1] * 5, 7) * 9;
    const uint64_t t      = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
}

// CPUID.01H:ECX[30] = rdrand support.
bool has_rdrand() {
    uint32_t eax = 1, ebx = 0, ecx = 0, edx = 0;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    return (ecx & (1u << 30)) != 0;
}

// rdrand with retry (the instruction can transiently underflow). 10 retries.
bool rdrand64(uint64_t* out) {
    for (int i = 0; i < 10; ++i) {
        uint64_t      v;
        unsigned char ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) : : "cc");
        if (ok) {
            *out = v;
            return true;
        }
    }
    return false;
}

uint64_t rdtsc() {
    uint32_t hi, lo;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

}  // namespace

void KRandom::init() {
    // Mix entropy sources: TSC (cycle count), PIT ticks (boot timing), the
    // kernel image's own address (KASLR-ish), and rdrand if the CPU has it.
    uint64_t seed = rdtsc();
    seed ^= cinux::drivers::PIT::get_ticks() << 16;
    seed ^= reinterpret_cast<uint64_t>(&g_random) >> 4;  // defeat a fixed guess
    const bool rdr = has_rdrand();
    if (rdr) {
        uint64_t r;
        if (rdrand64(&r)) {
            seed ^= r;
        }
    }

    // splitmix64: expand the 64-bit seed into a non-zero 256-bit xoshiro state
    // (an all-zero state would make xoshiro stuck).
    uint64_t z = seed != 0 ? seed : 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 4; ++i) {
        z += 0x9E3779B97F4A7C15ULL;
        uint64_t t = z;
        t          = (t ^ (t >> 30)) * 0xBF58476D1CE4E5B9ULL;
        t          = (t ^ (t >> 27)) * 0x94D049BB133111EBULL;
        state_[i]  = t ^ (t >> 31);
    }
    initialized_ = true;
    kprintf("[KRANDOM] seeded (rdrand=%d)\n", static_cast<int>(rdr));
}

uint32_t KRandom::next32() {
    if (!initialized_) {
        init();
    }
    return static_cast<uint32_t>(xoshiro256starstar(state_) >> 32);
}

uint64_t KRandom::next64() {
    if (!initialized_) {
        init();
    }
    return xoshiro256starstar(state_);
}

void KRandom::fill(void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    while (len >= 8) {
        *reinterpret_cast<uint64_t*>(p) = next64();
        p += 8;
        len -= 8;
    }
    if (len > 0) {
        const uint64_t v = next64();
        for (size_t i = 0; i < len; ++i) {
            p[i] = static_cast<uint8_t>(v >> (i * 8));
        }
    }
}

KRandom g_random;

}  // namespace cinux::lib
