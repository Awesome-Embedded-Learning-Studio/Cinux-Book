#include <array>
#include <cstdint>

#include "cinux/byte_order.hpp"
#include "framework.hpp"
#include "kernel/driver/acpi/madt.hpp"
#include "kernel/driver/acpi/madt_config.hpp"
#include "test_assert.hpp"

namespace {

using cinux::driver::acpi::ParseMadt;
using cinux::driver::acpi::kMaxProcessorApics;

class MadtImage {
public:
    MadtImage() {
        bytes_[0] = 'A';
        bytes_[1] = 'P';
        bytes_[2] = 'I';
        bytes_[3] = 'C';
        cinux::base::WriteWord32(bytes_.data(), 36, 0xFEE00000);
        cinux::base::WriteWord32(bytes_.data(), 40, 1);
        finish();
    }

    void processor(uint8_t apic, uint32_t flags) {
        auto* entry = append(0, 8);
        entry[3]    = apic;
        cinux::base::WriteWord32(entry, 4, flags);
    }

    void io_apic(uint32_t address, uint32_t gsi_base) {
        auto* entry = append(1, 12);
        cinux::base::WriteWord32(entry, 4, address);
        cinux::base::WriteWord32(entry, 8, gsi_base);
    }

    void override_irq(uint8_t bus, uint8_t source, uint32_t gsi) {
        auto* entry = append(2, 10);
        entry[2]    = bus;
        entry[3]    = source;
        cinux::base::WriteWord32(entry, 4, gsi);
        cinux::base::WriteWord16(entry, 8, 15);
    }

    unsigned char* append(uint8_t type, uint8_t length) {
        auto* entry = bytes_.data() + length_;
        entry[0]    = type;
        entry[1]    = length;
        length_ += length;
        finish();
        return entry;
    }

    void               finish() { cinux::base::WriteWord32(bytes_.data(), 4, length_); }
    [[nodiscard]] auto parse() const { return ParseMadt(bytes_.data(), length_); }
    auto&              bytes() { return bytes_; }

private:
    std::array<unsigned char, 1024> bytes_{};
    uint32_t                        length_ = 44;
};

TEST("madt: absent short oversized and wrong signature images are refused") {
    ASSERT_EQ(ParseMadt(nullptr, 0).processor_count, 0U);
    MadtImage image;
    image.processor(1, 1);
    ASSERT_EQ(ParseMadt(image.bytes().data(), 43).local_apic_address, 0ULL);
    cinux::base::WriteWord32(image.bytes().data(), 4, 1025);
    ASSERT_EQ(ParseMadt(image.bytes().data(), 1024).local_apic_address, 0ULL);
    image.finish();
    image.bytes()[0] = 'X';
    ASSERT_EQ(image.parse().local_apic_address, 0ULL);
}

TEST("madt: enabled and online capable processors survive unknown unaligned entries") {
    MadtImage image;
    image.append(127, 3);
    image.processor(2, 1);
    image.processor(3, 2);
    image.processor(4, 0);
    const auto kTopology = image.parse();
    ASSERT_EQ(kTopology.local_apic_address, 0xFEE00000ULL);
    ASSERT_TRUE(kTopology.pcat_compatible);
    ASSERT_EQ(kTopology.processor_count, 2U);
    ASSERT_EQ(kTopology.processor_apic_ids[0], 2U);
    ASSERT_EQ(kTopology.processor_apic_ids[1], 3U);
}

TEST("madt: first IOAPIC and ISA overrides preserve GSI bases and flags") {
    MadtImage image;
    image.io_apic(0xFEC00000, 24);
    image.io_apic(0xFEC01000, 48);
    image.override_irq(0, 0, 2);
    image.override_irq(1, 1, 30);
    image.override_irq(0, 16, 31);
    const auto kTopology = image.parse();
    ASSERT_EQ(kTopology.io_apic_address, 0xFEC00000U);
    ASSERT_EQ(kTopology.io_apic_gsi_base, 24U);
    ASSERT_EQ(kTopology.override_count, 1U);
    ASSERT_EQ(kTopology.overrides[0].source, 0U);
    ASSERT_EQ(kTopology.overrides[0].gsi, 2U);
    ASSERT_EQ(kTopology.overrides[0].flags, 15U);
}

TEST("madt: known truncated entries and incomplete headers refuse the image") {
    for (const uint8_t kType : {0, 1, 2}) {
        MadtImage image;
        image.append(kType, 2);
        ASSERT_EQ(image.parse().local_apic_address, 0ULL);
    }
    for (const uint8_t kLength : {0, 1, 255}) {
        MadtImage image;
        auto*     entry = image.append(127, 2);
        entry[1]        = kLength;
        ASSERT_EQ(image.parse().local_apic_address, 0ULL);
    }
    MadtImage image;
    image.append(127, 3);
    cinux::base::WriteWord32(image.bytes().data(), 4, 48);
    ASSERT_EQ(ParseMadt(image.bytes().data(), 48).local_apic_address, 0ULL);
}

TEST("madt: fixed capacity never overwrites the retained topology") {
    MadtImage image;
    for (unsigned int index = 0; index < kMaxProcessorApics + 3; ++index) {
        image.processor(static_cast<uint8_t>(index), 1);
    }
    for (unsigned int index = 0; index < cinux::driver::acpi::kMaxIrqOverrides + 3; ++index) {
        image.override_irq(0, 0, index);
    }
    const auto kTopology = image.parse();
    ASSERT_EQ(kTopology.processor_count, kMaxProcessorApics);
    ASSERT_EQ(kTopology.override_count, cinux::driver::acpi::kMaxIrqOverrides);
    ASSERT_EQ(kTopology.processor_apic_ids[kMaxProcessorApics - 1], kMaxProcessorApics - 1);
}

}

int main() {
    return cinux::test::RunAll();
}
