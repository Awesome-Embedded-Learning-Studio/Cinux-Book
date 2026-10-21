#include "kernel/fs/pipe.hpp"

#include <atomic>

#include "cinux/result.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::fs {

namespace {

/// One boot-wide counter: every pipe born gets the next number.
std::atomic<unsigned long> g_pipe_identities{0};

/// The stat a pipe answers: a fifo, both ends one identity.
base::Result<void> pipe_stat(void* /*self*/, FsNode node, FsStat& probed) {
    probed       = FsStat{};
    probed.ino   = node.ino;
    probed.mode  = 0100600;
    probed.links = 1;
    return {};
}

}  // namespace

Pipe::Pipe() : readable_(0), writable_(static_cast<long long>(kPipeBytes)) {
    identity_ = g_pipe_identities.fetch_add(1, std::memory_order_relaxed) + 1;
}

Pipe::~Pipe() {
    readable_.retire();
    writable_.retire();
    ring_gate_.retire();
}

namespace {

/// Posts one semaphore once per byte moved, so waiting partners see the
/// space or the payload arrive one count at a time.
void post_each(proc::Semaphore& gate, unsigned long times) {
    for (unsigned long index = 0; index < times; ++index) {
        gate.post();
    }
}

}  // namespace

base::Result<unsigned long> Pipe::pull(void* sink, unsigned long count) {
    if (count == 0) {
        return 0;
    }
    auto* const kOut   = static_cast<unsigned char*>(sink);
    bool        waited = false;
    for (;;) {
        unsigned long taken = 0;
        bool          closed;
        {
            const auto kGate = ring_gate_.guard();
            while (taken < count && ring_.pop(kOut[taken])) {
                ++taken;
            }
            closed = writers_ == 0;
        }
        if (taken != 0) {
            post_each(writable_, taken);
            return taken;
        }
        if (closed) {
            if (waited) {
                readable_.post();
            }
            return 0;
        }
        readable_.wait();
        waited = true;
    }
}

base::Result<unsigned long> Pipe::push(const void* source, unsigned long count) {
    unsigned long     put = 0;
    auto const* const kIn = static_cast<const unsigned char*>(source);
    while (put < count) {
        writable_.wait();
        {
            const auto kGate = ring_gate_.guard();
            if (!ring_.push(kIn[put])) {
                return base::KernelError::kOutOfMemory;
            }
        }
        ++put;
        readable_.post();
    }
    return put;
}

void Pipe::add_writer() {
    const auto kGate = ring_gate_.guard();
    ++writers_;
}

void Pipe::drop_writer() {
    bool closed;
    {
        const auto kGate = ring_gate_.guard();
        if (writers_ != 0) {
            --writers_;
        }
        closed = writers_ == 0;
    }
    if (closed) {
        readable_.post();
    }
}

bool Pipe::has_bytes() const {
    const auto kGate = ring_gate_.guard();
    return ring_.count() != 0;
}

bool Pipe::writers_gone() const {
    const auto kGate = ring_gate_.guard();
    return writers_ == 0;
}

bool Pipe::has_room() const {
    const auto kGate = ring_gate_.guard();
    return ring_.count() < kPipeBytes;
}

namespace {

// NOLINTNEXTLINE(readability-function-size) the FsOps signature names five
base::Result<unsigned long> pipe_read(void* self, FsNode /*node*/, unsigned long /*offset*/,
                                      void* sink, unsigned long count) {
    return static_cast<Pipe*>(self)->pull(sink, count);
}

// NOLINTNEXTLINE(readability-function-size) the FsOps signature names five
base::Result<unsigned long> pipe_write(void*       self, FsNode /*node*/, unsigned long /*offset*/,
                                       const void* source, unsigned long count) {
    return static_cast<Pipe*>(self)->push(source, count);
}

void pipe_retain(void* self, FsNode /*node*/) {
    static_cast<Pipe*>(self)->retain();
}

void pipe_release(void* self, FsNode node) {
    auto* const kPipe = static_cast<Pipe*>(self);
    if (node.opaque != nullptr) {
        kPipe->drop_writer();
    }
    if (kPipe->release()) {
        delete kPipe;
    }
}

const FsOps kOps{
    .lookup     = nullptr,
    .make_file  = nullptr,
    .make_dir   = nullptr,
    .unlink     = nullptr,
    .read       = pipe_read,
    .write      = pipe_write,
    .read_dir   = nullptr,
    .stat       = pipe_stat,
    .retain     = pipe_retain,
    .release    = pipe_release,
    .readable   = nullptr,
    .ioctl      = nullptr,
    .adopt_open = nullptr,
};

}  // namespace

const FsOps* PipeOps() {
    return &kOps;
}

}  // namespace cinux::fs
