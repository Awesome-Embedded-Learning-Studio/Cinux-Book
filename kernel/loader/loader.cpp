#include "kernel/loader/loader.hpp"

#include <cstddef>
#include <cstdint>

#include "cinux/math.hpp"
#include "cinux/memory.hpp"
#include "cinux/ptr.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/arch/x86_64/page.hpp"
#include "kernel/arch/x86_64/page_entry.hpp"
#include "kernel/fs/open_file.hpp"
#include "kernel/loader/elf_layout.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vma.hpp"

namespace cinux::loader {

namespace {

using arch::page::kSize;

bool map_tail(const LoadWorld& world, const mm::VmaRegion& region, std::uint64_t file_end) {
    const auto kFrame = mm::Pmm::self().allocate_page();
    if (kFrame == mm::Pmm::PhysAddr{}) {
        return false;
    }
    auto* const kDestination = base::PtrAt<std::uint8_t>(mm::DirectMapVirt(kFrame.raw));
    base::SetBytes(kDestination, 0, kSize);
    const auto kBytes = file_end - region.start;
    const auto kRead =
        world.file_ops->read(world.file_self, world.file, region.file_offset, kDestination, kBytes);
    if (!kRead.ok() || kRead.value() != kBytes ||
        !world.space.map_user(region.start, kFrame.raw,
                              arch::page::UserLeafTerms(region.writable, region.executable))) {
        mm::Pmm::self().free_page(kFrame);
        return false;
    }
    return true;
}

bool map_segment(const LoadWorld& world, const ElfSegment& segment,
                 const base::Shared<fs::OpenFile>& backing) {
    if (segment.file_offset > world.file_bytes ||
        segment.file_bytes > world.file_bytes - segment.file_offset ||
        segment.file_bytes > segment.memory_bytes || world.base >= mm::kUserTop ||
        segment.virtual_address >= mm::kUserTop - world.base) {
        return false;
    }
    const std::uint64_t kSegmentVa = world.base + segment.virtual_address;
    if (segment.memory_bytes > mm::kUserTop - kSegmentVa ||
        segment.file_offset % kSize != kSegmentVa % kSize) {
        return false;
    }
    if (segment.memory_bytes == 0) {
        return true;
    }
    const auto kPageBase = base::math::AlignDown(kSegmentVa, kSize);
    const auto kFileEnd  = kSegmentVa + segment.file_bytes;
    const auto kWholeEnd =
        segment.file_bytes == 0 ? kPageBase : base::math::AlignDown(kFileEnd, kSize);
    const auto    kMemoryEnd = base::math::AlignUp(kSegmentVa + segment.memory_bytes, kSize);
    mm::VmaRegion region{};
    region.start         = kPageBase;
    region.end           = kWholeEnd;
    region.kind          = mm::VmaKind::kFile;
    region.readable      = (segment.flags & kSegmentRead) != 0;
    region.writable      = (segment.flags & kSegmentWrite) != 0;
    region.executable    = (segment.flags & kSegmentExec) != 0;
    region.backing       = world.file;
    region.file_ops      = world.file_ops;
    region.file_self     = world.file_self;
    region.file_lifetime = world.file_lifetime;
    region.open_file     = backing;
    region.file_offset   = segment.file_offset - (kSegmentVa - kPageBase);
    if (region.start < region.end && !world.vmas.register_range(region).ok()) {
        return false;
    }
    region.start = kWholeEnd;
    region.end   = kMemoryEnd;
    region.kind  = mm::VmaKind::kAnonymous;
    region.file_offset += kWholeEnd - kPageBase;
    if (region.start < region.end && !world.vmas.register_range(region).ok()) {
        return false;
    }
    return segment.file_bytes == 0 || kFileEnd == kWholeEnd ||
           (world.file_ops != nullptr && map_tail(world, region, kFileEnd));
}

}  // namespace

bool LoadElfIntoSpace(const std::uint8_t* data, std::size_t bytes, const LoadWorld& world,
                      EntryFacts& facts) {
    const base::Result<ElfImage> kParsed = ParseElf64(data, bytes, world.role);
    if (!kParsed.ok()) {
        return false;
    }
    const ElfImage             kImage = kParsed.value();
    base::Shared<fs::OpenFile> backing;
    if (world.file_ops != nullptr) {
        backing = base::Shared<fs::OpenFile>(
            new fs::OpenFile(world.file_ops, world.file_self, world.file));
        if (!backing.holds()) {
            return false;
        }
        world.file_ops->retain(world.file_self, world.file);
        backing->lifetime = world.file_lifetime;
    }
    for (std::uint16_t index = 0; index < kImage.segment_count; ++index) {
        ElfSegment segment{};
        if (!ReadSegment(kImage, index, segment)) {
            continue;
        }
        if (!map_segment(world, segment, backing)) {
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
