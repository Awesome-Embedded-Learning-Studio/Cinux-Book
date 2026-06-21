---
title: 参考 · 内存:PMM、VMM、内核堆与地址空间
---

# 参考 · 内存:PMM、VMM、内核堆与地址空间

> 查阅层。这一页是 Cinux 内存子系统的速查表,不按 tag 组织,给后续章节(堆分配、用户地址空间 022、CoW page fault 035、GUI 大缓冲 029……)查区段地址、页表 flag、分配器接口用。实现以最终 tag `035_multi_terminal` 的源码为准。
>
> 范围:物理页分配(PMM 位图)、4 级分页(VMM)、内核堆(首次适配 + 合并)、每进程地址空间(AddressSpace)、高半区 direct map。**不含 slab、不含 swap、不含内存压缩**——Cinux 是按需分页 + 简单 free-list 堆。

## 子系统地图

```text
   E820 BIOS 内存图(usable RAM)                内核镜像 + 栈(loader 已映射)
        │  parse_memory_map                       │
        │  过滤 type-1、去 <1MB、4KB 对齐          │
        ▼                                         ▼
   ┌──────────────────────────┐       ┌──────────────────────────────────┐
   │  PMM(位图,1 bit/page)    │       │  VMM(4 级页表 PML4→PDPT→PD→PT)     │
   │  alloc_page → 物理地址     │◀──────│  map(virt,phys,flags)             │
   │  0 = OOM                  │ 分配页 │  缺中间表时向 PMM 要页、清零、接线  │
   └──────────────┬───────────┘       └──────────────┬───────────────────┘
                  │ 也供堆 expand                       │ 也驱动
                  ▼                                    ▼
   ┌──────────────────────────┐       ┌──────────────────────────────────┐
   │  Heap(free-list,32B 头)   │       │  AddressSpace(每进程 PML4)        │
   │  first-fit + split + 合并 │       │  内核半 [256..511] 镜像全局映射     │
   │  expand 经 VMM,有上限      │       │  用户半 [0..255] 私有              │
   │  g_heap(全局 new/delete)  │       │  activate() = 写 CR3              │
   └──────────────────────────┘       └──────────────────────────────────┘
```

两条地址翻译习惯用法贯穿全书:`phys_to_virt(p) = p + KERNEL_VMA`(走 direct map,前提是该物理页被 loader 全量映射);`virt_to_phys(v) = v - KERNEL_VMA`。

## 内核虚拟内存布局

`kernel/arch/x86_64/memory_layout.hpp` 把高半区(从 `KMEM_BASE = 0xFFFF800000000000`)切成连续区段,每段 = 前一段 base+size。区段别重叠是布局健全性的第一层(028e 收拢),区段**内部**有上限是第二层(029 补),direct map 覆盖全是第三层(029 补):

| 区段 | 相对 KMEM_BASE 偏移 | 绝对基址 | 大小 | 用途 |
|---|---|---|---|---|
| HEAP | `+0x0000_0000` | `0xFFFF8000_00000000` | **128 MB**(`0x800_0000`) | 内核堆(`g_heap`),`new`/`delete` |
| MMIO | `+0x0800_0000` | `0xFFFF8000_08000000` | 2 MB(`0x20_0000`) | 设备 MMIO(AHCI BAR5 等) |
| FB | `+0x0820_0000` | `0xFFFF8000_08200000` | 16 MB(`0x100_0000`) | 帧缓冲(2MB 对齐,大页映射) |
| STACK | `+0x0920_0000` | `0xFFFF8000_09200000` | ~1 MB(隐式) | 每任务内核栈(向上分配) |
| DMA | `+0x0930_0000` | `0xFFFF8000_09300000` | 1 MB(`0x10_0000`) | 临时 DMA 缓冲(扇区读等) |
| EXT2_DMA | `+0x0940_0000` | `0xFFFF8000_09400000` | 1 MB(`0x10_0000`) | ext2 块缓存 / DMA |

`KERNEL_VMA = 0xFFFFFFFF80000000`(高半区 direct map 基址,必须和 `linker.ld` 一致)。整套布局合计 ~149 MB 虚拟预留;**预留虚拟区段 ≠ 立刻吃物理内存**——物理页按需由 PMM 分配。

> 改这张表要同步跑 `scripts/check_memory_layout.py`(检查区段不重叠、不越界)。canvas 那次就是堆从 1 MB 涨到 3 MB 撞进 MMIO/Stack,见 [029-canvas-heap-directmap.md](../debug-notes/029-canvas-heap-directmap.md)。

## 物理内存管理 PMM

`cinux::mm::PMM`(位图分配器,1 bit / 4KB page,`g_pmm` 全局,内含 `Spinlock`):

| 接口 | 说明 |
|---|---|
| `init(BootInfo&)` | 从 E820 图建位图,标记内核镜像/栈已用 |
| `alloc_page() → phys` | 单页,返回物理地址,`0` = OOM |
| `alloc_pages(count) → base` | count 个**连续**页(`__builtin_ctzll` 加速扫描) |
| `free_page(phys)` / `free_pages(phys,count)` | 释放(0 或已空闲则 no-op) |
| `free_page_count()` / `total_page_count()` | 空闲 / 总页数 |
| `alloc_page_locked()` / `free_page_locked()` | **不上锁**版本——仅在关中断、无并发 PMM 访问时用(如 page fault handler 在中断门里) |

`parse_memory_map(BootInfo, regions, max)`:过滤 E820 type-1(usable)、去掉 1 MB 以下、4KB 对齐。位图本身放在 `__kernel_stack_top` 之后的虚拟页。

## 虚拟内存管理 VMM

`cinux::mm::VMM`(4 级页表实例对象——为多地址空间设计,`g_vmm` 全局,内含 `Spinlock`):

| 接口 | 说明 |
|---|---|
| `init()` | 读 CR3 存为内核 PML4 |
| `map(virt, phys, flags, pml4=null)` | 4KB 页,缺中间表时向 PMM 要页+清零+接线;返回 bool(PMM 失败则 false) |
| `map_2mb(virt, phys, flags, pml4=null)` | 2MB 大页(virt/phys 必须 2MB 对齐) |
| `unmap(virt, pml4=null)` | 清 PT 项 + `invlpg`;**不释放物理页**(调用方还 PMM) |
| `split_2mb_page(virt) → bool` | 把一个 2MB 大页拆成 512×4KB(保留 flag),拆完才能逐页 unmap |
| `translate(virt, pml4=null) → phys` | 走页表查物理地址,未映射返回 0 |
| `map_nolock(...)` | 不上锁版,关中断上下文用(如 PF handler) |
| `kernel_pml4()` | 内核 PML4 物理地址 |

所有 `pml4` 参数省略时走内核 PML4;`AddressSpace` 把自己的 PML4 传进去做用户态映射。

## 页表项与 flag

`PageEntry`(8 字节 union,`paging.hpp`)位域:`present, writable, user, pwt, pcd, accessed, dirty, huge, global, _avail[3], addr[40], _avail2[11], nx`。`ADDR_MASK = 0x000FFFFFFFFFF000`(物理页基址 52 位)。flag(`paging_config.hpp`):

| flag | 位 | 值 | 含义 |
|---|---|---|---|
| `FLAG_PRESENT` | 0 | `0x001` | 存在位 |
| `FLAG_WRITABLE` | 1 | `0x002` | 可写 |
| `FLAG_USER` | 2 | `0x004` | 用户态可访问(DPL3) |
| `FLAG_PWT` | 3 | `0x008` | write-through 缓存策略 |
| `FLAG_PCD` | 4 | `0x010` | 禁缓存(MMIO 必备) |
| `FLAG_ACCESSED` | 5 | `0x020` | CPU 置(读/写命中) |
| `FLAG_DIRTY` | 6 | `0x040` | CPU 置(写命中) |
| `FLAG_HUGE` | 7 | `0x080` | 大页(PD 级 = 2MB) |
| `FLAG_GLOBAL` | 8 | `0x100` | 全局页(CR3 切换不刷 TLB) |
| `FLAG_COW` | 9 | `0x200` | **可用位 9:Cinux 复用为 CoW 标记** |
| `FLAG_NX` | 63 | `1<<63` | 不可执行 |

索引提取:`PML4_INDEX = (v>>39)&0x1FF`、`PDPT_INDEX=(v>>30)`、`PD_INDEX=(v>>21)`、`PT_INDEX=(v>>12)`。`PAGE_SIZE=4096`、每表 `PT_ENTRIES=512`。`is_user_vaddr(v)` 判 bit47==0(用户空间 `0..0x00007FFF_FFFFFFFF`)。

TLB:`flush_tlb(virt)` 单页 `invlpg`;`flush_tlb_all()` 重写 CR3;`read_cr3()/write_cr3()`。`map_mmio(phys,size)` 是帧缓冲驱动用的旧接口。

> **`FLAG_COW`(bit9)存在 ≠ CoW 完整。** 035 把 `handle_cow_fault` 接进了 `#PF`(present+write+user 路径),但 CoW 的引用计数仍有限;`split_2mb_page` 虽定义、guard-page 的 unmap 消费者**在 035 未接线**(死代码)。转述这两个机制前先 `git show <tag>:kernel/mm/...` 核对。

## 内核堆 Heap

`cinux::mm::Heap`(free-list,`g_heap` 全局;C++ `new`/`delete` 重定向到它):

- **块头 32 字节**(`BlockHeader`):`magic`、`size`(payload,不含头)、`free`、`_pad[12]`、`next`。`magic` 在 `free()` 时校验,防双重释放/越界。
- **首次适配 + 分裂**:`alloc(size, align=16)` 顺 free list 找第一个够大的块,太大就分裂。
- **合并**:`free()` 把块标空闲,并与相邻空闲块合并降碎片。
- **自动扩容 + 硬上限**:`expand(min_bytes) → bool`——free list 不够时经 VMM 映射新页。**029 起扩容前查 `size_ + 增量 <= max_size_`(`=KMEM_HEAP_SIZE=128MB`),失败返 `false`、不递归硬冲**。这是 canvas 3MB 事件后补的洞一。
- 并发:`Spinlock lock_` 保护;`alloc_locked`/内部路径在持锁态跑。

## 地址空间 AddressSpace

`cinux::mm::AddressSpace`(每进程一个,move-only,拷贝 delete):

- **构造**:向 PMM 要一页做 PML4、清零,再把内核半 `PML4[256..511]` 从内核 PML4 拷过来(每个地址空间都能看到内核映射);用户半 `[0..255]` 私有。
- **析构**:递归释放用户半整棵子树(PDPT→PD→PT)回 PMM,最后释放 PML4 页。
- `map/unmap/translate`:委托 VMM,以本空间 PML4 为根。
- `activate()`:把 `pml4_phys_` 写进 CR3(隐式刷 TLB)——进程切换的核心动作。
- `init_kernel()`:启动时读 CR3 存为内核 PML4(必须在建任何 AddressSpace 之前调一次)。

## 约束与边界(本子系统的真实限制)

- **`phys_to_virt(p)=p+KERNEL_VMA` 成立的前提:direct map 覆盖了 PMM 可能返回的全部物理地址。** loader 只映射内核镜像不够;PMM 管多少 RAM,direct map 就得映射多少(029 改成全量映射,用 2MB/1GB 大页低开销盖住)。否则 `alloc_page` 返回高地址页,`phys_to_virt` 给个没人映射的虚拟地址,一访问就 PF。
- **堆有 128MB 硬上限。** 超过 `max_size_` 的 `expand` 返 false、`alloc` 返 `nullptr`。预留 128MB 虚拟不等于吃 128MB 物理(按需分页)。
- **`unmap` 不还物理页。** 谁映射谁还;漏还 = 内存泄漏。
- **`alloc_page_locked`/`map_nolock` 不上锁**,只能在关中断、无并发时用(典型:中断门里的 PF handler)。普通路径必须走带锁版本,否则与别的核/别的中断踩踏位图与页表。
- **CoW / guard page 在 035 是半成品**(见上)。引用「写时复制已工作」「guard page 会触发」前先核对源码。
- **单核假设。** PMM/VMM/Heap 的 Spinlock 在单核 + 中断串行下够用;真上 SMP 要重新审页表自旋与 per-CPU 缓存。

## 验证入口

- host 单测:`ctest --test-dir build -R "pmm|vmm|heap" --output-on-failure`(`test/unit/test_pmm.cpp`、`test_vmm.cpp`、`test_heap.cpp`)。
- QEMU 机内测:`cmake --build build --target run-big-kernel-test`(`kernel/test/` 下 mm 相关套,走真页表/真 PF)。
- 布局自检:`scripts/check_memory_layout.py`。

## 源码索引

- 布局:[memory_layout.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/memory_layout.hpp) + [check_memory_layout.py](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/check_memory_layout.py)。
- PMM:[pmm.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/pmm.hpp) / [pmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/pmm.cpp)。
- VMM:[vmm.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/vmm.hpp) / [vmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/vmm.cpp)。
- 分页原语:[paging.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/paging.hpp) / [paging_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/paging_config.hpp) / [paging.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/paging.cpp)。
- 堆:[heap.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/heap.hpp) / [heap.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/heap.cpp)。
- 地址空间:[address_space.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/address_space.hpp) / [address_space.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/address_space.cpp)。
- direct map 全量映射:loader(`bigKernelLoader`/`boot`)phase2 扫 E820。

## 权威依据

- Intel SDM Vol 3,Ch 4(4 级分页、PML4/PDPT/PD/PT、页表项位编码、大页、PAT/PCD/PWT 缓存属性、CR3):页表结构与 flag 位定义的唯一来源。
- Intel SDM Vol 3,Ch 6(`#PF` 错误码、present/write/user 位):CoW / demand paging 的判定依据。
- OSDev — [Page Table](https://wiki.osdev.org/Page_Table)、[Paging](https://wiki.osdev.org/Paging)、[Detecting Memory (x86)](https://wiki.osdev.org/Detecting_Memory_(x86))(E820)。
- Linux direct map / `phys_to_virt`(`phys + PAGE_OFFSET`):全量映射物理内存的参照模型。<https://www.kernel.org/doc/html/latest/core-api/mm-api.html>
- xv6 MIT 6.S081:简单 free-list 堆 + 三级页表(RISC-V)的设计对比。
