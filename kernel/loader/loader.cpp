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
using arch::page::kUser;
using arch::page::kWritable;

struct ChunkPlan {
    const std::uint8_t* source;
    std::size_t         bytes;
    bool                writable;
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
        const std::uint64_t kFileEnd = segment.virtual_address + segment.file_bytes;
        for (std::uint64_t page = 0; page < kSpan; ++page) {
            const std::uint64_t kSpot    = kPageBase + (page * kSize);
            const std::uint64_t kPageEnd = kSpot + kSize;
            const std::uint64_t kCopyFrom =
                kSpot > segment.virtual_address ? kSpot : segment.virtual_address;
            const std::uint64_t kCopyUntil = kPageEnd < kFileEnd ? kPageEnd : kFileEnd;
            const std::uint64_t kOverlap   = kCopyUntil > kCopyFrom ? kCopyUntil - kCopyFrom : 0;
            const std::uint8_t* chunk_source =
                data + segment.file_offset + (kCopyFrom - segment.virtual_address);
            const auto kChunkBytes = static_cast<std::size_t>(kOverlap);
            map_segment_page(world.space, kSpot,
                             ChunkPlan{
                                 .source      = chunk_source,
                                 .bytes       = kChunkBytes,
                                 .writable    = (segment.flags & kSegmentWrite) != 0,
                                 .dest_offset = kCopyFrom - kSpot,
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

unsigned long WriteEntryPicture(unsigned char* stack_page, unsigned long entry_rsp,
                                const char* const* argv, const char* const* envp) {
    const unsigned long kPageBase  = entry_rsp - (entry_rsp % kSize);
    unsigned long       strings_at = 0;
    unsigned long       words_at   = entry_rsp - kPageBase;
    const unsigned long kNullWord  = 0;
    const auto          kRoomLeft  = [&] { return words_at + 16 < kSize - strings_at; };
    const auto          kPutNull   = [&] {
        cinux::base::CopyBytes(stack_page + words_at, &kNullWord, sizeof(kNullWord));
        words_at += 8;
    };
    const auto kPutString = [&](const char* text) -> bool {
        if (text == nullptr) {
            return false;
        }
        unsigned long length = 0;
        while (text[length] != '\0') {
            ++length;
        }
        if (!kRoomLeft()) {
            return false;
        }
        cinux::base::CopyBytes(stack_page + strings_at, text, length + 1);
        const unsigned long kLanded = kPageBase + strings_at;
        strings_at += length + 1;
        cinux::base::CopyBytes(stack_page + words_at, &kLanded, sizeof(kLanded));
        words_at += 8;
        return true;
    };
    unsigned long argc = 0;
    while (argv[argc] != nullptr) {
        ++argc;
    }
    unsigned long envc = 0;
    if (envp != nullptr) {
        while (envp[envc] != nullptr) {
            ++envc;
        }
    }
    cinux::base::CopyBytes(stack_page + words_at, &argc, sizeof(argc));
    words_at += 8;
    for (unsigned long index = 0; index < argc; ++index) {
        if (!kPutString(argv[index])) {
            return 0;
        }
    }
    kPutNull();
    for (unsigned long index = 0; index < envc; ++index) {
        if (!kPutString(envp[index])) {
            return 0;
        }
    }
    kPutNull();
    kPutNull();
    kPutNull();
    return entry_rsp;
}

}  // namespace cinux::loader
