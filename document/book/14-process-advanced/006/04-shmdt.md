---
title: 04 · shmdt 长度陷阱:取段 page_count,不取合并后 VMA 跨度
---

# shmdt 长度陷阱:取段 page_count,不取合并后 VMA 跨度

真坑,有专门回归测兜。这一节是个「设计决策的根因链」,咱们顺着讲。

`shmdt(addr)` 要算「拆多少页」。一个直觉的做法是查这个 addr 落在哪个 VMA、用 `vma->end - vma->start` 当长度。**这个直觉是错的**,原因是一条 VMA 合并规则。

VMA store 插入新映射时会合并相邻同标志、且**无 backing**(file_offset/backing 元数据)的 VMA(`vma.cpp:119-122`):

```cpp
const bool merge_prev = (prev != nullptr) && (prev->end == start) && (prev->flags == flags) &&
                        prev->backing == nullptr;
const bool merge_next =
    (cur != nullptr) && (cur->start == end) && (cur->flags == flags) && cur->backing == nullptr;
```

两个背靠背的 SHM 映射正好全中:**它们的 flags 完全一样**(都是 `Read|Shared`,可写的话加 `Write`)、`backing == nullptr`、地址相邻——结果被并成一个 VMA,跨度 = 两段之和。如果按 VMA 算 unmap 长度,shmdt 第一个段会顺带拆掉第二个段的页。

> 串一句:为什么 SHM 映射不带 `Anonymous` 位?看 `sys_shm.cpp:171-175` 的 VmaFlags:

```cpp
using cinux::mm::VmaFlags;
VmaFlags vf = VmaFlags::Read | VmaFlags::Shared;
if (!readonly) {
    vf |= VmaFlags::Write;
}
```

刻意只设 Read|Shared(+Write),**不带 Anonymous 位**。因为 SHM 段背后是**已 alloc 的真实物理页,eager map**(shmat 时立刻把每页装进页表),**不走缺页/demand-paging 路径**。匿名 mmap 才带 Anonymous(写时分配);SHM 不带,这跟它的 eager 语义一致。但代价是:两个 SHM 映射 flags 全等 + 无 backing,正好满足合并条件。

## 正解——用段自己的 page_count 算长度

`sys_shm.cpp:228-253`:

```cpp
const uint64_t phys_base = task->addr_space->translate(addr);
if (phys_base == 0) {
    return -kEinval;
}
auto shmid_r = ShmRegistry::instance().find_by_phys(phys_base);
if (!shmid_r.ok()) {
    return -kEinval;
}
const int                     shmid = shmid_r.value();
const cinux::ipc::ShmSegment* seg   = ShmRegistry::instance().segment(shmid);
if (seg == nullptr || seg->page_count == 0) {
    return -kEinval;
}

const uint64_t pages = seg->page_count;
const uint64_t len   = pages * kPageSize;
for (uint64_t i = 0; i < pages; ++i) {
    const uint64_t v = addr + i * kPageSize;
    const uint64_t p = phys_base + i * kPageSize;
    task->addr_space->unmap(v);
    static_cast<void>(cinux::mm::g_pmm.pte_count_dec_and_test(p));
}

static_cast<void>(task->addr_space->vmas().remove(addr, addr + len));
```

([sys_shm.cpp](../../../kernel/syscall/sys_shm.cpp#L228-L253),有删节——省略了 detach 后的 free_pages 路径。)调用方 addr → `translate` 得 phys_base → `find_by_phys` 定位段 → **取段自己的 `page_count`** 算 `len = pages * 4096` → 逐页 unmap + `pte_count_dec_and_test` → `vmas().remove(addr, addr + len)`。`remove()` 戳洞,正确处理合并 VMA 的切分(把合并节点劈成两半,不会误删邻居)。

源码注释把这条决策讲得很直白(`sys_shm.cpp:221-227`):

> We deliberately do NOT use vma->start / vma->end for the length: vmas().insert() coalesces adjacent same-flags mappings, so two back-to-back SHM attachments in one address space share a single VMA and its [start,end) would span more than this segment. The segment's own page_count is the authoritative teardown length; remove() then punches a clean hole even out of a merged VMA.

**根因链一条讲完**:为什么 SHM 映射不带 Anonymous?背后是已 alloc 的真实物理页(eager map,不走缺页)。为什么两 SHM 映射会合并?合并条件只看 flags 相等 + 无 backing,SHM 恰好满足。为什么 shmdt 不能取 VMA 长度?合并后跨度大于单段。为什么取 `page_count` 就对?因为**段是分配的权威单元**(shmget 一次 alloc 整块),`remove()` 会从合并 VMA 戳干净洞。你能记住「权威长度来自段不是 VMA」这个真坑(footgun)。

> **调试五段标签——shmdt 长度踩坑回归**:
> - **症状**:两个相邻 SHM 段,shmdt 第一个后第二个页被踩、读出脏值或 #PF。
> - **根因**:shmdt 按 VMA 跨度算 unmap 长度,合并后 VMA 跨度 = 两段之和,第一个的 unmap 把第二个的页也拆了。
> - **定位**:`vma.cpp:119-122` 合并条件 + `sys_shm.cpp` 原 shmdt 路径取 VMA 长度。
> - **修复**:shmdt 改成 `translate(addr) → find_by_phys → seg->page_count` 取段自己的页数当权威长度,`remove()` 戳洞处理合并 VMA 切分。
> - **防复发**:`test_shm.cpp:277-319` 的 `test_adjacent_detach_preserves_peer` 专门锁——两段贴邻(a1=base、a2=base+4096)、写不同 magic(0x1111... / 0x2222...)、shmdt 第一段后断言 `translate(a1)==0`(已拆)且 `translate(a2)!=0`(还在)+ 读 a2 仍得 0x2222... 而非被踩。
