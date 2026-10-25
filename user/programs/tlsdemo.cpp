#include <api/syscall.hpp>

namespace {

/// arch_prctl op: make addr the caller's FS base.
inline constexpr unsigned long long kArchSetFs = 0x1002;
/// arch_prctl op: report the caller's FS base through addr.
inline constexpr unsigned long long kArchGetFs = 0x1003;
/// arch_prctl op: make addr the caller's GS base.
inline constexpr unsigned long long kArchSetGs = 0x1001;
/// arch_prctl op: report the caller's GS base through addr.
inline constexpr unsigned long long kArchGetGs = 0x1004;

void put(const char* text) {
    unsigned long long length = 0;
    while (text[length] != '\0') {
        length++;
    }
    user::Write(1, text, length);
}

void put_signed(long long value) {
    char               digits[24] = {};
    unsigned long long count      = 0;
    auto               raw        = static_cast<unsigned long long>(value < 0 ? -value : value);
    if (raw == 0) {
        put("0");
        return;
    }
    while (raw != 0) {
        digits[count] = static_cast<char>('0' + (raw % 10ULL));
        count++;
        raw /= 10ULL;
    }
    if (value < 0) {
        put("-");
    }
    char out[24] = {};
    for (unsigned long long index = 0; index < count; ++index) {
        out[index] = digits[count - 1 - index];
    }
    put(out);
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" void UserMain() {
    unsigned long long probe = 0;

    put("[tls] tid=");
    put_signed(user::Gettid());
    put(" pid=");
    put_signed(user::Getpid());
    put("\n");

    unsigned long long join_word = 0;
    put("[tls] set_tid_address answers ");
    put_signed(user::SetTidAddress(reinterpret_cast<unsigned long long>(&join_word)));
    put("\n");

    put("[tls] arch_prctl(SET_FS, mine) answers ");
    put_signed(user::ArchPrctl(kArchSetFs, reinterpret_cast<unsigned long long>(&probe)));
    put("\n");

    put("[tls] arch_prctl(GET_FS) reads ");
    put_signed(user::ArchPrctl(kArchGetFs, reinterpret_cast<unsigned long long>(&probe)));
    put(" base=");
    put_signed(static_cast<long long>(probe & 0x7FFFFFFFFFFFFFFFULL));
    put("\n");

    put("[tls] arch_prctl(SET_FS, non-canonical) answers ");
    put_signed(user::ArchPrctl(kArchSetFs, 0x0000800000000000ULL));
    put(" (negative: the base was refused)\n");

    put("[tls] arch_prctl(SET_GS + GET_GS) answers ");
    put_signed(user::ArchPrctl(kArchSetGs, reinterpret_cast<unsigned long long>(&join_word)));
    put("/");
    put_signed(user::ArchPrctl(kArchGetGs, reinterpret_cast<unsigned long long>(&probe)));
    put(" base=");
    put_signed(static_cast<long long>(probe));
    put("\n");

    put("[tls] alive and addressing through the new FS base\n");
    user::Exit(0);
}
