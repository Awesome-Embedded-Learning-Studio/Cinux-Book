#include "kernel/net/socket.hpp"

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "cinux/memory.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/net/net_config.hpp"
#include "kernel/net/net_layout.hpp"
#include "kernel/net/raw.hpp"
#include "kernel/net/udp.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::net {

namespace {

/// One boot-wide counter: every socket born gets the next number.
std::atomic<unsigned long> g_socket_identities{0};

/// The mode a socket answers stat with: a socket, owner read-write.
constexpr unsigned int kSocketMode = 0140600;

base::Result<void> socket_stat(void* /*self*/, fs::FsNode node, fs::FsStat& probed) {
    probed       = fs::FsStat{};
    probed.ino   = node.ino;
    probed.mode  = kSocketMode;
    probed.links = 1;
    return {};
}

// NOLINTNEXTLINE(readability-function-size) the FsOps signature names five
base::Result<unsigned long> socket_read(void* self, fs::FsNode /*node*/, unsigned long /*offset*/,
                                        void* sink, unsigned long count) {
    return static_cast<Socket*>(self)->receive(sink, count, nullptr);
}

// NOLINTNEXTLINE(readability-function-size) the FsOps signature names five
base::Result<unsigned long> socket_write(void* self, fs::FsNode /*node*/, unsigned long /*offset*/,
                                         const void* source, unsigned long count) {
    const SocketAddress kAnyone = {};
    return static_cast<Socket*>(self)->transmit(source, count, kAnyone);
}

void socket_retain(void* self, fs::FsNode /*node*/) {
    static_cast<Socket*>(self)->retain();
}

void socket_release(void* self, fs::FsNode /*node*/) {
    auto* const kSocket = static_cast<Socket*>(self);
    if (kSocket->release()) {
        delete kSocket;
    }
}

bool socket_readable(void* self, fs::FsNode /*node*/) {
    return static_cast<Socket*>(self)->holds_input();
}

const fs::FsOps kOps{
    .lookup     = nullptr,
    .make_file  = nullptr,
    .make_dir   = nullptr,
    .unlink     = nullptr,
    .read       = socket_read,
    .write      = socket_write,
    .read_dir   = nullptr,
    .stat       = socket_stat,
    .retain     = socket_retain,
    .release    = socket_release,
    .readable   = socket_readable,
    .ioctl      = nullptr,
    .adopt_open = nullptr,
};

}  // namespace

Socket::Socket(int socket_type)
    : type_(socket_type),
      identity_(g_socket_identities.fetch_add(1, std::memory_order_relaxed) + 1) {}

UdpSocket::UdpSocket(UdpModule& module) : Socket(kSockDgram), module_(module) {}

UdpSocket::~UdpSocket() {
    const proc::PreemptGuard kPin;
    if (seated_) {
        module_.unbind(local_.port);
    }
    ring_gate_.retire();
}

base::Result<unsigned long> UdpSocket::receive(void* sink, unsigned long count,
                                               SocketAddress* from) {
    if (count == 0) {
        return 0;
    }
    const auto kGate = ring_gate_.guard();
    Waiting    taken{};
    if (!waiting_.pop(taken)) {
        return base::KernelError::kWouldBlock;
    }
    const unsigned long kBytes = taken.length < count ? taken.length : count;
    base::CopyBytes(sink, taken.bytes, kBytes);
    if (from != nullptr) {
        *from = taken.from;
    }
    return kBytes;
}

base::Result<unsigned long> UdpSocket::transmit(const void* source, unsigned long count,
                                                const SocketAddress& destination) {
    const proc::PreemptGuard kPin;
    if (count > kUdpSocketPayloadBytes) {
        return base::KernelError::kNotSupported;
    }
    SocketAddress target = destination;
    if (target.address == 0 && target.port == 0) {
        if (!named_) {
            return base::KernelError::kNotFound;
        }
        target = peer_;
    }

    if (!seated_) {
        const uint16_t kGranted = module_.ephemeral_port();
        if (kGranted == 0 || !module_.bind(kGranted, this, &UdpSocket::take)) {
            return base::KernelError::kAlreadyExists;
        }
        seated_     = true;
        local_.port = kGranted;
    }
    if (!module_.send(target.address, local_.port, target.port,
                      static_cast<const unsigned char*>(source), count)) {
        return base::KernelError::kIOError;
    }
    return count;
}

base::Result<void> UdpSocket::adopt_local(const SocketAddress& local) {
    const proc::PreemptGuard kPin;
    if (seated_) {
        return base::KernelError::kAlreadyExists;
    }
    uint16_t port = local.port;
    if (port == 0) {
        port = module_.ephemeral_port();
        if (port == 0) {
            return base::KernelError::kAlreadyExists;
        }
    }
    if (!module_.bind(port, this, &UdpSocket::take)) {
        return base::KernelError::kAlreadyExists;
    }
    local_      = local;
    local_.port = port;
    seated_     = true;
    return {};
}

base::Result<void> UdpSocket::adopt_peer(const SocketAddress& peer) {
    peer_  = peer;
    named_ = true;
    return {};
}

base::Result<void> UdpSocket::report_local(SocketAddress& local) const {
    local = local_;
    if (local.address == 0) {
        local.address = module_.own_address();
    }
    return {};
}

base::Result<void> UdpSocket::report_peer(SocketAddress& peer) const {
    if (!named_) {
        return base::KernelError::kNotFound;
    }
    peer = peer_;
    return {};
}

bool UdpSocket::holds_input() const {
    const auto kGate = ring_gate_.guard();
    return waiting_.count() != 0;
}

unsigned long UdpSocket::dropped() const {
    const auto kGate = ring_gate_.guard();
    return dropped_;
}

void UdpSocket::take(void* context, const UdpDelivery& delivery) {
    auto* const kSelf = static_cast<UdpSocket*>(context);
    const auto  kGate = kSelf->ring_gate_.guard();
    Waiting     room{};
    room.from = SocketAddress{.address = delivery.sender, .port = delivery.sender_port};
    room.length =
        delivery.length < kUdpSocketPayloadBytes ? delivery.length : kUdpSocketPayloadBytes;
    base::CopyBytes(room.bytes, delivery.bytes, room.length);
    if (!kSelf->waiting_.push(room)) {
        ++kSelf->dropped_;
    }
}

RawSocket::RawSocket(RawModule& module) : Socket(kSockRaw), module_(module) {
    seated_ = module_.bind(this, take);
}

RawSocket::~RawSocket() {
    if (seated_) {
        module_.unbind(this);
    }
    gate_.retire();
}

base::Result<unsigned long> RawSocket::receive(void* sink, unsigned long count,
                                               SocketAddress* from) {
    const auto kGuard = gate_.guard();
    Waiting    taken{};
    if (!waiting_.pop(taken)) {
        return base::KernelError::kWouldBlock;
    }
    const auto kBytes = taken.length < count ? taken.length : count;
    base::CopyBytes(sink, taken.bytes, kBytes);
    if (from != nullptr) {
        *from = taken.from;
    }
    return kBytes;
}

base::Result<unsigned long> RawSocket::transmit(const void* source, unsigned long count,
                                                const SocketAddress& destination) {
    SocketAddress target = destination;
    {
        const auto kGuard = gate_.guard();
        if (target.address == 0 && named_) {
            target = peer_;
        }
    }
    if (target.address == 0) {
        return base::KernelError::kNotFound;
    }
    if (count > kMaxDatagramBytes - kMinIpv4HeaderBytes) {
        return base::KernelError::kNotSupported;
    }
    if (!module_.send(target.address, static_cast<const unsigned char*>(source), count)) {
        return base::KernelError::kIOError;
    }
    return count;
}

base::Result<void> RawSocket::adopt_local(const SocketAddress& local) {
    const auto kGuard = gate_.guard();
    local_            = local;
    return {};
}

base::Result<void> RawSocket::adopt_peer(const SocketAddress& peer) {
    const auto kGuard = gate_.guard();
    peer_             = peer;
    named_            = true;
    return {};
}

base::Result<void> RawSocket::report_local(SocketAddress& local) const {
    const auto kGuard = gate_.guard();
    local             = local_;
    return {};
}

base::Result<void> RawSocket::report_peer(SocketAddress& peer) const {
    const auto kGuard = gate_.guard();
    if (!named_) {
        return base::KernelError::kNotFound;
    }
    peer = peer_;
    return {};
}

bool RawSocket::holds_input() const {
    const auto kGuard = gate_.guard();
    return waiting_.count() != 0;
}

void RawSocket::take(void* context, const unsigned char* bytes, size_t length,
                     const Ipv4View& header) {
    auto&      self   = *static_cast<RawSocket*>(context);
    const auto kGuard = self.gate_.guard();
    if ((self.local_.address != 0 && self.local_.address != header.destination) ||
        (self.named_ && self.peer_.address != header.source)) {
        return;
    }
    Waiting entry{};
    entry.from.address = header.source;
    entry.length       = length;
    base::CopyBytes(entry.bytes, bytes, length);
    static_cast<void>(self.waiting_.push(entry));
}

const fs::FsOps* SocketOps() {
    return &kOps;
}

}  // namespace cinux::net
