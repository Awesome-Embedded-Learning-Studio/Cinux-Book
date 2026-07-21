---
title: Lab 046 · clone / futex / TLS 验证
---

# Lab 046 · clone / futex / TLS 验证

> 对应 `document/book/14-process-advanced/046-clone-futex-tls.md`。验证档 **A 档**(clone 起线程可见)+ B(共享/复制内部)。验证靠构建 + 测试 + grep + 双线程交错。

## 目标

确认四件事:

1. `sys_clone` / `sys_futex` 在,`CLONE_*` flag 路由(共享 vs 复制);
2. TLS(`fs_base`)——`CLONE_SETTLS` 设子线程 fs_base,context_switch 恢复;
3. cleartid(`task_exit_cleartid`)——`CLONE_CHILD_CLEARTID` 的 pthread_join 协议;
4. run-kernel-test 从 045 的 783 涨到 809。

## 步骤

### 1. 构建 + 测试

```bash
git checkout 046_proc_clone_futex_tls 2>/dev/null || git checkout 046_*
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

### 2. clone / futex / TLS

```bash
grep -rn 'sys_clone\|sys_futex' kernel/syscall/
grep -rn 'fs_base\|task_exit_cleartid\|CLONE_SETTLS\|CLONE_CHILD_CLEARTID\|CLONE_VM' kernel/proc/process.hpp kernel/proc/fork.cpp
```

**思考**:`CLONE_VM` 共享地址空间时,生出来的是线程还是进程?——线程(两执行流跑同一套页表)。`CLONE_THREAD` 的兄弟关系:同 tgid,`getpid` 都返组长 pid。**cleartid 怎么实现 pthread_join?**——见章节:join 的线程在 cleartid 地址 futex_wait;被 join 的 exit 时清零+wake,零额外机制。

### 3.(GOTCHA #18)子进程栈返回

去看 `fork.cpp` 里 clone 子帧的 user_rsp patch(`*(child->kernel_stack_top-96)=stack`)。**思考**:为什么能直接按 `kernel_stack_top` 定位帧,不用从当前 rsp 算偏移?——见章节:syscall 入口建的 pt_regs 帧固定在 `[kernel_stack_top-96, kernel_stack_top)`,在栈顶固定位置。

### 4.(A 档)双线程交错

内核测试里 `clone(CLONE_VM|CLONE_THREAD|CLONE_SETTLS, stack, ...)` 起兄弟线程:两线程同 tgid、各跑各的 TLS、futex 协调。这就是 A 档端到端。真 pthread 程序要等 F10 musl libpthread。

## 验收清单

- [ ] 构建 `build=0`,run-kernel-test ~809。
- [ ] `sys_clone`/`sys_futex` 在,`CLONE_*` flag 路由。
- [ ] TLS(fs_base)+ cleartid 在。
- [ ] 能说清「fork vs clone(CLONE_VM)」「cleartid 怎么做 pthread_join」「GOTCHA#18 子栈返回」。

## 别做这些

- **别**在测试 helper 里改了 `current` 不配对恢复——`TEST_ASSERT` 早返回会跳过 `set_current(prev)` → current 悬垂 → 后续测试崩(本弧调试 saga)。
- **别**忘了 clone 子帧要 patch user_rsp 槽(否则子进程返回父栈,不是调用者给的 stack)。
