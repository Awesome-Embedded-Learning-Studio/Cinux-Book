---
title: Debug · Canvas 的 3MB back buffer,撞出两个内存布局洞
---

# Debug · Canvas 的 3MB back buffer,撞出两个内存布局洞

> 出处:tag `029_gui_canvas`,`document/notes/029/1.md`。这里按「症状 → 定位 → 根因 → 修复 → 防复发」提炼,不照抄原始笔记。地址/常量以 tag 源码为准。

这次排错的特点是:**一个 `new[]` 连环引爆两个潜伏 bug**。canvas 一 `init` 就要 ~3 MB 后备缓冲,这 3 MB 先把「堆能无限扩展」的洞撑爆,修好后又把「direct map 覆盖不足」的洞逼出来。两个洞都是 028e 那张内存布局表里没堵上的薄弱处。

## 症状

把 GUI 画布接进内核,`CINUX_GUI=ON` 构建后:

- 第一挂:canvas 相关测试全过,但紧接着的 `test_fifo_ordering` 在创建一个任务时 **hang**。`CINUX_GUI=OFF` 时,356 项基线测试一个不少全过。
- 第二挂(修完第一挂后):`test_create_user_space` **hang**。

两次都是「GUI 引入的、非 GUI 路径不犯」,而且都发生在 canvas 测试之后——线索都指向 canvas 那块大缓冲。

## 定位

两个挂的定位思路不同,但起点都是同一句代码:

```cpp
back_buf_ = new uint32_t[width_ * height_];   // 1024×768×4 ≈ 3 MB
```

**第一挂**:canvas 测试通过、后续建任务 hang,典型的「前面的大块分配把后面的路挖断了」。怀疑堆:`KMEM_HEAP_SIZE` 在 028e 只预留了 **1 MB**,而这 1 MB 只是「初始大小」,`Heap::expand()` 找不到空闲块时会自动扩容。查 `expand()` 实现——**没有上限检查**。也就是说堆会一路涨过 1 MB、2 MB、3 MB……于是 canvas 这 3 MB 让堆涨出了自己的区段、踩进了紧挨着的 MMIO / Stack 区段。后续 `TaskBuilder` 给新任务映射内核栈时,`g_vmm.map()` 撞上被冲乱的页表,hang。

**第二挂**(堆加上限后):建用户地址空间 hang。`AddressSpace` 构造里有 `phys_to_virt(pml4_phys_) = pml4_phys_ + KERNEL_VMA`(`KERNEL_VMA = 0xFFFFFFFF80000000`)。这是内核里的习惯用法——把物理地址加个偏移变成内核可访问的虚拟地址。但它有个**隐含前提**:那个 `phys + KERNEL_VMA` 得真的被映射过。查 loader:它只 `identity_map` 到了 ELF 段末尾,约 ~20 MB;可 PMM 管着 9 GB,`alloc_page()` 能返回任意物理地址。canvas 那堆大块分配把低地址物理页耗得差不多了,后面的分配更容易落到 > 20 MB 的高地址——`phys_to_virt` 算出来的虚拟地址没人映射,一访问就 page fault,hang。

## 根因

把两个洞放一起看,它们都是 028e 那张布局表没堵上的薄弱处:

```text
028e 收拢了布局，但留下两个洞：
  洞一：区段内部无上限。KMEM_HEAP_SIZE 只标了“初始 1MB”，
        expand() 没有边界检查，堆以为头顶有无限空间，其实隔壁就是 MMIO/Stack。
  洞二：direct map 覆盖不足。loader 只映射到 ELF 末(~20MB)，
        phys_to_virt 却假设 phys+KERNEL_VMA 一定可访问；PMM 返回高地址就塌。
```

canvas 的 3 MB back buffer 是那个「大块消费者」——单测里谁也不会真分配 3 MB,所以这两个洞一直潜伏;canvas 一来,先撞洞一(堆 expand 越界),修好后剩余的大块分配把低物理页吃光、把洞二(PMM 返回高地址)也喂了出来。

## 修复

**洞一:给堆一个硬上限 + 把区段预留够大。**

- `heap.hpp` 加 `max_size_` 字段,`expand()` 改返回 `bool`;`init()` 里 `max_size_ = KMEM_HEAP_SIZE`。
- `expand()` 扩容前查边界:`if (size_ + expand_size > max_size_) return false`;`alloc` 拿到 expand 失败就返回 `nullptr`,不再递归重试。
- `memory_layout.hpp`:`KMEM_HEAP_SIZE` 从 1 MB 提到 **128 MB**(`0x8000000`)。GUI 的画面缓冲/控件/贴图都要堆,1 MB 不够;128 MB 是合理预留,物理页按需分配、不浪费物理内存。

**洞二:让 loader 全量映射物理内存(Linux 风格 direct map)。**

`big_kernel_loader.cpp` phase2 扫 BootInfo 的 E820 memory map,找最高可用 RAM,把全部物理内存映射到 `KERNEL_VMA` 起:

```cpp
for (uint32_t i = 0; i < bi->mmap_count; i++) {
    if (bi->mmap[i].type == 1)            // usable RAM
        highest_phys = max(highest_phys, base + length);
}
// 2MB 大页映射低段、1GB 大页映射高段
```

用**大页**(2 MB、1 GB)是关键:全量映射 9 GB,用 4 KB 页要一大堆页表;用大页几个页表项就盖住了(8 GB 量级约需 ~5 个页表页)。映射全部 RAM 后,`phys_to_virt` 对 PMM 返回的任何页都成立。

附带:`test_vmm.cpp` 的 `test_demand_page` 原本依赖「某地址未映射来触发 page fault」,全量 direct map 后这个假设不成立,改成「验证高地址能正常读写」。

## 防复发

这次留下三条条件反射。

**其一:「预留的虚拟区段」和「能涨到的上限」是两回事,得给每个会自动扩张的分配器一个硬上限。** 堆、栈、任何按需 expand 的结构,都不能假设「头顶有无限虚拟空间」——隔壁大概率是别的区段。`expand` 必须查 `size_ + 增量 <= max_size_`,失败就老老实实返回,不能递归、不能硬冲。

**其二:`phys_to_virt(p) = p + 偏移` 这类习惯用法,成立的前提是「那张 direct map 覆盖了 PMM 可能返回的全部地址」。** loader 只映射内核镜像是不够的——PMM 管多少物理内存,direct map 就得覆盖多少;否则一旦 `alloc_page` 返回高地址页,`phys_to_virt` 就给你一个没人映射的虚拟地址,一访问就 PF。要么全量映射(本 tag 的做法),要么实现一个真正的 `kmap` 按需映射。

**其三:单测覆盖不了「大块分配」。** 单测里没人会 `new` 出 3 MB,所以「堆越界」「direct map 不足」这类布局冲突在单测里永远隐形。只有像 canvas 这样的大块消费者、或内核集成测试(真去分配大块、真去建任务),才暴露得了。给大块资源消耗的场景补集成测试,是防这类潜伏 bug 的根本。

> 三条合起来其实是一句:028e 把「区段别重叠」做对了,但**地址布局的健全性**还有两层——「区段内部有边界」「direct map 覆盖全」。canvas 这个大块消费者,把后两层一次逼了出来。

---

## 参考

- Linux 内核 direct map / `phys_to_virt`(`phys + PAGE_OFFSET`):loader 全量映射物理内存的参照模型。<https://www.kernel.org/doc/html/latest/core-api/mm-api.html>
- x86-64 大页(2 MB PD、1 GB PDP):全量 direct map 用大页低开销的依据。Intel SDM Vol 3(分页);OSDev Page Table:<https://wiki.osdev.org/Page_Table>。
- E820 BIOS memory map:loader 扫描「最高可用 RAM」的依据。OSDev:<https://wiki.osdev.org/Detecting_Memory_(x86)>。
- 原始排查笔记:[1.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/029/1.md)。
