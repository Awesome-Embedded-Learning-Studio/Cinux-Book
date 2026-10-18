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

/**
 * @brief         Writes the hosted entry picture onto one zeroed stack
 *                page and hands back the entry stack pointer.
 *
 * @param[in,out] stack_page    The page's bytes through the direct map;
 *                              its low end receives the argument and
 *                              environment strings, NUL included.
 * @param[in]     entry_rsp     Where the first instruction finds the
 *                              stack pointer; the pointer words start
 *                              here and climb.
 * @param[in]     argv          The argument runs, NUL-terminated, ended
 *                              by a null entry.
 * @param[in]     envp          The environment runs, ended by a null
 *                              entry; may be null itself for none.
 * @return        The entry stack pointer, the same value handed in.
 * @note          The picture is the hosted shape: argc, argv with its
 *              null end, envp with its null end, then a bare AT_NULL
 *              auxiliary vector — the layout musl's first instructions
 *              already know how to walk. Strings land at the page's
 *              low end and climb toward the pointers; the caller owns
 *              keeping both halves inside the one page.
 * @warning       None
 * @throws        None
 * @since         0.3.0
 * @ingroup       kernel_loader
 */
unsigned long WriteEntryPicture(unsigned char* stack_page, unsigned long entry_rsp,
                                const char* const* argv, const char* const* envp);

}  // namespace cinux::loader
