#include "kernel/loader/loader.hpp"

#include <cstddef>
#include <cstdint>

#include "cinux/assert.hpp"
#include "cinux/math.hpp"
#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
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

struct ChunkPlan {
    const std::uint8_t* source;
    std::size_t         bytes;
    bool                writable;
    bool                executable;
    unsigned long long  dest_offset;
};

void map_segment_page(const mm::AddressSpace& space, std::uint64_t spot, const ChunkPlan& chunk) {
    const mm::Pmm::PhysAddr kFrame = mm::Pmm::self().allocate_page();
    cinux::base::safety::Check(kFrame != mm::Pmm::PhysAddr{}, "loader out of frames");
    auto* const kDestination = cinux::base::PtrAt<std::uint8_t>(mm::DirectMapVirt(kFrame.raw));
    cinux::base::SetBytes(kDestination, 0, kSize);
    if (chunk.bytes > 0) {
        cinux::base::CopyBytes(kDestination + chunk.dest_offset, chunk.source, chunk.bytes);
    }
    const cinux::arch::page::Entry kBits =
        cinux::arch::page::UserLeafTerms(chunk.writable, chunk.executable);
    cinux::base::safety::Check(space.map_user(spot, kFrame.raw, kBits),
                               "loader map refused a page");
}

bool map_segment(const std::uint8_t* data, std::size_t bytes, const LoadWorld& world,
                 const ElfSegment& segment) {
    cinux::base::safety::Check(
        segment.file_offset <= bytes && segment.file_bytes <= bytes - segment.file_offset,
        "loader segment runs past the file");
    const std::uint64_t kSegmentVa  = world.base + segment.virtual_address;
    const std::uint64_t kPageNumber = cinux::base::math::Floor(kSegmentVa, kSize);
    const std::uint64_t kPageBase   = kPageNumber * kSize;
    const std::uint64_t kInPage     = kSegmentVa - kPageBase;
    const std::uint64_t kSpan    = cinux::base::math::Ceil(kInPage + segment.memory_bytes, kSize);
    const std::uint64_t kFileEnd = kSegmentVa + segment.file_bytes;
    for (std::uint64_t page = 0; page < kSpan; ++page) {
        const std::uint64_t kSpot        = kPageBase + (page * kSize);
        const std::uint64_t kPageEnd     = kSpot + kSize;
        const std::uint64_t kCopyFrom    = kSpot > kSegmentVa ? kSpot : kSegmentVa;
        const std::uint64_t kCopyUntil   = kPageEnd < kFileEnd ? kPageEnd : kFileEnd;
        const std::uint64_t kOverlap     = kCopyUntil > kCopyFrom ? kCopyUntil - kCopyFrom : 0;
        const std::uint8_t* chunk_source = data + segment.file_offset + (kCopyFrom - kSegmentVa);
        const auto          kChunkBytes  = static_cast<std::size_t>(kOverlap);
        map_segment_page(world.space, kSpot,
                         ChunkPlan{
                             .source      = chunk_source,
                             .bytes       = kChunkBytes,
                             .writable    = (segment.flags & kSegmentWrite) != 0,
                             .executable  = (segment.flags & kSegmentExec) != 0,
                             .dest_offset = kCopyFrom - kSpot,
                         });
    }
    mm::VmaRegion segment_range{};
    segment_range.start         = kPageBase;
    segment_range.end           = kPageBase + (kSpan * kSize);
    segment_range.kind          = mm::VmaKind::kFile;
    segment_range.readable      = (segment.flags & kSegmentRead) != 0;
    segment_range.writable      = (segment.flags & kSegmentWrite) != 0;
    segment_range.executable    = (segment.flags & kSegmentExec) != 0;
    segment_range.backing       = world.file;
    segment_range.file_ops      = world.file_ops;
    segment_range.file_self     = world.file_self;
    segment_range.file_lifetime = world.file_lifetime;
    return world.vmas.register_range(segment_range).ok();
}

}  // namespace

bool LoadElfIntoSpace(const std::uint8_t* data, std::size_t bytes, const LoadWorld& world,
                      EntryFacts& facts) {
    const base::Result<ElfImage> kParsed = ParseElf64(data, bytes, world.role);
    if (!kParsed.ok()) {
        return false;
    }
    const ElfImage kImage = kParsed.value();
    for (std::uint16_t index = 0; index < kImage.segment_count; ++index) {
        ElfSegment segment{};
        if (!ReadSegment(kImage, index, segment)) {
            continue;
        }
        if (!map_segment(data, bytes, world, segment)) {
            return false;
        }
    }
    facts.program_entry   = world.base + kImage.entry;
    facts.program_headers = kImage.headers_address == 0 ? 0 : world.base + kImage.headers_address;
    facts.header_entry_bytes = kImage.segment_size;
    facts.header_count       = kImage.segment_count;
    return true;
}

}  // namespace cinux::loader
