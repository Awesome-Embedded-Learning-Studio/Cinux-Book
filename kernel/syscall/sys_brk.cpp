#include "kernel/syscall/sys_brk.hpp"

#include "cinux/result.hpp"
#include "kernel/mm/vma.hpp"
#include "kernel/proc/task.hpp"
#include "kernel/syscall/syscall_internal.hpp"

namespace cinux::syscall {

long long HandleBrk(Word target_word, Word /*second*/, Word /*third*/) {
    const proc::Task* const kSelf = CurrentTask();
    if (kSelf == nullptr || kSelf->vmas == nullptr) {
        return 0;
    }
    const base::Result<unsigned long> kMoved = kSelf->vmas->move_heap_end(target_word);
    if (kMoved.ok()) {
        return static_cast<long long>(kMoved.value());
    }
    return static_cast<long long>(kSelf->vmas->heap_end());
}

}  // namespace cinux::syscall
