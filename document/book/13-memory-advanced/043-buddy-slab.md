---
title: 043 · Buddy PMM 与 Slab
---

# 043 · Buddy PMM 与 Slab:物理分配器升伙伴,小对象交给 Slab,Heap 退役

> 这是 F2 内存弧的收尾,动的是**分配器底层**——和 040-042 的虚拟内存层是两个层面。两件事:PMM 的 flat bitmap 换成**伙伴系统(buddy)**;小对象分配从手写 Heap 全迁到分层 **Slab**(大对象走 buddy + direct-map)。手写的 `heap.{hpp,cpp}` 整个删掉(净 -1951 行),`operator new`/`kmalloc` 从此走 Slab。
>
> B 档:纯分配器重构,没有新用户能力,验证靠构建 + 测试 + 看碎片率/分配密度。但这一弧**踩的三个雷**(GOTCHA #13/#14/#15)比哪一章都多,值得细讲。

## 这章咱们要点亮什么

1. **Buddy PMM**:`bitmap` flat 分配 → per-order 伙伴系统(按 2 的幂拆/合并)。
2. **Slab 分层分配器**:buddy 之上,小对象(≤2048B)走 8 档通用缓存 + 专用缓存(Task/VMA/CachedPage),大对象走 buddy + direct-map。
3. **Heap 退役**:`heap.{hpp,cpp}` 删除,`kmalloc/kfree` + `operator new/delete` 全转 Slab。

## 为什么动分配器

PMM 用 flat bitmap 分页,简单但**没有合并**——碎片化无从治理。Heap 是 first-fit + coalesce 的手写堆,小对象多了就碎、还和 PMM 各管一摊。要上 mmap/CoW/共享内存这些 v1.0.0 特性,分配器得有层级、得能抗碎片。buddy(物理页,按 order 合并)+ slab(小对象,按类缓存)是教科书级的两层结构,Linux 也是这套。

## Buddy:per-order 伙伴系统

`kernel/mm/buddy.hpp:35 class BuddyAllocator`。把物理内存按 2 的幂(order)组织成多条 free-list,分配时从对应 order 拿、没有就拆大块,释放时和伙伴合并回大块——天然抗碎片。

### 两个非平凡的坑(都和 direct-map 有关)

buddy 本身的算法不复杂,坑全在它和 **direct-map**(`virt = phys + DIRECT_MAP_BASE`,loader 永久 identity 映射)的交互上:

**GOTCHA #13 —— `DmaPool::alloc` 不能再 `g_vmm.map` direct-map 区。** 之前 DmaPool 给 DMA 缓冲分配时,顺手 `g_vmm.map(virt=phys+DIRECT_MAP_BASE, ...)`。但 direct-map 区用的是 **1GB huge entry**(PDPT,PS bit),`VMM::map` 的 `walk_level` 撞上这个 huge entry 触发 **huge-split**,把 `pdpt[0]` 改成 4KB PT 指针,**破坏全局 direct-map**,后续 `phys_to_virt` 走错页表 → reserved PF(`err=0x9`)。修法很反直觉:**删掉那个 `VMM.map`**——direct-map 已经被 loader 永久映射了,根本不用再 map。

**GOTCHA #14 —— 侵入式 free-list 在 nested-KVM 上写读不一致。** buddy 初版把 `next` 指针直接写进 free 页头(经 direct-map),省一份元数据。这在 TCG(`CINUX_NO_KVM=1`)上 742/0 全绿,但在 **WSL2 nested KVM(AMD)** 上崩:EPT 对「huge page 内 sub-page 写」做不到写读一致——同一地址 main 读 valid、buddy op 读到毒值(`0xCAFEBABEDEADC0DE`),振荡 → `pop_free` 遍历 `#GP`。修法:**buddy 改成非侵入式 per-order bitmap free-list**——bitmap 存在元数据区(不写 free 页),`find_first_set` 天然 low-first。

> 教训:TCG 绿 ≠ KVM 绿。nested KVM 的 EPT 对 huge page 内 sub-page 访问有真实可见性差异,凡是在 direct-map huge 窗口里写 sub-page 的设计(侵入式 free-list、slab 页),都得避开——写元数据去别处。

## Slab:小对象的分层缓存

`kernel/mm/slab.hpp:76 class SlabAllocator`。buddy 之上,小对象(有效 `max(size,align) ≤ 2048`)走 Slab 的 8 档通用缓存(16/32/.../2048B);大对象走 buddy `alloc_pages` + direct-map 复用(和 DmaPool 同款,virt=phys+DIRECT_MAP_BASE,免 map 免元数据)。

几个设计点:

- **Slab 页走 `KMEM_SLAB` 区(PML4[256],4K 映射),绝不 direct-map huge(PML4[272])**。这正是 GOTCHA #14 的教训——sub-page 写不能在 huge 窗口里。slab 页用独立 4K 映射,整页 `memzero`(干净基线 + 毒检)。
- **double-free 毒检(O(1))**:freed 对象的 word[1] 写 sentinel;fresh 对象被 zero,命中即重复 free。
- **类专属缓存**:`create_cache` 给 Task/VMA/CachedPage 各建专用缓存,每个类加 `operator new/delete` 重载自动路由——所有 `new Task`/`delete p` **无调用点改动**就走上专用缓存。专用缓存用 `kObjAlign=16` 统一对齐,任意 obj_size 都能密集排布(实测 Task 1008B→4/slab,原 1024 档只有 3;VMA 56B→72/slab,原 64 档 63)。
- **`IF=0` 安全**:`alloc/free` 用 `irq_guard`,PF handler 里 `new CachedPage` 走 slab 安全。

### GOTCHA #15 —— page_cache 陈旧命中

slab 上线后顺带逼出一个真 bug:`page_cache` 原来按物理页地址键控缓存,slab 复用同物理页后,**陈旧命中**——读到上一次的内容。修法:page_cache 改按 `inode` 键控。这是"换底层分配器暴露了上层隐藏假设"的典型——slab 让物理页复用变频繁,原来"物理页基本不复用"的隐含假设破了。

## Heap 退役

`heap.{hpp,cpp}` + `test_heap`(内核 + host)全删,净 **-1951 行**。`kmalloc/kfree`(`slab.hpp:151/160`)成为统一分配入口,`crt_stub.cpp` 的 `operator new/delete` 7 重载全转 `kmalloc`。从这一弧起,内核只有一套分配器:buddy(页)+ slab(小对象)。

> 迁移时一个冲突点:`crt_stub.cpp` 的 include 从 `heap.hpp` 换成 `slab.hpp`,context 没对齐,手收一下(`--theirs` 取 patch 侧)。

## 验证

```bash
# buddy + slab 在,heap 删了
grep -n 'class BuddyAllocator' kernel/mm/buddy.hpp
grep -n 'class SlabAllocator\|void\* kmalloc\|void kfree' kernel/mm/slab.hpp
ls kernel/mm/heap.* 2>/dev/null && echo "heap 还在!" || echo "heap 已删 ✓"
# crt_stub 走 slab
grep -n 'slab.hpp\|kmalloc' kernel/arch/x86_64/crt_stub.cpp
```

构建 + 内核测试(这一弧 run-kernel-test 从 042 的 734 涨上去:buddy + slab 测;host 单测 `test_slab`/`test_kmalloc`):

```bash
cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

> 想看分配密度收益,专用缓存的 slab 利用率(Task 1008B→4/slab vs 旧的 3)是最好的指标——同样的 4KB 页,塞进更多对象,就是碎片下降的直接证据。

## 小结与下一站

F2 内存弧到这儿收口:PMM 升 buddy(抗碎片),小对象走 slab(密度 + 速度),Heap 退役。三个 GOTCHA(#13 direct-map 别 map、#14 nested-KVM 侵入式不可靠、#15 page_cache 键控)都是 direct-map + 物理页复用这套架构暴露出来的,记牢后面 F3/F4 还会碰到同类。

下一站 **044** 离开内存,补一块横切基建——`kallsyms` + panic backtrace(F0/F12-A 可观测性),给内核装上"崩了能看懂栈"的眼睛。那是个 C 档(调试路径),放 04-developer 卷。
