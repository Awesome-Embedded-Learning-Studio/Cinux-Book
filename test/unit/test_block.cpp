#include <stdint.h>

#include <array>

#include "../framework/framework.hpp"
#include "cinux/memory.hpp"
#include "kernel/driver/block/block_device_concept.hpp"
#include "kernel/driver/block/block_queue.hpp"
#include "test/mock/mock_block_disk.hpp"
#include "test_assert.hpp"

namespace {

using cinux::test::kMockDiskBlocks;
using cinux::test::kMockDiskSector;
using cinux::test::MockDisk;

void fill_tagged(uint8_t* buffer, unsigned long bytes, unsigned tag) {
    for (unsigned long index = 0; index < bytes; ++index) {
        buffer[index] = static_cast<uint8_t>((static_cast<unsigned long>(tag) * 16) + index);
    }
}

}  // namespace

TEST("block: an unadopted port refuses every call politely") {
    const cinux::driver::BlockPort kPort;
    ASSERT_FALSE(kPort.read_blocks(cinux::driver::Lba{.value = 0},
                                   cinux::driver::BlockSpan{.value = 1}, nullptr));
    ASSERT_FALSE(kPort.flush());
    ASSERT_EQ(kPort.block_count(), 0U);
    ASSERT_EQ(kPort.block_size(), 0U);
}

TEST("block: an adopted port forwards to the seated disk") {
    MockDisk                 disk;
    cinux::driver::BlockPort port;
    port.adopt(disk);
    std::array<uint8_t, kMockDiskSector> pattern{};
    std::array<uint8_t, kMockDiskSector> readback{};
    fill_tagged(pattern.data(), pattern.size(), 0);
    ASSERT_TRUE(port.write_blocks(cinux::driver::Lba{.value = 2},
                                  cinux::driver::BlockSpan{.value = 1}, pattern.data()));
    ASSERT_TRUE(port.read_blocks(cinux::driver::Lba{.value = 2},
                                 cinux::driver::BlockSpan{.value = 1}, readback.data()));
    ASSERT_TRUE(cinux::base::EqualBytes(pattern.data(), readback.data(), pattern.size()));
    ASSERT_TRUE(port.flush());
    ASSERT_EQ(disk.flush_count(), 1U);
    ASSERT_EQ(port.block_count(), kMockDiskBlocks);
    ASSERT_EQ(port.block_size(), kMockDiskSector);
}

TEST("block: a lone read and write travel the queue") {
    MockDisk                             disk;
    cinux::driver::BlockQueue            queue(disk);
    std::array<uint8_t, kMockDiskSector> pattern{};
    std::array<uint8_t, kMockDiskSector> readback{};
    fill_tagged(pattern.data(), pattern.size(), 1);
    ASSERT_TRUE(queue.write(cinux::driver::Lba{.value = 9}, cinux::driver::BlockSpan{.value = 1},
                            pattern.data()));
    ASSERT_TRUE(queue.read(cinux::driver::Lba{.value = 9}, cinux::driver::BlockSpan{.value = 1},
                           readback.data()));
    ASSERT_TRUE(cinux::base::EqualBytes(pattern.data(), readback.data(), pattern.size()));
    ASSERT_TRUE(queue.flush());
    ASSERT_EQ(disk.flush_count(), 1U);
}

TEST("block: an out-of-range request carries the device refusal back") {
    MockDisk                             disk;
    cinux::driver::BlockQueue            queue(disk);
    std::array<uint8_t, kMockDiskSector> readback{};
    ASSERT_FALSE(queue.read(cinux::driver::Lba{.value = kMockDiskBlocks},
                            cinux::driver::BlockSpan{.value = 1}, readback.data()));
}

TEST("block: interleaved regions keep their bytes apart through the queue") {
    MockDisk                             disk;
    cinux::driver::BlockQueue            queue(disk);
    std::array<uint8_t, kMockDiskSector> first_pattern{};
    std::array<uint8_t, kMockDiskSector> second_pattern{};
    std::array<uint8_t, kMockDiskSector> readback{};
    fill_tagged(first_pattern.data(), first_pattern.size(), 2);
    fill_tagged(second_pattern.data(), second_pattern.size(), 3);
    ASSERT_TRUE(queue.write(cinux::driver::Lba{.value = 20}, cinux::driver::BlockSpan{.value = 1},
                            first_pattern.data()));
    ASSERT_TRUE(queue.flush());
    ASSERT_TRUE(queue.write(cinux::driver::Lba{.value = 21}, cinux::driver::BlockSpan{.value = 1},
                            second_pattern.data()));
    ASSERT_TRUE(queue.read(cinux::driver::Lba{.value = 20}, cinux::driver::BlockSpan{.value = 1},
                           readback.data()));
    ASSERT_TRUE(cinux::base::EqualBytes(first_pattern.data(), readback.data(), readback.size()));
    ASSERT_TRUE(queue.read(cinux::driver::Lba{.value = 21}, cinux::driver::BlockSpan{.value = 1},
                           readback.data()));
    ASSERT_TRUE(cinux::base::EqualBytes(second_pattern.data(), readback.data(), readback.size()));
    ASSERT_EQ(disk.flush_count(), 1U);
}

int main() {
    return cinux::test::RunAll();
}
