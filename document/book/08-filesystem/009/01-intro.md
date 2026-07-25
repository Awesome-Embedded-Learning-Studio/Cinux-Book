---
title: 01 · 导引:启动逻辑不在任何可调度的线程上
---

# 导引:启动逻辑不在任何可调度的线程上

> 但这次重构不是「照着模型改完就收工」。改完一跑,ext2 挂载直接失败。于是这一章大半篇幅是一次硬核排错:顺着一个「设备读着读着消失了」的现象,一层层剥到一个潜伏了好几个 tag 的虚拟地址碰撞。换句话说,这一章给你两样东西——一个线程化的启动模型,和一次「重排执行顺序如何激活潜伏 bug」的真实复盘。

## 为什么现在需要它:启动逻辑不在任何可调度的线程上

先看清 008 的启动路径长什么样。`kernel_main` 里大致是这个顺序:

```text
... GDT/IDT/PIC/PIT/PMM/VMM/Heap/帧缓冲/键盘 ...   ← 一堆初始化
AHCI init + 读扇区 0
ext2.mount()                  ← 挂载
vfs_mount_init() + mount "/"
run_concurrent_stress()       ← 进调度器、起 stress 线程、最后 launch_first_user
键盘轮询 while(1){ hlt; poll }   ← 兜底循环
```

问题在于:`ext2.mount()`、`vfs_mount_add`、`launch_first_user` 这些都发生在**调度器启动之前**,或者发生在 `run_concurrent_stress` 内部那个临时搭的 stress 框架里。它们都不属于任何「正经的可调度线程」。后果是:

- 启动中途想做 `Scheduler::yield()` / `block()`?不行,你不在运行队列里,调度器不认识你。
- 008 的 `Mutex`/`Semaphore` 想用在启动路径(比如等磁盘)?没处下嘴。
- 那个临时的 `run_concurrent_stress` 框架,本来是 008 用来验证并发安全的脚手架,现在却**兼着**「启动到 shell」的职责——两件事纠缠在一个函数里,不干净。

009 要做的就是解耦:调度器在 `kernel_main` 里直接初始化,然后 spawn 一个 `kernel_init` 线程,让**它**去挂文件系统、起 shell。这样一来,启动逻辑就跟普通内核线程一样,能被调度、能阻塞、能让出——008 的阻塞原语终于有了用武之地。
