/**
 * @file    loader.hpp
 * @brief   Placing one static ELF image inside an address space.
 *
 * The mapping half of exec: everything the parser accepted becomes
 * pages — each file-backed byte lands where its segment says, and the
 * gap between file bytes and memory bytes arrives as zeros, so bss
 * starts life clean without the image carrying it. Pages are mapped
 * whole even when a segment begins mid-page: the head of the first
 * page belongs to whatever the linker placed before it, and the tail
 * past the last file byte is zero, which is exactly the memory an
 * initialized-then-bss image expects.
 *
 * @author  Charliechen114514
 * @date    2026-10-12
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_loader
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "kernel/fs/vfs.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/vma.hpp"

namespace cinux::loader {

/**
 * @brief   Where one image lands: the world, the register, the source.
 * @note    One bundle because the three travel together on every exec
 *          call: pages go into the space, the segment report goes into
 *          the register, and the register's file-backed ranges name the
 *          node the image was read from. The node rides by value; the
 *          register keeps it as identity, never as authority. The face
 *          beside it travels too, so the register's promises stay
 *          serviceable after exec returns and the FsView they came
 *          from is gone.
 * @since   0.2.0
 * @ingroup kernel_loader
 */
struct LoadWorld {
    const mm::AddressSpace& space;        ///< The freshly built world pages land in.
    mm::VmaStore&           vmas;         ///< The range register segments report to.
    fs::FsNode              file{};       ///< The node the image bytes came from.
    const fs::FsOps*        file_ops{};   ///< The backend face serving file's bytes.
    void*                   file_self{};  ///< The backend instance behind file_ops.
};

/**
 * @brief         Maps every loadable segment of one image into a world
 *                and reports each to the range register.
 *
 * @param[in]     data    The image bytes.
 * @param[in]     bytes   How many are readable at data.
 * @param[in,out] world   The space to fill, the register to report to,
 *                        and the source node.
 * @param[out]    entry   The first instruction's address.
 * @return        true when the image parsed, every page mapped, and
 *                every segment registered; false when the parser refused
 *                or a segment's range collided with one already recorded.
 * @note          Allocates one frame per page the segments cover and
 *                writes through the direct map, so the live CR3 never
 *                matters here. A segment running past the file stops
 *                the machine loudly rather than mapping garbage.
 * @warning       None
 * @throws        None
 * @since         0.2.0
 * @ingroup       kernel_loader
 */
[[nodiscard]] bool LoadElfIntoSpace(const std::uint8_t* data, std::size_t bytes, LoadWorld& world,
                                    std::uint64_t& entry);

}  // namespace cinux::loader
