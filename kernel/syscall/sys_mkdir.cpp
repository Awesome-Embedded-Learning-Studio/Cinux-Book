#include "kernel/syscall/sys_mkdir.hpp"

#include "cinux/result.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/fs_config.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleMkdir(unsigned long long path, [[maybe_unused]] unsigned long long second,
                      [[maybe_unused]] unsigned long long third) {
    char storage[fs::kFsPathMax] = {};
    if (const long long kBad = CopyUserPath(path, storage, fs::kFsPathMax); kBad < 0) {
        return kBad;
    }
    const base::Result<fs::FsView> kView = fs::FileWorld::self().resolve(storage);
    if (!kView.ok()) {
        return -ErrnoFrom(kView.error());
    }
    const base::Result<fs::FsNode> kMade =
        kView.value().ops->make_dir(kView.value().self, kView.value().rest);
    return kMade.ok() ? 0 : -ErrnoFrom(kMade.error());
}

}  // namespace cinux::syscall
