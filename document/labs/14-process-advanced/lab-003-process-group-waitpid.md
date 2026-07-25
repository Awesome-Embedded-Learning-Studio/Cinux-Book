---
title: Lab 003 · 进程组与 waitpid 阻塞 验证
---

# Lab 003 · 进程组与 waitpid 阻塞 验证

> 对应 `document/book/14-process-advanced/003/`。验证档 **A 档**(killpg/waitpid 阻塞可见)。验证靠构建 + 测试 + grep + waitpid 阻塞唤醒。

## 目标

确认四件事:

1. 进程组/会话身份在(`pgid`/`sid`/`session_leader`/`controlling_tty`),fork 继承规则集中;
2. `setpgid`/`setsid`/`getpgid`/`getsid` + `killpg` 在,闭环 `sys_kill(pid<0)`;
3. waitpid 阻塞 + Zombie reap 在(exit Dead→Zombie、scheduler 跳 Zombie、exit 唤醒父、WNOHANG 非阻塞);
4. run-kernel-test 从 002 的 809 涨到 827。

## 步骤

### 1. 构建 + 测试

```bash
git checkout 003_proc_process_group 2>/dev/null || git checkout 003_*
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

### 2. 进程组身份 + killpg

```bash
grep -rn 'killpg\|setpgid\|setsid' kernel/proc/signal.hpp kernel/proc/process_group.cpp
grep -rn 'pgid\|session_leader\|inherit_process_identity' kernel/proc/process.hpp kernel/proc/process_new.cpp
```

**思考**:`killpg(pgid=0, sig)` 为什么解析成调用者自己的组?——见章节:0 是"当前组"约定,方便 shell 给自己所在组发信号。`sys_kill(pid<0)` 怎么经 killpg 闭环?——pid<0 解析为"给进程组 -pid 发信号"。

### 3. waitpid 阻塞 + Zombie

```bash
grep -rn 'Zombie\|waiting_for_child\|WNOHANG\|kWaitNoHang' kernel/proc/ | grep -v test | head
grep -rn 'pick_next\|TaskState' kernel/proc/scheduler.cpp kernel/proc/process.hpp | head
```

去看 exit 的 Dead→Zombie 改动 + scheduler pick_next 跳过 Zombie。**思考**:为什么 waitpid 阻塞要拆成 4a(契约)+ 4b(阻塞)两批?——见章节:阻塞的正确性依赖 exit Zombie + scheduler skip + 调用点 WNOHANG,是依赖链;契约先行降风险。改默认阻塞前必须 grep 全 waitpid 调用点(`test_waitpid_not_exited` 会挂死)。

### 4.(GOTCHA #21)Scheduler::current

去看 `Scheduler::current()` 读的是 static `current_` 还是 `g_per_cpu.current`。**单测设 current 必须用 `Scheduler::set_current(&t)`(两者都设)**,不能只设 `g_per_cpu.current`——否则经 `current()` 的路径(waitpid/killpg)读到旧值。

## 验收清单

- [ ] 构建 `build=0`,run-kernel-test ~827。
- [ ] 进程组身份 + setpgid/setsid/getpgid/getsid + killpg 在。
- [ ] waitpid 阻塞 + Zombie reap(exit Zombie、scheduler skip、exit 唤醒父、WNOHANG)。
- [ ] 能说清「root fork 自成组」「waitpid 阻塞为何拆批」「GOTCHA#21」。

## 别做这些

- **别**在改默认阻塞前不 grep 全 waitpid 调用点——会挂死(R1)。
- **别**让 scheduler `pick_next` 选 Zombie——Zombie 设 Running 必崩。
- **别**单测里只设 `g_per_cpu.current`——`Scheduler::current()` 读 static current_,两者要同步(GOTTCHA#21)。
