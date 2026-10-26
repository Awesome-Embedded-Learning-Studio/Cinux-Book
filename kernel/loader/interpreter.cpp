#include "cinux/own.hpp"
#include "cinux/result.hpp"
#include "kernel/fs/file_world.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/loader/elf_layout.hpp"
#include "kernel/loader/loader.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/layout.hpp"
#include "kernel/mm/vma.hpp"

namespace cinux::loader {

base::Result<EntryFacts> LoadInterpreter(const char* path, const mm::AddressSpace& space,
                                         mm::VmaStore& ranges) {
    if (path == nullptr) {
        return EntryFacts{};
    }
    const auto kView = fs::FileWorld::self().resolve(path);
    if (!kView.ok()) {
        return kView.error();
    }
    unsigned long              image_bytes = 0;
    base::Own<unsigned char[]> image;
    const auto                 kRead = fs::ReadFilePrefix(kView.value(), 65536, image, image_bytes);
    if (!kRead.ok()) {
        return kRead.error();
    }
    const auto kNode = kView.value().ops->lookup(kView.value().self, kView.value().rest);
    if (!kNode.ok()) {
        return kNode.error();
    }
    fs::FsStat probed{};
    if (kView.value().ops->stat == nullptr ||
        !kView.value().ops->stat(kView.value().self, kNode.value(), probed).ok()) {
        return base::KernelError::kIOError;
    }
    const LoadWorld kWorld{
        .space         = space,
        .vmas          = ranges,
        .file          = kNode.value(),
        .file_bytes    = probed.size,
        .file_ops      = kView.value().ops,
        .file_self     = kView.value().self,
        .file_lifetime = kView.value().lifetime,
        .role          = ImageRole::kInterpreter,
        .base          = mm::kUserInterpreterBase,
    };
    EntryFacts facts{};
    if (!LoadElfIntoSpace(image.get(), image_bytes, kWorld, facts)) {
        return base::KernelError::kInvalidArgument;
    }
    facts.interpreter_base = mm::kUserInterpreterBase;
    return facts;
}

}  // namespace cinux::loader
