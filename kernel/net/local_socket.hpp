/**
 * @file local_socket.hpp
 * @brief Construction face for local stream and datagram sockets.
 * @author Charliechen114514
 * @date 2026-10-25
 * @version 0.1
 * @since 0.8.0
 * @ingroup kernel_net
 * @copyright Copyright (c) 2026
 */
#pragma once

#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/net/socket.hpp"

namespace cinux::net {
/**
 * @brief Constructs an unbound local socket.
 * @param[in] type Stream or datagram type.
 * @return Socket ownership, empty on allocation failure.
 */
[[nodiscard]] base::Shared<Socket> MakeLocalSocket(int type);
/**
 * @brief Constructs two connected local endpoints.
 * @param[in] type Stream or datagram type.
 * @param[out] pair The two socket owners.
 * @return Success or allocation failure.
 */
[[nodiscard]] base::Result<void>   MakeLocalPair(int type, base::Shared<Socket> (&pair)[2]);
}  // namespace cinux::net
