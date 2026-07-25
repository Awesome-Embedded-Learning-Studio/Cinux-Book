---
title: Lab 001 · kallsyms 与 panic backtrace 验证
---

# Lab 001 · kallsyms 与 panic backtrace 验证

> 对应 `document/book/04-developer/001/`。验证档 **C 档**(调试路径)。验证靠触发 panic 看符号化栈 + grep。

## 目标

确认四件事:

1. 全局 `-fno-omit-frame-pointer`(frame pointer 地基);
2. `kallsyms_set_table`/`kallsyms_lookup` 在,注入源解耦;
3. `backtrace_capture`/`backtrace_from` 在,用栈范围检查(非 translate);
4. 统一 panic(收编 dump_registers/kpanic/fatal_halt + backtrace + memstats)。

## 步骤

### 1. 构建 + 测试

```bash
git checkout 001_dev_kallsyms_panic 2>/dev/null || git checkout 001_*
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

### 2. 四块都在

```bash
grep -n 'fno-omit-frame-pointer' CMakeLists.txt cmake/*.cmake 2>/dev/null
grep -n 'kallsyms_set_table\|kallsyms_lookup' kernel/lib/kallsyms.hpp
grep -n 'backtrace_capture\|backtrace_from' kernel/arch/x86_64/backtrace.hpp
grep -rn 'dump_memory_stats' kernel/mm/diagnostics.hpp
```

**思考**:backtrace 为什么用"栈范围检查"而非 `VMM::translate()`?——见章节:translate 不支持 huge 页(返 0),且取锁在 panic 路径有死锁风险。栈范围检查无 lock、对 huge 有效、IF=0 安全。

### 3.(C 档)触发一次 panic 看 backtrace

在内核里临时加一句 `cinux::lib::kpanic("lab trigger");`(或访问非法地址),构建运行,看输出:

- 一行消息(`lab trigger`);
- 寄存器 dump(rax/rbx/.../rip/rsp);
- backtrace(每帧地址,经 kallsyms 符号化的会是 `函数名+偏移`,1b 注入就绪前是裸地址);
- 当前 task;
- `dump_memory_stats`(PMM/Slab/PageCache 汇总)。

**裸地址怎么读?** 1b 符号注入就绪前,把 backtrace 里的地址拿去 host 端 `addr2line -e build/kernel/big/big_kernel <addr>`(或 `nm build/...|grep <addr>`)符号化。**看完记得删掉触发代码。**

### 4.(思考)注入源解耦的价值

`kallsyms` 模块只认注入进来的表,不管表从哪来。去看 `kallsyms_set_table` 的调用方——生产是 build 时 nm 抽符号(1b follow-up),单测是 fixture 表。**这种解耦让 1b 可以后加、不阻塞 backtrace/panic。**

## 验收清单

- [ ] 构建 `build=0`,测试全绿(含 -O2 Release:首次 -O2 验证)。
- [ ] frame pointer 全局开;kallsyms/backtrace/统一 panic/memstats 都在。
- [ ] backtrace 用栈范围检查,不用 translate。
- [ ] 触发过一次 panic,看到 backtrace + 寄存器 + memstats。

## 别做这些

- **别**在 backtrace 里用 `VMM::translate()` 判栈页——huge 页返 0 + 取锁死锁。
- **别**在 kprintf 里用 `%zu`/`%ld`——freestanding 子集只有 `%x`(uint64)/`%u`(unsigned)/`%d`,用 `(unsigned)` cast。
