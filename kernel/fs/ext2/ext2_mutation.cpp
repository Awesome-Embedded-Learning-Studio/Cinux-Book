#include "kernel/fs/ext2/ext2_mutation.hpp"

#include "cinux/assert.hpp"
#include "cinux/memory.hpp"
#include "kernel/arch/x86_64/irq_guard.hpp"
#include "kernel/driver/block/block_device_concept.hpp"
#include "kernel/fs/ext2/ext2.hpp"
#include "kernel/fs/vfs.hpp"
#include "kernel/proc/sync.hpp"
namespace cinux::fs::ext2 {
Ext2::Mutation::Mutation(Ext2& volume)
    : volume_(volume), batch_(volume.cache_), before_(new State) {
    base::safety::Check(volume.mutation_ == nullptr, "nested volume mutation");
    before_->superblock = volume.sb_;
    base::CopyBytes(before_->groups, volume.groups_, sizeof(before_->groups));
    volume.mutation_ = this;
}
Ext2::Mutation::~Mutation() {
    if (!committed_) {
        volume_.sb_ = before_->superblock;
        base::CopyBytes(volume_.groups_, before_->groups, sizeof(before_->groups));
    }
    volume_.mutation_ = nullptr;
}
bool Ext2::Mutation::commit() {
    committed_ = batch_.commit();
    return committed_;
}
bool Ext2::read_storage(driver::Lba first, void* sink, unsigned long bytes) {
    return mutation_ == nullptr ? cache_.read(first, sink, bytes)
                                : mutation_->read(first, sink, bytes);
}
bool Ext2::patch_storage(driver::Lba first, unsigned long offset, unsigned long bytes,
                         const void* source) {
    return mutation_ == nullptr ? cache_.patch(first, offset, bytes, source)
                                : mutation_->patch(first, offset, bytes, source);
}
Ext2::InodeHold* Ext2::held_inode(unsigned long ino) {
    return held_.find_if([ino](const InodeHold& hold) { return hold.ino == ino; });
}
bool Ext2::has_hold(unsigned long ino) {
    const arch::IrqGuard kIrq;
    const auto           kGuard = hold_gate_.guard();
    return held_inode(ino) != nullptr;
}
void Ext2::retain_locked(FsNode node) {
    const arch::IrqGuard kIrq;
    const auto           kGuard = hold_gate_.guard();
    auto*                hold   = held_inode(node.ino);
    if (hold == nullptr) {
        hold      = new InodeHold;
        hold->ino = node.ino;
        held_.push_back(*hold);
    }
    ++hold->count;
}
void Ext2::retain(FsNode node) {
    const proc::MutexGuard kGate(write_gate_);
    retain_locked(node);
}
bool Ext2::reap_inode(unsigned long ino) {
    const auto kInode = read_inode(ino);
    if (!kInode.ok()) {
        return false;
    }
    if (kInode.value().i_links_count != 0) {
        return true;
    }
    Mutation mutation(*this);
    Inode    dead = kInode.value();
    dead.i_dtime  = clock_ == nullptr ? sb_.s_mkfs_time : now_stamp();
    free_data_blocks(dead);
    if (!write_inode(ino, dead)) {
        return false;
    }
    free_inode(ino);
    return mutation.commit();
}
void Ext2::release(FsNode node) {
    const arch::IrqGuard kIrq;
    const auto           kGuard = hold_gate_.guard();
    auto*                hold   = held_inode(node.ino);
    base::safety::Check(hold != nullptr && hold->count != 0, "unowned ext2 inode release");
    --hold->count;
}
bool Ext2::reap_pending() {
    auto* hold = held_.head();
    while (hold != nullptr) {
        auto* next   = held_.after(*hold);
        bool  unused = false;
        {
            const arch::IrqGuard kIrq;
            const auto           kGuard = hold_gate_.guard();
            unused                      = hold->count == 0;
        }
        if (unused) {
            if (!reap_inode(hold->ino)) {
                return false;
            }
            {
                const arch::IrqGuard kIrq;
                const auto           kGuard = hold_gate_.guard();
                base::safety::Check(held_.remove(*hold), "ext2 hold disappeared");
            }
            delete hold;
        }
        hold = next;
    }
    return true;
}
}
