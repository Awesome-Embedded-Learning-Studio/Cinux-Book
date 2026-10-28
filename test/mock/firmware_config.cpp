#include "test/mock/firmware_config.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "cinux/byte_order.hpp"
#include "kernel/arch/x86_64/firmware_config.hpp"

namespace cinux::test {

MockFirmwareConfig::MockFirmwareConfig() {
    items_[0] = {'Q', 'E', 'M', 'U'};
    items_[0x19].resize(4);
}

arch::FirmwareTransport MockFirmwareConfig::transport() {
    return {
        .context = this,
        .select =
            [](void* context, std::uint16_t selector) {
                static_cast<MockFirmwareConfig*>(context)->select(selector);
            },
        .read = [](void* context) { return static_cast<MockFirmwareConfig*>(context)->read(); },
    };
}

void MockFirmwareConfig::add_file(std::string_view name, std::string_view bytes) {
    const auto kSelector = static_cast<std::uint16_t>(0x20 + count_);
    auto&      directory = items_[0x19];
    const auto kOffset   = directory.size();
    directory.resize(kOffset + 64);
    base::WriteWord32Be(directory.data(), 0, ++count_);
    base::WriteWord32Be(directory.data() + kOffset, 0, static_cast<std::uint32_t>(bytes.size()));
    base::WriteWord16Be(directory.data() + kOffset, 4, kSelector);
    std::ranges::copy(name.substr(0, 55), directory.begin() + static_cast<long>(kOffset) + 8);
    items_[kSelector] = std::vector<unsigned char>(bytes.begin(), bytes.end());
}

void MockFirmwareConfig::set_item(std::uint16_t selector, std::vector<unsigned char> bytes) {
    items_[selector] = std::move(bytes);
}

void MockFirmwareConfig::select(std::uint16_t selector) {
    selected_ = selector;
    offset_   = 0;
}

unsigned char MockFirmwareConfig::read() {
    const auto kItem = items_.find(selected_);
    if (kItem == items_.end() || offset_ >= kItem->second.size()) {
        return 0;
    }
    return kItem->second[offset_++];
}

}  // namespace cinux::test
