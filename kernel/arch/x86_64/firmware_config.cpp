#include "kernel/arch/x86_64/firmware_config.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "cinux/byte_order.hpp"

namespace cinux::arch {

namespace {

constexpr std::uint16_t kDirectorySelector   = 0x19;
constexpr std::uint32_t kMaxDirectoryEntries = 1024;
struct DirectoryEntry {
    std::array<unsigned char, 4> size{};
    std::array<unsigned char, 2> selector{};
    std::array<unsigned char, 2> reserved{};
    std::array<char, 56>         name{};
};
static_assert(sizeof(DirectoryEntry) == 64);
static_assert(offsetof(DirectoryEntry, selector) == 4);
static_assert(offsetof(DirectoryEntry, name) == 8);
void read_bytes(FirmwareTransport transport, std::span<unsigned char> bytes) {
    for (auto& byte : bytes) {
        byte = transport.read(transport.context);
    }
}

}  // namespace

bool ReadFirmwareString(FirmwareTransport transport, std::string_view name,
                        std::span<char> output) {
    if (output.empty()) {
        return false;
    }
    output[0] = '\0';
    if (name.empty() || name.size() >= 56) {
        return false;
    }
    transport.select(transport.context, 0);
    std::array<unsigned char, 4> signature{};
    read_bytes(transport, signature);
    constexpr std::array<unsigned char, 4> kSignature{'Q', 'E', 'M', 'U'};
    if (signature != kSignature) {
        return false;
    }
    transport.select(transport.context, kDirectorySelector);
    std::array<unsigned char, 4> count_bytes{};
    read_bytes(transport, count_bytes);
    const auto kCount = base::ReadWord32Be(count_bytes.data(), 0);
    if (kCount > kMaxDirectoryEntries) {
        return false;
    }
    for (std::uint32_t index = 0; index < kCount; ++index) {
        DirectoryEntry entry{};
        read_bytes(transport, {reinterpret_cast<unsigned char*>(&entry), sizeof(entry)});
        const auto* const kEnd    = std::ranges::find(entry.name, '\0');
        const auto        kLength = static_cast<std::size_t>(kEnd - entry.name.begin());
        if (std::string_view{entry.name.data(), kLength} != name) {
            continue;
        }
        const auto kSize     = base::ReadWord32Be(entry.size.data(), 0);
        const auto kSelector = base::ReadWord16Be(entry.selector.data(), 0);
        if (kSize == 0 || kSize >= output.size() || kSelector < 0x20 || kSelector >= 0x4000) {
            return false;
        }
        transport.select(transport.context, kSelector);
        for (std::uint32_t offset = 0; offset < kSize; ++offset) {
            output[offset] = static_cast<char>(transport.read(transport.context));
            if (output[offset] == '\0' && offset + 1 != kSize) {
                output[0] = '\0';
                return false;
            }
        }
        output[kSize] = '\0';
        return true;
    }
    return false;
}

}  // namespace cinux::arch
