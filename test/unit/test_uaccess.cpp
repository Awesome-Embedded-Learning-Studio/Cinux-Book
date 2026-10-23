#include <cstdint>
#include <sys/mman.h>

#include "../framework/framework.hpp"
#include "cinux/ptr.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/uaccess.hpp"
#include "test_assert.hpp"

namespace {

using cinux::syscall::uaccess::CopyFromUser;
using cinux::syscall::uaccess::CopyToUser;

/// A fixed page the tests own outright: below the user limit, above
/// anything a hosted process maps on its own, and inside every
/// sanitizer's application memory, so the plain hosts and the checked
/// hosts agree.
constexpr unsigned long long kGuestPage = 0x0000'5600'0000'0000ULL;

/// The one mapping every case below shares; null until main maps it.
unsigned char* g_page = nullptr;

/// The door's round trip, cross-direction by design: one face plants
/// the pattern, the other harvests it, so a reversed copy cannot pass
/// both ways at once.
constexpr std::uint64_t kPattern = 0x0123'4567'89AB'CDEFULL;

}  // namespace

TEST("uaccess: copy to user lands the bytes at the named address") {
    ASSERT_TRUE(CopyToUser(kGuestPage, &kPattern, sizeof(kPattern)));
    std::uint64_t seen = 0;
    for (unsigned long i = 0; i < sizeof(seen); ++i) {
        seen |= static_cast<std::uint64_t>(g_page[i]) << (8U * i);
    }
    ASSERT_EQ(seen, kPattern);
}

TEST("uaccess: copy from user brings back what copy to user planted") {
    ASSERT_TRUE(CopyToUser(kGuestPage + 32, &kPattern, sizeof(kPattern)));
    std::uint64_t harvested = 0;
    ASSERT_TRUE(CopyFromUser(&harvested, kGuestPage + 32, sizeof(harvested)));
    ASSERT_EQ(harvested, kPattern);
}

TEST("uaccess: a stretch crossing the user limit is refused both ways") {
    std::uint64_t hold = 0;
    ASSERT_FALSE(CopyFromUser(&hold, cinux::syscall::kUserAddressLimit - 4, 8));
    ASSERT_FALSE(CopyToUser(cinux::syscall::kUserAddressLimit - 4, &kPattern, 8));
}

TEST("uaccess: the null address and the kernel half are refused") {
    std::uint64_t hold = 0;
    ASSERT_FALSE(CopyFromUser(&hold, 0, 8));
    ASSERT_FALSE(CopyToUser(0, &kPattern, 8));
    ASSERT_FALSE(CopyFromUser(&hold, 0xFFFF'FFFF'8020'0000ULL, 8));
    ASSERT_FALSE(CopyToUser(0xFFFF'FFFF'8020'0000ULL, &kPattern, 8));
}

TEST("uaccess: a zero-width move succeeds without touching anything") {
    g_page[64] = 0xA5;
    ASSERT_TRUE(CopyFromUser(nullptr, kGuestPage + 64, 0));
    ASSERT_TRUE(CopyToUser(kGuestPage + 64, nullptr, 0));
    ASSERT_EQ(g_page[64], static_cast<unsigned char>(0xA5));
}

int main() {
    g_page = static_cast<unsigned char*>(
        mmap(cinux::base::PtrAt<void>(static_cast<unsigned long>(kGuestPage)), 4096,
             PROT_READ | PROT_WRITE, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (g_page != cinux::base::PtrAt<unsigned char>(static_cast<unsigned long>(kGuestPage))) {
        return 3;
    }
    const int kResult = cinux::test::RunAll();
    munmap(g_page, 4096);
    return kResult;
}
