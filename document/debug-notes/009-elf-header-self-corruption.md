---
title: 调试档案 009 · 加载器把自己读的 ELF 头覆盖了
tag: 009_large_kernel_entry
---

# 调试档案 009 · 加载器把自己读的 ELF 头覆盖了

> 从 `notes/009/009-01-elf-loader-header-corruption.md` 提炼,配套 [009 · 大内核登场](../book/03-big-kernel/009-large-kernel-entry.md)。这是 009 最经典的一个 bug:加载器一边读 ELF 头、一边把段拷到目的地址,结果目的地址就是 ELF 头所在的地方——自己把自己的"施工图纸"涂掉了。

## 案例:load_elf 拷段时三重故障,且无任何异常输出

- **症状**:`test_big_kernel_load` 打印完 `PT_LOAD[0]` 的信息(vaddr/paddr/filesz)后突然中断,"Loaded segment" 那行永远不出现,QEMU 直接退出,连 `#PF`/`#GP` 都没有。崩溃精确卡在"打印段信息"和"memcpy 拷段"之间。
- **原因**:big kernel 的 staging 缓冲区(`BIG_KERNEL_LOAD_ADDR = 0x1000000`)和它的 `PT_LOAD[0].p_paddr`(`0x1000000`)是**同一个物理地址**。`load_elf` 先从 staging 读 ELF 头和 program header(位于镜像头部)来决定怎么加载,然后 `memcpy(dest = p_paddr = 0x1000000, src = staging + p_offset, filesz)` 拷段。但 `dest` 就是 staging 的起点——这一拷,段数据直接盖到了 ELF 头和 program header 上。循环进入下一段、再去读 program header 时,读到的已经是段数据的垃圾,要么指针飞、要么访问越界,当场崩。本质上这是"load-in-place"布局下 `memcpy` 的源区与目的区重叠(`memcpy` 对重叠区是未定义行为,且这里重叠的恰恰是还在被读取的元数据)。
- **定位**:对照两个地址——staging(`BIG_KERNEL_LOAD_ADDR`)和 ELF 里 `PT_LOAD[0].p_paddr`——发现它们相等,立刻锁定。GDB 在 `load_elf` 的 memcpy 前后看 staging 头部 ELF magic(`7F 45 4C 46`),memcpy 后变成段数据,就是被覆盖的铁证。
- **修复**:不让加载器的源(staging)和目的(p_paddr)在拷贝时发生"边读边覆盖元数据"的重叠。Cinux 的做法是在 `load_elf` 动数据前先把整组 program header 快照到一个本地数组(`saved_phdrs`),后续循环只读快照、不再碰 staging 里会被覆盖的那份;段拷贝改用 `memmove`(重叠区安全)而不是 `memcpy`。注意 009 并没有把 staging 和 p_paddr 分到不同地址——它们就是同一个(load-in-place),靠"快照 + memmove"化解重叠。加载器还在 phase2 里跑一次运行时 `check_memory_overlaps`(登记页表/mini kernel/PT_LOAD 目标区两两查重叠,但故意不登记 staging),加上构建期辅助的 `scripts/check_memory_layout.py`,把"段落到 mini kernel/页表上"这种致命重叠挡在加载前。
- **防复发**:凡是"从一个缓冲区解析头部、又把数据写回该缓冲区(或与之重叠的区域)"的加载器,都要先快照头部、再动数据;并用一个布局校验脚本把"staging 与目标地址冲突"这类自毁配置挡在构建期。

---

### 一句话总结

加载器最阴的 bug 不是"读错了",而是"边读边把自己正在读的东西覆盖了"。当 staging 地址和目标物理地址重合,memcpy 就会涂掉 ELF 头——记住"先快照元数据、再搬数据",外加一道构建期布局校验,这种自毁就不会再发生。
