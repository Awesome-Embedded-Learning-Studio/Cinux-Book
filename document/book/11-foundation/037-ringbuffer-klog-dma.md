---
title: 037 · RingBuffer / 内核日志 / DMA 池
---

# 037 · 把三块基建也搬上 Cinux-Base:RingBuffer、dmesg、DMA 池

> 036 把 Cinux-Base 接了进来、把错误换成了 `ErrorOr`。这一章接着收口 **F1 基建剩下那三块**——它们都是同一种活:**Cinux-Base 的类型已经就绪,咱们不在 `kernel/` 里重造轮子,只做消费迁移 + 在 `kernel/` 加薄薄一层增量**。
>
> 三块分别是:散落成两套、还互相不一致的手写环形缓冲(`RingBuffer` 统一);从来没有历史的内核日志(`dmesg`);以及每个驱动各自为政、写到哪算哪的 DMA(`DmaPool` 收口)。这一章也是 B 档为主——除了 `dmesg` 用户态读得到,另外两块是内部重构,验证靠构建 + 测试 + 看指标。

## 这章咱们要点亮什么

三件事,一根线(都是"消费 Cinux-Base,在 kernel/ 加薄增量")。

1. **RingBuffer 统一**:`pipe` 和键盘驱动各写了一套手写环形缓冲,策略还不一样;现在两处都换成 Cinux-Base 的 `cinux::lib::RingBuffer<T,N>`,光 pipe 就净减约 109 行。
2. **内核日志 dmesg**:以前只有实时吐字的 `kprintf`,启动过后啥都回看不了;现在有了 `KernelLog` 环形历史 + `sys_dmesg`(编号 103)读出来。
3. **DMA 池**:把"`g_pmm.alloc_pages` + 硬编码 `+0xFFFFFFFF80000000` 偏移 + 手动 `g_vmm.map`"这套散装 DMA,收进设备无关的 `DmaBuffer` / `DmaPool` / `PrdtBuilder`。

## RingBuffer:干掉两套手写

036 之前,`kernel/` 里有两套手写环形缓冲,而且策略还不一致:

| 位置 | 容量 | 满空判定 |
|---|---|---|
| `kernel/ipc/pipe.cpp` | 4096 | 用 `count_` 区分满空(不牺牲槽位) |
| `kernel/drivers/keyboard/keyboard.cpp` | 64(实占 63) | 牺牲一个槽位(`head_==tail_` 空、`next==head_` 满) |

而 Cinux-Base 的 `cinux::lib::RingBuffer<T,N>` 用的正是 `count_` 策略——和 pipe 那套**完全一致**。所以这一弧的活很干净:把两处手写都换成它,存储层让出去,外层语义原样保留。

直接看真实签名:

```cpp
// kernel/ipc/pipe.hpp:168
cinux::lib::RingBuffer<char, PIPE_BUFFER_SIZE> buf_;

// kernel/drivers/keyboard/keyboard.hpp:112
static cinux::lib::RingBuffer<KeyEvent, KEY_QUEUE_SIZE> buf_;
```

两处的改动要点:

- **pipe**:手写的 `buffer_/head_/tail_/count_` 那一整套两段回绕拷贝,换成 `RingBuffer` 的 `push_batch/pop_batch`。外层的 `Spinlock` + `irq_save/restore`、阻塞 spin-wait、reader/writer close 的 EOF 语义**一律不动**——RingBuffer 只接管存储。净减约 109 行(就是那段两段回绕拷贝的代码量)。
- **keyboard**:`queue_[64]/head_/tail_`(牺牲槽位)换成 `RingBuffer<KeyEvent,64>::push/pop`。原代码满时 drop-newest,正好由 `push` 返 `false` 实现;`InterruptGuard` 同步保留。公共接口(`init/irq1_handler/poll`)签名不变 → `sys_read`、GUI、main 这些调用方**零波及**。

一个值得一提的小细节:**键盘队列容量从 63 变成了 64**。原来牺牲一个槽位,实际只能装 63;RingBuffer 用 `count_` 不牺牲,容量就是字面的 64,更贴合 `KEY_QUEUE_SIZE` 这个名字。测试没依赖精确容量(没有 full-buffer 测试),所以这个变化是安全的。

> 这里笔者特意**没有**做一个看起来更"通用"的事:把 RingBuffer 包一层 MPSC(多生产者单消费者)的 `ConcurrentRingBuffer` 放进 Cinux-Base。因为 pipe 和键盘都是 SPSC,各有各的定制同步(pipe 自带锁 + EOF,键盘是 IRQ 单生产者 + poll 单消费者),通用封装对它们没价值。MPSC 封装真正有用的地方是下面的 `dmesg`(多 CPU、IRQ 上下文都要往里写),所以它被推迟到 F1-M2,而且放在 `kernel/lib/`(因为要依赖内核的 `Spinlock`,Cinux-Base 是 freestanding、没有锁的概念)。**不是所有"看起来能抽象的"都值得现在抽象——等第二个真实用户出现再抽,成本才划算。**

## 内核日志 dmesg:给 kprintf 配一段记忆

036 之前,内核只有 `kprintf`:它逐字符往 serial/framebuffer 吐,是**实时**的——你站在串口边能看见,但启动一过、屏幕一滚,之前发生了什么就没了。调试 late-init 的问题、或者用户态想知道内核启动历程,都抓瞎。

这一弧给它配上**历史**。链路长这样:

```
kprintf / klog_*  →  KernelLog ring(IRQ 安全,带 level + 时间戳)
                              ↓
                        sys_dmesg(103)  →  用户态读 "[LEVEL] tick: msg"
```

分层很清楚,Cinux-Base 出叶子能力、`kernel/` 出组合:

| 层 | 来源 | 内容 |
|---|---|---|
| `LogLevel` / `Logger` / `RingBuffer` | Cinux-Base(复用) | 类型就绪,不重写 |
| `ConcurrentRingBuffer<T,N>` | `kernel/lib/`(新) | RingBuffer + `Spinlock::irq_guard`,MPSC、IRQ 安全 |
| `KernelLog` | `kernel/lib/`(新) | `LogEntry` ring + `klog_*` 宏 + kprintf 攒行 sink |
| `sys_dmesg` | `kernel/syscall/`(新) | `SYS_dmesg=103`,格式化历史读取 |

`ConcurrentRingBuffer`(`kernel/lib/concurrent_ring_buffer.hpp:40`)是 F1-M1 故意推迟过来的那个 MPSC 封装:每个操作(`push/pop/push_batch/...`)都过一遍 `Spinlock::irq_guard()`——禁中断 + 拿锁。这一步让日志 sink 能在 **IRQ 甚至 panic 上下文**里安全调用,这是它必须待在 `kernel/lib/`(而不是 Cinux-Base)的根本原因:Cinux-Base 是 freestanding,没有锁的概念。

`KernelLog` 里每条是 `LogEntry { timestamp; LogLevel; char message[256]; }`,存在 `ConcurrentRingBuffer<LogEntry, 128>` 里(~33 KiB)。有个**很实用的桥接**:`kprintf_register_sink(klog_kprintf_sink)`——那些还没迁移的裸 `kprintf` 调用,逐字符攒成一行,遇到 `\n` 刷一条 `INFO` 进 ring。这样老代码不用动,日志也自动进历史。

用户态这边,`sys_dmesg(buf, len)`(`kernel/syscall/sys_dmesg.hpp:28`)drain 历史,格式化成 `[LEVEL] tick: message\n` 写进用户 buf。编号 103 对齐 Linux 的 `SYS_syslog`,带 canonical 地址检查(同 `sys_read`,`-EFAULT`)。

## DMA 池:设备无关的 DMA 基建

最后一块,也是最值得讲踩坑的一块。036 之前,谁要 DMA(比如 AHCI)就自己来一套:

```cpp
// 散装 DMA,每个驱动各写一遍
auto phys = g_pmm.alloc_pages(...);
auto virt = phys + 0xFFFFFFFF80000000ULL;   // 硬编码 direct-map 偏移
g_vmm.map(virt, phys, ...);
```

这套东西散在各个驱动里,口径不一、出错没人管。这一弧把它收进 `kernel/drivers/dma/`(`cinux::drivers::dma`),三个类型各司其职:

| 类型 | 文件 | 职责 |
|---|---|---|
| `DmaBuffer` | `dma_buffer.hpp:42` | move-only 句柄:phys/virt 配对 + size,RAII 析构经回调归还 |
| `DmaPool` | `dma_pool.hpp:46` | `alloc(size)→ErrorOr<DmaBuffer>`,封装 PMM + VMM,复用 direct-map |
| `PrdtBuilder<MaxSegments>` | `prdt_builder.hpp:45` | 设备无关 scatter-gather 段构建器 |

两个设计点值得记:

**复用 direct-map,不另开 virt 分配器。** `DmaPool` 直接用 `virt = phys + KERNEL_VMA`(`dma_pool.hpp:7` 的注释明写)——物理地址唯一决定虚拟地址,省掉了独立的 virt 分配器,也没有泄漏。`VMM::map` 负责覆盖/建立 PTE。

**RAII 归还用回调解耦。** `DmaBuffer` 析构时要归还内存,但归还逻辑在 `DmaPool` 里;为了不让 `dma_buffer.hpp`(header-only)前向依赖 `DmaPool`,归还逻辑做成函数指针回调(`DmaReleaseFn`)。这样头文件无循环依赖。

### 踩坑:direct-map 的 PTE 绝不能 unmap

这是这一弧最大的一个雷(CinuxOS 的 PLAN 里记成 GOTCHA #7)。`DmaPool` 第一版的 `free()` 想"干净",除了回收物理页,还顺手 `vmm.unmap(virt)`——结果把 direct-map 的**永久映射槽**给拆了。后面 demand paging 再用到那块虚拟地址,反复映射到错的物理页,**QEMU 直接卡死在死循环里**。

修复很反直觉:`free()` **只** `free_pages(phys)`,**绝不 unmap**。direct-map 的 PTE 是永久的,phys 回收了、virt 那条映射留着无害(下次 alloc 同样的 phys 还会落到同样的 virt)。

> 教训:direct-map 是"物理 ↔ 虚拟"的永久对照表,它的 PTE 不是普通动态映射,**不能按 RAII 的直觉去 unmap**。AHCI 驱动那边同款——map 完不 unmap。笔者在这一坑上浪费过时间,所以单独拎出来记。

## 验证

三块各自的验证抓手:

```bash
# RingBuffer:两处都换了 Cinux-Base 的
grep -rn 'cinux::lib::RingBuffer' kernel/ipc/ kernel/drivers/keyboard/
# 期望:pipe.hpp:168、keyboard.hpp:112 各一处

# dmesg:syscall 103 在、KernelLog 在
grep -rn 'sys_dmesg\|class KernelLog\|ConcurrentRingBuffer' kernel/syscall/sys_dmesg.hpp kernel/lib/
# 期望:sys_dmesg.hpp:28 的签名、concurrent_ring_buffer.hpp:40 的类

# DMA:三个类型都在
ls kernel/drivers/dma/
# 期望:dma_buffer.hpp、dma_pool.hpp/.cpp、prdt_builder.hpp
```

构建 + 跑内核测试(这一弧 run-kernel-test 从 662 涨到 694,DMA 那块加了 20 条):

```bash
cmake --build build -j$(nproc) && cmake --build build --target run-kernel-test
```

想亲眼看 dmesg 的话(这是这一弧唯一用户态摸得到的东西),在用户程序里调 `syscall(103, buf, len)`——注意 Book 这条教学线还没把用户态铺到能随手写 C 程序跑(那要等 F10 musl 弧),所以现在更现实的是看内核测试里 `sys_dmesg` 的输出断言,或者启动日志里 `[INFO]`/`[WARN]` 带 tick 的行。

## 小结与下一站

这一章把 F1 基建收口了:

- **RingBuffer**——pipe/keyboard 两套手写归一,净减一百多行;
- **dmesg**——内核终于有了日志历史,`sys_dmesg` 把它交到用户态;
- **DMA 池**——散装 DMA 收进 `DmaBuffer/DmaPool/PrdtBuilder`,设备无关,direct-map 的 unmap 坑也踩平了。

加上 036 的 `ErrorOr`,F1 这层地基算是夯实了:类型库、错误处理、容器、日志、DMA 都有了公共的家。下一站 **038** 立最后一块基建——块设备抽象 `IBlockDevice`,让 ext2 不再直挂在 AHCI 上。那是文件系统升级(F6)和后续驱动(VirtIO/NVMe)的共同前置。
