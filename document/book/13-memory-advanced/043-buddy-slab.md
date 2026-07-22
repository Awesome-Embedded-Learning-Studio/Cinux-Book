---
title: 043 · Buddy PMM 与 Slab
---

# 043 · 物理分配器升伙伴,小对象交给 Slab,Heap 退役

> 040-042 把虚拟内存层(地址空间、按需分页、缓存)做厚了。这一章往下挖一层,动**分配器**本身——它和虚拟内存是两个层面。问题在:物理页分配用的是 flat bitmap(分了就分了,没合并,碎片化无从治理);小对象用的是手写堆(first-fit,碎、还和物理分配各管一摊)。要上写时复制、共享内存这些,分配器得有层级、能抗碎片。这一章换成教科书级的两层结构:**buddy**(物理页,按 2 的幂合并)+ **slab**(小对象,按类缓存),手写的堆整个删掉。

## 为什么动分配器

物理内存用 flat bitmap 分页,简单但**没有合并**——释放的页不会和邻居拼回去,用久了全是碎片。手写堆是 first-fit + 合并,小对象多了就碎,还和物理分配各管一摊,口径不一。buddy(按 order 合并)+ slab(按类缓存)是 Linux 那套两层结构,抗碎片、密度高,正是后面写时复制 / 共享内存需要的底子。

## buddy:per-order 伙伴系统

`kernel/mm/buddy.hpp` 的 `BuddyAllocator` 把物理内存按 2 的幂(order)组织成多条 free-list:分配时从对应 order 拿、没有就拆大块,释放时和**伙伴**(同 order 的另一半)拼回大块——天然抗碎片。算法本身不复杂,坑全在它和 direct-map 的交互上。

### 坑一:free-list 不能"侵入式"写进空闲页(nested-KVM 的可见性问题)

buddy 第一版图省事,把 `next` 指针直接写进空闲页的头部(经 direct-map 访问),省一份元数据。这在 QEMU 的纯软件模拟(TCG)下全绿;但在 **WSL2 嵌套 KVM(AMD)** 上崩——嵌套虚拟化的 EPT 对"在 1GB 大页里写一个子页"做不到写读一致:同一个地址,普通读是合法值,buddy 操作读到的却是毒值,振荡 → 遍历 free-list 时崩。

修法:**free-list 改成非侵入式的 per-order bitmap**——bitmap 存在元数据区(不写空闲页),用 `find_first_set` 找空闲块。这就避开了"在大页窗口里写子页"这个不可靠操作。

> 教训:**纯软件模拟绿 ≠ 嵌套虚拟化绿**。凡是在 direct-map 的大页窗口里写子页的设计(侵入式 free-list、slab 页),在嵌套 KVM 上都可能撞这个可见性问题。元数据得放到别处。

### 坑二:direct-map 区不能再 `map`

buddy 接 direct-map 后,还连带修了 037 那条纪律的一个违反:`DmaPool` 给 direct-map 区分配时顺手调了 `vmm.map`——可 direct-map 区用的是 1GB 大页表项,`vmm.map` 的页表遍历撞上大页项触发"大页拆分",**破坏了全局 direct-map**。修法是删掉那个 `map`——direct-map 已经被加载器永久映射了,根本不用再 map。这条 037 提过,这里再确认:**direct-map 的页表项是永久对照表,map 可以,unmap/重 map 绝对不行。**

## slab:小对象的分层缓存

`kernel/mm/slab.hpp` 的 `SlabAllocator` 在 buddy 之上做小对象分配:有效大小 ≤ 2048 字节的,走 8 档通用缓存(16/32/.../2048);更大的,走 buddy 的 `alloc_pages` + direct-map 复用。

几个设计点:

- **slab 页走独立的 4K 映射区(`KMEM_SLAB`),不进 direct-map 大页窗口。** 这正是上面那个坑的教训——子页写入不能在大页窗口里做。slab 页用独立 4K 映射,整页清零(给下面的毒检一个干净基线)。
- **双重释放检测(O(1))**:释放的对象在一个固定槽写个哨兵值;新分配的对象被清零,所以"释放一个已经释放过的"会命中哨兵——当场发现。
- **类专属缓存 + 自动路由**:`Task`、VMA、`CachedPage` 这些高频类型各建一个专属缓存,每个类加 `operator new/delete` 重载。于是所有 `new Task` / `delete ptr` **调用点不用改**,自动走上专属缓存。专属缓存用统一对齐,任意大小的对象都能密集排布——实测 `Task`(1008 字节)一页能塞 4 个(以前按 1024 档只能塞 3 个),碎片实打实下降。

## 堆退役

手写的 `heap.{hpp,cpp}` 整个删掉(净减近两千行)。`kmalloc`/`kfree` 成为统一分配入口,`operator new`/`delete` 全部转发到它。从这一章起,内核只有一套分配器:buddy(物理页)+ slab(小对象),分工清楚。

### 连带修的一个真 bug:Page Cache 陈旧命中

slab 上线后,物理页复用变频繁,顺带逼出一个隐藏 bug:Page Cache 原来按物理页地址键控缓存,同一个物理页被 slab 复用后,缓存里**陈旧命中**——读到上一次的内容。修法:Page Cache 改按 `inode` 键控。这是"换底层分配器暴露了上层隐藏假设"的典型—— slab 让物理页复用变多,原来"物理页基本不复用"的隐含假设就破了。

## 验证

```bash
grep -n 'class BuddyAllocator' kernel/mm/buddy.hpp
grep -n 'class SlabAllocator\|void\* kmalloc\|void kfree' kernel/mm/slab.hpp
ls kernel/mm/heap.* 2>/dev/null && echo "heap 还在!" || echo "heap 已删 ✓"
grep -nE 'bitmap|find_first_set' kernel/mm/buddy.hpp kernel/mm/buddy.cpp | head   # 非侵入式 free-list
```

构建:

```bash
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
```

想看分配密度的收益:专属缓存的利用率是最好的指标——`Task`(1008B)一页塞 4 个(以前 3 个),同样的 4KB 页装更多对象,就是碎片下降的直接证据。双重释放检测可以自己试:故意 `kfree` 同一个指针两次,第二次会被哨兵抓出来——这是 slab 给的额外保险。
