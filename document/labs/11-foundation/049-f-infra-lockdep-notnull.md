---
title: Lab 049 · F-INFRA 基建加固 验证
---

# Lab 049 · F-INFRA 基建加固 验证

> 对应 `document/book/11-foundation/049-f-infra-lockdep-notnull.md`。验证档 **B/C 档**。验证靠构建 + grep + 开 lockdep 跑测试。

## 目标

确认三件事:

1. `CINUX_LOCKDEP` opt-in 在,`schedule()` 入口有持锁深度断言;
2. `NotNull<Task*>` 用在 `add_task`/`remove_task`/`run_first`;
3. 开 `CINUX_LOCKDEP=ON` 构建跑测试不炸(锁纪律干净)。

## 步骤

### 1. 构建 + 测试

```bash
git checkout 049_f_infra_lockdep 2>/dev/null || git checkout 049_*
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

### 2. lockdep + NotNull

```bash
grep -n 'CINUX_LOCKDEP\|g_lockdep_held_depth' kernel/CMakeLists.txt kernel/proc/scheduler.cpp
grep -rn 'NotNull<Task\*>\|NotNull<' kernel/proc/scheduler.hpp kernel/proc/scheduler.cpp | head
```

**思考**:lockdep 为什么 opt-in(默认关)?——见章节:有运行时开销,是开发期检查不是生产路径。开发/CI 开,生产关,对齐 Linux `CONFIG_LOCKDEP`。**NotNull 把什么从注释变成类型?**——"参数不能空"的契约,调用方传空编不过。

### 3.(C 档)开 lockdep 验证锁纪律

```bash
cmake -B build -S . -DCINUX_LOCKDEP=ON && cmake --build build -j$(nproc) > /tmp/bl.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

**期望**:全绿 = 没有持锁跨 schedule 的代码。如果某处持锁调了 `schedule()`,会 kpanic 暴露。这给 F4 SMP 多核并发上了"开发期必炸"的保险。

### 4.(思考)static_assert 锁布局

去看 046 提到的 `static_assert(offsetof(CpuContext, fs_base)==80)`——这就是 F-INFRA 的"static_assert 锁结构体布局"在 `CpuContext` 上的落地。改布局时编译期挡住。

## 验收清单

- [ ] 构建 `build=0`(普通 + `CINUX_LOCKDEP=ON` 两种),测试全绿。
- [ ] `CINUX_LOCKDEP` opt-in + schedule 持锁断言在。
- [ ] `NotNull<Task*>` 用在 scheduler 关键接口。
- [ ] 能说清「lockdep 为何 opt-in」「NotNull 把契约写进类型的意义」。

## 别做这些

- **别**在生产构建开 `CINUX_LOCKDEP`——有运行时开销,它是开发期检查。
- **别**绕过 `NotNull` 传裸可能空的指针——契约写进类型就是为了编译期挡。
