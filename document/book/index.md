---
title: 主书 · Cinux 64 位 OS 教程
---

# 主书 · Cinux 64 位 OS 教程

> 主线阅读教程。每章绑定一个 git tag、从源码提炼,从 Real Mode bootloader 一路到多核、网络、用户态生态。讲清「为什么现在需要它 + 本阶段新增的核心设计 + 关键代码 + 至少一个真实踩坑 + 验证命令」,保留 Cinux 的实践感与折腾感。

## 十七卷

每卷自成体系,卷内章节从 001 起连续编号。定位一章用「卷名 / 卷内号」,例如 `09-gui/003` 是 GUI 卷第 3 章(原生终端应用)。每章是一个目录,内含 `index.md` 路由页 + 若干节文章。

- **[01 启动](01-boot/)** — Real Mode → 保护模式 → Long Mode → 装载 mini kernel(4 章)
- **[02 mini kernel](02-mini-kernel/)** — 入口、PMM、中断、装载大内核(4 章)
- **[03 big kernel](03-big-kernel/)** — GDT/IDT、PIC/PIT、kprintf/SSE、VGA 帧缓冲、键盘(8 章)
- **[04 开发者 / 可观测性](04-developer/)** — kallsyms、验证基建、HPET/RTC(3 章)
- **[05 内存](05-memory/)** — PMM、VMM、内核堆、地址空间(4 章)
- **[06 进程](06-process/)** — 上下文切换、抢占式调度、同步原语(3 章)
- **[07 用户态](07-userland/)** — ring3、syscall、shell、musl、TTY、PTY、动态链接、busybox、ABI(10 章)
- **[08 文件系统](08-filesystem/)** — AHCI、ramdisk、VFS、ext2 读写、DevFS、ProcFS、tmpfs、mount(18 章)
- **[09 GUI](09-gui/)** — 画布、窗口管理器、原生终端、管道、图标、桌面、解耦、xHCI、设备接口(12 章)
- **[10 多任务](10-multitasking/)** — fork/exec、通电 fork、多终端(3 章)
- **[11 基础设施](11-foundation/)** — Cinux-Base、RingBuffer、IBlockDevice、lockdep(4 章)
- **[12 存储](12-storage/)** — AHCI DMA、NVMe/VirtIO(2 章)
- **[13 内存增强](13-memory-advanced/)** — VMA/mmap、brk/PageCache、demand paging、buddy/slab、双计数(5 章)
- **[14 进程增强](14-process-advanced/)** — 信号、clone/futex、进程组、调度类、pipe、shm、poll、timer(8 章)
- **[15 SMP](15-smp/)** — ACPI/APIC、per-CPU、trampoline、多核调度、加固、迁移、竞态(6 章)
- **[16 安全](16-security/)** — NX/SMEP/SMAP、ASLR、凭证、SMAP+SMP(4 章)
- **[17 网络](17-net/)** — e1000、IPv4/ICMP、UDP、TCP、AF_UNIX(5 章)

配套动手实验见 [实验册](/labs/);跨章节子系统速查见 [参考](/reference/);真实排错故事见 [调试笔记](/debug-notes/)。
