/**
 * @file shared_pages.hpp
 * @brief Shared physical pages and per-address-space attachment ownership.
 * @author Charliechen114514
 * @date 2026-10-25
 * @version 0.1
 * @since 0.8.0
 * @ingroup kernel_mm
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <atomic>
#include <utility>

#include "cinux/own.hpp"
#include "cinux/ref_count.hpp"
#include "cinux/shared.hpp"

namespace cinux::mm {

/** @brief Shared page ownership with an injected frame retirement operation. */
struct SharedPages : base::RefCount<> {
    base::Own<unsigned long[]> frames;       ///< Physical frame addresses.
    unsigned long              count{};      ///< Initialized frames in the array.
    void (*retire)(unsigned long){};         ///< Releases one owned frame.
    std::atomic<unsigned long> attachments;  ///< Address spaces attached.

    /** @brief Releases every owned frame through the injected operation. */
    ~SharedPages() {
        for (unsigned long index = 0; index < count; ++index) {
            retire(frames[index]);
        }
    }
};

/** @brief One attachment, shared between split VMAs within an address space. */
struct SharedAttachment : base::RefCount<> {
    base::Shared<SharedPages> pages;    ///< The segment's physical storage.
    unsigned long             start{};  ///< Original attachment base, used by shmdt.
    unsigned long             end{};    ///< Original attachment end.

    /**
     * @brief Counts one address space's attachment.
     * @param[in] storage Segment storage.
     * @param[in] base Attachment start.
     * @param[in] limit Attachment end.
     */
    SharedAttachment(base::Shared<SharedPages> storage, unsigned long base, unsigned long limit)
        : pages(std::move(storage)), start(base), end(limit) {
        pages->attachments.fetch_add(1, std::memory_order_relaxed);
    }

    /** @brief Removes this address space's attachment count. */
    ~SharedAttachment() { pages->attachments.fetch_sub(1, std::memory_order_relaxed); }
};

}  // namespace cinux::mm
