---
title: 03 · DMA 池:收编散装 DMA 与 direct-map 那个坑
---

# DMA 池:收编散装 DMA 与 direct-map 那个坑

## DMA 池:把散装 DMA 收成设备无关

第三件,也是坑最深的一件。036 之前,谁要 DMA(比如 AHCI)就自己来一套:

```cpp
// 散装 DMA —— 每个驱动各写一遍,口径不一
auto phys = g_pmm.alloc_pages(...);
auto virt = phys + 0xFFFFFFFF80000000ULL;   // 硬编码 direct-map 偏移
g_vmm.map(virt, phys, ...);
```

这套散在各个驱动里。这一章把它收进 `kernel/drivers/dma/`,三个类型各司其职:

- **`DmaBuffer`**:move-only 句柄,持一对物理/虚拟地址,析构时归还(RAII)。
- **`DmaPool`**:`alloc(size)` 返一个 `DmaBuffer`,内部封装"分配物理页 + 建映射"。
- **`PrdtBuilder`**:设备无关的 scatter-gather 段构建器——给一段可能不连续的缓冲,拆成一个个设备能消化的段(后面 AHCI/NVMe/VirtIO 各转各的硬件格式)。

### direct-map:复用,别另开映射

`DmaPool` 有个关键设计:**直接复用 direct-map,不另开虚拟地址分配**。内核有一块"direct-map"区,物理地址 `phys` 永久映射在 `phys + KERNEL_VMA`——**物理地址唯一决定虚拟地址**。所以 DMA 缓冲的虚拟地址直接取 `phys + KERNEL_VMA` 就行,不用单独分配虚拟地址、也不用单独建映射。

### 一个反直觉的坑:direct-map 的页表项,绝对不能 unmap

这是这一章最值得记的坑。`DmaPool` 第一版的 `free()` 想"干净",除了回收物理页,还顺手把那段虚拟映射 `unmap` 掉——结果把 direct-map 的**永久映射槽**给拆了。后面 demand paging 再用到那段虚拟地址,反复映射到错的物理页,QEMU 直接卡死在死循环里。

修复很反直觉:`free()` **只回收物理页,绝不 unmap**。direct-map 的页表项是永久的"物理↔虚拟"对照表,物理页回收了、那条虚拟映射留着无害(下次分配同一个物理页,还会落到同一个虚拟地址)。**按 RAII 的直觉去 unmap,在这里是错的。**

> 这条教训后面还会反复出现:direct-map 不是普通的动态映射,它是一张永久对照表。凡是在它上面做 alloc/free 的代码(后面的 AHCI 驱动、slab 分配器),都得记着——**map 可以(覆盖/建立),unmap 绝对不行**。

## 验证

三块各自的验证抓手:

```bash
# 环形缓冲:两处都换成 Cinux-Base 的了
grep -rn 'cinux::lib::RingBuffer' kernel/ipc/ kernel/drivers/keyboard/
# 期望:pipe、keyboard 各一处,且手写 buffer_/head_/tail_ 的痕迹没了

# 内核日志:sys_dmesg + KernelLog + ConcurrentRingBuffer 在
grep -rn 'sys_dmesg\|class KernelLog\|ConcurrentRingBuffer' kernel/syscall/ kernel/lib/

# DMA:三个类型在,free 只回收物理页、不 unmap
ls kernel/drivers/dma/
grep -nE 'free_pages|unmap' kernel/drivers/dma/dma_pool.cpp   # 期望:free 路径只 free_pages,无 unmap
```

构建:

```bash
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
```

想亲眼看内核日志,跑内核测试时找串口输出里 `[INFO]`/`[WARN]` 开头、带 tick 数的行——那就是 `KernelLog` 格式化出来的历史。
