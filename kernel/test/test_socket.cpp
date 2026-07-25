/**
 * @file kernel/test/test_socket.cpp
 * @brief In-kernel tests for the F7-M6 socket syscall plumbing (batch 1b).
 *
 * B1b ships the fd + syscall machinery with a STUB Socket: socket()/close()
 * work, and bind/connect/listen/accept/sendto/recvfrom return -ENOSYS until B2
 * (UdpSocket) / B3 (TcpSocket) override the virtuals.  These tests prove the
 * plumbing -- socket() allocates a real fd backed by a SocketOps inode, arg
 * validation rejects a bad family/type, the fd resolves back to the Socket, and
 * the stub send() is reached.  Real send/recv + loopback echo land with B2/B3.
 *
 * The stub Socket needs no net stack, so this runs in the test kernel (which
 * does not bring up production net_init) -- the create_socket() factory returns
 * a bare Socket for AF_INET regardless of g_ready in B1b.
 */

#include <stdint.h>

#include "kernel/errno.hpp"                   // kEafnosupport / kEprotonosupport
#include "kernel/fs/file.hpp"                 // File
#include "kernel/fs/vfs_mount.hpp"            // current_fd_table()
#include "kernel/net/arp.hpp"                 // ArpModule (echo stack)
#include "kernel/net/icmp.hpp"                // IcmpModule
#include "kernel/net/ipv4.hpp"                // Ipv4Module / kIpProto*
#include "kernel/net/loopback_device.hpp"     // LoopbackDevice
#include "kernel/net/net_stack.hpp"           // NetStack / InDevice
#include "kernel/net/socket.hpp"              // Socket / socket_ops / kAfInet / kSock*
#include "kernel/net/tcp.hpp"                 // TcpModule
#include "kernel/net/tcp_socket.hpp"          // TcpSocket
#include "kernel/net/udp.hpp"                 // UdpModule
#include "kernel/net/udp_socket.hpp"          // UdpSocket
#include "kernel/net/unix_socket.hpp"         // UnixSocket (F8-M3)
#include "kernel/syscall/sys_accept4.hpp"     // sys_accept4 (F-ECO batch 7a)
#include "kernel/syscall/sys_close.hpp"       // sys_close
#include "kernel/syscall/sys_getsockopt.hpp"  // sys_getsockopt / do_getsockopt_kernel (F-ECO batch 7a)
#include "kernel/syscall/sys_setsockopt.hpp"  // sys_setsockopt (F-ECO batch 7a)
#include "kernel/syscall/sys_socket.hpp"      // sys_socket
#include "kernel/syscall/sys_socketpair.hpp"  // do_socketpair_kernel (F-ECO batch 7b)
#include "kernel/test/big_kernel_test.h"

using cinux::fs::current_fd_table;
using cinux::net::ArpModule;
using cinux::net::IcmpModule;
using cinux::net::InDevice;
using cinux::net::Ipv4Addr;
using cinux::net::Ipv4Module;
using cinux::net::LoopbackDevice;
using cinux::net::NetDevice;
using cinux::net::NetStack;
using cinux::net::Socket;
using cinux::net::TcpModule;
using cinux::net::TcpSocket;
using cinux::net::UdpModule;
using cinux::net::UdpSocket;
using cinux::net::UnixSocket;
using cinux::net::kAfInet;
using cinux::net::kAfUnix;
using cinux::net::kEtherTypeArp;
using cinux::net::kEtherTypeIpv4;
using cinux::net::kIpProtoTcp;
using cinux::net::kIpProtoUdp;
using cinux::net::kLoopbackAddr;
using cinux::net::kSockDgram;
using cinux::net::kSockStream;
using cinux::net::socket_ops;
using cinux::syscall::sys_close;
using cinux::syscall::sys_socket;
using cinux::syscall::sys_accept4;           // F-ECO batch 7a
using cinux::syscall::sys_setsockopt;        // F-ECO batch 7a
using cinux::syscall::sys_getsockopt;        // F-ECO batch 7a
using cinux::syscall::do_getsockopt_kernel;  // F-ECO batch 7a
using cinux::syscall::do_socketpair_kernel;  // F-ECO batch 7b

namespace test_socket {

static constexpr uint64_t kFiller = 0;

void test_socket_dgram_returns_fd() {
    int64_t fd = sys_socket(kAfInet, kSockDgram, 0, kFiller, kFiller, kFiller);
    TEST_ASSERT_GE(fd, 0);
    current_fd_table().close(static_cast<int>(fd));  // do not leak the fd
}

void test_socket_stream_returns_fd() {
    int64_t fd = sys_socket(kAfInet, kSockStream, 0, kFiller, kFiller, kFiller);
    TEST_ASSERT_GE(fd, 0);
    current_fd_table().close(static_cast<int>(fd));  // do not leak the fd
}

void test_socket_rejects_bad_family() {
    // AF_INET6 (10) -- supported families are AF_UNIX (1) and AF_INET (2).
    int64_t r = sys_socket(10, kSockDgram, 0, kFiller, kFiller, kFiller);
    TEST_ASSERT_EQ(r, -cinux::kEafnosupport);
}

void test_socket_rejects_bad_type() {
    // SOCK_RDM (4) / an unsupported type -- STREAM / DGRAM / RAW are supported,
    // anything else is -EPROTONOSUPPORT.  (SOCK_RAW=3 was added for busybox
    // ping; pick a value outside the supported set.)
    int64_t r = sys_socket(kAfInet, 4, 0, kFiller, kFiller, kFiller);
    TEST_ASSERT_EQ(r, -cinux::kEprotonosupport);
}

void test_socket_fd_routes_to_socket_stub() {
    int64_t fd = sys_socket(kAfInet, kSockDgram, 0, kFiller, kFiller, kFiller);
    TEST_ASSERT_GE(fd, 0);

    cinux::fs::File* f = current_fd_table().get(static_cast<int>(fd));
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_NOT_NULL(f->inode);
    // The fd's inode ops IS the shared SocketOps -> it is a socket fd.
    TEST_ASSERT_EQ(f->inode->ops, &socket_ops());

    auto* s = static_cast<Socket*>(f->inode->fs_private);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQ(s->type(), kSockDgram);

    // Dispatch reaches the Socket: send on an unconnected/unconfigured socket
    // fails (UdpSocket -> InvalidArgument; a bare stub -> NotImplemented).  The
    // exact errno is protocol-specific; here we only assert the fd routes to a
    // live Socket that answers.
    uint8_t b = 0;
    auto    r = s->send(&b, 0);
    TEST_ASSERT_FALSE(r.ok());

    // close() frees the File (Socket/Inode share the pipe-style hobby leak).
    TEST_ASSERT_EQ(
        sys_close(static_cast<uint64_t>(fd), kFiller, kFiller, kFiller, kFiller, kFiller), 0);
}

// --- B2: UdpSocket end-to-end over a deterministic loopback stack ---
namespace {
// echo_route is a function pointer (UdpSocket::DevRoute) so it cannot capture;
// the loopback device it returns lives at file scope.
LoopbackDevice echo_lo;
NetDevice&     echo_route(Ipv4Addr /*dst*/) {
    return echo_lo;
}
// A second loopback for the TCP echo test (a NetDevice is attached to one stack).
LoopbackDevice tcp_echo_lo;
NetDevice&     tcp_echo_route(Ipv4Addr /*dst*/) {
    return tcp_echo_lo;
}
}  // namespace

void test_udp_socket_loopback_echo() {
    // Function-local statics mirror test_net::test_udp_loopback: the modules +
    // LoopbackDevice are too large for the 16 KB kernel stack.
    static ArpModule  arp;
    static IcmpModule icmp;
    static Ipv4Module ipv4(icmp, &arp);
    static UdpModule  udp;
    static NetStack   stack;
    static bool       init = false;
    if (!init) {
        stack.add_protocol(kEtherTypeArp, arp);
        stack.add_protocol(kEtherTypeIpv4, ipv4);
        ipv4.add_l4(kIpProtoUdp, udp);
        InDevice cfg{};
        cfg.local   = kLoopbackAddr;  // 127.0.0.1
        cfg.gateway = kLoopbackAddr;
        stack.attach(echo_lo, cfg);
        init = true;
    }

    UdpSocket server(udp, ipv4, stack, echo_route);
    UdpSocket client(udp, ipv4, stack, echo_route);
    TEST_ASSERT_TRUE(server.bind(7777).ok());
    TEST_ASSERT_TRUE(client.bind(1234).ok());

    // client -> server (127.0.0.1:7777). One poll drains the loopback round-trip:
    // send enqueues the IP packet, poll dispatches it -> UdpModule -> server.on_udp.
    static const uint8_t msg[] = {'e', 'c', 'h', 'o'};
    auto                 sr    = client.sendto(kLoopbackAddr, 7777, msg, sizeof(msg));
    TEST_ASSERT_TRUE(sr.ok());
    stack.poll();

    uint8_t  rbuf[8] = {};
    Ipv4Addr src{};
    uint16_t sport = 0;
    auto     rr    = server.recv(rbuf, sizeof(rbuf), &src, &sport);
    TEST_ASSERT_TRUE(rr.ok());
    TEST_ASSERT_EQ(*rr, static_cast<int64_t>(sizeof(msg)));
    TEST_ASSERT_EQ(sport, 1234u);  // client's bound source port

    // echo back: server -> client (127.0.0.1:1234).
    auto er = server.sendto(kLoopbackAddr, 1234, rbuf, static_cast<uint32_t>(sizeof(msg)));
    TEST_ASSERT_TRUE(er.ok());
    stack.poll();
    uint8_t cbuf[8] = {};
    auto    cr      = client.recv(cbuf, sizeof(cbuf), nullptr, nullptr);
    TEST_ASSERT_TRUE(cr.ok());
    TEST_ASSERT_EQ(*cr, static_cast<int64_t>(sizeof(msg)));
}

// --- B3: TcpSocket end-to-end over a deterministic loopback stack ---
void test_tcp_socket_loopback_echo() {
    static ArpModule  arp;
    static IcmpModule icmp;
    static Ipv4Module ipv4(icmp, &arp);
    static TcpModule  tcp;
    static NetStack   stack;
    static bool       init = false;
    if (!init) {
        stack.add_protocol(kEtherTypeArp, arp);
        stack.add_protocol(kEtherTypeIpv4, ipv4);
        ipv4.add_l4(kIpProtoTcp, tcp);
        InDevice cfg{};
        cfg.local   = kLoopbackAddr;
        cfg.gateway = kLoopbackAddr;
        stack.attach(tcp_echo_lo, cfg);
        init = true;
    }

    TcpSocket server(tcp, ipv4, stack, tcp_echo_route);
    TcpSocket client(tcp, ipv4, stack, tcp_echo_route);
    TEST_ASSERT_TRUE(server.bind(9999).ok());
    TEST_ASSERT_TRUE(server.listen(1).ok());
    TEST_ASSERT_TRUE(client.connect(kLoopbackAddr, 9999).ok());

    // Drain the 3-way handshake (SYN -> SYN-ACK -> ACK); server.on_accept fires.
    // The poll() budget loop advances the FSM a few rounds each call.
    for (int i = 0; i < 6; ++i) {
        stack.poll();
    }

    Ipv4Addr raddr{};
    uint16_t rport = 0;
    auto     acc   = server.accept(&raddr, &rport);
    TEST_ASSERT_TRUE(acc.ok());
    TEST_ASSERT_NOT_NULL(*acc);
    auto* child = static_cast<TcpSocket*>(*acc);

    // client -> server child.
    static const uint8_t msg[] = {'p', 'i', 'n', 'g'};
    auto                 sr    = client.send(msg, sizeof(msg));
    TEST_ASSERT_TRUE(sr.ok());
    for (int i = 0; i < 4; ++i) {
        stack.poll();
    }
    uint8_t rbuf[8] = {};
    auto    rr      = child->recv(rbuf, sizeof(rbuf), nullptr, nullptr);
    TEST_ASSERT_TRUE(rr.ok());
    TEST_ASSERT_EQ(*rr, static_cast<int64_t>(sizeof(msg)));
    bool payload_ok = true;
    for (uint32_t i = 0; i < sizeof(msg); ++i) {
        if (rbuf[i] != msg[i]) {
            payload_ok = false;
        }
    }
    TEST_ASSERT_TRUE(payload_ok);

    // echo back: server child -> client.
    auto er = child->send(rbuf, static_cast<uint32_t>(sizeof(msg)));
    TEST_ASSERT_TRUE(er.ok());
    for (int i = 0; i < 4; ++i) {
        stack.poll();
    }
    uint8_t cbuf[8] = {};
    auto    cr      = client.recv(cbuf, sizeof(cbuf), nullptr, nullptr);
    TEST_ASSERT_TRUE(cr.ok());
    TEST_ASSERT_EQ(*cr, static_cast<int64_t>(sizeof(msg)));
}

// --- F8-M3: AF_UNIX socket ---
//
// AF_UNIX has no NIC / L4 module / loopback device: connect_path() wires two
// sockets as peers through an in-memory name registry (UnixRegistry), and send()
// copies bytes straight into the peer's RX ring.  test_unix_socket_returns_fd
// drives the SYSCALL creation path (sys_socket(AF_UNIX) -> UnixSocket behind a
// SocketOps fd).  The echo + negatives drive the UnixSocket METHODS directly --
// the same choice the AF_INET TCP/UDP echo tests make, because the ring0 test
// kernel cannot hand sys_bind a "user" address (copy_from_user's is_user_vaddr
// range check rejects kernel-stack pointers, so the sockaddr_un parsing in
// sys_bind is exercised in production via musl, covered here by inspection).
void test_unix_socket_returns_fd() {
    int64_t fd = sys_socket(kAfUnix, kSockStream, 0, kFiller, kFiller, kFiller);
    TEST_ASSERT_GE(fd, 0);

    cinux::fs::File* f = current_fd_table().get(static_cast<int>(fd));
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_NOT_NULL(f->inode);
    // The fd's inode ops IS the shared SocketOps -> it is a socket fd.
    TEST_ASSERT_EQ(f->inode->ops, &socket_ops());
    auto* s = static_cast<Socket*>(f->inode->fs_private);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQ(s->domain(), kAfUnix);
    TEST_ASSERT_EQ(s->type(), kSockStream);

    TEST_ASSERT_EQ(
        sys_close(static_cast<uint64_t>(fd), kFiller, kFiller, kFiller, kFiller, kFiller), 0);
}

void test_unix_socket_loopback_echo() {
    // Function-local statics (the 4 KB RX ring makes each UnixSocket too large
    // for two on the 16 KB kernel stack -- mirrors the TcpSocket echo test).
    static UnixSocket server(kSockStream);
    static UnixSocket client(kSockStream);

    TEST_ASSERT_TRUE(server.bind_path("/unix_echo").ok());
    TEST_ASSERT_TRUE(server.listen(1).ok());
    TEST_ASSERT_TRUE(client.connect_path("/unix_echo").ok());

    // client -> server: send BEFORE accept.  connect_path already created the
    // accepted child + wired the pair as peers, so bytes buffer in the child's
    // RX ring until accept() pulls it off.
    static const uint8_t msg[] = {'u', 'n', 'i', 'x'};
    auto                 sr    = client.send(msg, sizeof(msg));
    TEST_ASSERT_TRUE(sr.ok());
    TEST_ASSERT_EQ(*sr, static_cast<int64_t>(sizeof(msg)));

    // server: accept the child + recv the EXACT bytes (semantic match, not count).
    auto acc = server.accept(nullptr, nullptr);
    TEST_ASSERT_TRUE(acc.ok());
    TEST_ASSERT_NOT_NULL(*acc);
    Socket* child   = *acc;
    uint8_t rbuf[8] = {};
    auto    rr      = child->recv(rbuf, sizeof(rbuf), nullptr, nullptr);
    TEST_ASSERT_TRUE(rr.ok());
    TEST_ASSERT_EQ(*rr, static_cast<int64_t>(sizeof(msg)));
    bool payload_ok = true;
    for (uint32_t i = 0; i < sizeof(msg); ++i) {
        if (rbuf[i] != msg[i]) {
            payload_ok = false;
        }
    }
    TEST_ASSERT_TRUE(payload_ok);

    // echo back: server child -> client.
    auto er = child->send(rbuf, static_cast<uint32_t>(sizeof(msg)));
    TEST_ASSERT_TRUE(er.ok());
    TEST_ASSERT_EQ(*er, static_cast<int64_t>(sizeof(msg)));
    uint8_t cbuf[8] = {};
    auto    cr      = client.recv(cbuf, sizeof(cbuf), nullptr, nullptr);
    TEST_ASSERT_TRUE(cr.ok());
    TEST_ASSERT_EQ(*cr, static_cast<int64_t>(sizeof(msg)));
    bool echo_ok = true;
    for (uint32_t i = 0; i < sizeof(msg); ++i) {
        if (cbuf[i] != msg[i]) {
            echo_ok = false;
        }
    }
    TEST_ASSERT_TRUE(echo_ok);
}

void test_unix_socket_connect_unbound() {
    UnixSocket client(kSockStream);
    auto       r = client.connect_path("/unix_nobody");  // never bound
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQ(r.error(), cinux::lib::Error::NotFound);  // registry miss -> ENOENT
}

void test_unix_socket_bind_duplicate() {
    static UnixSocket a(kSockStream);
    static UnixSocket b(kSockStream);
    TEST_ASSERT_TRUE(a.bind_path("/unix_dup").ok());
    auto r = b.bind_path("/unix_dup");
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQ(r.error(), cinux::lib::Error::AlreadyExists);  // -> EEXIST
}

// --- F-ECO batch 7a: accept4 + setsockopt/getsockopt (socket-syscall alignment) ---
namespace {
constexpr uint64_t kSolSocketB7   = 1;         // SOL_SOCKET
constexpr uint64_t kSoTypeB7      = 3;         // SO_TYPE
constexpr uint64_t kSoReuseaddrB7 = 2;         // SO_REUSEADDR
constexpr uint64_t kSockCloexecB7 = 02000000;  // SOCK_CLOEXEC (Linux O_CLOEXEC)
}  // namespace

void test_setsockopt_accepts_options() {
    int64_t fd = sys_socket(kAfUnix, kSockStream, 0, kFiller, kFiller, kFiller);
    TEST_ASSERT_GE(fd, 0);
    // Any option at SOL_SOCKET is accepted as a no-op (no socket-option storage).
    // optval is ignored by the handler, so the ring0 test kernel can pass a
    // kernel addr without hitting copy_from_user.
    uint8_t optval = 1;
    int64_t r      = sys_setsockopt(static_cast<uint64_t>(fd), kSolSocketB7, kSoReuseaddrB7,
                                    reinterpret_cast<uint64_t>(&optval), sizeof(optval), 0);
    TEST_ASSERT_EQ(r, 0);
    // Bad fd -> -EBADF.
    TEST_ASSERT_EQ(sys_setsockopt(999, kSolSocketB7, kSoReuseaddrB7,
                                  reinterpret_cast<uint64_t>(&optval), sizeof(optval), 0),
                   -cinux::kEbadf);
    sys_close(static_cast<uint64_t>(fd), kFiller, kFiller, kFiller, kFiller, kFiller);
}

void test_getsockopt_so_type() {
    int64_t sfd = sys_socket(kAfUnix, kSockStream, 0, kFiller, kFiller, kFiller);
    int64_t dfd = sys_socket(kAfUnix, kSockDgram, 0, kFiller, kFiller, kFiller);
    TEST_ASSERT_GE(sfd, 0);
    TEST_ASSERT_GE(dfd, 0);
    int32_t v = 0;
    TEST_ASSERT_EQ(do_getsockopt_kernel(static_cast<uint64_t>(sfd), kSolSocketB7, kSoTypeB7, &v),
                   0);
    TEST_ASSERT_EQ(v, 1);  // SOCK_STREAM
    TEST_ASSERT_EQ(do_getsockopt_kernel(static_cast<uint64_t>(dfd), kSolSocketB7, kSoTypeB7, &v),
                   0);
    TEST_ASSERT_EQ(v, 2);  // SOCK_DGRAM
    sys_close(static_cast<uint64_t>(sfd), kFiller, kFiller, kFiller, kFiller, kFiller);
    sys_close(static_cast<uint64_t>(dfd), kFiller, kFiller, kFiller, kFiller, kFiller);
}

void test_getsockopt_bad_level_and_fd() {
    int64_t fd = sys_socket(kAfUnix, kSockStream, 0, kFiller, kFiller, kFiller);
    TEST_ASSERT_GE(fd, 0);
    int32_t v = 0;
    // Non-SOL_SOCKET level (IPPROTO_TCP=6) -> -EOPNOTSUPP.
    TEST_ASSERT_EQ(do_getsockopt_kernel(static_cast<uint64_t>(fd), 6, kSoTypeB7, &v),
                   -cinux::kEopnotsupp);
    // Bad fd -> -EBADF.
    TEST_ASSERT_EQ(do_getsockopt_kernel(999, kSolSocketB7, kSoTypeB7, &v), -cinux::kEbadf);
    sys_close(static_cast<uint64_t>(fd), kFiller, kFiller, kFiller, kFiller, kFiller);
}

void test_accept4_cloexec_flag() {
    // Server: a UnixSocket bound + listening, installed as an fd so accept4 can
    // resolve it (install_socket_fd wraps the Socket* in a SocketOps inode).
    UnixSocket* server = new UnixSocket(kSockStream);
    int64_t     sfd    = cinux::syscall::install_socket_fd(server);
    TEST_ASSERT_GE(sfd, 0);
    TEST_ASSERT_TRUE(server->bind_path("/un_acc4").ok());
    TEST_ASSERT_TRUE(server->listen(1).ok());
    UnixSocket client(kSockStream);
    TEST_ASSERT_TRUE(client.connect_path("/un_acc4").ok());

    // accept4 with SOCK_CLOEXEC -> the accepted fd is close-on-exec.
    int64_t nfd = sys_accept4(static_cast<uint64_t>(sfd), 0, 0, kSockCloexecB7, 0, 0);
    TEST_ASSERT_GE(nfd, 0);
    cinux::fs::File* nf = current_fd_table().get(static_cast<int>(nfd));
    TEST_ASSERT_NOT_NULL(nf);
    TEST_ASSERT_TRUE(nf->cloexec);

    // Without the flag, the accepted fd is NOT cloexec (regression guard).
    UnixSocket client2(kSockStream);
    TEST_ASSERT_TRUE(client2.connect_path("/un_acc4").ok());
    int64_t nfd2 = sys_accept4(static_cast<uint64_t>(sfd), 0, 0, 0, 0, 0);
    TEST_ASSERT_GE(nfd2, 0);
    cinux::fs::File* nf2 = current_fd_table().get(static_cast<int>(nfd2));
    TEST_ASSERT_NOT_NULL(nf2);
    TEST_ASSERT_FALSE(nf2->cloexec);

    sys_close(static_cast<uint64_t>(sfd), kFiller, kFiller, kFiller, kFiller, kFiller);
    sys_close(static_cast<uint64_t>(nfd), kFiller, kFiller, kFiller, kFiller, kFiller);
    sys_close(static_cast<uint64_t>(nfd2), kFiller, kFiller, kFiller, kFiller, kFiller);
}

// --- F-ECO batch 7b: getsockname/getpeername + shutdown + socketpair ---
// These exercise the Socket virtuals / pair_with directly (the sys_getsockname
// etc. user-boundary shims just copy_to_user the same bytes -- is_user_vaddr
// keeps the ring0 test kernel off that path, same as every other batch).
void test_unix_getsockname_local_addr() {
    static UnixSocket s(kSockStream);
    TEST_ASSERT_TRUE(s.bind_path("/un_gsn").ok());
    cinux::net::SockAddrStorage st;
    TEST_ASSERT_TRUE(s.get_local_addr(&st));
    auto* sa = reinterpret_cast<cinux::net::SockAddrUn*>(st.bytes);
    TEST_ASSERT_EQ(sa->family, kAfUnix);
    const char* expected = "/un_gsn";
    bool        path_ok  = true;
    for (uint32_t i = 0; expected[i] != '\0' || sa->path[i] != '\0'; ++i) {
        if (expected[i] != sa->path[i]) {
            path_ok = false;
            break;
        }
    }
    TEST_ASSERT_TRUE(path_ok);
}

void test_unix_getpeer_addr_connected() {
    static UnixSocket a(kSockStream);
    static UnixSocket b(kSockStream);
    a.pair_with(&b);  // wire a<->b as peers
    cinux::net::SockAddrStorage st;
    TEST_ASSERT_TRUE(a.get_peer_addr(&st));  // connected -> peer name exists
    auto* sa = reinterpret_cast<cinux::net::SockAddrUn*>(st.bytes);
    TEST_ASSERT_EQ(sa->family, kAfUnix);  // anonymous peer: family set, empty path
    // An unconnected socket reports no peer.
    static UnixSocket c(kSockStream);
    TEST_ASSERT_FALSE(c.get_peer_addr(&st));
}

void test_shutdown_directions() {
    static UnixSocket a(kSockStream);
    static UnixSocket b(kSockStream);
    a.pair_with(&b);
    // SHUT_WR -> further send is EPIPE-shaped (BrokenPipe).
    a.do_shutdown(cinux::net::kShutWr);
    uint8_t msg = 'x';
    auto    sr  = a.send(&msg, 1);
    TEST_ASSERT_FALSE(sr.ok());
    TEST_ASSERT_EQ(sr.error(), cinux::lib::Error::BrokenPipe);
    // SHUT_RD -> further recv is EOF (0).
    b.do_shutdown(cinux::net::kShutRd);
    uint8_t buf = 0;
    auto    rr  = b.recv(&buf, 1, nullptr, nullptr);
    TEST_ASSERT_TRUE(rr.ok());
    TEST_ASSERT_EQ(*rr, 0);
}

void test_socketpair_roundtrip() {
    int     sv[2] = {-1, -1};
    int64_t r     = do_socketpair_kernel(kAfUnix, kSockStream, sv);
    TEST_ASSERT_EQ(r, 0);
    TEST_ASSERT_GE(sv[0], 0);
    TEST_ASSERT_NE(sv[0], sv[1]);
    // The two fds are wired as peers: write via sv[0] -> read via sv[1].  Drive
    // the pipe via the inode ops with a KERNEL buffer (sys_write needs a user
    // ptr the ring0 test kernel cannot supply).
    cinux::fs::File* f0 = current_fd_table().get(sv[0]);
    cinux::fs::File* f1 = current_fd_table().get(sv[1]);
    TEST_ASSERT_NOT_NULL(f0);
    TEST_ASSERT_NOT_NULL(f1);
    uint8_t m  = 'P';
    auto    wr = f0->inode->ops->write(f0->inode, 0, &m, 1);
    TEST_ASSERT_TRUE(wr.ok());
    TEST_ASSERT_EQ(*wr, 1);
    uint8_t got = 0;
    auto    rd  = f1->inode->ops->read(f1->inode, 0, &got, 1);
    TEST_ASSERT_TRUE(rd.ok());
    TEST_ASSERT_EQ(*rd, 1);
    TEST_ASSERT_EQ(got, 'P');
    sys_close(static_cast<uint64_t>(sv[0]), kFiller, kFiller, kFiller, kFiller, kFiller);
    sys_close(static_cast<uint64_t>(sv[1]), kFiller, kFiller, kFiller, kFiller, kFiller);
}

}  // namespace test_socket

extern "C" void run_socket_tests() {
    TEST_SECTION("F7-M6 socket (B1b plumbing)");
    RUN_TEST(test_socket::test_socket_dgram_returns_fd);
    RUN_TEST(test_socket::test_socket_stream_returns_fd);
    RUN_TEST(test_socket::test_socket_rejects_bad_family);
    RUN_TEST(test_socket::test_socket_rejects_bad_type);
    RUN_TEST(test_socket::test_socket_fd_routes_to_socket_stub);
    RUN_TEST(test_socket::test_udp_socket_loopback_echo);
    RUN_TEST(test_socket::test_tcp_socket_loopback_echo);
    TEST_SECTION("F8-M3 AF_UNIX socket");
    RUN_TEST(test_socket::test_unix_socket_returns_fd);
    RUN_TEST(test_socket::test_unix_socket_loopback_echo);
    RUN_TEST(test_socket::test_unix_socket_connect_unbound);
    RUN_TEST(test_socket::test_unix_socket_bind_duplicate);
    TEST_SECTION("F-ECO batch 7a socket-syscall alignment");
    RUN_TEST(test_socket::test_setsockopt_accepts_options);
    RUN_TEST(test_socket::test_getsockopt_so_type);
    RUN_TEST(test_socket::test_getsockopt_bad_level_and_fd);
    RUN_TEST(test_socket::test_accept4_cloexec_flag);
    TEST_SECTION("F-ECO batch 7b getsockname/getpeername + shutdown + socketpair");
    RUN_TEST(test_socket::test_unix_getsockname_local_addr);
    RUN_TEST(test_socket::test_unix_getpeer_addr_connected);
    RUN_TEST(test_socket::test_shutdown_directions);
    RUN_TEST(test_socket::test_socketpair_roundtrip);
    TEST_SUMMARY();
}
