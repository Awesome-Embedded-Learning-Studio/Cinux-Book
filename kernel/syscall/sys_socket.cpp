#include "kernel/syscall/sys_socket.hpp"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>

#include "cinux/byte_order.hpp"
#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/fs/file_table.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_world.hpp"
#include "kernel/net/socket.hpp"
#include "kernel/net/udp.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/sys_clock.hpp"
#include "kernel/syscall/syscall.hpp"
#include "kernel/syscall/syscall_internal.hpp"
#include "kernel/syscall/uaccess.hpp"

namespace cinux::syscall {

namespace {

constexpr unsigned long kSockaddrInBytes  = 16;
constexpr unsigned long kSockaddrMinBytes = 8;
constexpr unsigned long kFamilyAt         = 0;
constexpr unsigned long kPortAt           = 2;
constexpr unsigned long kAddressAt        = 4;

constexpr unsigned long long kSockCloexec  = 0x80000;
constexpr unsigned long long kSockNonblock = 0x800;

/// Finds the socket behind a descriptor, or answers why not.
long long socket_of(Word descriptor, net::Socket** out, base::Shared<fs::OpenFile>& held) {
    *out             = nullptr;
    const auto kOpen = LiveSlot(descriptor);
    if (!kOpen.ok()) {
        return -kEbadf;
    }
    if (kOpen.value()->ops != net::SocketOps()) {
        return -kEnotsock;
    }
    held = kOpen.value();
    *out = static_cast<net::Socket*>(held->backend);
    return 0;
}

/// Reads one internet address from ring 3, wire byte order and all.
long long read_sockaddr(Word address, Word length, net::SocketAddress& out) {
    if (address == 0 || length < kSockaddrMinBytes) {
        return -kEinval;
    }
    unsigned char record[kSockaddrInBytes] = {};
    const Word    kBytes                   = length < kSockaddrInBytes ? length : kSockaddrInBytes;
    if (!uaccess::CopyFromUser(record, address, kBytes)) {
        return -kEfault;
    }
    uint16_t family = 0;
    base::CopyBytes(&family, record + kFamilyAt, sizeof(family));
    if (family != net::kAfInet) {
        return -kEafnosupport;
    }
    out.port    = base::ReadWord16Be(record, kPortAt);
    out.address = base::ReadWord32Be(record, kAddressAt);
    return 0;
}

/// Writes one internet address back to ring 3 and reports its length.
long long write_sockaddr(Word address, Word length_word, const net::SocketAddress& from) {
    if (address == 0 || length_word == 0) {
        return -kEfault;
    }
    uint32_t wanted = 0;
    if (!uaccess::CopyFromUser(&wanted, length_word, sizeof(wanted))) {
        return -kEfault;
    }
    unsigned char record[kSockaddrInBytes] = {};
    const auto    kFamily                  = static_cast<uint16_t>(net::kAfInet);
    base::CopyBytes(record + kFamilyAt, &kFamily, sizeof(kFamily));
    base::WriteWord16Be(record, kPortAt, from.port);
    base::WriteWord32Be(record, kAddressAt, from.address);
    const uint32_t kWhole = kSockaddrInBytes;
    const auto     kBytes = wanted < kWhole ? wanted : kWhole;
    if (!uaccess::CopyToUser(address, record, kBytes)) {
        return -kEfault;
    }
    if (!uaccess::CopyToUser(length_word, &kWhole, sizeof(kWhole))) {
        return -kEfault;
    }
    return 0;
}

/// Translates the socket family's own refusals into errno words.
/// One send call's byte ceiling, by the socket's birth type.
unsigned long send_ceiling_of(int socket_type) {
    if (socket_type == net::kSockRaw) {
        return net::kMaxDatagramBytes - net::kMinIpv4HeaderBytes;
    }
    if (socket_type == net::kSockStream) {
        return net::kTcpMssBytes;
    }
    return net::kUdpSocketPayloadBytes;
}

long long errno_of_socket(base::KernelError error) {
    switch (error) {
    case base::KernelError::kAlreadyExists:
        return -kEaddrinuse;
    case base::KernelError::kNotFound:
        return -kEdestaddrreq;
    case base::KernelError::kNotSupported:
        return -kEmsgsize;
    case base::KernelError::kNotConnected:
        return -kEnotconn;
    default:
        return -ErrnoFrom(error);
    }
}

/// Whether the calling task has a deliverable signal waiting.
bool signal_pending() {
    const proc::Task* const kSelf = CurrentTask();
    return proc::HasPendingSignal(*kSelf);
}

/// Seats one accepted socket behind a fresh descriptor, polling while
/// the queue stays empty; the caller's address question is answered
/// when room for it was named.
long long accept_common(const SyscallFrame& frame, Word extra_flags) {
    base::Shared<fs::OpenFile> held;
    net::Socket*               socket = nullptr;
    const long long            kFound = socket_of(frame.rdi, &socket, held);
    if (kFound != 0) {
        return kFound;
    }
    base::Shared<net::Socket> born;
    for (;;) {
        const auto kTaken = socket->accept_one();
        if (kTaken.ok()) {
            born = kTaken.value();
            break;
        }
        if (kTaken.error() != base::KernelError::kWouldBlock) {
            return kTaken.error() == base::KernelError::kNotSupported
                       ? -kEopnotsupp
                       : errno_of_socket(kTaken.error());
        }
        // accept4 flags apply to the new descriptor. Only the listener's
        // O_NONBLOCK decides whether an empty queue may wait here.
        if ((held->status_flags & kOpenNonblock) != 0) {
            return -kEagain;
        }
        if (signal_pending()) {
            return -kEintr;
        }
        proc::Scheduler::self().yield();
    }
    const fs::FsNode kNode = {
        .opaque = born.get(),
        .type   = fs::InodeType::kFile,
        .ino    = born->identity(),
        .size   = 0,
    };
    const auto kSeat = CurrentTask()->files->alloc(
        net::SocketOps(), born.get(), kNode,
        {
            .status  = (extra_flags & kSockNonblock) != 0 ? kOpenNonblock : 0,
            .cloexec = (extra_flags & kSockCloexec) != 0,
        });
    if (!kSeat.ok()) {
        return -kEmfile;
    }
    if (frame.rsi != 0) {
        net::SocketAddress peer{};
        if (born->report_peer(peer).ok()) {
            write_sockaddr(frame.rsi, frame.rdx, peer);
        }
    }
    return static_cast<long long>(kSeat.value());
}

}  // namespace

long long HandleSocket(const SyscallFrame& frame) {
    if (frame.rdi != static_cast<Word>(net::kAfInet)) {
        return -kEafnosupport;
    }
    const Word kRawType   = frame.rsi;
    const Word kFlags     = kRawType & (kSockCloexec | kSockNonblock);
    const Word kPlainType = kRawType & ~(kSockCloexec | kSockNonblock);
    const auto kProtocol  = static_cast<Word>(frame.rdx);

    base::Shared<net::Socket> socket;
    if (kPlainType == static_cast<Word>(net::kSockStream)) {
        if (kProtocol != 0 && kProtocol != static_cast<Word>(net::kIpv4ProtocolTcp)) {
            return -kEprotonosupport;
        }
        socket = base::Shared<net::Socket>{new net::TcpSocket(net::NetWorld::self().tcp())};
    } else if (kPlainType == static_cast<Word>(net::kSockDgram)) {
        if (kProtocol != 0 && kProtocol != static_cast<Word>(net::kIpv4ProtocolUdp)) {
            return -kEprotonosupport;
        }
        socket = base::Shared<net::Socket>{new net::UdpSocket(net::NetWorld::self().udp())};
    } else if (kPlainType == static_cast<Word>(net::kSockRaw)) {
        if (CurrentTask()->euid != 0) {
            return -kEperm;
        }
        if (kProtocol != kIpProtoIcmp) {
            return -kEprotonosupport;
        }
        auto* raw = new net::RawSocket(net::NetWorld::self().raw());
        socket    = base::Shared<net::Socket>{raw};
        if (raw == nullptr || !raw->seated()) {
            return -kEnomem;
        }
    } else {
        return -kEsocktnosupport;
    }

    if (!socket.holds()) {
        return -kEnomem;
    }
    const fs::FsNode kNode = {
        .opaque = socket.get(),
        .type   = fs::InodeType::kFile,
        .ino    = socket->identity(),
        .size   = 0,
    };
    const auto kSeat =
        CurrentTask()->files->alloc(net::SocketOps(), socket.get(), kNode,
                                    {
                                        .status = (kFlags & kSockNonblock) != 0 ? kOpenNonblock : 0,
                                        .cloexec = (kFlags & kSockCloexec) != 0,
                                    });
    if (!kSeat.ok()) {
        return -kEmfile;
    }
    return static_cast<long long>(kSeat.value());
}

long long HandleBind(const SyscallFrame& frame) {
    base::Shared<fs::OpenFile> held;
    net::Socket*               socket = nullptr;
    const long long            kFound = socket_of(frame.rdi, &socket, held);
    if (kFound != 0) {
        return kFound;
    }
    net::SocketAddress asked{};
    const long long    kRead = read_sockaddr(frame.rsi, frame.rdx, asked);
    if (kRead != 0) {
        return kRead;
    }
    if (asked.address != 0 && asked.address != net::NetWorld::self().udp().own_address() &&
        asked.address != net::kLoopbackAddress) {
        return -kEaddrnotavail;
    }
    const auto kSeated = socket->adopt_local(asked);
    if (!kSeated.ok()) {
        return errno_of_socket(kSeated.error());
    }
    return 0;
}

long long HandleConnect(const SyscallFrame& frame) {
    base::Shared<fs::OpenFile> held;
    net::Socket*               socket = nullptr;
    const long long            kFound = socket_of(frame.rdi, &socket, held);
    if (kFound != 0) {
        return kFound;
    }
    net::SocketAddress named{};
    const long long    kRead = read_sockaddr(frame.rsi, frame.rdx, named);
    if (kRead != 0) {
        return kRead;
    }
    const auto kNamed = socket->adopt_peer(named);
    if (!kNamed.ok()) {
        return errno_of_socket(kNamed.error());
    }
    const unsigned long long kDeadline =
        MonotonicNanoseconds() + (75ULL * 1000ULL * 1000ULL * 1000ULL);
    while (socket->handshake_pending() && MonotonicNanoseconds() < kDeadline) {
        proc::Scheduler::self().yield();
    }
    if (socket->handshake_failed()) {
        return -kEconnrefused;
    }
    if (socket->handshake_pending()) {
        return -kEinprogress;
    }
    return 0;
}

long long HandleSendto(const SyscallFrame& frame) {
    base::Shared<fs::OpenFile> held;
    net::Socket*               socket = nullptr;
    const long long            kFound = socket_of(frame.rdi, &socket, held);
    if (kFound != 0) {
        return kFound;
    }
    if (frame.r10 != 0) {
        return -kEopnotsupp;
    }
    const auto          kCount = static_cast<unsigned long>(frame.rdx);
    const unsigned long kLimit = send_ceiling_of(socket->type());
    if (kCount > kLimit) {
        return -kEmsgsize;
    }
    unsigned char payload[net::kMaxDatagramBytes] = {};
    if (kCount != 0 && !uaccess::CopyFromUser(payload, frame.rsi, kCount)) {
        return -kEfault;
    }
    net::SocketAddress destination{};
    if (frame.r8 != 0) {
        const long long kRead = read_sockaddr(frame.r8, frame.r9, destination);
        if (kRead != 0) {
            return kRead;
        }
    } else {
        const auto kNamed = socket->report_peer(destination);
        if (!kNamed.ok()) {
            return -kEdestaddrreq;
        }
    }
    const auto kGiven = socket->transmit(payload, kCount, destination);
    if (!kGiven.ok()) {
        return errno_of_socket(kGiven.error());
    }
    return static_cast<long long>(kGiven.value());
}

long long HandleRecvfrom(const SyscallFrame& frame) {
    base::Shared<fs::OpenFile> held;
    net::Socket*               socket = nullptr;
    const long long            kFound = socket_of(frame.rdi, &socket, held);
    if (kFound != 0) {
        return kFound;
    }
    if (frame.r10 != 0) {
        return -kEopnotsupp;
    }
    const auto kCount = static_cast<unsigned long>(frame.rdx);
    if (kCount == 0) {
        return 0;
    }
    unsigned char       payload[net::kMaxDatagramBytes] = {};
    const unsigned long kWant = kCount < net::kMaxDatagramBytes ? kCount : net::kMaxDatagramBytes;
    for (;;) {
        net::SocketAddress from{};
        const auto         kTaken = socket->receive(payload, kWant, &from);
        if (kTaken.ok()) {
            if (!uaccess::CopyToUser(frame.rsi, payload, kTaken.value())) {
                return -kEfault;
            }
            if (frame.r8 != 0) {
                const long long kWrote = write_sockaddr(frame.r8, frame.r9, from);
                if (kWrote != 0) {
                    return kWrote;
                }
            }
            return static_cast<long long>(kTaken.value());
        }
        if (kTaken.error() != base::KernelError::kWouldBlock) {
            return errno_of_socket(kTaken.error());
        }
        if ((held->status_flags & kOpenNonblock) != 0) {
            return -kEagain;
        }
        if (signal_pending()) {
            return -kEintr;
        }
        proc::Scheduler::self().yield();
    }
}

long long HandleShutdown(const SyscallFrame& frame) {
    base::Shared<fs::OpenFile> held;
    net::Socket*               socket = nullptr;
    const long long            kFound = socket_of(frame.rdi, &socket, held);
    if (kFound != 0) {
        return kFound;
    }
    if (frame.rsi > 2) {
        return -kEinval;
    }
    if (frame.rsi != 1) {
        return -kEopnotsupp;
    }
    const auto kClosed = socket->shutdown_write();
    return kClosed.ok() ? 0 : errno_of_socket(kClosed.error());
}

long long HandleListen(const SyscallFrame& frame) {
    base::Shared<fs::OpenFile> held;
    net::Socket*               socket = nullptr;
    const long long            kFound = socket_of(frame.rdi, &socket, held);
    if (kFound != 0) {
        return kFound;
    }
    const auto kOffered = socket->begin_listen(frame.rsi);
    if (!kOffered.ok()) {
        return kOffered.error() == base::KernelError::kNotSupported
                   ? -kEopnotsupp
                   : errno_of_socket(kOffered.error());
    }
    return 0;
}


long long HandleAccept(const SyscallFrame& frame) {
    return accept_common(frame, 0);
}

long long HandleAccept4(const SyscallFrame& frame) {
    if ((frame.r10 & ~(kSockCloexec | kSockNonblock)) != 0) {
        return -kEinval;
    }
    return accept_common(frame, static_cast<Word>(frame.r10));
}

long long HandleGetsockname(const SyscallFrame& frame) {
    base::Shared<fs::OpenFile> held;
    net::Socket*               socket = nullptr;
    const long long            kFound = socket_of(frame.rdi, &socket, held);
    if (kFound != 0) {
        return kFound;
    }
    net::SocketAddress local{};
    const auto         kReported = socket->report_local(local);
    if (!kReported.ok()) {
        return errno_of_socket(kReported.error());
    }
    return write_sockaddr(frame.rsi, frame.rdx, local);
}

long long HandleGetpeername(const SyscallFrame& frame) {
    base::Shared<fs::OpenFile> held;
    net::Socket*               socket = nullptr;
    const long long            kFound = socket_of(frame.rdi, &socket, held);
    if (kFound != 0) {
        return kFound;
    }
    net::SocketAddress peer{};
    const auto         kReported = socket->report_peer(peer);
    if (!kReported.ok()) {
        return -kEnotconn;
    }
    return write_sockaddr(frame.rsi, frame.rdx, peer);
}

long long HandleSocketpair(const SyscallFrame& /*frame*/) {
    return -kEopnotsupp;
}

long long HandleSetsockopt(const SyscallFrame& frame) {
    base::Shared<fs::OpenFile> held;
    net::Socket*               socket = nullptr;
    const long long            kFound = socket_of(frame.rdi, &socket, held);
    if (kFound != 0) {
        return kFound;
    }
    if (frame.r8 < sizeof(unsigned int)) {
        return -kEinval;
    }
    unsigned int value = 0;
    if (frame.r10 == 0 || !uaccess::CopyFromUser(&value, frame.r10, sizeof(value))) {
        return -kEfault;
    }
    const auto kLevel = frame.rsi;
    const auto kName  = frame.rdx;
    if (kLevel == kSolSocket) {
        if (kName == kSoReuseaddr || kName == kSoSndbuf || kName == kSoRcvbuf ||
            kName == kSoBroadcast) {
            return 0;
        }
        return -kEnoprotoopt;
    }
    if (kLevel == kIpProtoIp && (kName == kIpTtl || kName == kIpMulticastTtl)) {
        return 0;
    }
    return -kEnoprotoopt;
}

long long HandleGetsockopt(const SyscallFrame& frame) {
    base::Shared<fs::OpenFile> held;
    net::Socket*               socket = nullptr;
    const long long            kFound = socket_of(frame.rdi, &socket, held);
    if (kFound != 0) {
        return kFound;
    }
    if (frame.r10 == 0 || frame.r8 == 0) {
        return -kEfault;
    }
    unsigned int answer = 0;
    const auto   kLevel = frame.rsi;
    const auto   kName  = frame.rdx;
    if (kLevel == kSolSocket && kName == kSoType) {
        answer = static_cast<unsigned int>(socket->type());
    } else if (kLevel == kSolSocket && kName == kSoRcvbuf) {
        answer =
            static_cast<unsigned int>(net::kUdpSocketPayloadBytes * net::kUdpSocketQueueDatagrams);
    } else {
        return -kEnoprotoopt;
    }
    uint32_t length = 0;
    if (!uaccess::CopyFromUser(&length, frame.r8, sizeof(length))) {
        return -kEfault;
    }
    length = std::min(length, static_cast<uint32_t>(sizeof(answer)));
    if (!uaccess::CopyToUser(frame.r10, &answer, length) ||
        !uaccess::CopyToUser(frame.r8, &length, sizeof(length))) {
        return -kEfault;
    }
    return 0;
}

}  // namespace cinux::syscall
