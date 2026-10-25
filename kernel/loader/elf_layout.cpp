#include "kernel/loader/elf_layout.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "cinux/byte_order.hpp"
#include "cinux/result.hpp"

namespace cinux::loader {

namespace {

constexpr std::size_t  kIdentBytes  = 16;
constexpr std::size_t  kEhdr64Bytes = 64;
constexpr std::size_t  kPhdr64Bytes = 56;
constexpr std::uint8_t kClass64     = 2;
constexpr std::uint8_t kDataLittle  = 1;

}  // namespace

base::Result<ElfImage> ParseElf64(const std::uint8_t* data, std::size_t bytes, ImageRole role) {
    if (data == nullptr || bytes < kEhdr64Bytes) {
        return base::KernelError::kInvalidArgument;
    }
    for (unsigned int index = 0; index < 4; ++index) {
        if (data[index] != kElfMagic[index]) {
            return base::KernelError::kInvalidArgument;
        }
    }
    if (data[4] != kClass64 || data[5] != kDataLittle) {
        return base::KernelError::kInvalidArgument;
    }
    const std::uint16_t kType    = base::ReadWord16(data, kIdentBytes);
    const std::uint16_t kMachine = base::ReadWord16(data, kIdentBytes + 2);
    const std::uint16_t kWanted  = role == ImageRole::kProgram ? kTypeExecutable : kTypeShared;
    if (kType != kWanted || kMachine != kMachineX8664) {
        return base::KernelError::kInvalidArgument;
    }
    const std::uint64_t kEntry      = base::ReadWord64(data, 24);
    const std::uint64_t kPhdrOffset = base::ReadWord64(data, 32);
    const std::uint16_t kPhdrSize   = base::ReadWord16(data, 54);
    const std::uint16_t kPhdrCount  = base::ReadWord16(data, 56);
    if (kPhdrSize < kPhdr64Bytes || kPhdrCount == 0) {
        return base::KernelError::kInvalidArgument;
    }
    const std::uint64_t kTableBytes = static_cast<std::uint64_t>(kPhdrSize) * kPhdrCount;
    if (kPhdrOffset > bytes || kTableBytes > bytes - kPhdrOffset) {
        return base::KernelError::kInvalidArgument;
    }
    std::uint64_t headers_address = 0;
    for (std::uint16_t index = 0; index < kPhdrCount; ++index) {
        const std::uint8_t* const kAt =
            data + (kPhdrOffset +
                    (static_cast<std::uint64_t>(index) * static_cast<std::uint64_t>(kPhdrSize)));
        if (base::ReadWord32(kAt, 0) != kProgramLoad) {
            continue;
        }
        const std::uint64_t kOffset = base::ReadWord64(kAt, 8);
        const std::uint64_t kFiles  = base::ReadWord64(kAt, 32);
        if (kOffset <= kPhdrOffset && kPhdrOffset < kOffset + kFiles) {
            headers_address = base::ReadWord64(kAt, 16) + (kPhdrOffset - kOffset);
            break;
        }
    }
    return ElfImage{
        .entry           = kEntry,
        .headers_address = headers_address,
        .segments        = data + kPhdrOffset,
        .segment_count   = kPhdrCount,
        .segment_size    = kPhdrSize,
    };
}

bool ReadSegment(const ElfImage& image, std::uint16_t index, ElfSegment& segment) {
    if (index >= image.segment_count) {
        return false;
    }
    const std::uint8_t* const kAt = image.segments + (static_cast<std::size_t>(index) *
                                                      static_cast<std::size_t>(image.segment_size));
    if (base::ReadWord32(kAt, 0) != kProgramLoad) {
        return false;
    }
    segment = ElfSegment{
        .virtual_address = base::ReadWord64(kAt, 16),
        .file_offset     = base::ReadWord64(kAt, 8),
        .file_bytes      = base::ReadWord64(kAt, 32),
        .memory_bytes    = base::ReadWord64(kAt, 40),
        .flags           = base::ReadWord32(kAt, 4),
    };
    return true;
}

bool ReadInterpreterPath(const std::uint8_t* data, std::size_t bytes, char* path,
                         std::size_t path_bytes) {
    const base::Result<ElfImage> kParsed = ParseElf64(data, bytes, ImageRole::kProgram);
    if (!kParsed.ok() || path == nullptr || path_bytes == 0) {
        return false;
    }
    const ElfImage kImage = kParsed.value();
    for (std::uint16_t index = 0; index < kImage.segment_count; ++index) {
        const std::uint8_t* const kAt =
            kImage.segments +
            (static_cast<std::size_t>(index) * static_cast<std::size_t>(kImage.segment_size));
        if (base::ReadWord32(kAt, 0) != kProgramInterp) {
            continue;
        }
        const std::uint64_t kOffset = base::ReadWord64(kAt, 8);
        const std::uint64_t kFiles  = base::ReadWord64(kAt, 32);
        if (kOffset > bytes || kFiles > bytes - kOffset) {
            return false;
        }
        const std::uint64_t kCopy = std::min(kFiles, static_cast<std::uint64_t>(path_bytes - 1));
        for (std::uint64_t done = 0; done < kCopy; ++done) {
            path[done] = static_cast<char>(data[kOffset + done]);
        }
        path[kCopy] = '\0';
        return true;
    }
    return false;
}

bool ReadGnuStackFlags(const std::uint8_t* data, std::size_t bytes, std::uint32_t& flags) {
    const base::Result<ElfImage> kParsed = ParseElf64(data, bytes, ImageRole::kProgram);
    if (!kParsed.ok()) {
        return false;
    }
    const ElfImage kImage = kParsed.value();
    for (std::uint16_t index = 0; index < kImage.segment_count; ++index) {
        const std::uint8_t* const kAt =
            kImage.segments +
            (static_cast<std::size_t>(index) * static_cast<std::size_t>(kImage.segment_size));
        if (base::ReadWord32(kAt, 0) != kProgramGnuStack) {
            continue;
        }
        flags = base::ReadWord32(kAt, 4);
        return true;
    }
    return false;
}

}  // namespace cinux::loader
