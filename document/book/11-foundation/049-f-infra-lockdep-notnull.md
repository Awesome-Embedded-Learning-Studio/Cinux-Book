---
title: 049 · F-INFRA 基建加固(lockdep + NotNull)
---

# 049 · 基建加固:lockdep 锁序图 + NotNull 指针契约

> 横切基建加固(F-INFRA),插在 F3 和 F4 之间。马上要进 F4 **SMP 多核**——并发是竞态/死锁的高发区,先给锁纪律上个**自动检查**(lockdep),再把指针契约写进类型(NotNull)。这一弧还有 freestanding 头门禁、编译零警告等一堆"看不见但扛事"的加固。B/C 档:lockdep 是 C(调试路径),其余是 B(构建/类型)。

## 这章咱们要点亮什么

1. **lockdep**(opt-in):断言"没有 spinlock 跨 `schedule()` 持有"——持锁调度是死锁和竞态的经典根源。
2. **`NotNull<T>`**:指针契约类型,把"这个指针不能空"从注释写进编译期。
3. **freestanding 头门禁 + 零警告**:`-Wpedantic` 等,修 `<array>` + GCC 断言等基建坑。

## lockdep:持锁跨 schedule 的自动检查

持着自旋锁去 `schedule()` 是内核里一类经典 bug——别的 task 要拿同一把锁就死锁,或者中断路径撞上就竞态。单核时偶尔侥幸,多核必爆。F4 SMP 之前先上 lockdep 给它个自动检查:

```cmake
# kernel/CMakeLists.txt:88 —— opt-in,默认关(有运行时开销)
if(CINUX_LOCKDEP)
    target_compile_definitions(big_kernel_common PUBLIC CINUX_LOCKDEP)
endif()
```

`CINUX_LOCKDEP=ON` 时,每个 spinlock 的 acquire/release 维护一个全局深度 `g_lockdep_held_depth`,`schedule()` 入口断言它是 0:

```cpp
// kernel/proc/scheduler.cpp:341-347
#ifdef CINUX_LOCKDEP
    if (g_lockdep_held_depth > 0) {
        // 持锁跨 schedule —— 死锁/竞态根源,opt-in 下直接 kpanic 暴露
    }
#endif
```

opt-in(默认关)是因为它有运行时开销 + 它是**开发期检查**,不是生产路径。开发/CI 打开它跑测试,持锁调度的代码当场炸;生产构建关掉,零开销。这和 Linux 的 `CONFIG_LOCKDEP` 一个路子。

> 这是 F4 SMP 的"保险":多核下持锁调度的 bug 极难复现,lockdep 把它从"偶发 Heisenbug"变成"开发期必炸"。

## NotNull<T>:指针契约写进类型

`lib::NotNull<T>` 是个"不能空"的指针类型。这一弧把 `Scheduler` 的关键接口参数从 `Task*` 换成 `NotNull<Task*>`:

```cpp
// kernel/proc/scheduler.cpp:227 / :237 / :289
void Scheduler::add_task(lib::NotNull<Task*> task);
void Scheduler::remove_task(lib::NotNull<Task*> task);
void Scheduler::run_first(lib::NotNull<Task*> boot_task);
```

意义:把"`add_task` 的参数不能是 nullptr"这条**注释契约**写进**类型**——调用方传空编不过,不用靠运行时检查或文档。这是"用类型防错"的思路,和 036 的 ErrorOr(用类型表达错误)一脉相承。

## freestanding 头门禁 + 零警告

F-INFRA 还收了一批"看不见但扛事"的基建:

- **freestanding 头门禁**:内核是 freestanding(没 libc),但要小心——某些标准头(如 `<array>`)在 freestanding + 特定 GCC 下有坑,门禁挡住误用 + 修 GCC 断言。
- **编译零警告**:`-Wpedantic` 等收紧到零警告(后面 CinuxOS 的 CI 把它做成门禁)。警告不是"能过就行",一堆警告里藏的真 bug 会被淹没。
- **`kprintf` format 属性**:给 `kprintf` 加 `__attribute__((format))`,GCC 检查格式串和参数类型匹配(修了一批 `%` 不匹配)。
- **`static_assert` 锁结构体布局**:关键结构体(`CpuContext` 等)的成员 offset 加 `static_assert`,改布局时编译期挡住(还记得 046 的 `offsetof(CpuContext, fs_base)==80` 吗?就是这来的)。

## 验证

```bash
grep -n 'CINUX_LOCKDEP\|lockdep' kernel/CMakeLists.txt kernel/proc/scheduler.cpp
grep -rn 'NotNull' kernel/proc/scheduler.hpp kernel/proc/scheduler.cpp | head
# 开 lockdep 构建 + 跑测试(开发期检查)
cmake -B build -S . -DCINUX_LOCKDEP=ON && cmake --build build -j$(nproc)
cmake --build build --target run-kernel-test
```

C 档端到端:开 `CINUX_LOCKDEP=ON`,如果代码里有持锁跨 schedule 的地方,测试会 kpanic 暴露;全绿 = 锁纪律干净。

## 小结与下一站

F-INFRA 给并发上了保险:lockdep 自动查持锁调度,NotNull 把指针契约写进类型,freestanding/警告/static_assert 加固了地基。这些都是"看不见但扛事"的东西——平时没存在感,bug 来了才知道值。

下一站 **050** 进 **F4 SMP 多核**:ACPI 静态表 + LAPIC/IOAPIC + PIC→APIC 切换。那是 v1.0.0 最大的差异化弧,lockdep 这时上正好。
