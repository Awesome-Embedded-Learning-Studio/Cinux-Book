---
title: Lab 034 · 给进程加上 fork / execve / waitpid
---

# Lab 034 · 给进程加上 fork / execve / waitpid

> 这个实验对应主书 [034 · fork / execve / waitpid](../../book/10-multitasking/034-process-fork-exec.md)。我们不在 lab 里贴完整答案代码——你要自己把五大原语一层层接上。这里只给目标、约束、验证手段和排错方向。

## 实验目标

在 033 的基础上,让内核具备 Unix 进程模型的三个核心原语:

1. **fork**——复制当前进程,得到一个几乎相同的子进程(Copy-On-Write)。
2. **execve**——把当前进程的用户空间映像换成磁盘上的一个 ELF 程序,PID 不变。
3. **waitpid**——等待子进程退出、收集退出码、清理它的 TCB(收尸)。

并配套实现 PID 分配器、TCB 的「家谱」字段、CoW page fault 处理,最后把这五件事(加 `getpid`/`getppid`)暴露成系统调用。

## 前置条件

- 跑通 019/020 的调度器(`Scheduler::current()`、`Scheduler::add_task()`、`context_switch`)。
- 跑通 022 的用户态切换与 023 的 syscall 框架(`syscall_register`、`syscall_dispatch`)。
- 跑通 027 的 VFS(本 lab 的 execve 要靠 `vfs_resolve` + `fs->lookup` 读 ELF)。
- 跑通 016/018 的分页与 `AddressSpace`(本 lab 的 CoW 要直接操作 4 级页表)。

## 任务分解

按依赖顺序,分七块做。每块都先写**测试**再写实现——034 自带两套测试可以对照。

### 任务 1:PID 分配器

新建 `kernel/proc/pid.{hpp,cpp}`,实现一个 `PidAllocator` 类,放进 `cinux::proc` 命名空间,并定义全局实例 `g_pid_alloc`。

- **职责**:管理 1..PID_MAX 的进程号池;分配、回收、查询、计数。
- **要点**:`alloc()` 用 hint 游标绕圈找空闲位,池满返回 `PID_NONE`;`free()` 要安全(越界、重复 free 都得 no-op),并把 hint 往回拉到刚释放的更低号。
- **先写测试**:`test/unit/test_fork_exec.cpp` 里那一整段 PID 分配器测试(顺序分配、free 后复用最低号、耗尽返回 `PID_NONE`、各种非法 free 不崩)。

### 任务 2:TCB 扩展

改 `kernel/proc/process.hpp` 的 `Task` 结构体,补上进程身份与家谱。

- **新字段**:`pid`、`ppid`、`exit_status`、`children`(指向第一个子任务的指针)、`parent`(指向父任务)。
- **复用 `wait_next`**:它原本是互斥锁/信号量等待队列的侵入式链表指针,这一章**兼作** children 链表的 next。想清楚为什么一个指针能两用(父子链表和等待队列不会同时用到同一个 task)。
- **新状态**:给 `TaskState` 加一个 `Zombie`(介于 `Blocked` 和 `Dead` 之间),表示「进程已退出、TCB 等收尸」。

> 提醒:确认 `CpuContext` 里**只有 callee-saved 寄存器 + rsp + rip,没有 rax**。这一点会在「任务 3」的讨论里变得很关键——别顺手给它加 rax。

### 任务 3:fork(Copy-On-Write)

在 `kernel/proc/process.cpp` 实现 `int fork(PidAllocator& pid_alloc)`。步骤顺序照主书的流程图:

1. 取 `Scheduler::current()` 作父;分配子 PID。
2. `new` 一个 `Task`,`memcpy` 整个父 TCB 过去,再修子专属字段(`tid`/`pid`/`ppid`/`state=Ready`/`parent`/`children=nullptr`/`exit_status=0`)。
3. 给子进程**新分配** 16KB 内核栈(STACK_PAGES=4),map 进内核地址空间,底部写 `STACK_MAGIC`。
4. 计算父栈「已用区」大小,把这段拷到子新栈的**同样高度**,并据此设 `child->ctx.rsp`。
5. CoW 页表(见任务 4)。
6. 头插法挂进 `parent->children`;`Scheduler::add_task(child)`。
7. 返回 `child_pid`。

- **接口约束**:`fork` 返回 int(父视角的子 PID,失败 -1)。
- **必读**:主书「调试现场」专门讲了 034 的 fork **没有**接「子进程返回 0」。本 lab 也按这个边界实现——**不要**试图在本 lab 里把子进程返回值闭环(那需要改上下文约定或造 trampoline,超出 034 范围)。能正确造出子进程、父进程拿到 PID、CoW 生效,就算完成任务 3。

### 任务 4:CoW 页表与 page fault 处理

实现递归复制页表的 `copy_page_table_level(src_phys, dst_phys, level)` 和写时复制 `bool handle_cow_fault(uint64_t fault_vaddr)`。**注意:034 并没有把 `handle_cow_fault` 接进 `#PF` handler**——它是写好但无人调用的死代码(`handle_pf` 只做 demand-paging,写保护故障直接 fatal_halt)。本 lab 按这个边界来,实现这两个函数即可,不要去改 `exception_handlers.cpp` 的 `#PF` 路径(那是后面的事)。

- **页表复制**:外层处理 PML4[0..255]\(用户半区),给每个在用的 PML4 项分配新页表页;`copy_page_table_level` 从 PDPT(level=3)递归到 PT(level=1)。中间层分配新页表页并递归;**叶子层**(PT)把父子双方的 PTE 都改成「去掉 WRITABLE + 置 FLAG_COW」,物理页**共享**。
- **`FLAG_COW`**:用 PTE 的 bit 9(软件可用位)。在 `paging_config.hpp` 加 `FLAG_COW = 1ULL << 9`。
- **写时复制**:`handle_cow_fault` 确认 PTE「present + 只读 + 带 COW」后,分配新物理页、拷 4096 字节、改 PTE 指向新页并恢复 WRITABLE/清 COW、`invlpg` 刷 TLB。
- **本 lab 的边界(两条,都要在注解里写清)**:① CoW **不带引用计数**——无条件写时复制,不更新「另一方」PTE(多方共享、原始页回收都不保证);② `handle_cow_fault` **不接进 `#PF`**——真写一张 CoW 页会 fatal_halt,这是 034 的已知状态,不是你的 bug。本 lab 只验 PTE 级标记与转换逻辑,不验端到端写时复制。

### 任务 5:ELF 类型与校验

新建 `kernel/proc/elf_types.{hpp,cpp}`,给 execve 用。

- **结构**:`Elf64_Ehdr`(64 字节,`static_assert` 钉死)、`Elf64_Phdr`(56 字节)。都 `__attribute__((packed))`。
- **常量**:`ELF_MAGIC = 0x464C457F`、`ELF_CLASS_64=2`、`ELF_DATA_LSB=1`、`ET_EXEC=2`、`EM_X86_64=62`、`PT_LOAD=1`、`PF_X/PF_W/PF_R`。
- **`validate_elf_header`**:逐项校验(魔数、64 位、小端、x86-64、ET_EXEC、phoff 合法、phentsize==56、phnum>0),返回 `ElfValidateResult`。

### 任务 6:execve

在 `process.cpp` 实现 `ExecveResult execve(const char* path, const char* const argv[], const char* const envp[])`。

- **流程**:校验 path → 取 current → 确保有 addr_space → `vfs_resolve`+`lookup` 拿 inode → 确认是 `Regular` 文件 → 读 64 字节 ELF 头并校验 → 读 program headers → `clear_user_mappings` 清旧用户页 → 逐 `PT_LOAD` 段逐页「分配物理页 + 整页清零 + 按 `p_filesz` 拷文件字节 + `addr_space->map`」→ 设 `task->ctx.rip = e_entry`。
- **权限**:页 flags 由段 flags 推导(`PF_W`→可写;无 `PF_X`→置 `NX`)。
- **BSS**:靠「先清零整页、再只拷 `filesz`」自然落地,不要单独写 BSS 逻辑。
- **错误码**:`ExecveResult` 的值 = 负 errno(`ENOENT`/`EISDIR`/`EIO`/`ENOEXEC`/`ENOMEM`/`ESRCH`/`EINVAL`)。
- **本 lab 的边界**:`argv`/`envp` 这章**不**铺用户栈(可以 `(void)` 掉),入口写进 `ctx.rip` 即交差。

### 任务 7:waitpid + 系统调用接线

- **waitpid**:实现 `WaitpidResult waitpid(int pid, int* status, PidAllocator& pid_alloc)`。校验 pid(`-1` 任意 / `>0` 指定,其余非法)→ children 为空返回 `NoChildren` → 扫链表定位 zombie(找不到指定 PID → `NotFound`;还没退 → `NotExited`)→ 收 `*status` → 从单链表摘除 → `pid_alloc.free` → 标 `Dead`。**注意是非阻塞**:孩子没退就返回 `NotExited`。
- **五个 `sys_*` 封装**:`sys_getpid`/`sys_getppid`(读 TCB)、`sys_fork`(转 `fork`)、`sys_execve`(path 先当内核指针,转 `execve`)、`sys_waitpid`(Ok→返回子 PID,NotExited→返回 0,否则负 errno)。
- **syscall 号**:在 `syscall_nums.hpp` 加 `getpid=39`、`getppid=110`、`fork=57`、`execve=59`、`waitpid=61`(Linux x86_64)。
- **注册**:在 `syscall.cpp` 的 `register_builtin_handlers()` 里把这五个 handler 注册进分发表。

## 接口约束

- **PID**:`PID_NONE=0`(保留/失败),`PID_MAX=256`,有效范围 1..256。
- **`FLAG_COW`**:PTE bit 9,且必须 `FLAG_COW == 1ULL << 9`(host/内核单测都硬断言)。
- **ELF 结构尺寸**:`sizeof(Elf64_Ehdr)==64`、`sizeof(Elf64_Phdr)==56`,用 `static_assert` 守住。
- **syscall 号**:严格沿用 Linux x86_64(`fork=57`、`execve=59`、`waitpid=61`、`getpid=39`、`getppid=110`)。
- **错误码**:沿用 Linux errno 数值,`ExecveResult`/`WaitpidResult` 枚举值就是负 errno。

## 验证步骤

**第一步:host 单元测试**(链 `pid.cpp` + `elf_types.cpp`,不碰硬件):

```bash
ctest --test-dir build -R fork_exec --output-on-failure
```

或一次跑全部 host 测试:

```bash
cmake --build build --target test_host
```

预期:PID 分配器全场景、TCB 字段默认值、`Zombie` 状态、`FLAG_COW` 位运算、CoW PTE 状态机(标记只读+COW → 写后私有可写页)、syscall 号常量、`ExecveResult` errno 映射、ELF 结构尺寸与各种坏头校验,全部通过。

**第二步:QEMU kernel 测试**(真走 syscall 分发):

```bash
cmake --build build --target run-big-kernel-test
```

预期:`run_fork_exec_tests()` 跑过——getpid/getppid 直调与分发一致、PID 分配器机内 smoke、`FLAG_COW`、CoW PTE 转换、sys_fork 分发路径可达、ExecveResult errno、ELF 校验。最终 `[TEST] ALL TESTS PASSED`。

**第三步:端到端**:

```bash
cmake --build build --target run
```

留意串口里 `[PROC] fork: created child pid=...`、`[EXECVE] loaded ... entry=...`、`[WAITPID] reaped child pid=...` 三类日志能被走到,确认五个 syscall 已接进分发、底层设施各就各位。

## 常见故障

- **一写 CoW 页就 fatal_halt**:这是 034 的预期行为——`handle_cow_fault` 没接进 `#PF`,写保护故障直接停机。不是你的 bug,别去改 `#PF` 路径。
- **父子数据互相污染(若你自行把 #PF 接上来测)**:八成是叶子层只改了**子**的 PTE 没改**父**的。CoW 的前提是「任何共享方都只读」,父也得去掉 `WRITABLE` + 置 `COW`,否则父直接写共享页,子的数据就脏了。
- **`FLAG_COW` 位选错**:别用 bit 0-8(那些 CPU 会解释:present/writable/user/...),也别用 bit 63(NX)。bit 9-11 是软件可用位,bit 9 是最常用的选择。
- **execve 后进程跑飞**:多半是入口没设对(`task->ctx.rip = ehdr->e_entry`),或某段没按 `p_memsz` 映射全(漏了 BSS 尾页)。记住「先整页清零再拷 `filesz`」。
- **`clear_user_mappings` 行为和注释对不上**:注释说「不释放页表页」,但代码其实释放了 PT/PD/PDPT。**以代码为准**,别被过期注释带歪。
- **waitpid 一直返回 0(收不到)**:034 的 waitpid 是**非阻塞**的,孩子没退就返回 0。这是设计如此,不是 bug——别误以为它该阻塞。
- **fork 之后「分不清父子」**:这正是 034 的已知缺口——子进程没有「返回 0」的闭环。本 lab 不要求修,但要在代码注释/报告里**写明**这个边界,别假装它已经实现。

## 通过标准

- [ ] host `-R fork_exec` 全绿;`test_host` 整体不回归。
- [ ] `run-big-kernel-test` 里 `run_fork_exec_tests()` 通过,最终 ALL TESTS PASSED。
- [ ] 五个系统调用经 `syscall_dispatch` 可达,号与 Linux x86_64 一致。
- [ ] fork 能造出子进程(新内核栈 + CoW 页表 + 挂 children + 入调度器),父拿到子 PID。
- [ ] CoW 页表标记正确:fork 后父子双方 PTE 都去掉 `WRITABLE`、置 `FLAG_COW`,且指向同一物理页;`handle_cow_fault` 的 PTE 转换逻辑(新页 + 恢复写 + 清 COW)正确(host 单测覆盖)。端到端「写触发自动复制」因 `#PF` 未接线,本 lab 不作要求。
- [ ] execve 能把一个合法 ELF64 x86-64 可执行文件的 `PT_LOAD` 段正确铺进地址空间,入口写入 `ctx.rip`;各种坏头/坏路径返回正确 errno。
- [ ] waitpid 能收僵尸子进程(收退出码、摘链、回收 PID、标 Dead),非阻塞语义正确。
- [ ] 在代码或报告里**诚实标注**两个 034 边界:CoW 无引用计数、fork 子进程返回值未闭环。不把未实现的东西写成已实现。
