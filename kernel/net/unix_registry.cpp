/**
 * @file kernel/net/unix_registry.cpp
 * @brief UnixRegistry implementation (in-memory AF_UNIX path -> listener map)
 *
 * Split from unix_socket.cpp to keep that file under the 500-line limit.
 * The registry is an independent class (fixed table + Spinlock); its methods
 * are pure registry logic with no socket-internal state, so the split is clean.
 *
 * Namespace: cinux::net
 */

#include "kernel/net/unix_socket.hpp"

namespace cinux::net {

UnixRegistry& UnixRegistry::instance() {
    static UnixRegistry reg;
    return reg;
}

int UnixRegistry::find_locked(const char* path) const {
    if (path == nullptr) {
        return -1;
    }
    for (uint32_t i = 0; i < kUnixRegistryMax; ++i) {
        if (!entries_[i].used) {
            continue;
        }
        const char* a = entries_[i].path;
        uint32_t    j = 0;
        while (a[j] != '\0' && path[j] != '\0') {
            if (a[j] != path[j]) {
                break;
            }
            ++j;
        }
        if (a[j] == '\0' && path[j] == '\0') {
            return static_cast<int>(i);
        }
    }
    return -1;
}

cinux::lib::ErrorOr<void> UnixRegistry::register_listener(const char* path, UnixSocket* sock) {
    if (path == nullptr || path[0] == '\0' || sock == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    auto g = lock_.guard();
    if (find_locked(path) >= 0) {
        return cinux::lib::Error::AlreadyExists;
    }
    for (uint32_t i = 0; i < kUnixRegistryMax; ++i) {
        if (!entries_[i].used) {
            uint32_t j = 0;
            while (j + 1 < kUnixPathMax && path[j] != '\0') {
                entries_[i].path[j] = path[j];
                ++j;
            }
            entries_[i].path[j] = '\0';
            entries_[i].used    = true;
            entries_[i].sock    = sock;
            return {};
        }
    }
    return cinux::lib::Error::OutOfMemory;  // table full
}

cinux::lib::ErrorOr<UnixSocket*> UnixRegistry::lookup(const char* path) {
    if (path == nullptr) {
        return cinux::lib::Error::InvalidArgument;
    }
    auto g = lock_.guard();
    int  i = find_locked(path);
    if (i < 0) {
        return cinux::lib::Error::NotFound;
    }
    return entries_[i].sock;
}

void UnixRegistry::unregister(const char* path) {
    auto g = lock_.guard();
    int  i = find_locked(path);
    if (i < 0) {
        return;
    }
    entries_[i].used    = false;
    entries_[i].path[0] = '\0';
    entries_[i].sock    = nullptr;
}

}  // namespace cinux::net
