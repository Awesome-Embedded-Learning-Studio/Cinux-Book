/**
 * @file nvme.hpp
 * @brief Serialized NVMe block device with checked completions.
 * @author Charliechen114514
 * @date 2026-10-25
 * @version 0.1
 * @since 0.8.0
 * @ingroup kernel_driver
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <cstdint>

#include "cinux/singleton.hpp"
#include "kernel/driver/block/block_device_concept.hpp"

namespace cinux::driver {

/** @brief The first NVM namespace, behind the common block-device face. */
class Nvme : public base::Singleton<Nvme> {
    friend class base::Singleton<Nvme>;

public:
    /** @brief Identifies and enables the controller. @return True on complete initialization. */
    bool                        init();
    /** @brief Whether a usable namespace is online. @return Online state. */
    [[nodiscard]] bool          ready() const;
    /** @brief Whether this is the generated scratch namespace. @return Scratch identity match. */
    [[nodiscard]] bool          scratch_namespace() const;
    /** @brief Reads sectors. @param[in] first Start sector. @param[in] span Sector count.
     * @param[out] sink Destination. @return True on checked completion. */
    bool                        read_blocks(Lba first, BlockSpan span, std::uint8_t* sink);
    /** @brief Writes sectors. @param[in] first Start sector. @param[in] span Sector count.
     * @param[in] source Source bytes. @return True on checked completion. */
    bool                        write_blocks(Lba first, BlockSpan span, const std::uint8_t* source);
    /** @brief Number of namespace blocks. @return Block count. */
    [[nodiscard]] std::uint64_t block_count() const;
    /** @brief Namespace block size. @return Bytes per block. */
    [[nodiscard]] unsigned long block_size() const;
    /** @brief Flushes namespace writes. @return True on checked completion. */
    bool                        flush();
    /** @brief Exercises queue wrapping while restoring the tested sector. @return True on complete
     * round trips. */
    bool                        verify_io();

private:
    Nvme() = default;
    struct State;
    State* state_{};
};

static_assert(BlockDevice<Nvme>);
}  // namespace cinux::driver
