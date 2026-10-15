#include "kernel/mm/vma_diag.hpp"

#include "kernel/arch/x86_64/isr.hpp"  // NOLINT(misc-include-cleaner) fault-code words shared with the reporter
#include "kernel/boot/print.hpp"
#include "kernel/mm/vma.hpp"

namespace cinux::mm {

namespace {

const char* kind_name(VmaKind kind) {
    switch (kind) {
    case VmaKind::kAnonymous:
        return "anon";
    case VmaKind::kFile:
        return "file";
    case VmaKind::kStack:
        return "stack";
    }
    return "?";
}

const char* access_name(unsigned long long error_code) {
    return (error_code & cinux::arch::isr::kFaultWasWrite) != 0 ? "write" : "read";
}

const char* granted(bool yes) {
    return yes ? "on" : "off";
}

}  // namespace

void PrintFaultVerdict(const VmaStore* store, unsigned long address,
                       unsigned long long error_code) {
    if (store == nullptr) {
        print::Println("[kern]  vma: addr=%X, but this task keeps no range register", address);
        return;
    }
    const VmaRegion* const kHit = store->find(address);
    if (kHit == nullptr) {
        print::Println("[kern]  vma: addr=%X sits in no registered range; a true segfault",
                       address);
        return;
    }
    print::Println("[kern]  vma: addr=%X in [%X,%X) kind=%s access=%s rights r=%s w=%s x=%s",
                   address, kHit->start, kHit->end, kind_name(kHit->kind), access_name(error_code),
                   granted(kHit->readable), granted(kHit->writable), granted(kHit->executable));
}

}  // namespace cinux::mm
