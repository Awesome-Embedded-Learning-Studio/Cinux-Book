#include <string_view>

#include "../framework/framework.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/devfs/devfs.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "test_assert.hpp"

namespace {

unsigned long g_source_calls = 0;

cinux::base::Result<unsigned long> echo_sink(void* /*context*/, void* sink, unsigned long count) {
    auto* const kOut = static_cast<unsigned char*>(sink);
    for (unsigned long index = 0; index < count; ++index) {
        kOut[index] = static_cast<unsigned char>(0xA5U ^ index);
    }
    return count;
}

cinux::base::Result<unsigned long> eat_source(void* /*context*/, const void* /*source*/,
                                              unsigned long count) {
    ++g_source_calls;
    return count;
}

const cinux::fs::DeviceOps kEchoOps{
    .read      = echo_sink,
    .write     = eat_source,
    .has_input = nullptr,
    .ioctl     = nullptr,
    .release   = nullptr,
    .context   = nullptr,
};

cinux::base::Result<unsigned long> stock_null_read(void* /*context*/, void* /*sink*/,
                                                   unsigned long /*count*/) {
    return 0;
}

cinux::base::Result<unsigned long> stock_null_write(void* /*context*/, const void* /*source*/,
                                                    unsigned long count) {
    return count;
}

cinux::base::Result<unsigned long> stock_zero_read(void* /*context*/, void* sink,
                                                   unsigned long count) {
    auto* const kOut = static_cast<unsigned char*>(sink);
    for (unsigned long index = 0; index < count; ++index) {
        kOut[index] = 0;
    }
    return count;
}

const cinux::fs::DeviceOps kStockNullOps{
    .read      = stock_null_read,
    .write     = stock_null_write,
    .has_input = nullptr,
    .ioctl     = nullptr,
    .release   = nullptr,
    .context   = nullptr,
};

const cinux::fs::DeviceOps kStockZeroOps{
    .read      = stock_zero_read,
    .write     = stock_null_write,
    .has_input = nullptr,
    .ioctl     = nullptr,
    .release   = nullptr,
    .context   = nullptr,
};

/// One devfs with the builtin pair seated, the shape every mount shows.
cinux::fs::DevFs& stocked(cinux::fs::DevFs& devfs) {
    const cinux::base::Result<void> kNull =
        devfs.register_device("null", cinux::fs::MakeDeviceId(1, 3), kStockNullOps);
    if (!kNull.ok()) {
        return devfs;
    }
    const cinux::base::Result<void> kZero =
        devfs.register_device("zero", cinux::fs::MakeDeviceId(1, 5), kStockZeroOps);
    if (!kZero.ok()) {
        return devfs;
    }
    return devfs;
}

}  // namespace

TEST("devfs: the root is inode one and a directory") {
    cinux::fs::DevFs                             stock;
    const cinux::base::Result<cinux::fs::FsNode> kRoot = stocked(stock).lookup("");
    ASSERT_TRUE(kRoot.ok());
    ASSERT_EQ(kRoot.value().type, cinux::fs::InodeType::kDirectory);
    ASSERT_EQ(kRoot.value().ino, 1UL);
}

TEST("devfs: enumeration opens with dot and dot-dot") {
    cinux::fs::DevFs                             stock;
    const cinux::base::Result<cinux::fs::FsNode> kRoot = stocked(stock).lookup("");

    cinux::fs::FsDirent entry;
    ASSERT_TRUE(stock.read_dir(kRoot.value(), 0, entry).value());
    ASSERT_STREQ(entry.name, ".");
    ASSERT_TRUE(stock.read_dir(kRoot.value(), 1, entry).value());
    ASSERT_STREQ(entry.name, "..");
}

TEST("devfs: the builtin pair enumerates in seating order") {
    cinux::fs::DevFs                             stock;
    const cinux::base::Result<cinux::fs::FsNode> kRoot = stocked(stock).lookup("");

    cinux::fs::FsDirent entry;
    ASSERT_TRUE(stock.read_dir(kRoot.value(), 2, entry).value());
    ASSERT_STREQ(entry.name, "null");
    ASSERT_EQ(entry.type, cinux::fs::InodeType::kCharDevice);
}

TEST("devfs: enumeration stops past the last device") {
    cinux::fs::DevFs                             stock;
    const cinux::base::Result<cinux::fs::FsNode> kRoot = stocked(stock).lookup("");

    cinux::fs::FsDirent entry;
    ASSERT_TRUE(stock.read_dir(kRoot.value(), 3, entry).value());
    ASSERT_STREQ(entry.name, "zero");
    ASSERT_FALSE(stock.read_dir(kRoot.value(), 4, entry).value());
}

TEST("devfs: null reads a clean zero") {
    cinux::fs::DevFs                             stock;
    const cinux::base::Result<cinux::fs::FsNode> kNull = stocked(stock).lookup("null");
    ASSERT_TRUE(kNull.ok());

    unsigned char                            buffer[4] = {1, 2, 3, 4};
    const cinux::base::Result<unsigned long> kPulled =
        stock.read(kNull.value(), 0, buffer, sizeof(buffer));
    ASSERT_TRUE(kPulled.ok());
    ASSERT_EQ(kPulled.value(), 0UL);
}

TEST("devfs: null swallows every write") {
    cinux::fs::DevFs                             stock;
    const cinux::base::Result<cinux::fs::FsNode> kNull      = stocked(stock).lookup("null");
    const unsigned char                          kBuffer[4] = {1, 2, 3, 4};
    const cinux::base::Result<unsigned long>     kPushed =
        stock.write(kNull.value(), 0, kBuffer, sizeof(kBuffer));
    ASSERT_TRUE(kPushed.ok());
    ASSERT_EQ(kPushed.value(), 4UL);
}

TEST("devfs: zero reads zero bytes") {
    cinux::fs::DevFs                             stock;
    const cinux::base::Result<cinux::fs::FsNode> kZero = stocked(stock).lookup("zero");
    ASSERT_TRUE(kZero.ok());

    unsigned char buffer[8] = {};
    for (unsigned char& byte : buffer) {
        byte = 0xFF;
    }
    const cinux::base::Result<unsigned long> kPulled =
        stock.read(kZero.value(), 0, buffer, sizeof(buffer));
    ASSERT_TRUE(kPulled.ok());
    ASSERT_EQ(kPulled.value(), 8UL);
    for (const unsigned char kByte : buffer) {
        ASSERT_EQ(static_cast<unsigned>(kByte), 0U);
    }
}

TEST("devfs: stat answers the device mode and identity") {
    cinux::fs::DevFs                             stock;
    const cinux::base::Result<cinux::fs::FsNode> kNull = stocked(stock).lookup("null");
    ASSERT_TRUE(kNull.ok());
    cinux::fs::FsStat probed{};
    ASSERT_TRUE(stock.stat(kNull.value(), probed).ok());
    ASSERT_EQ(probed.mode, cinux::fs::kFsCharDevMode);
    ASSERT_EQ(probed.rdev, cinux::fs::MakeDeviceId(1, 3));
}

TEST("devfs: stat answers the directory mode for the root") {
    cinux::fs::DevFs                             stock;
    const cinux::base::Result<cinux::fs::FsNode> kRoot = stocked(stock).lookup("");
    cinux::fs::FsStat                            probed{};
    ASSERT_TRUE(stock.stat(kRoot.value(), probed).ok());
    ASSERT_EQ(probed.mode, cinux::fs::kFsDirMode);
    ASSERT_EQ(probed.rdev, 0UL);
}

TEST("devfs: a registered device sees reads") {
    cinux::fs::DevFs stock;
    stocked(stock);
    ASSERT_TRUE(stock.register_device("echo", cinux::fs::MakeDeviceId(9, 9), kEchoOps).ok());

    const cinux::base::Result<cinux::fs::FsNode> kEcho = stock.lookup("echo");
    ASSERT_TRUE(kEcho.ok());
    ASSERT_EQ(kEcho.value().ino, 4UL);

    unsigned char                            buffer[3] = {};
    const cinux::base::Result<unsigned long> kPulled =
        stock.read(kEcho.value(), 0, buffer, sizeof(buffer));
    ASSERT_TRUE(kPulled.ok());
    ASSERT_EQ(kPulled.value(), 3UL);
    ASSERT_EQ(static_cast<unsigned>(buffer[0]), 0xA5U);
}

TEST("devfs: a registered device sees writes") {
    cinux::fs::DevFs stock;
    ASSERT_TRUE(stock.register_device("echo", cinux::fs::MakeDeviceId(9, 9), kEchoOps).ok());
    const cinux::base::Result<cinux::fs::FsNode> kEcho      = stock.lookup("echo");
    const unsigned char                          kBuffer[3] = {};

    const unsigned long                      kBefore = g_source_calls;
    const cinux::base::Result<unsigned long> kPushed =
        stock.write(kEcho.value(), 0, kBuffer, sizeof(kBuffer));
    ASSERT_TRUE(kPushed.ok());
    ASSERT_EQ(kPushed.value(), 3UL);
    ASSERT_EQ(g_source_calls, kBefore + 1);
}

TEST("devfs: registration refuses repeats and bad shapes") {
    cinux::fs::DevFs stock;
    ASSERT_TRUE(stock.register_device("echo", 0, kEchoOps).ok());
    ASSERT_EQ(stock.register_device("echo", 0, kEchoOps).error(),
              cinux::base::KernelError::kAlreadyExists);
    ASSERT_EQ(stock.register_device("bad/name", 0, kEchoOps).error(),
              cinux::base::KernelError::kInvalidArgument);
}

TEST("devfs: the flat namespace refuses walking and creating") {
    cinux::fs::DevFs stock;
    ASSERT_EQ(stocked(stock).lookup("null/extra").error(), cinux::base::KernelError::kNotFound);
    ASSERT_EQ(stock.lookup("missing").error(), cinux::base::KernelError::kNotFound);
    ASSERT_EQ(stock.make_file("x").error(), cinux::base::KernelError::kNotSupported);
    ASSERT_EQ(stock.unlink("null").error(), cinux::base::KernelError::kNotSupported);
}

TEST("devfs: nodes refuse their wrong operations") {
    cinux::fs::DevFs                             stock;
    const cinux::base::Result<cinux::fs::FsNode> kRoot = stocked(stock).lookup("");
    ASSERT_EQ(stock.read(kRoot.value(), 0, nullptr, 0).error(),
              cinux::base::KernelError::kIsADirectory);

    const cinux::base::Result<cinux::fs::FsNode> kNull = stock.lookup("null");
    cinux::fs::FsDirent                          entry;
    ASSERT_EQ(stock.read_dir(kNull.value(), 0, entry).error(),
              cinux::base::KernelError::kNotADirectory);
}

TEST("devfs: a mount hands the prefix to the table") {
    cinux::fs::DevFs stock;
    cinux::fs::Vfs   table;
    ASSERT_TRUE(table.mount("/dev", stocked(stock)).ok());

    const cinux::base::Result<cinux::fs::FsView> kDevice = table.resolve("/dev/null");
    ASSERT_TRUE(kDevice.ok());
    ASSERT_EQ(kDevice.value().rest, std::string_view("null"));
    const cinux::base::Result<cinux::fs::FsNode> kNode =
        kDevice.value().ops->lookup(kDevice.value().self, kDevice.value().rest);
    ASSERT_TRUE(kNode.ok());
    ASSERT_EQ(kNode.value().type, cinux::fs::InodeType::kCharDevice);
}

TEST("devfs: the mount root resolves with empty rest") {
    cinux::fs::DevFs stock;
    cinux::fs::Vfs   table;
    ASSERT_TRUE(table.mount("/dev", stocked(stock)).ok());

    const cinux::base::Result<cinux::fs::FsView> kRoot = table.resolve("/dev");
    ASSERT_TRUE(kRoot.ok());
    ASSERT_TRUE(kRoot.value().rest.empty());
    ASSERT_EQ(table.resolve("/nobody").error(), cinux::base::KernelError::kNotFound);
}

int main() {
    return cinux::test::RunAll();
}
