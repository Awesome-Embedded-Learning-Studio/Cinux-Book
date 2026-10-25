#include "kernel/syscall/sys_ioctl.hpp"

#include <cstdint>

#include "cinux/byte_order.hpp"
#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/driver/tty/tty_config.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/net/ifaces.hpp"
#include "kernel/net/socket.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

namespace {

/// The interface-question words, Linux-shaped: one ifreq in, one out.
constexpr unsigned long long kSiocgifflags   = 0x8913;
constexpr unsigned long long kSiocgifaddr    = 0x8915;
constexpr unsigned long long kSiocgifdstaddr = 0x8917;
constexpr unsigned long long kSiocgifbrdaddr = 0x8919;
constexpr unsigned long long kSiocgifnetmask = 0x891b;
constexpr unsigned long long kSiocgifmetric  = 0x891d;
constexpr unsigned long long kSiocgifmtu     = 0x8921;
constexpr unsigned long long kSiocgifhwaddr  = 0x8927;
constexpr unsigned long long kSiocgiftxqlen  = 0x8942;

/// The interface flag words the kernel answers: everything it serves
/// is up and running; the loopback adds its own kind, the wire broadcast.
constexpr unsigned short kIffUp        = 0x1;
constexpr unsigned short kIffBroadcast = 0x2;
constexpr unsigned short kIffLoopback  = 0x8;
constexpr unsigned short kIffRunning   = 0x40;

/// Bytes in one ifreq: sixteen of name, twenty-four of answer.
constexpr unsigned long kIfreqBytes   = 40;
/// The default transmit queue length the kernel books for any wire.
constexpr unsigned int  kIfTxQueueLen = 1000;

/// The one enumeration question: an ifconf is a length and a buffer,
/// and the answer is one ifreq per interface, address family included.
constexpr unsigned long long kSiocgifconf = 0x8912;
/// Bytes in one ifconf: a four-byte length, padding, an eight-byte buffer word.
constexpr unsigned long      kIfconfBytes = 16;

/// One interface enumeration over one socket: one ifreq per seat.
long long answer_ifconf(Word argument) {
    unsigned char conf[kIfconfBytes] = {};
    if (!uaccess::CopyFromUser(conf, argument, kIfconfBytes)) {
        return -kEfault;
    }
    int  have   = 0;
    Word buffer = 0;
    base::CopyBytes(&have, conf, sizeof(have));
    base::CopyBytes(&buffer, conf + 8, sizeof(buffer));
    if (buffer == 0) {
        return -kEfault;
    }

    unsigned long written = 0;
    for (unsigned long ordinal = 0;; ++ordinal) {
        char name[net::kIfNameMax] = {};
        if (!net::NameInterfaceAt(ordinal, name)) {
            break;
        }
        net::InterfaceFacts facts{};
        if (!net::ReadInterfaceFacts(name, facts)) {
            break;
        }
        if (have >= 0 && written + kIfreqBytes > static_cast<unsigned long>(have)) {
            break;
        }
        unsigned char request[kIfreqBytes] = {};
        for (unsigned long index = 0; name[index] != '\0'; ++index) {
            request[index] = static_cast<unsigned char>(name[index]);
        }
        const auto kFamily = static_cast<uint16_t>(net::kAfInet);
        base::CopyBytes(request + 16, &kFamily, sizeof(kFamily));
        base::WriteWord32Be(request, 20, facts.address);
        if (!uaccess::CopyToUser(buffer + written, request, kIfreqBytes)) {
            return -kEfault;
        }
        written += kIfreqBytes;
    }
    base::CopyBytes(conf, &written,
                    sizeof(written) < sizeof(have) ? sizeof(written) : sizeof(have));
    base::SetBytes(conf + 4, 0, 4);
    if (!uaccess::CopyToUser(argument, conf, kIfconfBytes)) {
        return -kEfault;
    }
    return 0;
}

/// One interface question over one socket: find the roll, fill the ifreq.
long long socket_if_ioctl(unsigned long long word, Word argument) {
    if (word == kSiocgifconf) {
        return answer_ifconf(argument);
    }
    if (word == kSiocgifdstaddr) {
        return -kEnotty;
    }
    unsigned char request[kIfreqBytes] = {};
    if (!uaccess::CopyFromUser(request, argument, kIfreqBytes)) {
        return -kEfault;
    }
    request[kIfreqBytes - 1] = '\0';
    net::InterfaceFacts facts{};
    if (!net::ReadInterfaceFacts(reinterpret_cast<const char*>(request), facts)) {
        return -kEnodev;
    }
    const bool kLoopbackKind = facts.hardware_type == net::kHardwareLoopback;

    base::SetBytes(request + 16, 0, kIfreqBytes - 16);
    if (word == kSiocgifflags) {
        unsigned short flags = kIffUp | kIffRunning;
        flags |= kLoopbackKind ? kIffLoopback : kIffBroadcast;
        base::CopyBytes(request + 16, &flags, sizeof(flags));
    } else if (word == kSiocgifaddr || word == kSiocgifnetmask || word == kSiocgifbrdaddr) {
        uint32_t chosen = facts.broadcast;
        if (word == kSiocgifaddr) {
            chosen = facts.address;
        } else if (word == kSiocgifnetmask) {
            chosen = facts.netmask;
        }
        const auto kFamily = static_cast<uint16_t>(net::kAfInet);
        base::CopyBytes(request + 16, &kFamily, sizeof(kFamily));
        base::WriteWord32Be(request, 20, chosen);
    } else if (word == kSiocgifhwaddr) {
        const auto kKind = facts.hardware_type;
        base::CopyBytes(request + 16, &kKind, sizeof(kKind));
        base::CopyBytes(request + 18, facts.hardware.bytes, 6);
    } else if (word == kSiocgifmtu) {
        const unsigned int kMtu = facts.mtu;
        base::CopyBytes(request + 16, &kMtu, sizeof(kMtu));
    } else if (word == kSiocgifmetric) {
        const unsigned int kZero = 0;
        base::CopyBytes(request + 16, &kZero, sizeof(kZero));
    } else if (word == kSiocgiftxqlen) {
        base::CopyBytes(request + 16, &kIfTxQueueLen, sizeof(kIfTxQueueLen));
    } else {
        return -kEnotty;
    }
    if (!uaccess::CopyToUser(argument, request, kIfreqBytes)) {
        return -kEfault;
    }
    return 0;
}

/// One pointer-carrying command's span: how many bytes, and whether
/// the caller hands them over or takes them back. Zero bytes marks the
/// wordless commands, whose argument is a flag, not an address.
struct WordSpan {
    unsigned long long word;
    unsigned long      bytes;
    bool               caller_gives;
};

const WordSpan kWordSpans[] = {
    {.word = driver::kTtyTcgets2, .bytes = driver::kTermios2Bytes, .caller_gives = false},
    {.word = driver::kTtyTcsets2, .bytes = driver::kTermios2Bytes, .caller_gives = true},
    {.word = driver::kTtyTcsetsw2, .bytes = driver::kTermios2Bytes, .caller_gives = true},
    {.word = driver::kTtyTcsetsf2, .bytes = driver::kTermios2Bytes, .caller_gives = true},
    {.word = driver::kTtyTcgets, .bytes = driver::kTermiosBytes, .caller_gives = false},
    {.word = driver::kTtyTcsets, .bytes = driver::kTermiosBytes, .caller_gives = true},
    {.word = driver::kTtyTcsetsw, .bytes = driver::kTermiosBytes, .caller_gives = true},
    {.word = driver::kTtyTcsetsf, .bytes = driver::kTermiosBytes, .caller_gives = true},
    {.word = driver::kTtyTiocsctty, .bytes = 0, .caller_gives = true},
    {.word = driver::kTtyTiocgpgrp, .bytes = 4, .caller_gives = false},
    {.word = driver::kTtyTiocspgrp, .bytes = 4, .caller_gives = true},
    {.word = driver::kTtyTiocgwinsz, .bytes = 8, .caller_gives = false},
    {.word = driver::kTtyTiocsptlck, .bytes = 4, .caller_gives = true},
    {.word = driver::kTtyTiocgptn, .bytes = 4, .caller_gives = false},
};

/// The span one command's argument carries, or nullptr when unknown.
const WordSpan* span_of(unsigned long long word) {
    for (const WordSpan& span : kWordSpans) {
        if (span.word == word) {
            return &span;
        }
    }
    return nullptr;
}

}  // namespace

long long HandleIoctl(const SyscallFrame& frame) {
    const arch::IrqGuard                           kGuard;
    const base::Result<base::Shared<fs::OpenFile>> kSlot = LiveSlot(frame.rdi);
    if (!kSlot.ok()) {
        return -kEbadf;
    }
    const fs::OpenFile* const kFile = kSlot.value().get();
    if (kFile->ops == net::SocketOps()) {
        return socket_if_ioctl(frame.rsi, frame.rdx);
    }
    if (kFile->ops->ioctl == nullptr) {
        return -kEnotty;
    }
    const WordSpan* const kSpan = span_of(frame.rsi);
    if (kSpan == nullptr) {
        return -kEnotty;
    }
    if (kSpan->bytes == 0) {
        return kFile->ops->ioctl(kFile->backend, kFile->node, frame.rsi, frame.rdx);
    }
    unsigned char stage[driver::kTermios2Bytes];
    cinux::base::SetBytes(stage, 0, sizeof(stage));
    if (kSpan->caller_gives && !uaccess::CopyFromUser(stage, frame.rdx, kSpan->bytes)) {
        return -kEfault;
    }
    const auto      kStageWord = reinterpret_cast<unsigned long long>(stage);
    const long long kAnswer = kFile->ops->ioctl(kFile->backend, kFile->node, frame.rsi, kStageWord);
    if (kAnswer >= 0 && !kSpan->caller_gives &&
        !uaccess::CopyToUser(frame.rdx, stage, kSpan->bytes)) {
        return -kEfault;
    }
    return kAnswer;
}

}  // namespace cinux::syscall
