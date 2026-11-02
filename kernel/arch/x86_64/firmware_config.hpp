/**
 * @file firmware_config.hpp
 * @brief Bounded boot-time text reads from QEMU firmware configuration.
 * @author Charliechen114514
 * @date 2026-10-27
 * @version 0.1
 * @ingroup kernel_arch
 */
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace cinux::arch {

/**
 * @brief Selector and byte operations supplied by one firmware transport.
 * @note The caller serializes the complete select/read transaction.
 */
struct FirmwareTransport {
    void* context                        = nullptr;
    void (*select)(void*, std::uint16_t) = nullptr;
    unsigned char (*read)(void*)         = nullptr;
};

/**
 * @brief Read a bounded string through an explicitly supplied transport.
 * @param[in] transport Selector and data port operations.
 * @param[in] name Exact directory name.
 * @param[out] output Destination including the terminating byte.
 * @return True for a complete, valid string.
 */
bool ReadFirmwareString(FirmwareTransport transport, std::string_view name, std::span<char> output);

/**
 * @brief Reads a named firmware string during serialized initial-process setup.
 * @param[in] name Firmware file name.
 * @param[out] output Storage including room for a terminating NUL.
 * @return True for a complete string; false for absent, oversized or malformed data.
 * @note Failure leaves an empty string when output is nonempty.
 * @ingroup kernel_arch
 */
bool ReadFirmwareString(std::string_view name, std::span<char> output);

}  // namespace cinux::arch
