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

#include "kernel/mm/address_space.hpp"

namespace cinux::loader {

/**
 * @brief         Maps every loadable segment of one image into a space.
 *
 * @param[in]     data    The image bytes.
 * @param[in]     bytes   How many are readable at data.
 * @param[out]    space   The freshly built world the image lands in.
 * @param[out]    entry   The first instruction's address.
 * @return        true when the image parsed and every page mapped;
 *                false when the parser refused it.
 * @note          Allocates one frame per page the segments cover and
 *                writes through the direct map, so the live CR3 never
 *                matters here. A segment running past the file stops
 *                the machine loudly rather than mapping garbage.
 * @warning       None
 * @throws        None
 * @since         0.1.0
 * @ingroup       kernel_loader
 */
[[nodiscard]] bool LoadElfIntoSpace(const std::uint8_t* data, std::size_t bytes,
                                    const mm::AddressSpace& space, std::uint64_t& entry);

}  // namespace cinux::loader
