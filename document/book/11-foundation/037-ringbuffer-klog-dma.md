---
title: 037 · RingBuffer / 内核日志 / DMA 池
---

# 037 · 别再重复造轮子:统一环形缓冲、给内核一段记忆、收编散装 DMA

> 上一章把公共类型库 `Cinux-Base` 接了进来。这一章就拿它干三件越来越碍事的事——而且三件事其实是同一个毛病:**同一种东西,内核里各写一份,还口径不一**。
>
> 环形缓冲,`pipe` 写一套、键盘驱动写一套,连"满了怎么判定"都不一样;内核日志,只有实时往屏幕吐的 `kprintf`,启动一过、屏幕一滚,之前发生了什么全没了;DMA,每个需要它的驱动自己 `alloc_pages` + 硬编码偏移 + 手动映射,出错没人管。这一章把它们分别收进统一的实现:环形缓冲归一、日志补上历史、DMA 做成设备无关的池。三件事串起来,就是"**别重复造轮子,有公共的就消费公共的**"。

## 环形缓冲:两套手写,先合成一套

先看环形缓冲。在 036 之前,`kernel/` 里有两套手写的环形缓冲,而且**策略还不一样**:

| 在哪 | 容量 | 怎么判满 |
|---|---|---|
| `pipe` | 4096 | 用一个 `count_` 记元素数(不牺牲槽位) |
| 键盘驱动 | 64(实占 63) | 牺牲一个槽位(`head_==tail_` 是空、`next==head` 是满) |

两套都能跑,但维护两份逻辑、还两种约定,谁改谁头疼。而 `Cinux-Base` 里正好有个 `cinux::lib::RingBuffer<T,N>`——它用的就是 `count_` 策略,和 `pipe` 那套一模一样。所以收编很干净:两处都换成它,只把**存储层**交出去,外层语义原样保留。

```cpp
// kernel/ipc/pipe.hpp
cinux::lib::RingBuffer<char, PIPE_BUFFER_SIZE> buf_;       // pipe:两段回绕拷贝 → push_batch/pop_batch
// kernel/drivers/keyboard/keyboard.hpp
static cinux::lib::RingBuffer<KeyEvent, KEY_QUEUE_SIZE> buf_;  // 键盘:牺牲槽位 → push/pop
```

两处的改动要点,都遵循"换存储、不动外层":

- **pipe**:原来那一整套 `buffer_/head_/tail_/count_` 加两段回绕拷贝,换成 `RingBuffer` 的 `push_batch/pop_batch`。外层的锁、阻塞、EOF 语义**一律不动**。光这一处就少了一百多行(就是那段回绕拷贝的代码量)。
- **键盘**:`queue_/head_/tail_`(牺牲槽位)换成 `push/pop`。原来"满了丢最新的",正好用 `push` 返 `false` 实现;中断保护保留。公共接口签名不变,所以 `sys_read`、GUI 这些调用方**一个字都不用改**。

有个小细节值得留意:键盘队列**容量从 63 变成了 64**。原来牺牲一个槽位,实际只装 63;换成 `count_` 策略不牺牲,容量就是字面的 64。更贴合 `KEY_QUEUE_SIZE` 这个名字,而且没有测试依赖精确容量,所以这个变化是安全的。

> 这里有个**故意不做**的事:没有把 `RingBuffer` 包一层"多生产者单消费者(MPSC)"的通用封装塞进 `Cinux-Base`。因为 pipe 和键盘都是单生产者单消费者(SPSC),各有各的定制同步(pipe 自带锁和 EOF,键盘是中断单生产者 + 轮询单消费者),通用封装对它们没价值。**真正需要 MPSC 的是下面的内核日志**(多个 CPU、中断上下文都要往里写),所以那个封装推迟到那里,而且因为要依赖内核的锁,它待在 `kernel/lib/` 而不是 freestanding 的 `Cinux-Base`。这印证了一条:**抽象的时机是"第二个真实用户出现",不是"看起来能抽象"。**

## 内核日志:给 kprintf 配一段记忆

第二件事。036 之前内核只有 `kprintf`——它逐字符往串口/屏幕吐,是**实时**的:你站在屏幕前能看见,但启动一过、画面一滚,之前发生过什么就没了。想排查一个 late-init 的问题,或者让用户程序知道内核启动经历了什么,都抓瞎。

这一章给它补上**历史**。思路是把 `Cinux-Base` 里现成的几样叶子能力(`LogLevel`、`Logger`、`RingBuffer`)组合成内核的持久化日志:

```
kprintf / klog_*  →  KernelLog 环形历史(中断安全,带级别 + 时间戳)
                              ↓
                        sys_dmesg  →  用户态读 "[LEVEL] tick: msg"
```

分层很清楚——`Cinux-Base` 出叶子类型,`kernel/` 出组合:

| 层 | 从哪来 | 干什么 |
|---|---|---|
| `LogLevel` / `Logger` / `RingBuffer` | `Cinux-Base`(复用) | 类型现成,不重写 |
| `ConcurrentRingBuffer<T,N>` | `kernel/lib/`(新) | `RingBuffer` + 锁,MPSC、中断安全 |
| `KernelLog` | `kernel/lib/`(新) | 日志条目环形 + `klog_*` 宏 + kprintf 攒行桥 |
| `sys_dmesg` | `kernel/syscall/`(新) | 把历史格式化读给用户态 |

那个 `ConcurrentRingBuffer` 就是上一节说"推迟过来"的 MPSC 封装:每个操作都过一遍锁(禁中断 + 获取)。这一步让日志能在**中断、甚至 panic 上下文**里安全写——这是它必须待在 `kernel/lib/`(而不是 `Cinux-Base`)的根本原因:`Cinux-Base` 是 freestanding,没有锁的概念。

`KernelLog` 里每条是 `{ 时间戳, 级别, 256 字节消息 }`,存一个固定大小的环形。还有个很实用的桥:`kprintf` 注册一个 sink,那些还没迁移的裸 `kprintf` 调用,逐字符攒成一行,遇到换行就刷一条进历史——老代码不用动,日志也自动进账。

用户态这边,`sys_dmesg` 把历史一条条格式化成 `[LEVEL] tick: message` 写进用户缓冲。用户程序终于能"问内核:你启动到现在都经历了什么"。

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
