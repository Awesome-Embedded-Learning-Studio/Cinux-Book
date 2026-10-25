#include "kernel/net/local_socket.hpp"

#include <algorithm>
#include <utility>

#include "cinux/container/ring_queue.hpp"
#include "cinux/memory.hpp"
#include "cinux/ref_count.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "cinux/singleton.hpp"
#include "kernel/net/local_config.hpp"
#include "kernel/net/socket.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::net {
namespace {

struct Datagram {
    SocketAddress from{};
    unsigned long bytes{};
    unsigned char payload[kUdpSocketPayloadBytes]{};
};

struct Channel : base::RefCount<> {
    base::container::RingQueue<unsigned char, kLocalStreamBytes + 1> streams[2];
    base::container::RingQueue<Datagram, kLocalQueue + 1>            datagrams[2];
    bool                                                             closed[2]{};
    bool                                                             write_closed[2]{};
};

struct LocalState : base::RefCount<> {
    SocketAddress                                                     local{.family = kAfUnix};
    SocketAddress                                                     peer{.family = kAfUnix};
    base::Shared<Channel>                                             channel;
    base::container::RingQueue<Datagram, kLocalQueue + 1>             messages;
    base::container::RingQueue<base::Shared<Socket>, kLocalQueue + 1> accepted;
    unsigned int                                                      side{};
    unsigned long                                                     backlog{};
    int                                                               type{};
    bool                                                              alive = true;
    bool                                                              bound{};
    bool                                                              connected{};
    bool                                                              listening{};
};

struct Binding {
    base::Shared<LocalState> state;
};

class LocalWorld : public base::Singleton<LocalWorld> {
    friend class base::Singleton<LocalWorld>;

public:
    proc::Spinlock gate;
    Binding*       bindings[kLocalBindings]{};

    [[nodiscard]] base::Shared<LocalState> find(const SocketAddress& address) const {
        for (const Binding* binding : bindings) {
            if (binding == nullptr || binding->state->local.name_bytes != address.name_bytes) {
                continue;
            }
            bool equal = true;
            for (unsigned long index = 0; index < address.name_bytes; ++index) {
                equal = equal && binding->state->local.name[index] == address.name[index];
            }
            if (equal && binding->state->alive) {
                return binding->state;
            }
        }
        return {};
    }

private:
    LocalWorld() = default;
};

base::Result<unsigned long> send_datagram(const LocalState& state, const void* source,
                                          unsigned long count, const SocketAddress& destination) {
    if (count > kUdpSocketPayloadBytes) {
        return base::KernelError::kNotSupported;
    }
    Datagram message;
    message.from  = state.local;
    message.bytes = count;
    base::CopyBytes(message.payload, source, count);
    if (state.channel.holds()) {
        if (state.channel->closed[1 - state.side]) {
            return base::KernelError::kConnectionRefused;
        }
        if (!state.channel->datagrams[1 - state.side].push(message)) {
            return base::KernelError::kWouldBlock;
        }
        return count;
    }
    const auto& target =
        destination.family == kAfUnix && destination.name_bytes != 0 ? destination : state.peer;
    const auto kPeer = LocalWorld::self().find(target);
    if (!kPeer.holds() || kPeer->type != kSockDgram) {
        return base::KernelError::kConnectionRefused;
    }
    if (!kPeer->messages.push(message)) {
        return base::KernelError::kWouldBlock;
    }
    return count;
}

class LocalSocket final : public Socket {
public:
    explicit LocalSocket(int type) : Socket(type, kAfUnix), state_(new LocalState{}) {
        if (state_.holds()) {
            state_->type = type;
        }
    }

    ~LocalSocket() override {
        if (!state_.holds()) {
            return;
        }
        {
            auto&      world  = LocalWorld::self();
            const auto kGuard = world.gate.guard();
            state_->alive     = false;
            if (state_->channel.holds()) {
                state_->channel->closed[state_->side]       = true;
                state_->channel->write_closed[state_->side] = true;
            }
            for (Binding*& binding : world.bindings) {
                if (binding != nullptr && binding->state.get() == state_.get()) {
                    delete binding;
                    binding = nullptr;
                }
            }
        }
        for (;;) {
            base::Shared<Socket> pending;
            {
                const auto kGuard = LocalWorld::self().gate.guard();
                if (!state_->accepted.pop(pending)) {
                    break;
                }
            }
        }
    }

    [[nodiscard]] bool ready() const { return state_.holds(); }

    void pair_with(LocalSocket& peer, const base::Shared<Channel>& channel) {
        state_->channel        = channel;
        state_->connected      = true;
        state_->side           = 0;
        state_->peer           = peer.state_->local;
        peer.state_->channel   = channel;
        peer.state_->connected = true;
        peer.state_->side      = 1;
        peer.state_->peer      = state_->local;
    }

    base::Result<unsigned long> receive(void* sink, unsigned long count,
                                        SocketAddress* from) override {
        if (count == 0) {
            return 0;
        }
        const auto kGuard = LocalWorld::self().gate.guard();
        if (type() == kSockDgram) {
            Datagram message;
            auto&    queue = state_->channel.holds() ? state_->channel->datagrams[state_->side]
                                                     : state_->messages;
            if (!queue.pop(message)) {
                return base::KernelError::kWouldBlock;
            }
            const auto kBytes = std::min(count, message.bytes);
            base::CopyBytes(sink, message.payload, kBytes);
            if (from != nullptr) {
                *from = message.from;
            }
            return kBytes;
        }
        if (!state_->channel.holds()) {
            return base::KernelError::kNotConnected;
        }
        auto&         channel = *state_->channel;
        auto*         out     = static_cast<unsigned char*>(sink);
        unsigned long bytes   = 0;
        while (bytes < count && channel.streams[state_->side].pop(out[bytes])) {
            ++bytes;
        }
        if (from != nullptr) {
            *from = state_->peer;
        }
        if (bytes == 0 && !channel.write_closed[1 - state_->side]) {
            return base::KernelError::kWouldBlock;
        }
        return bytes;
    }

    base::Result<unsigned long> transmit(const void* source, unsigned long count,
                                         const SocketAddress& destination) override {
        const auto kGuard = LocalWorld::self().gate.guard();
        if (type() == kSockDgram) {
            return send_datagram(*state_, source, count, destination);
        }
        if (!state_->channel.holds()) {
            return base::KernelError::kNotConnected;
        }
        auto& channel = *state_->channel;
        if (channel.write_closed[state_->side] || channel.closed[1 - state_->side]) {
            return base::KernelError::kBrokenPipe;
        }
        const auto*   bytes   = static_cast<const unsigned char*>(source);
        unsigned long written = 0;
        while (written < count && channel.streams[1 - state_->side].push(bytes[written])) {
            ++written;
        }
        if (count != 0 && written == 0) {
            return base::KernelError::kWouldBlock;
        }
        return written;
    }

    base::Result<void> adopt_local(const SocketAddress& local) override {
        if (local.family != kAfUnix || local.name_bytes == 0 ||
            local.name_bytes > kLocalNameBytes) {
            return base::KernelError::kInvalidArgument;
        }
        auto&      world  = LocalWorld::self();
        const auto kGuard = world.gate.guard();
        if (state_->bound || world.find(local).holds()) {
            return base::KernelError::kAlreadyExists;
        }
        for (Binding*& binding : world.bindings) {
            if (binding == nullptr) {
                binding = new Binding{.state = state_};
                if (binding == nullptr) {
                    return base::KernelError::kOutOfMemory;
                }
                state_->local = local;
                state_->bound = true;
                return {};
            }
        }
        return base::KernelError::kOutOfMemory;
    }

    base::Result<void> adopt_peer(const SocketAddress& peer) override {
        base::Shared<Socket>  accepted;
        base::Shared<Channel> channel;
        LocalSocket*          accepted_socket = nullptr;
        if (type() == kSockStream) {
            accepted_socket = new LocalSocket(type());
            accepted        = base::Shared<Socket>(accepted_socket);
            channel         = base::Shared<Channel>(new Channel{});
            if (accepted_socket == nullptr || !accepted_socket->ready() || !channel.holds()) {
                return base::KernelError::kOutOfMemory;
            }
        }
        auto&      world     = LocalWorld::self();
        const auto kGuard    = world.gate.guard();
        const auto kListener = world.find(peer);
        if (!kListener.holds() || kListener->type != type()) {
            return base::KernelError::kConnectionRefused;
        }
        if (type() == kSockDgram) {
            state_->peer      = peer;
            state_->connected = true;
            return {};
        }
        if (state_->connected || !kListener->listening) {
            return base::KernelError::kConnectionRefused;
        }
        if (kListener->accepted.count() >= kListener->backlog) {
            return base::KernelError::kWouldBlock;
        }
        if (accepted_socket == nullptr) {
            return base::KernelError::kInvalidArgument;
        }
        accepted_socket->state_->local = kListener->local;
        pair_with(*accepted_socket, channel);
        kListener->accepted.push(std::move(accepted));
        return {};
    }

    base::Result<void> report_local(SocketAddress& local) const override {
        const auto kGuard = LocalWorld::self().gate.guard();
        local             = state_->local;
        return {};
    }

    base::Result<void> report_peer(SocketAddress& peer) const override {
        const auto kGuard = LocalWorld::self().gate.guard();
        if (!state_->connected) {
            return base::KernelError::kNotConnected;
        }
        peer = state_->peer;
        return {};
    }

    [[nodiscard]] bool holds_input() const override { return (poll_events(1U) & 1U) != 0; }

    [[nodiscard]] unsigned short poll_events(unsigned int events) const override {
        const auto   kGuard = LocalWorld::self().gate.guard();
        unsigned int answer = 0;
        bool         input  = state_->listening ? state_->accepted.count() != 0 : false;
        bool         output = !state_->listening;
        if (state_->channel.holds()) {
            const auto& channel = *state_->channel;
            const auto  kSide   = state_->side;
            input  = type() == kSockStream
                         ? channel.streams[kSide].count() != 0 || channel.write_closed[1 - kSide]
                         : channel.datagrams[kSide].count() != 0;
            output = type() == kSockStream ? channel.streams[1 - kSide].count() < kLocalStreamBytes
                                           : channel.datagrams[1 - kSide].count() < kLocalQueue;
            if (channel.closed[1 - kSide]) {
                answer |= 0x10U;
            }
            if (channel.write_closed[kSide]) {
                output = false;
            }
        } else if (type() == kSockDgram) {
            input = state_->messages.count() != 0;
        }
        return static_cast<unsigned short>(answer | (input ? events & 1U : 0U) |
                                           (output ? events & 4U : 0U));
    }

    base::Result<void> begin_listen(unsigned long backlog) override {
        const auto kGuard = LocalWorld::self().gate.guard();
        if (type() != kSockStream || !state_->bound || state_->connected) {
            return base::KernelError::kNotSupported;
        }
        state_->listening = true;
        state_->backlog   = std::clamp(backlog, 1UL, static_cast<unsigned long>(kLocalQueue));
        return {};
    }

    base::Result<base::Shared<Socket>> accept_one() override {
        const auto kGuard = LocalWorld::self().gate.guard();
        if (!state_->listening) {
            return base::KernelError::kNotSupported;
        }
        base::Shared<Socket> accepted;
        if (!state_->accepted.pop(accepted)) {
            return base::KernelError::kWouldBlock;
        }
        return accepted;
    }

    base::Result<void> shutdown_write() override {
        const auto kGuard = LocalWorld::self().gate.guard();
        if (!state_->channel.holds()) {
            return base::KernelError::kNotConnected;
        }
        state_->channel->write_closed[state_->side] = true;
        return {};
    }

private:
    base::Shared<LocalState> state_;
};

}  // namespace

base::Shared<Socket> MakeLocalSocket(int type) {
    if (type != kSockStream && type != kSockDgram) {
        return {};
    }
    auto* const          kSocket = new LocalSocket(type);
    base::Shared<Socket> socket(kSocket);
    if (kSocket == nullptr || !kSocket->ready()) {
        return {};
    }
    return socket;
}

base::Result<void> MakeLocalPair(int type, base::Shared<Socket> (&pair)[2]) {
    auto                        first  = MakeLocalSocket(type);
    auto                        second = MakeLocalSocket(type);
    const base::Shared<Channel> kChannel(new Channel{});
    if (!first.holds() || !second.holds() || !kChannel.holds()) {
        return base::KernelError::kOutOfMemory;
    }
    static_cast<LocalSocket*>(first.get())
        ->pair_with(*static_cast<LocalSocket*>(second.get()), kChannel);
    pair[0] = std::move(first);
    pair[1] = std::move(second);
    return {};
}

}  // namespace cinux::net
