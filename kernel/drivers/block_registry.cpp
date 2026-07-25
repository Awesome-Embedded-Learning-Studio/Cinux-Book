/**
 * @file kernel/drivers/block_registry.cpp
 * @brief BlockRegistry -- name -> IBlockDevice table for sys_mount (F6-M1 B1b)
 */

#include "block_registry.hpp"

#include "kernel/lib/string.hpp"  // strcmp
#include "kernel/proc/sync.hpp"   // Spinlock

namespace cinux::drivers {

namespace {

struct Entry {
    char          name[BlockRegistry::NAME_MAX];
    IBlockDevice* dev;
};

Entry                entries_[BlockRegistry::MAX_DEVICES]{};
uint32_t             count_{0};
cinux::proc::Spinlock lock_;

/// Copy @p name into @p dst (NUL-terminated, truncated at cap-1).
void copy_name(char* dst, const char* name, uint32_t cap) {
    uint32_t i = 0;
    while (i + 1 < cap && name[i] != '\0') {
        dst[i] = name[i];
        ++i;
    }
    dst[i] = '\0';
}

}  // namespace

bool BlockRegistry::register_device(const char* name, IBlockDevice* dev) {
    if (name == nullptr || dev == nullptr) {
        return false;
    }
    auto g = lock_.guard();
    if (count_ >= MAX_DEVICES) {
        return false;
    }
    for (uint32_t i = 0; i < count_; ++i) {
        if (strcmp(entries_[i].name, name) == 0) {
            return false;  // duplicate name -- a bug in boot wiring
        }
    }
    copy_name(entries_[count_].name, name, NAME_MAX);
    entries_[count_].dev = dev;
    ++count_;
    return true;
}

IBlockDevice* BlockRegistry::lookup(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    auto g = lock_.guard();
    for (uint32_t i = 0; i < count_; ++i) {
        if (strcmp(entries_[i].name, name) == 0) {
            return entries_[i].dev;
        }
    }
    return nullptr;
}

uint32_t BlockRegistry::count() {
    auto g = lock_.guard();
    return count_;
}

const char* BlockRegistry::name_at(uint32_t i) {
    auto g = lock_.guard();
    return (i < count_) ? entries_[i].name : nullptr;
}

IBlockDevice* BlockRegistry::device_at(uint32_t i) {
    auto g = lock_.guard();
    return (i < count_) ? entries_[i].dev : nullptr;
}

}  // namespace cinux::drivers
