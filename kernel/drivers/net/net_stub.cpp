/**
 * @file kernel/drivers/net/net_stub.cpp
 * @brief No-op stubs for the net boot API when CINUX_NET is off.
 *
 * CODING-TASTE §14: net is a CMake file gate, not a source-level #ifdef.  When
 * CINUX_NET is off, e1000.cpp / e1000_init.cpp / net_init.cpp are not compiled;
 * this file supplies empty cinux::drivers::net::init() + cinux::net::init() /
 * ping() so main.cpp's call sites link with no #ifdef.  Exactly one of
 * {e1000_init.cpp + net_init.cpp, net_stub.cpp} is selected by drivers/CMakeLists.txt.
 */

#include "e1000_init.hpp"
#include "kernel/net/net_init.hpp"

namespace cinux::drivers::net {

void init() {
    // Net compiled out -- nothing to bring up.
}

}  // namespace cinux::drivers::net

namespace cinux::net {

void init() {
    // L3 stack compiled out -- no-op.
}

cinux::lib::ErrorOr<PingResult> ping(Ipv4Addr /*dst*/, uint16_t /*id*/, uint16_t /*seq*/) {
    return cinux::lib::Error::NotImplemented;  // no stack -> ping unavailable
}

Socket* create_socket(int /*domain*/, int /*type*/) {
    return nullptr;  // net compiled out -> sys_socket maps to -EPROTONOSUPPORT
}

}  // namespace cinux::net
