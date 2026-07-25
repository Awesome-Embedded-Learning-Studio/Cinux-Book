---
title: 02 · 质量加固:饱和引用计数、用户指针、并发债、CoW、门禁
---

# 质量加固:饱和引用计数、用户指针、并发债、CoW、门禁

## 饱和引用计数:堵住 wrap 出来的 use-after-free

共享对象(共享地址空间、共享 cwd、共享信号表)靠引用计数管生命周期:有人用就 `acquire()`(计数 +1),不用了 `release()`(计数 -1),减到 0 最后一个人释放。听起来用一个 `std::atomic<int>` 就行,问题恰恰在这。

**普通原子计数器会 wrap**。假设有个 bug 让 `release()` 多调了一次,计数从 0 减到 -1,再减就 wrap 成一个很大的正数(无符号)或继续往负数走(有符号)。于是一个**本该被释放的对象,计数看起来还很大**,后续 `acquire()` 觉得它还活着——use-after-free;或者 wrap 回 0 触发**重复释放**。plain atomic 的计数一旦越过 0,就再也救不回来。

解法是**饱和**(saturation):计数到一个特殊的饱和值就**不再动**(`RefCount` in `third_party/Cinux-Base/include/cinux/refcount.hpp`)。`release()` 减到 0 就停在那里(标记"已释放,别再碰"),`acquire()` 加到饱和值也停。这样越界不会 wrap 成假活,只会"卡在已死状态",bug 表现成卡死/明确报错,而不是隐蔽的 UAF。这正是 Linux 内核 `refcount_t` 的设计(`include/linux/refcount.h`)。

饱和值选 `INT_MIN/2`(不是 `INT_MIN`):让它离 0 和 `INT_MAX` 都大致等距,把"取值与饱和钳位之间那个短暂非原子窗口"里、并发 acquire/release 把计数漂移到危险区的概率压到最小。

一个实现细节值得记:`RefCount` 用 GCC 的 `__atomic_*` 内联函数,**不是 `std::atomic`**。因为 `std::atomic<uint32_t>` 会拖出 libstdc++ 的 `__glibcxx_assert_fail` 符号,在内核的 `-ffreestanding -nostdlib` 下链不过。`__atomic_*` 在 x86-64 上是 lock-free 内联(零外部符号),内核和 host 测试都能用。

## 用户指针标记:用类型给"不可信指针"挂牌

内核经常要处理用户空间传进来的指针(系统调用参数)。这些指针**不可信**:可能未映射、可能越界、可能是恶意地址。直接解引用就炸。Linux 的做法是用 sparse 静态检查器给这类指针标 `__user`,强制它们只能通过 `copy_to_user`/`copy_from_user` 访问。

`UserPtr<T>`(`kernel/lib/user_ptr.hpp`)就是这个标记的 Cinux 版:一个零开销的、单指针的契约包装,把"这个指针来自用户空间"编码进类型系统,**不改运行时行为**。像 `NotNull` 编码"永不为空"一样,`UserPtr` 编码"来自用户空间、必须校验、只能跨越用户/内核边界触碰"。

> 诚实划边界:`UserPtr` 在这一步是**类型先行的脚手架**——它本身**没有消费者**,引入它并不会立刻让用户指针更安全(直接解引用 `UserPtr` 的目标对象,和直接解引用裸指针一样危险)。运行时的规范地址校验仍在现有的 `validate_user_ptr()`。`UserPtr` 是为后续的 `access_ok` + `copy_to/from_user` 铺路的类型标记。别把它的存在误读成"用户指针问题已解决"。

## SMP 并发债清算:分配器和注册表加锁

两个核真跑起来后,第一批撞上的是那些"单核时天然串行"的全局结构。

**PID 分配器**。`PidAllocator` 维护一个空闲 PID 位图。单核时,分配 PID 是串行的(一个核一次调一个)。双核下,两个核可能**同时**进入分配,读到同一位、都以为它空闲、返回同一个 PID——两个不同进程拿到相同 PID,后续 waitpid/信号全乱。解法:给分配器加一个 **irq-safe 自旋锁**(`Spinlock` + `irq_guard`),分配/释放路径进临界区。irq-safe 是说取锁前先关本核中断(防中断处理程序里又取同一把锁死锁)。

**任务注册表**同理。全局的任务表(task registry)单核串行访问没事;双核下一个核注册新任务、另一个核遍历表,并发读写就崩。也上 irq-safe 自旋锁。

> 这和上一章的 `SharedCwd`/`SharedSigActions` 原子引用计数是同一类债的不同层次:那两个是"共享对象的引用计数并发",这俩是"全局数据结构的访问并发"。SMP 一开,所有"靠单核串行天然安全"的假设都得重新审一遍。

## CoW 页的引用计数:fork/exec 的写时复制防 UAF

fork 写时复制(CoW):父子进程先共享同一批物理页,谁写谁再复制。这就要求每个物理页知道"自己被几个地址空间共享"——**per-page 引用计数**。Batch3 PMM 拆分后这个计数被拆成两个维度(见 `kernel/mm/phys_ref.hpp` 顶部注释,镜像 Linux 的 `page->_refcount` + `_mapcount` 切分):

- **`pte_count`**(用户 PTE 映射数):共享时 `pte_count_inc`,复制或取消映射时 `pte_count_dec`/`pte_count_dec_and_test`。它**只记账、永不直接释放**(`pte_count_dec` 注释明说 "never frees")。
- **`refcount`**(所有权):真正决定"该不该真释放"。`refcount_dec_and_test` 减到 0 才把页还回 buddy(`kernel/mm/pmm.hpp:106-108`)。页缓存(`CachePhysRef`)或活 shmem 段持有的页,即使 `pte_count` 归零,`refcount` 仍 > 0,页不释放。

旧的统一 `mapcount_dec_and_test`(记账 + 释放合在一起)已被这个两段式取代;`pte_count_dec_and_test` 还保留这个名字只是为了让 7 个 teardown 调用点机械改写(它在 `pte_count` 归零时顺带 `refcount_dec_and_test`,把释放决策交给 refcount 维度)。

没这个计数会怎样?fork 后父子共享一页,子进程 exit 取消映射,内核以为没人用了就把物理页还回 PMM——可父进程还指着它,**use-after-free**。或者 exec 换页表时把共享页误释放。`pte_count` + `refcount` 分工让"该不该真释放"这个判断有据可依:两个计数都归零才是最后一个人。

`handle_cow_fault`(写时复制缺页处理)是收口点:旧共享页 `pte_count_dec`,新复制页 `pte_count` 初始化。共享地址空间(`AddressSpace`)本身也挂一个 `RefCount`,`clone(CLONE_VM)` 时 `acquire()`,线程退出 `release()`,最后一个释放整个地址空间。

## 退出路径:reap + deferred-free

任务退出的资源释放也有债。一个任务退出,它持有的内核栈、地址空间、共享的 sig/cwd/fd 都要释放。但释放时机讲究:

- **waitpid reap**:父进程 waitpid 回收僵尸子进程时,才释放子进程的 Task 结构(栈、addr_space、共享对象)。正常路径。
- **exit_current deferred-free**:异常路径(任务自己 exit 但没被 wait),不能在 exit 的当口立刻 free 自己(还在它自己的栈上跑),要延迟到调度器 reap 时再 free。

这两条路径合上,Task 的生命周期才没有泄漏或 UAF。

## 零警告门禁 + UBSan:把隐患变成硬错误

加固不只是修 bug,还有**让 bug 藏不住**。

**零警告门禁**(`kernel/CMakeLists.txt`):开一批检查(`-Woverloaded-virtual`/`-Wformat=2`/`-Wimplicit-fallthrough` 等),把零噪音的那些升级到 `-Werror`——有违反就**编译失败**。这是 Linux 的零警告纪律(`CONFIG_W*` 的精神):警告一旦放任,真 bug 就淹没在噪音里。`-Wnull-dereference` 保持普通警告(GCC 跨函数分析偶尔误报),`-Wframe-larger-than` 暂缓(几个 syscall handler 在 16 KB 栈上放 `char[PATH_MAX]`,现在开会破坏零警告构建)。

**UBSan 桩**:`__ubsan_*` 处理函数(`kernel/lib/ubsan.cpp`)在触发时**报告具体类型**(原来只报"undefined behavior",现在报"signed integer overflow"/"null pointer dereference"等)。UBSan 把未定义行为从"silent 出错"变成"运行期明确报告",定位 UB 才有可能。

host 测试侧还有个 opt-in 的 AddressSanitizer + UBSan + gcov(`CINUX_HOST_ASAN`),只在 host 单测上、零内核改动,抓 InodeOps/mock 层的 UAF/OOB/UB。
