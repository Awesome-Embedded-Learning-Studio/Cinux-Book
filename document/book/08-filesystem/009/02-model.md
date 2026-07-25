---
title: 02 · Linux init 模型对齐:boot、init、idle
---

# Linux init 模型对齐:boot、init、idle

## Linux init 模型对齐:boot、init、idle 三个角色

Cinux 借的是 Linux 的**模型**(注意,只是模型,不是 Linux init 的全部能力),三个角色:

```text
   kernel_main (boot CPU 的原始执行流)
        │
        │  Scheduler::init()        ← 建好调度器 + idle task
        │  spawn kernel_init 线程    ← PID 1 的类比:接管 FS + shell
        │  spawn boot task          ← boot CPU 上下文成为这个 task
        │  run_first(boot_task)     ← boot 成为 current，随后切给 kernel_init
        ▼
   ┌─────────────────────────────────────────────┐
   │  kernel_init 线程 (PID 1 类比)                 │
   │    AHCI::instance() → ext2.mount()            │
   │    vfs_mount_init() + mount "/"               │
   │    launch_first_user()  ← shell               │
   │    exit_current()                             │
   └─────────────────────────────────────────────┘
        │ shell 退出 / 无事可做时
        ▼
   idle task (Scheduler::init 创建):hlt 空转
```

- **idle task**:调度器自己的,`Scheduler::init()` 里就建好了,运行队列空时由它 `hlt` 空转(008 已有)。
- **boot task**:`run_first(boot_task)` 把 boot CPU 当前的执行流「认领」成一个 task,作为 handoff 的起点。它的 entry 是个只打印 `UNEXPECTED` 然后 `hlt` 的 lambda——正常情况下控制权一交出去就再也不回来,真回到它说明哪里错了。
- **kernel_init**:`init.cpp` 里的 `kernel_init_thread`,干 PID 1 的活:挂 ext2、挂 VFS、`launch_first_user` 起 shell,最后 `exit_current`。

`main.cpp` 的步骤 22 就是这一切的发动点:

```cpp
Scheduler::init();

auto* init_task = TaskBuilder()
    .set_entry(cinux::proc::kernel_init_thread)
    .set_name("kernel_init")
    .build();
Scheduler::add_task(init_task);

auto* boot_task = TaskBuilder()
    .set_entry([]() { /* UNEXPECTED, hlt */ })
    .set_name("boot")
    .build();
Scheduler::run_first(boot_task);   // boot 成为 current，随即调度到 kernel_init
```

而 `kernel_init_thread` 本体非常薄,就是把原来散在 `kernel_main` 末尾的「挂载 + 起 shell」搬进来:

```cpp
void kernel_init_thread() {
    auto* self = Scheduler::current();
    kprintf("[INIT] kernel_init started tid=%u\n", self ? self->tid : 0);

    static Ext2 ext2(AHCI::instance(), 1);
    if (!ext2.mount()) { kprintf("[INIT] ext2 mount failed!\n"); }

    vfs_mount_init();
    vfs_mount_add("/", &ext2);

    launch_first_user();          // shell
    Scheduler::exit_current();
}
```

> ⚠️ 注意措辞:Cinux 借的是「init 线程」这个**组织模型**(boot 当 handoff 源、一个 init 线程接管后续),**没有** Linux 的 fork、进程树、信号、wait 那一整套。别把它读成「Cinux 实现了 init 子系统」。
