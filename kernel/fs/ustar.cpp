/**
 * @file    ustar.cpp
 * @brief   The ustar walk: block by block, octal by octal.
 *
 * Format facts live here as named constants; nothing outside this file
 * needs them, because callers receive parsed entries, not offsets.
 *
 * @author  Charliechen114514
 * @date    2026-10-07
 * @version 0.1
 * @since   0.1.0
 * @ingroup kernel_fs
 * @copyright Copyright (c) 2026
 */

#include "kernel/fs/ustar.hpp"

#include <string_view>

#include "cinux/math.hpp"
#include "cinux/memory.hpp"
#include "cinux/parse.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/vfs.hpp"

namespace cinux::fs {

namespace {

constexpr unsigned long kBlock       = 512;
constexpr unsigned long kNameOffset  = 0;
constexpr unsigned long kNameField   = 100;
constexpr unsigned long kSizeOffset  = 124;
constexpr unsigned long kSizeField   = 12;
constexpr unsigned long kTypeOffset  = 156;
constexpr unsigned long kMagicOffset = 257;
constexpr unsigned long kMagicSize   = 5;

base::Result<unsigned long> name_length(const unsigned char* header) {
    for (unsigned long index = 0; index < kNameField; ++index) {
        if (header[kNameOffset + index] == 0) {
            return index;
        }
    }
    return base::KernelError::kInvalidArgument;
}

base::Result<InodeType> entry_type(unsigned char flag) {
    if (flag == '0' || flag == 0) {
        return InodeType::kFile;
    }
    if (flag == '5') {
        return InodeType::kDirectory;
    }
    return base::KernelError::kInvalidArgument;
}

}  // namespace

UstarReader::UstarReader(const unsigned char* base, unsigned long limit)
    : base_(base), limit_(limit) {}

base::Result<bool> UstarReader::next(UstarEntry& entry) {
    if (done_) {
        return false;
    }
    if (cursor_ + kBlock > limit_) {
        return base::KernelError::kInvalidArgument;
    }

    const unsigned char* const kHeader = base_ + cursor_;
    if (base::BytesAre(kHeader, 0, kBlock)) {
        done_ = true;
        return false;
    }
    for (unsigned long index = 0; index < kMagicSize; ++index) {
        if (kHeader[kMagicOffset + index] != static_cast<unsigned char>("ustar"[index])) {
            return base::KernelError::kInvalidArgument;
        }
    }

    const base::Result<unsigned long> kNameBytes = name_length(kHeader);
    if (!kNameBytes.ok()) {
        return kNameBytes.error();
    }
    const base::Result<unsigned long> kParsedSize = base::ParseOctal(
        std::string_view(reinterpret_cast<const char*>(kHeader + kSizeOffset), kSizeField));
    if (!kParsedSize.ok()) {
        return kParsedSize.error();
    }
    const base::Result<InodeType> kParsedType = entry_type(kHeader[kTypeOffset]);
    if (!kParsedType.ok()) {
        return kParsedType.error();
    }

    const unsigned long kSize     = kParsedSize.value();
    const unsigned long kDataHead = cursor_ + kBlock;
    const unsigned long kPadded   = base::math::Ceil(kSize, kBlock) * kBlock;
    if (kDataHead + kPadded > limit_) {
        return base::KernelError::kInvalidArgument;
    }

    entry.name =
        std::string_view(reinterpret_cast<const char*>(kHeader + kNameOffset), kNameBytes.value());
    entry.size = kSize;
    entry.type = kParsedType.value();
    entry.data = std::string_view(reinterpret_cast<const char*>(base_ + kDataHead), kSize);
    cursor_    = kDataHead + kPadded;
    return true;
}

}  // namespace cinux::fs
