#include "kernel/loader/loader.hpp"

#include <cstddef>
#include <cstdint>

#include "cinux/assert.hpp"
#include "cinux/math.hpp"
#include "cinux/result.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/loader/elf_layout.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vma.hpp"

namespace cinux::loader {

namespace {

using arch::page::kSize;
using arch::page::kUser;
using arch::page::kWritable;

struct ChunkPlan {
    const std::uint8_t* source;
    std::size_t         bytes;
    bool                writable;
};

void map_segment_page(const mm::AddressSpace& space, std::uint64_t spot, const ChunkPlan& chunk) {
    const mm::Pmm::PhysAddr kFrame = mm::Pmm::self().allocate_page();
    cinux::base::safety::Check(kFrame != mm::Pmm::PhysAddr{}, "loader out of frames");
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const kDestination = reinterpret_cast<std::uint8_t*>(mm::DirectMapVirt(kFrame.raw));
    for (std::size_t index = 0; index < kSize; ++index) {
        // NOLINTNEXTLINE(clang-analyzer-optin.core.FixedAddressDereference)
        kDestination[index] = index < chunk.bytes ? chunk.source[index] : 0;
    }
    cinux::arch::page::Entry bits = kUser;
    if (chunk.writable) {
        bits = bits | kWritable;
    }
    cinux::base::safety::Check(space.map_user(spot, kFrame.raw, bits), "loader map refused a page");
}

}  // namespace

bool LoadElfIntoSpace(const std::uint8_t* data, std::size_t bytes, LoadWorld& world,
                      std::uint64_t& entry) {
    const base::Result<ElfImage> kParsed = ParseElf64(data, bytes);
    if (!kParsed.ok()) {
        return false;
    }
    const ElfImage kImage = kParsed.value();
    for (std::uint16_t index = 0; index < kImage.segment_count; ++index) {
        ElfSegment segment{};
        if (!ReadSegment(kImage, index, segment)) {
            continue;
        }
        cinux::base::safety::Check(
            segment.file_offset <= bytes && segment.file_bytes <= bytes - segment.file_offset,
            "loader segment runs past the file");
        const std::uint64_t kPageNumber = cinux::base::math::Floor(segment.virtual_address, kSize);
        const std::uint64_t kPageBase   = kPageNumber * kSize;
        const std::uint64_t kInPage     = segment.virtual_address - kPageBase;
        const std::uint64_t kSpan = cinux::base::math::Ceil(kInPage + segment.memory_bytes, kSize);
        for (std::uint64_t page = 0; page < kSpan; ++page) {
            const std::uint64_t kSpot        = kPageBase + (page * kSize);
            const std::uint64_t kInto        = kSpot - segment.virtual_address;
            const std::uint8_t* chunk_source = data + segment.file_offset + kInto;
            const std::uint64_t kRemaining =
                kInto < segment.file_bytes ? segment.file_bytes - kInto : 0;
            const std::size_t kChunkBytes =
                kRemaining > kSize ? kSize : static_cast<std::size_t>(kRemaining);
            map_segment_page(world.space, kSpot,
                             ChunkPlan{
                                 .source   = chunk_source,
                                 .bytes    = kChunkBytes,
                                 .writable = (segment.flags & kSegmentWrite) != 0,
                             });
        }
        mm::VmaRegion segment_range{};
        segment_range.start      = kPageBase;
        segment_range.end        = kPageBase + (kSpan * kSize);
        segment_range.kind       = mm::VmaKind::kFile;
        segment_range.readable   = (segment.flags & kSegmentRead) != 0;
        segment_range.writable   = (segment.flags & kSegmentWrite) != 0;
        segment_range.executable = (segment.flags & kSegmentExec) != 0;
        segment_range.backing    = world.file;
        segment_range.file_ops   = world.file_ops;
        segment_range.file_self  = world.file_self;
        if (!world.vmas.register_range(segment_range).ok()) {
            return false;
        }
    }
    entry = kImage.entry;
    return true;
}

}  // namespace cinux::loader
