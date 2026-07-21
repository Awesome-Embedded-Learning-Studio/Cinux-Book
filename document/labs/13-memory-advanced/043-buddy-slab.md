---
title: Lab 043 · Buddy PMM 与 Slab 验证
---

# Lab 043 · Buddy PMM 与 Slab 验证

> 对应 `document/book/13-memory-advanced/043-buddy-slab.md`。验证档 **B 档**(分配器重构)。验证靠构建 + 测试 + grep + 分配密度。

## 目标

确认四件事:

1. `BuddyAllocator`(per-order,非侵入式 bitmap free-list)在;
2. `SlabAllocator` + `kmalloc/kfree` 在,`operator new` 转过去;
3. `heap.{hpp,cpp}` 已删,`crt_stub.cpp` include 换成 slab;
4. 三个 GOTCHA 的修复都在(别 map direct-map、bitmap 非侵入式、page_cache 按 inode 键控)。

## 步骤

### 1. 构建 + 测试

```bash
git checkout 043_mm_buddy_slab 2>/dev/null || git checkout 043_*
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

**期望**:`build=0`;run-kernel-test + host `test_slab`/`test_kmalloc` 全绿。

### 2. 分配器在位 + heap 删除

```bash
grep -n 'class BuddyAllocator' kernel/mm/buddy.hpp
grep -n 'class SlabAllocator\|void\* kmalloc\|void kfree' kernel/mm/slab.hpp
ls kernel/mm/heap.* 2>/dev/null && echo "heap 还在!" || echo "heap 已删 ✓"
grep -n 'slab.hpp\|kmalloc' kernel/arch/x86_64/crt_stub.cpp
```

**思考**:`operator new` 为什么不显式调 `kmalloc`?——见章节:类专属 `operator new/delete` 重载(Task/VMA/CachedPage 各加 4 重载),`new T` 自动路由到专用缓存,**无调用点改动**。

### 3. 三个 GOTCHA 的修复

```bash
# GOTCHA#13:DmaPool::alloc 不该再 map direct-map(应删了 vmm.map)
grep -nE 'vmm\.map|VMM::map' kernel/drivers/dma/dma_pool.cpp
# 期望:alloc 路径无 map(direct-map 已被 loader 永久映射)
# GOTCHA#14:buddy 用 bitmap 非侵入式(不该把 next 写进 free 页)
grep -nE 'bitmap|find_first_set' kernel/mm/buddy.hpp kernel/mm/buddy.cpp | head
# GOTCHA#15:page_cache 按 inode 键控(非物理页地址)
grep -nE 'Inode\*|ino' kernel/mm/page_cache.hpp | head
```

### 4.(指标)分配密度

去看专用缓存的 slab 利用率:Task 1008B → 4 个/slab(旧 1024 档 3 个),VMA 56B → 72 个/slab(旧 64 档 63 个)。同 4KB 页塞更多对象 = 碎片下降的直接证据。

## 验收清单

- [ ] 构建 `build=0`,内核 + host 测试全绿。
- [ ] `BuddyAllocator` + `SlabAllocator` + `kmalloc/kfree` 在。
- [ ] `heap.*` 已删,`crt_stub.cpp` 走 slab。
- [ ] 三个 GOTCHA 修复在位。
- [ ] 能说清「为什么 buddy 不能侵入式写 free 页(GOTCHA#14)」「为什么 DmaPool 不能 map direct-map(GOTCHA#13)」。

## 别做这些

- **别**在 direct-map huge 窗口(PML4[272])里写 sub-page——nested-KVM EPT 写读不一致(GOTCHA#14)。slab 页用 KMEM_SLAB 区(PML4[256])4K 映射。
- **别**给 DmaPool 的 direct-map 分配加 `vmm.map`——huge-split 破坏全局 direct-map(GOTCHA#13)。
- **别**让 page_cache 按物理页地址键控——slab 复用物理页会陈旧命中(GOTCHA#15),按 inode 键。
