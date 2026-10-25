/**
 * @file local_config.hpp
 * @brief Bounds of the local socket namespace and queues.
 * @author Charliechen114514
 * @date 2026-10-25
 * @version 0.1
 * @since 0.8.0
 * @ingroup kernel_net
 * @copyright Copyright (c) 2026
 */
#pragma once

namespace cinux::net {
/** @brief AF_UNIX family number. */
inline constexpr int           kAfUnix           = 1;
/** @brief Bytes in Linux sockaddr_un.sun_path. */
inline constexpr unsigned long kLocalNameBytes   = 108;
/** @brief Simultaneously bound local names. */
inline constexpr unsigned long kLocalBindings    = 16;
/** @brief Pending connections or datagrams per endpoint. */
inline constexpr unsigned int  kLocalQueue       = 8;
/** @brief Byte capacity of each local stream direction. */
inline constexpr unsigned int  kLocalStreamBytes = 4096;
}  // namespace cinux::net
