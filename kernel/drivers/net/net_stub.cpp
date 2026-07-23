/**
 * @file kernel/drivers/net/net_stub.cpp
 * @brief No-op stub for the net boot API when CINUX_NET is off
 *
 * CODING-TASTE §14: the net subsystem is a CMake file gate, not a source-level
 * #ifdef.  When CINUX_NET is off, e1000.cpp / e1000_init.cpp are not compiled;
 * this file supplies an empty net::init() so callers (kernel/main.cpp) link
 * without any #ifdef at the call site.  Exactly one of {e1000_init.cpp,
 * net_stub.cpp} is selected by drivers/CMakeLists.txt.
 *
 * Namespace: cinux::drivers::net
 */

#include "e1000_init.hpp"

namespace cinux::drivers::net {

void init() {
    // Net compiled out -- nothing to bring up.
}

}  // namespace cinux::drivers::net
