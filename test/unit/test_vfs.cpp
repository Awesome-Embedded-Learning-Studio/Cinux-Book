#include <string>
#include <string_view>

#include "../framework/framework.hpp"
#include "cinux/own.hpp"
#include "cinux/result.hpp"
#include "cinux/shared.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/mount_lease.hpp"
#include "kernel/fs/vfs.hpp"
#include "test_assert.hpp"

namespace {

struct FakeFs {
    cinux::base::KernelError lookup_error    = cinux::base::KernelError::kOk;
    cinux::base::KernelError stat_error      = cinux::base::KernelError::kOk;
    cinux::base::KernelError read_error      = cinux::base::KernelError::kOk;
    unsigned long            file_bytes      = 0;
    unsigned long            read_bytes      = 0;
    mutable unsigned long    requested_bytes = 0;
    void                     retain([[maybe_unused]] cinux::fs::FsNode node) {}
    void                     release([[maybe_unused]] cinux::fs::FsNode node) {}
    bool                     readable([[maybe_unused]] cinux::fs::FsNode node) { return true; }
    long long                ioctl([[maybe_unused]] cinux::fs::FsNode node, unsigned long long word,
                                   unsigned long long argument) {
        static_cast<void>(word);
        static_cast<void>(argument);
        return -25;
    }
    cinux::base::Result<cinux::fs::FsNode> adopt_open(cinux::fs::FsNode node,
                                                      std::string_view  path) {
        static_cast<void>(path);
        return node;
    }
    cinux::base::Result<cinux::fs::FsNode> lookup([[maybe_unused]] std::string_view path) const {
        if (lookup_error != cinux::base::KernelError::kOk) {
            return lookup_error;
        }
        return cinux::fs::FsNode{
            .opaque = nullptr,
            .type   = cinux::fs::InodeType::kFile,
            .ino    = 7,
            .size   = 0,
        };
    }
    cinux::base::Result<cinux::fs::FsNode> make_file([[maybe_unused]] std::string_view path) {
        return cinux::fs::FsNode{
            .opaque = nullptr,
            .type   = cinux::fs::InodeType::kFile,
            .ino    = 7,
            .size   = 0,
        };
    }
    cinux::base::Result<cinux::fs::FsNode> make_dir([[maybe_unused]] std::string_view path) {
        return cinux::fs::FsNode{
            .opaque = nullptr,
            .type   = cinux::fs::InodeType::kDirectory,
            .ino    = 7,
            .size   = 0,
        };
    }
    cinux::base::Result<void>          unlink([[maybe_unused]] std::string_view path) { return {}; }
    cinux::base::Result<unsigned long> read([[maybe_unused]] cinux::fs::FsNode node,
                                            [[maybe_unused]] unsigned long     offset,
                                            [[maybe_unused]] void*             sink,
                                            [[maybe_unused]] unsigned long     count) const {
        requested_bytes = count;
        if (read_error != cinux::base::KernelError::kOk) {
            return read_error;
        }
        for (unsigned long index = 0; index < read_bytes; ++index) {
            static_cast<unsigned char*>(sink)[index] = static_cast<unsigned char>(index);
        }
        return read_bytes;
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
    cinux::base::Result<void> stat([[maybe_unused]] cinux::fs::FsNode  node,
                                   [[maybe_unused]] cinux::fs::FsStat& probed) const {
        probed = cinux::fs::FsStat{};
        if (stat_error != cinux::base::KernelError::kOk) {
            return stat_error;
        }
        probed.size = file_bytes;
        return {};
    }
};

}  // namespace

TEST("vfs: whole-file reads preserve lookup, stat and read errors") {
    FakeFs                            backend;
    const cinux::fs::FsOps            kOps = cinux::fs::EraseFs<FakeFs>();
    const cinux::fs::FsView           kView{.ops = &kOps, .self = &backend, .rest = "missing"};
    cinux::base::Own<unsigned char[]> bytes;
    unsigned long                     count = 99;
    backend.lookup_error                    = cinux::base::KernelError::kNotFound;
    auto result                             = cinux::fs::ReadWholeFile(kView, bytes, count);
    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.error(), cinux::base::KernelError::kNotFound);
    backend.lookup_error = cinux::base::KernelError::kOk;
    backend.stat_error   = cinux::base::KernelError::kNotADirectory;
    result               = cinux::fs::ReadWholeFile(kView, bytes, count);
    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.error(), cinux::base::KernelError::kNotADirectory);
    backend.stat_error = cinux::base::KernelError::kOk;
    backend.read_error = cinux::base::KernelError::kIOError;
    result             = cinux::fs::ReadWholeFile(kView, bytes, count);
    ASSERT_FALSE(result.ok());
    ASSERT_EQ(result.error(), cinux::base::KernelError::kIOError);
    ASSERT_FALSE(bytes.holds());
    ASSERT_EQ(count, 99UL);
}

TEST("vfs: whole-file reads reject short images and publish complete buffers") {
    FakeFs                            backend;
    const cinux::fs::FsOps            kOps = cinux::fs::EraseFs<FakeFs>();
    const cinux::fs::FsView           kView{.ops = &kOps, .self = &backend, .rest = "program"};
    cinux::base::Own<unsigned char[]> bytes;
    unsigned long                     count = 99;
    backend.file_bytes                      = 32;
    backend.read_bytes                      = 16;
    const auto kResult                      = cinux::fs::ReadWholeFile(kView, bytes, count);
    ASSERT_FALSE(kResult.ok());
    ASSERT_EQ(kResult.error(), cinux::base::KernelError::kIOError);
    ASSERT_FALSE(bytes.holds());
    ASSERT_EQ(count, 99UL);
    backend.read_bytes = backend.file_bytes;
    ASSERT_TRUE(cinux::fs::ReadWholeFile(kView, bytes, count).ok());
    ASSERT_EQ(count, 32UL);
    ASSERT_EQ(bytes[31], 31U);
}

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

TEST("vfs: unmount takes one prefix back, and the seat reopens") {
    cinux::fs::Vfs vfs;
    FakeFs         backend;
    ASSERT_TRUE(vfs.mount("/", backend).ok());
    ASSERT_TRUE(vfs.mount("/tmp", backend).ok());
    ASSERT_TRUE(vfs.unmount("/tmp").ok());
    ASSERT_TRUE(vfs.mount("/tmp", backend).ok());
    const auto kAbsent = vfs.unmount("/nowhere");
    ASSERT_FALSE(kAbsent.ok());
    ASSERT_EQ(kAbsent.error(), cinux::base::KernelError::kNotFound);
    const auto kRelative = vfs.unmount("tmp");
    ASSERT_FALSE(kRelative.ok());
}

TEST("vfs: resolved views retain a detached backend until their last release") {
    cinux::fs::Vfs                             vfs;
    FakeFs                                     backend;
    bool                                       destroyed = false;
    cinux::base::Shared<cinux::fs::MountLease> lease(new cinux::fs::MountLease{});
    lease->backend = &destroyed;
    lease->destroy = [](void* marker) { *static_cast<bool*>(marker) = true; };
    ASSERT_TRUE(vfs.mount("/tmp", backend, "tmpfs", lease.get()).ok());
    {
        const auto kView = vfs.resolve("/tmp/file");
        ASSERT_TRUE(kView.ok());
        ASSERT_TRUE(vfs.unmount("/tmp").ok());
        lease.reset();
        ASSERT_FALSE(destroyed);
        ASSERT_TRUE(kView.value().self == &backend);
    }
    ASSERT_TRUE(destroyed);
}

int main() {
    return cinux::test::RunAll();
}

static void check_prefix_contents(const FakeFs&                            backend,
                                  const cinux::base::Own<unsigned char[]>& bytes,
                                  unsigned long                            count) {
    ASSERT_EQ(backend.requested_bytes, 64UL);
    ASSERT_EQ(count, 64UL);
    ASSERT_EQ(bytes[63], 63U);
}

TEST("vfs: prefix reads cap I/O and preserve the prior buffer on a short read") {
    FakeFs                  backend;
    const auto              kOps = cinux::fs::EraseFs<FakeFs>();
    const cinux::fs::FsView kView{.ops = &kOps, .self = &backend, .rest = "image"};
    backend.file_bytes = 1024UL * 1024;
    backend.read_bytes = 64;
    cinux::base::Own<unsigned char[]> bytes;
    unsigned long                     count = 0;
    ASSERT_TRUE(cinux::fs::ReadFilePrefix(kView, 64, bytes, count).ok());
    check_prefix_contents(backend, bytes, count);
    const auto* const kPrevious = bytes.get();
    backend.read_bytes          = 32;
    ASSERT_FALSE(cinux::fs::ReadFilePrefix(kView, 64, bytes, count).ok());
    ASSERT_EQ(bytes.get(), kPrevious);
    ASSERT_EQ(count, 64UL);
    backend.read_bytes = 0;
    ASSERT_TRUE(cinux::fs::ReadFilePrefix(kView, 0, bytes, count).ok());
    ASSERT_EQ(backend.requested_bytes, 0UL);
    ASSERT_EQ(count, 0UL);
}
