#include "kernel/syscall/sys_ioctl.hpp"

#include "cinux/memory.hpp"
#include "kernel/syscall/syscall.hpp"

namespace cinux::syscall {

namespace {

/// The hosted "read the terminal settings" ask.
constexpr unsigned long long kTcgets = 0x5401;

/// The hosted "how big is the terminal" ask: what isatty really probes.
constexpr unsigned long long kTiocgwinsz = 0x5413;

/// A termios record as the hosted world sizes it: four word-sized
/// fields plus two control strings, all zero from a console that has
/// never been configured.
constexpr unsigned long kTermiosBytes = 36;

/// The screen console's own shape, in rows then columns, as the winsize
/// record spells it: two shorts of cells, two of pixels.
struct WinSize {
    unsigned short rows;
    unsigned short columns;
    unsigned short pixels_x;
    unsigned short pixels_y;
};

/// The console the boot banner already names: 48 rows of 128 columns.
constexpr WinSize kConsoleShape{.rows = 48, .columns = 128, .pixels_x = 0, .pixels_y = 0};

}  // namespace

long long HandleIoctl(const SyscallFrame& frame) {
    if (frame.rdi > 2) {
        return -kEnotty;
    }
    if (frame.rdx == 0 || frame.rdx >= kUserAddressLimit) {
        return -kEfault;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const kOut = reinterpret_cast<unsigned char*>(frame.rdx);
    if (frame.rsi == kTcgets) {
        base::SetBytes(kOut, 0, kTermiosBytes);
        return 0;
    }
    if (frame.rsi == kTiocgwinsz) {
        base::CopyBytes(kOut, &kConsoleShape, sizeof(kConsoleShape));
        return 0;
    }
    return -kEnotty;
}

}  // namespace cinux::syscall
