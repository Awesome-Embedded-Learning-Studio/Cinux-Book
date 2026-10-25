/**
 * @file nvme_layout.hpp
 * @brief NVMe wire command and completion grammar.
 * @author Charliechen114514
 * @date 2026-10-25
 * @version 0.1
 * @since 0.8.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "cinux/bit_ops/bitmask.hpp"

namespace cinux::driver {
/** @brief One 64-byte NVMe submission entry. */
struct NvmeCommand {
    std::uint32_t words[16]{};  ///< Sixteen little-endian command words.
};
/** @brief One 16-byte NVMe completion entry. */
struct NvmeCompletion {
    std::uint32_t result{};           ///< Command-specific result.
    std::uint32_t reserved{};         ///< Reserved word.
    std::uint16_t submission_head{};  ///< Controller's SQ cursor.
    std::uint16_t queue{};            ///< Originating SQ ID.
    std::uint16_t identity{};         ///< Submitted command identity.
    std::uint16_t status{};           ///< Phase, status code, retry and error flags.
};
static_assert(sizeof(NvmeCommand) == 64);
static_assert(sizeof(NvmeCompletion) == 16);
static_assert(offsetof(NvmeCompletion, status) == 14);

/**
 * @brief Decodes completion success, including queue, identity and phase.
 * @param[in] completion Reported completion.
 * @param[in] expected Expected command identity.
 * @param[in] queue Expected submission queue.
 * @param[in] phase Expected ring phase.
 * @return True only for this command's successful completion.
 */
constexpr bool NvmeCompleted(const NvmeCompletion& completion, std::uint16_t expected,
                             std::uint16_t queue, bool phase) {
    const base::bit::BitMask<std::uint32_t> kStatus{completion.status};
    return kStatus.extract({.low = 0, .width = 1}) == static_cast<std::uint32_t>(phase) &&
           kStatus.extract({.low = 1, .width = 11}) == 0 && completion.identity == expected &&
           completion.queue == queue;
}
}  // namespace cinux::driver
