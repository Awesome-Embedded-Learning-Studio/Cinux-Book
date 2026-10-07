#include <string>
#include <string_view>

#include "../framework/framework.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "test_assert.hpp"

namespace {

struct FakeFs {
    void                                   retain([[maybe_unused]] cinux::fs::FsNode node) {}
    void                                   release([[maybe_unused]] cinux::fs::FsNode node) {}
    cinux::base::Result<cinux::fs::FsNode> lookup([[maybe_unused]] std::string_view path) {
        return cinux::fs::FsNode{
            .opaque = nullptr, .type = cinux::fs::InodeType::kFile, .ino = 7, .size = 0};
    }
    cinux::base::Result<cinux::fs::FsNode> make_file([[maybe_unused]] std::string_view path) {
        return cinux::fs::FsNode{
            .opaque = nullptr, .type = cinux::fs::InodeType::kFile, .ino = 7, .size = 0};
    }
    cinux::base::Result<cinux::fs::FsNode> make_dir([[maybe_unused]] std::string_view path) {
        return cinux::fs::FsNode{
            .opaque = nullptr, .type = cinux::fs::InodeType::kDirectory, .ino = 7, .size = 0};
    }
    cinux::base::Result<void>          unlink([[maybe_unused]] std::string_view path) { return {}; }
    cinux::base::Result<unsigned long> read([[maybe_unused]] cinux::fs::FsNode node,
                                            [[maybe_unused]] unsigned long     offset,
                                            [[maybe_unused]] void*             sink,
                                            [[maybe_unused]] unsigned long     count) {
        return 0UL;
    }
    cinux::base::Result<unsigned long> write([[maybe_unused]] cinux::fs::FsNode node,
                                             [[maybe_unused]] unsigned long     offset,
                                             [[maybe_unused]] const void*       source,
                                             [[maybe_unused]] unsigned long     count) {
        return 0UL;
    }
    cinux::base::Result<bool> read_dir([[maybe_unused]] cinux::fs::FsNode    node,
                                       [[maybe_unused]] unsigned long        index,
                                       [[maybe_unused]] cinux::fs::FsDirent& entry) {
        return false;
    }
};

}  // namespace

TEST("vfs: prefixes without a leading slash are refused") {
    cinux::fs::Vfs vfs;
    FakeFs         backend;
    const auto     kRelative = vfs.mount("tmp", backend);
    ASSERT_FALSE(kRelative.ok());
    ASSERT_EQ(kRelative.error(), cinux::base::KernelError::kInvalidArgument);
}

TEST("vfs: a trailing slash is refused unless the prefix is the root") {
    cinux::fs::Vfs vfs;
    FakeFs         backend;
    const auto     kTrailing = vfs.mount("/a/", backend);
    const auto     kRoot     = vfs.mount("/", backend);
    ASSERT_FALSE(kTrailing.ok());
    ASSERT_TRUE(kRoot.ok());
}

TEST("vfs: the same prefix mounts only once") {
    cinux::fs::Vfs vfs;
    FakeFs         backend;
    ASSERT_TRUE(vfs.mount("/", backend).ok());
    const auto kAgain = vfs.mount("/", backend);
    ASSERT_FALSE(kAgain.ok());
    ASSERT_EQ(kAgain.error(), cinux::base::KernelError::kAlreadyExists);
}

TEST("vfs: resolve strips the mount and one slash off the path") {
    cinux::fs::Vfs vfs;
    FakeFs         backend;
    ASSERT_TRUE(vfs.mount("/", backend).ok());

    const auto kRoot = vfs.resolve("/");
    const auto kLeaf = vfs.resolve("/a/b.txt");
    ASSERT_TRUE(kRoot.ok());
    ASSERT_EQ(kRoot.value().rest, std::string_view(""));
    ASSERT_EQ(kRoot.value().self, &backend);
    ASSERT_NE(kRoot.value().ops, nullptr);
    ASSERT_TRUE(kLeaf.ok());
    ASSERT_EQ(kLeaf.value().rest, std::string_view("a/b.txt"));
}

TEST("vfs: a trailing slash survives into the rest untouched") {
    cinux::fs::Vfs vfs;
    FakeFs         backend;
    ASSERT_TRUE(vfs.mount("/", backend).ok());

    const auto kSlash = vfs.resolve("/tmp/");
    ASSERT_TRUE(kSlash.ok());
    ASSERT_EQ(kSlash.value().rest, std::string_view("tmp/"));
}

TEST("vfs: the longest covering prefix wins") {
    cinux::fs::Vfs vfs;
    FakeFs         backend;
    ASSERT_TRUE(vfs.mount("/", backend).ok());
    ASSERT_TRUE(vfs.mount("/tmp", backend).ok());

    const auto kDeep  = vfs.resolve("/tmp/build/a.o");
    const auto kOther = vfs.resolve("/var/log.txt");
    const auto kExact = vfs.resolve("/tmp");
    ASSERT_TRUE(kDeep.ok());
    ASSERT_EQ(kDeep.value().rest, std::string_view("build/a.o"));
    ASSERT_TRUE(kOther.ok());
    ASSERT_EQ(kOther.value().rest, std::string_view("var/log.txt"));
    ASSERT_TRUE(kExact.ok());
    ASSERT_EQ(kExact.value().rest, std::string_view(""));
}

TEST("vfs: prefixes match only on component boundaries") {
    cinux::fs::Vfs vfs;
    FakeFs         backend;
    ASSERT_TRUE(vfs.mount("/tmp", backend).ok());

    const auto kNearMiss = vfs.resolve("/tmpx");
    const auto kMiddle   = vfs.resolve("/tmp/build");
    ASSERT_FALSE(kNearMiss.ok());
    ASSERT_EQ(kNearMiss.error(), cinux::base::KernelError::kNotFound);
    ASSERT_TRUE(kMiddle.ok());
    ASSERT_EQ(kMiddle.value().rest, std::string_view("build"));
}

TEST("vfs: non-absolute paths and empty tables refuse loudly") {
    const cinux::fs::Vfs kEmptyVfs;
    FakeFs               backend;
    const auto           kNothing = kEmptyVfs.resolve("/a");
    cinux::fs::Vfs       vfs;
    ASSERT_TRUE(vfs.mount("/", backend).ok());
    const auto kRelative = vfs.resolve("a/b");
    const auto kEmpty    = vfs.resolve("");
    ASSERT_FALSE(kNothing.ok());
    ASSERT_EQ(kNothing.error(), cinux::base::KernelError::kNotFound);
    ASSERT_FALSE(kRelative.ok());
    ASSERT_EQ(kRelative.error(), cinux::base::KernelError::kInvalidArgument);
    ASSERT_FALSE(kEmpty.ok());
}

TEST("vfs: the table runs out at kMountMax and says so") {
    cinux::fs::Vfs vfs;
    FakeFs         backend;
    for (unsigned long index = 0; index < cinux::fs::kMountMax; ++index) {
        const std::string kPrefix = "/m" + std::to_string(index);
        ASSERT_TRUE(vfs.mount(kPrefix, backend).ok());
    }
    const auto kOverflow = vfs.mount("/one-too-many", backend);
    ASSERT_FALSE(kOverflow.ok());
    ASSERT_EQ(kOverflow.error(), cinux::base::KernelError::kOutOfMemory);
}

int main() {
    return cinux::test::RunAll();
}
