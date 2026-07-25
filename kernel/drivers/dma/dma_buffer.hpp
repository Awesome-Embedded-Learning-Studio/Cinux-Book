/**
 * @file kernel/drivers/dma/dma_buffer.hpp
 * @brief DmaBuffer -- move-only handle pairing a virtual mapping with its bus address
 *
 * DmaBuffer binds a CPU-visible virtual address to its bus address (phys) so a
 * device driver can program a DMA engine with phys() while the kernel touches
 * the same bytes through virt().  Handles are move-only: copying would alias
 * one physical mapping under two owners and invite double release.
 *
 * Ownership.  A handle may carry a release hook (DmaReleaseFn) supplied by its
 * allocator (DmaPool, batch 2).  On destruction a handle that still owns its
 * mapping invokes the hook, returning the pages to the pool.  A handle created
 * without a hook performs no automatic release -- a plain, externally-managed
 * view.  This keeps the type header-only and free of a cyclic dependency on
 * DmaPool: the hook is a plain function pointer, so DmaPool need not be defined
 * here.  detach() surrenders ownership explicitly so a caller can re-home a
 * mapping (hand it to DmaPool::free, pin it in a device register) without
 * tripping the destructor.
 *
 * Namespace: cinux::drivers::dma
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace cinux::drivers::dma {

class DmaBuffer;

/// Release hook invoked by ~DmaBuffer when the handle still owns a mapping.
/// DmaPool (batch 2) installs a static function here so destruction returns
/// the pages automatically.  nullptr means "no automatic release".
using DmaReleaseFn = void (*)(const DmaBuffer& buf);

/**
 * @brief Move-only handle pairing a virtual mapping with its bus address
 *
 * @see DmaPool -- the allocator that produces owning buffers (batch 2).
 */
class DmaBuffer {
public:
    /** @brief Construct an empty (invalid) handle. */
    constexpr DmaBuffer() = default;

    /**
     * @brief Construct a handle over [virt, virt+size) backed by @p phys
     * @param phys    Bus address handed to devices
     * @param virt    CPU-visible address (nullptr => invalid mapping)
     * @param size    Length in bytes
     * @param release Optional release hook (set by DmaPool); nullptr = manual
     */
    DmaBuffer(uint64_t phys, void* virt, std::size_t size, DmaReleaseFn release = nullptr)
        : phys_(phys), virt_(virt), size_(size), release_(release) {}

    DmaBuffer(DmaBuffer&& other) noexcept
        : phys_(other.phys_), virt_(other.virt_), size_(other.size_), release_(other.release_) {
        other.phys_ = 0;
        other.virt_ = nullptr;
        other.size_ = 0;
        other.release_ = nullptr;
    }

    DmaBuffer& operator=(DmaBuffer&& other) noexcept {
        if (this != &other) {
            release_owned();  // return whatever we currently hold
            phys_ = other.phys_;
            virt_ = other.virt_;
            size_ = other.size_;
            release_ = other.release_;
            other.phys_ = 0;
            other.virt_ = nullptr;
            other.size_ = 0;
            other.release_ = nullptr;
        }
        return *this;
    }

    DmaBuffer(const DmaBuffer&) = delete;
    DmaBuffer& operator=(const DmaBuffer&) = delete;

    /** @brief Release the mapping back to its owner (if a hook is set). */
    ~DmaBuffer() { release_owned(); }

    // -- Accessors ------------------------------------------------------

    /** @brief Bus address for device programming. */
    uint64_t phys() const { return phys_; }

    /** @brief CPU-visible address of the mapping (nullptr if invalid). */
    void* virt() const { return virt_; }

    /** @brief Length in bytes. */
    std::size_t size() const { return size_; }

    /** @brief Whether the handle currently owns a mapping. */
    bool valid() const { return virt_ != nullptr; }

    // -- Ownership transfer --------------------------------------------

    /**
     * @brief Surrender ownership and return the raw triple
     *
     * Leaves *this invalid; the caller becomes responsible for the mapping
     * (e.g. handing it to DmaPool::free, or anchoring it in a device).
     */
    void detach(uint64_t& phys, void*& virt, std::size_t& size) {
        phys = phys_;
        virt = virt_;
        size = size_;
        phys_ = 0;
        virt_ = nullptr;
        size_ = 0;
        release_ = nullptr;
    }

private:
    /// Invoke the release hook (if any) for the current mapping, then clear.
    void release_owned() {
        if (release_ != nullptr && virt_ != nullptr) {
            release_(*this);
        }
        phys_ = 0;
        virt_ = nullptr;
        size_ = 0;
        release_ = nullptr;
    }

    uint64_t     phys_    = 0;
    void*        virt_    = nullptr;
    std::size_t  size_    = 0;
    DmaReleaseFn release_ = nullptr;
};

}  // namespace cinux::drivers::dma
