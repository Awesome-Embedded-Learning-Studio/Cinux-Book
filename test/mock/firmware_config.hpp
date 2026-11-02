#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

#include "kernel/arch/x86_64/firmware_config.hpp"

namespace cinux::test {

class MockFirmwareConfig {
public:
    MockFirmwareConfig();
    MockFirmwareConfig(const MockFirmwareConfig&)                = delete;
    MockFirmwareConfig&     operator=(const MockFirmwareConfig&) = delete;
    arch::FirmwareTransport transport();

    void          add_file(std::string_view name, std::string_view bytes);
    void          set_item(std::uint16_t selector, std::vector<unsigned char> bytes);
    void          select(std::uint16_t selector);
    unsigned char read();

private:
    std::map<std::uint16_t, std::vector<unsigned char>> items_;
    std::uint16_t                                       selected_ = 0;
    std::size_t                                         offset_   = 0;
    std::uint32_t                                       count_    = 0;
};

}  // namespace cinux::test
