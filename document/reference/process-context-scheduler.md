---
title: 参考 · 进程:上下文切换、调度器与同步原语
---

# 参考 · 进程:上下文切换、调度器与同步原语

> 查阅层。这一页是 Cinux 进程子系统的速查表,不按 tag 组织,给后续章节(调度器 020、同步 021、ring3 022、fork/exec 034、通电 fork 035……)查 `CpuContext` 布局、`context_switch` 序列、调度接口、锁原语用。实现以最终 tag `035_multi_terminal` 源码为准。
>
> 范围:任务控制块(Task/TCB)、callee-saved 上下文切换、抢占式轮转调度、Spinlock/Mutex/Semaphore。**不含真 SMP(单核 + 中断串行假设)、不含 cgroup/优先级反转继承、不含用户态线程(pthread)**。

## 子系统地图

```text
   Task(TCB: ctx + state + pid + 内核栈 + fd_table + fpu_state)
        │
        │  Scheduler.tick()  ←── PIT IRQ0 每拍递减 quantum,到 0 则 schedule()
        ▼
   ┌──────────────────────────────────────────────────────────┐
   │  SchedulingClass(抽象) → RoundRobin(MAX_TASKS=64)          │
   │   enqueue / dequeue        按类(priority)分桶,MAX_CLASSES=4 │
   └──────────────────────────────────────────────────────────┘
        │ schedule() 选出 next
        ▼
   ┌──────────────────────────────────────────────────────────┐
   │  context_switch(from=CpuContext*, to=CpuContext*)         │
   │   存 from 的 r15-r12/rbp/rbx/rsp/rip + gs_base/kgs_base     │
   │   取 to   的同上,wrmsr 换 gs_base,jmp *to.rip             │
   └──────────────────────────────────────────────────────────┘
        │ 阻塞路径
        ▼
   Scheduler.block(task, reason) ←── Mutex.lock() 争用 / Semaphore.wait()
        │ 把 task 挂进 WaitQueue(Task::wait_next 侵入式链表),state=Blocked
        ▼
   Mutex.unlock() / Semaphore.post() → Scheduler.unblock(task) → 回 Ready 队列
```

## CpuContext(上下文快照,80 字节)

`cinux::proc::CpuContext`(`alignas(16)`,`kernel/proc/process.hpp`),布局**必须**和 `context_switch.S` 的偏移逐字对应(有 `static_assert` 把守):

| 字段 | 偏移 | 说明 |
|---|---|---|
| r15 | 0 | callee-saved |
| r14 | 8 | callee-saved |
| r13 | 16 | callee-saved |
| r12 | 24 | callee-saved |
| rbp | 32 | callee-saved |
| rbx | 40 | callee-saved |
| rsp | 48 | 栈指针(切换的关键) |
| rip | 56 | 恢复点 |
| gs_base | 64 | `MSR_GS_BASE`(0xC0000101),per-CPU |
| kgs_base | 72 | `MSR_KERNEL_GS_BASE`(0xC0000102) |

`sizeof(CpuContext) == 80`。**只存 callee-saved(r15-r12、rbp、rbx)+ rsp + rip + gs_base/kgs_base。** 切换发生在已知调用边界,caller-saved 寄存器(rax、rcx、rdx、rsi、rdi、r8-r11)按 ABI 由调用方自己保存,不在快照里。

> **诚实点:`CpuContext` 没有 `rax` 字段。** 这一点常被源码注释带偏——`fork()` 的文档说子进程返回值「set in the child's TCB via ctx.rax」,但 `CpuContext` 根本没有 rax。子进程返回 0 靠的是 `fork_child_trampoline`(`xor %rax,%rax; ret`),不是存进 ctx。引用任何「ctx 里存了某 caller-saved 寄存器」前,先 `git show <tag>:kernel/proc/process.hpp` 核对字段。`gs_base`/`kgs_base` 是 035 才加的(此前 80 字节是 64)。

## 上下文切换 `context_switch`

`kernel/arch/x86_64/context_switch.S`,`context_switch(from=%rdi, to=%rsi)`:

```text
存 from:
  r15→from+0 ... rbx→from+40, rsp→from+48
  leaq .restore(%rip),%rax; mov %rax,from+56   # rip = 恢复点(.restore)
  rdmsr(0xC0000101) → from+64/68               # gs_base
  rdmsr(0xC0000102) → from+72/76               # kgs_base
取 to:
  to+0..40 → r15..rbx, to+48 → rsp             # 切栈
  wrmsr(0xC0000101, to+64/68)                   # 装 gs_base
  wrmsr(0xC0000102, to+72/76)                   # 装 kgs_base
  sti                                           # 新任务必须开中断进入
  jmp *to+56                                    # 跳到 to.rip
.restore: ret                                   # 被切走的任务,回来时从这里 ret
```

两个关键设计:一是 `rip` 存的是 `.restore` 的地址(而非任意代码点),所以「被切换出去的任务」下次被切回来时,从 `context_switch` 的 `ret` 正常返回到当初调用 `context_switch` 的地方——对调用方而言,`context_switch` 就像个普通函数调用,只是「返回」发生在很久以后。二是 `gs_base`/`kgs_base` 用 `rdmsr`/`wrmsr` 读写 `0xC0000101`/`0xC0000102`(配合 `swapgs` 实现 per-CPU 内核/用户 gs 切换),这是 035 为每 CPU 状态加的。

## fork 与子进程返回

`kernel/proc/fork.cpp` 的 `fork()`:`memcpy` 拷父 TCB → 给子进程分配新 pid、新内核栈(STACK_PAGES=4 + 1 guard page)、置 `state=Ready`、`fd_table=nullptr`(共享全局内核表,或后续设私有)→ 把子的 `ctx.rip = fork_child_trampoline`、`ctx.rsp` 指向子栈上 fork() 的返回地址。

`fork_child_trampoline`(`xor %rax,%rax; ret`)让子进程「第一次被调度时」从 fork() 调用点返回 0;父进程的 fork() 正常返回子 pid。**子进程返回 0 是靠 trampoline,不是靠 ctx 字段**(见上诚实点)。CoW:`handle_cow_fault` 在 035 接进了 `#PF`(present+write+user 路径),但引用计数有限——别把 CoW 写成「完整可用」。

## Task / TCB

`cinux::proc::Task`(process.hpp)关键字段:

| 字段 | 说明 |
|---|---|
| `CpuContext ctx` | 上下文快照(见上) |
| `TaskState state` | Running/Ready/Blocked/Zombie/Dead |
| `tid` / `pid` / `ppid` | 线程/进程/父进程 id |
| `priority` | 调度类归属 |
| `kernel_stack` / `kernel_stack_top` | 内核栈底 / 栈顶(初始 rsp) |
| `kernel_stack_guard_page` | guard 页(溢出检测) |
| `fd_table` | 文件描述符表(`nullptr` = 用全局内核表;非空 = 私有,035 多终端用) |
| `fpu_state[512]` | FPU/SSE 状态(alignas 16) |
| `wait_next` | 侵入式等待队列链表(Mutex/Semaphore 用,免堆分配) |

`TaskBuilder`(`STACK_MAGIC=0xDEADC0DE`、`STACK_PAGES=4`):从堆分配 Task、从 PMM 分配内核栈,初始化 `CpuContext` 使首次 `context_switch` 跳到 entry,栈顶写 magic 防溢出。

## TaskState 状态机

`enum class TaskState : uint8_t { Running, Ready, Blocked, Zombie, Dead }`:

```text
   TaskBuilder.create ──▶ Ready ──schedule()──▶ Running
                              ▲                     │
                              │                     │ block() / yield()
                              │                     ▼
                              └──unblock()──── Blocked
                                       
   Running ──exit──▶ Zombie ──父 waitpid 收尸──▶ Dead
```

## 调度器 Scheduler

`cinux::proc::Scheduler`(全静态,`scheduler.hpp`):

| 接口 | 说明 |
|---|---|
| `init()` / `is_initialized()` | 初始化(建 idle 任务) |
| `register_class(SchedulingClass*)` | 注册调度类(最多 `MAX_CLASSES=4`) |
| `add_task(Task*)` / `remove_task(Task*)` | 入/出调度 |
| `run_first(Task* boot_task)` | 启动第一个任务(不再返回) |
| `tick()` | PIT 每拍调;递减当前任务 quantum,到 0 触发抢占 |
| `schedule()` | 选 next、调 `context_switch` |
| `yield()` | 当前任务主动让出 |
| `block(Task*, reason)` / `unblock(Task*)` | 阻塞/唤醒(state ↔ Blocked) |
| `exit_current()` | 当前任务退出 |

调度类:`SchedulingClass`(抽象,`enqueue`/`dequeue`)→ `RoundRobin`(`MAX_TASKS=64` 固定数组)。`DEFAULT_TIME_SLICE=2`(每任务 2 拍)。抢占是 PIT tick 驱动的——这是 020 把「协作式」升级成「抢占式」的核心。

## 同步原语(`sync.hpp`)

| 原语 | 语义 | 实现 |
|---|---|---|
| `Spinlock` | 忙等互斥 | atomic test-and-set,`volatile bool locked_`;`Guard`(RAII)、`IrqGuard`(acquire + `cli`)。**绝不在持有 Spinlock 时 block/yield** |
| `Mutex` | 阻塞互斥 | 争用时把当前任务挂进 FIFO 等待队列、`block()`;`unlock()` 唤醒队首并转移所有权。内部 Spinlock 只在操作队列时短暂持有,释放后才 block,无死锁 |
| `Semaphore` | 计数信号量 | 基于 `block`/`unblock`,`wait()`/`post()` |
| WaitQueue | 等待队列 | **侵入式单链表**(`Task::wait_next`),`Mutex`/`Semaphore` 共用,零堆分配 |

`Spinlock` 用于保护短临界区(PMM/VMM/Heap 内部、原语自身元数据);`Mutex` 用于可能阻塞的长临界区。

## 约束与边界(本子系统的真实限制)

- **单核 + 中断串行假设。** Spinlock/Mutex 在单核 + 中断串行下成立;真上 SMP 要重审自旋页表/per-CPU。`IrqGuard` 是关中断保护,不是多核锁。
- **`CpuContext` 无 caller-saved 寄存器(含 rax)。** 切换只在已知调用边界发生;任何「ctx 保存了 rax」的说法都是错的(子进程返回 0 走 trampoline)。
- **RoundRobin 固定 64 任务上限。** 超过 `MAX_TASKS` 入队失败;`MAX_CLASSES=4`。
- **`waitpid` 在早期非阻塞**(NotExited 返回 0),035 才接通有界收尸;引用阻塞语义前核对。
- **FPU 状态切换简单。** `fpu_state[512]` 按任务存,未做惰性 `cr0.TS` 切换(那是真 OS 的优化)。
- **guard page 在 035 半成品。** `kernel_stack_guard_page` 字段存在,但「IST + unmap」的完整触发路径未全接线(见中断参考的 IST 诚实点)。

## 验证入口

- host 单测:`ctest --test-dir build -R "proc|scheduler|sync|context" --output-on-failure`。
- QEMU 机内测:`cmake --build build --target run-big-kernel-test`(`kernel/test/` 下进程/调度/同步套,跑真 `context_switch` + 真 PIT 抢占)。
- 可视化:`cmake --build build --target run`,看多任务轮转、`fork`/`exec` 后 shell 行为。

## 源码索引

- TCB / CpuContext / TaskBuilder:[process.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process.hpp) / [process_internal.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process_internal.hpp) / [task_builder.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/task_builder.cpp)。
- 切换:[context_switch.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/context_switch.S)(含 `fork_child_trampoline`)。
- 调度:[scheduler.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.hpp) / [scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.cpp)。
- 同步:[sync.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/sync.hpp) / [sync.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/sync.cpp)。
- fork/exec:[fork.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/fork.cpp) / [execve.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/execve.cpp)。
- PID:[pid.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/pid.hpp) / [pid.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/pid.cpp)。

## 权威依据

- Intel SDM Vol 3,Ch 3(Segment / `GS.base`、`MSR_GS_BASE`=`0xC0000101`、`MSR_KERNEL_GS_BASE`=`0xC0000102`、`swapgs`):per-CPU gs 与内核/用户 gs 交换的硬件依据。
- System V AMD64 ABI §3.2(callee-saved = `rbx,rbp,r12-r15`;caller-saved = 其余):为什么 `CpuContext` 只存这几本寄存器。<https://gitlab.com/x86-psABIs/x86-64-ABI>
- xv6 MIT 6.S081(`swtch` / `context`、`struct proc`、轮转调度):朴素上下文切换与 TCB 的经典参照。
- OSDev — [Context Switching](https://wiki.osdev.org/Context_Switching)、[Spinlock](https://wiki.osdev.org/Spinlock)。
