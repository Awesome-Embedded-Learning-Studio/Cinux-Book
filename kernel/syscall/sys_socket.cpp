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
#include "kernel/net/local_config.hpp"
#include "kernel/net/local_socket.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_world.hpp"
#include "kernel/net/socket.hpp"
#include "kernel/net/udp.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/signals.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/signal.hpp"
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
constexpr Word               kMsgDontwait  = 0x40;
constexpr Word               kMsgNosignal  = 0x4000;

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
    if (address == 0 || length < sizeof(uint16_t)) {
        return -kEinval;
    }
    uint16_t family = 0;
    if (!uaccess::CopyFromUser(&family, address, sizeof(family))) {
        return -kEfault;
    }
    if (family == net::kAfUnix) {
        if (length > sizeof(family) + net::kLocalNameBytes || length == sizeof(family)) {
            return -kEinval;
        }
        out.family     = net::kAfUnix;
        out.name_bytes = length - sizeof(family);
        if (!uaccess::CopyFromUser(out.name, address + sizeof(family), out.name_bytes)) {
            return -kEfault;
        }
        if (out.name[0] != 0) {
            for (unsigned long index = 0; index < out.name_bytes; ++index) {
                if (out.name[index] == 0) {
                    out.name_bytes = index + 1;
                    break;
                }
            }
        }
        return 0;
    }
    if (family != net::kAfInet || length < kSockaddrMinBytes) {
        return -kEafnosupport;
    }
    unsigned char record[kSockaddrInBytes] = {};
    const Word    kBytes                   = length < kSockaddrInBytes ? length : kSockaddrInBytes;
    if (!uaccess::CopyFromUser(record, address, kBytes)) {
        return -kEfault;
    }
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
    unsigned char record[sizeof(uint16_t) + net::kLocalNameBytes] = {};
    const auto    kFamily = static_cast<uint16_t>(from.family);
    base::CopyBytes(record + kFamilyAt, &kFamily, sizeof(kFamily));
    uint32_t whole = kSockaddrInBytes;
    if (from.family == net::kAfUnix) {
        whole = static_cast<uint32_t>(sizeof(kFamily) + from.name_bytes);
        base::CopyBytes(record + sizeof(kFamily), from.name, from.name_bytes);
    } else {
        base::WriteWord16Be(record, kPortAt, from.port);
        base::WriteWord32Be(record, kAddressAt, from.address);
    }
    const uint32_t kWhole = whole;
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
    case base::KernelError::kBrokenPipe:
        return -kEpipe;
    case base::KernelError::kConnectionRefused:
        return -kEconnrefused;
    default:
        return -ErrnoFrom(error);
    }
}

/// Whether the calling task has a deliverable signal waiting.
bool signal_pending() {
    const proc::Task* const kSelf = CurrentTask();
    return proc::HasPendingSignal(*kSelf);
}

struct Outgoing {
    const void*        source{};
    unsigned long      count{};
    Word               flags{};
    net::SocketAddress destination;
};

long long transmit_wait(net::Socket& socket, const fs::OpenFile& file, const Outgoing& outgoing) {
    for (;;) {
        const auto kGiven = socket.transmit(outgoing.source, outgoing.count, outgoing.destination);
        if (kGiven.ok()) {
            return static_cast<long long>(kGiven.value());
        }
        if (kGiven.error() != base::KernelError::kWouldBlock) {
            if (kGiven.error() == base::KernelError::kBrokenPipe &&
                (outgoing.flags & kMsgNosignal) == 0) {
                proc::SendSignal(*CurrentTask(), kSigPipe);
            }
            return errno_of_socket(kGiven.error());
        }
        if ((file.status_flags & kOpenNonblock) != 0 || (outgoing.flags & kMsgDontwait) != 0) {
            return -kEagain;
        }
        if (signal_pending()) {
            return -kEintr;
        }
        proc::Scheduler::self().yield();
    }
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
            .limit   = FileDescriptorLimit(),
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

long long make_local_socket(Word type, Word protocol, base::Shared<net::Socket>& socket) {
    if (protocol != 0) {
        return -kEprotonosupport;
    }
    if (type != net::kSockStream && type != net::kSockDgram) {
        return -kEsocktnosupport;
    }
    socket = net::MakeLocalSocket(static_cast<int>(type));
    return 0;
}

long long make_inet_dgram_socket(Word protocol, base::Shared<net::Socket>& socket) {
    if (protocol == 0 || protocol == static_cast<Word>(net::kIpv4ProtocolUdp)) {
        socket = base::Shared<net::Socket>{new net::UdpSocket(net::NetWorld::self().udp())};
        return 0;
    }
    if (protocol != kIpProtoIcmp) {
        return -kEprotonosupport;
    }
    auto* ping = new net::PingSocket(net::NetWorld::self().raw());
    socket     = base::Shared<net::Socket>{ping};
    if (ping == nullptr || !ping->seated()) {
        return -kEnomem;
    }
    return 0;
}

}  // namespace

long long HandleSocket(const SyscallFrame& frame) {
    if (frame.rdi != static_cast<Word>(net::kAfInet) &&
        frame.rdi != static_cast<Word>(net::kAfUnix)) {
        return -kEafnosupport;
    }
    const Word kRawType   = frame.rsi;
    const Word kFlags     = kRawType & (kSockCloexec | kSockNonblock);
    const Word kPlainType = kRawType & ~(kSockCloexec | kSockNonblock);
    const auto kProtocol  = static_cast<Word>(frame.rdx);

    base::Shared<net::Socket> socket;
    if (frame.rdi == net::kAfUnix) {
        const auto kMade = make_local_socket(kPlainType, kProtocol, socket);
        if (kMade != 0) {
            return kMade;
        }
    } else if (kPlainType == static_cast<Word>(net::kSockStream)) {
        if (kProtocol != 0 && kProtocol != static_cast<Word>(net::kIpv4ProtocolTcp)) {
            return -kEprotonosupport;
        }
        socket = base::Shared<net::Socket>{new net::TcpSocket(net::NetWorld::self().tcp())};
    } else if (kPlainType == static_cast<Word>(net::kSockDgram)) {
        const auto kMade = make_inet_dgram_socket(kProtocol, socket);
        if (kMade != 0) {
            return kMade;
        }
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
                                        .limit   = FileDescriptorLimit(),
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
    if (asked.family != socket->family()) {
        return -kEafnosupport;
    }
    if (asked.family == net::kAfInet && asked.address != 0 &&
        asked.address != net::NetWorld::self().udp().own_address() &&
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
    if (named.family != socket->family()) {
        return -kEafnosupport;
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
    if ((frame.r10 & ~(kMsgDontwait | kMsgNosignal)) != 0) {
        return -kEopnotsupp;
    }
    const unsigned long kLimit = send_ceiling_of(socket->type());
    const auto          kCount = socket->type() == net::kSockStream
                                     ? std::min(static_cast<unsigned long>(frame.rdx), kLimit)
                                     : static_cast<unsigned long>(frame.rdx);
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
    if (frame.r8 != 0 && destination.family != socket->family()) {
        return -kEafnosupport;
    }
    return transmit_wait(
        *socket, *held,
        {.source = payload, .count = kCount, .flags = frame.r10, .destination = destination});
}

long long HandleRecvfrom(const SyscallFrame& frame) {
    base::Shared<fs::OpenFile> held;
    net::Socket*               socket = nullptr;
    const long long            kFound = socket_of(frame.rdi, &socket, held);
    if (kFound != 0) {
        return kFound;
    }
    if ((frame.r10 & ~kMsgDontwait) != 0) {
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
        if ((held->status_flags & kOpenNonblock) != 0 || (frame.r10 & kMsgDontwait) != 0) {
            return -kEagain;
        }
        if (signal_pending()) {
            return -kEintr;
        }
        proc::Scheduler::self().yield();
    }
}

namespace {
struct Message {
    Word name;
    uint32_t name_length;
    uint32_t name_padding;
    Word vectors;
    Word vector_count;
    Word control;
    Word control_length;
    uint32_t flags;
    uint32_t flags_padding;
};
struct Vector { Word address; Word bytes; };
struct ControlHeader { Word length; int level; int type; };
static_assert(sizeof(Message) == 56);
static_assert(sizeof(ControlHeader) == 16);
constexpr Word kMaxVectors = 16;
constexpr Word kMsgTrunc = 0x20;
constexpr Word kMsgControlTrunc = 8;
constexpr Word kMsgErrorQueue = 0x2000;

long long read_message(Word address, Message& message, Vector* vectors, Word& capacity) {
    if (!uaccess::CopyFromUser(&message, address, sizeof(message))) { return -kEfault; }
    if (message.vector_count > kMaxVectors) { return -kEmsgsize; }
    if (message.vector_count != 0 && !uaccess::CopyFromUser(
            vectors, message.vectors, message.vector_count * sizeof(Vector))) { return -kEfault; }
    capacity = 0;
    for (Word index = 0; index < message.vector_count; ++index) {
        if (vectors[index].bytes > (kUserAddressLimit - 1) - capacity) { return -kEinval; }
        capacity += vectors[index].bytes;
    }
    return 0;
}
long long control_message(net::Socket& socket, Message& message, const net::SocketAddress& from) {
    unsigned char bytes[64]{};
    unsigned long used = 0;
    auto append = [&](int level, int type, const void* value, unsigned long length) {
        const ControlHeader header{sizeof(ControlHeader) + length, level, type};
        const auto kPadded = (header.length + 7) & ~7ULL;
        if (message.control == 0 || kPadded > message.control_length - used) {
            message.flags |= kMsgControlTrunc;
            return;
        }
        base::CopyBytes(bytes + used, &header, sizeof(header));
        base::CopyBytes(bytes + used + sizeof(header), value, length);
        used += kPadded;
    };
    if (socket.receive_timestamp.load()) {
        const auto kStamp = from.received_ns != 0 ? from.received_ns : RealtimeNanoseconds();
        const Word time[2]{kStamp / 1'000'000'000ULL, kStamp % 1'000'000'000ULL / 1000};
        append(1, 29, time, sizeof(time));  // SCM_TIMESTAMP
    }
    if (socket.receive_ttl.load() && from.received_ttl != 0) {
        const int kTtl = static_cast<int>(from.received_ttl);
        append(0, 2, &kTtl, sizeof(kTtl));  // IP_TTL
    }
    message.control_length = used;
    return used == 0 || uaccess::CopyToUser(message.control, bytes, used) ? 0 : -kEfault;
}
}

long long HandleSendmsg(const SyscallFrame& frame) {
    base::Shared<fs::OpenFile> held;
    net::Socket* socket = nullptr;
    const auto kFound = socket_of(frame.rdi, &socket, held);
    if (kFound != 0) { return kFound; }
    if ((frame.rdx & ~(kMsgDontwait | kMsgNosignal)) != 0) { return -kEopnotsupp; }
    Message message{};
    Vector vectors[kMaxVectors]{};
    Word capacity = 0;
    const auto kParsed = read_message(frame.rsi, message, vectors, capacity);
    if (kParsed != 0) { return kParsed; }
    if (message.control_length != 0) { return -kEopnotsupp; }
    if (capacity > send_ceiling_of(socket->type())) { return -kEmsgsize; }
    unsigned char payload[net::kMaxDatagramBytes]{};
    unsigned long offset = 0;
    for (Word index = 0; index < message.vector_count; ++index) {
        const auto& part = vectors[index];
        if (part.bytes != 0 && !uaccess::CopyFromUser(payload + offset, part.address, part.bytes)) {
            return -kEfault;
        }
        offset += part.bytes;
    }
    net::SocketAddress target{};
    if (message.name != 0) {
        const auto kAddress = read_sockaddr(message.name, message.name_length, target);
        if (kAddress != 0) { return kAddress; }
        if (target.family != socket->family()) { return -kEafnosupport; }
    } else if (!socket->report_peer(target).ok()) { return -kEdestaddrreq; }
    return transmit_wait(*socket, *held,
        {.source = payload, .count = capacity, .flags = frame.rdx, .destination = target});
}

long long HandleRecvmsg(const SyscallFrame& frame) {
    base::Shared<fs::OpenFile> held;
    net::Socket* socket = nullptr;
    const auto kFound = socket_of(frame.rdi, &socket, held);
    if (kFound != 0) { return kFound; }
    if ((frame.rdx & ~(kMsgDontwait | kMsgTrunc | kMsgErrorQueue)) != 0) { return -kEopnotsupp; }
    if ((frame.rdx & kMsgErrorQueue) != 0) { return -kEagain; }
    Message message{};
    Vector vectors[kMaxVectors]{};
    Word capacity = 0;
    const auto kParsed = read_message(frame.rsi, message, vectors, capacity);
    if (kParsed != 0) { return kParsed; }
    Word checked = 0;
    for (Word index = 0; index < message.vector_count && checked < net::kMaxDatagramBytes; ++index) {
        const auto kPart = std::min(vectors[index].bytes, net::kMaxDatagramBytes - checked);
        if (kPart != 0 && !UserRangeWritable(vectors[index].address, kPart)) { return -kEfault; }
        checked += kPart;
    }
    unsigned char payload[net::kMaxDatagramBytes]{};
    for (;;) {
        net::SocketAddress from{};
        const auto kTaken = socket->receive(payload, std::min<Word>(capacity, net::kMaxDatagramBytes), &from);
        if (!kTaken.ok()) {
            if (kTaken.error() != base::KernelError::kWouldBlock) { return errno_of_socket(kTaken.error()); }
            if ((frame.rdx & kMsgDontwait) != 0 || (held->status_flags & kOpenNonblock) != 0) {
                return -kEagain;
            }
            if (signal_pending()) { return -kEintr; }
            proc::Scheduler::self().yield();
            continue;
        }
        unsigned long copied = 0;
        for (Word index = 0; index < message.vector_count && copied < kTaken.value(); ++index) {
            const auto kPart = std::min<Word>(vectors[index].bytes, kTaken.value() - copied);
            if (kPart != 0 && !uaccess::CopyToUser(vectors[index].address, payload + copied, kPart)) {
                return -kEfault;
            }
            copied += kPart;
        }
        message.flags = from.datagram_bytes > copied ? kMsgTrunc : 0;
        if (message.name != 0) {
            const auto kNamed = write_sockaddr(message.name, frame.rsi + offsetof(Message, name_length), from);
            if (kNamed != 0) { return kNamed; }
            message.name_length = from.family == net::kAfUnix ?
                static_cast<uint32_t>(2 + from.name_bytes) : 16;
        } else { message.name_length = 0; }
        const auto kControl = control_message(*socket, message, from);
        if (kControl != 0) { return kControl; }
        if (!uaccess::CopyToUser(frame.rsi, &message, sizeof(message))) { return -kEfault; }
        return static_cast<long long>((frame.rdx & kMsgTrunc) != 0 ?
            std::max(from.datagram_bytes, copied) : copied);
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

long long HandleSocketpair(const SyscallFrame& frame) {
    if (frame.rdi != net::kAfUnix) {
        return -kEafnosupport;
    }
    const auto kType = frame.rsi & ~(kSockCloexec | kSockNonblock);
    if (frame.rdx != 0 || (kType != net::kSockStream && kType != net::kSockDgram)) {
        return -kEinval;
    }
    base::Shared<net::Socket> pair[2];
    if (!net::MakeLocalPair(static_cast<int>(kType), pair).ok()) {
        return -kEnomem;
    }
    int   descriptors[2] = {-1, -1};
    auto& files          = *CurrentTask()->files;
    for (unsigned long index = 0; index < 2; ++index) {
        const fs::FsNode kNode{
            .opaque = pair[index].get(),
            .type   = fs::InodeType::kFile,
            .ino    = pair[index]->identity(),
            .size   = 0,
        };
        const auto kSeat =
            files.alloc(net::SocketOps(), pair[index].get(), kNode,
                        {
                            .status  = (frame.rsi & kSockNonblock) != 0 ? kOpenNonblock : 0,
                            .cloexec = (frame.rsi & kSockCloexec) != 0,
                        });
        if (!kSeat.ok()) {
            if (descriptors[0] >= 0) {
                static_cast<void>(files.free(static_cast<unsigned long>(descriptors[0])));
            }
            return -kEmfile;
        }
        descriptors[index] = static_cast<int>(kSeat.value());
    }
    if (!uaccess::CopyToUser(frame.r10, descriptors, sizeof(descriptors))) {
        static_cast<void>(files.free(static_cast<unsigned long>(descriptors[0])));
        static_cast<void>(files.free(static_cast<unsigned long>(descriptors[1])));
        return -kEfault;
    }
    return 0;
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
        if (kName == 29) { socket->receive_timestamp.store(value != 0); return 0; }
        if (kName == kSoReuseaddr || kName == kSoSndbuf || kName == kSoRcvbuf ||
            kName == kSoBroadcast) {
            return 0;
        }
        return -kEnoprotoopt;
    }
    if (kLevel == kIpProtoIp && (kName == kIpTtl || kName == kIpMulticastTtl)) {
        return 0;
    }
    if (kLevel == kIpProtoIp && kName == 12) {
        socket->receive_ttl.store(value != 0);
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
