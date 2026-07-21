---
title: 044 · kallsyms 与 panic backtrace
---

# 044 · 给内核装上眼睛:kallsyms、backtrace、统一 panic

> 这是一章**横切基建**(插在 F2 和 F3 之间)。F2 那 15 个 GOTCHA,基本是靠临时加 `kprintf` 一行行定位的(nested-KVM 振荡、direct-map 走错、slab 陈旧命中……)。可一旦进 F3——信号、clone、futex、SMP——这类强并发、易死锁的东西,光靠 kprintf 不够,**崩了得能看懂调用链**。044 补的就是这套可观测性:frame pointer + kallsyms 符号查找 + backtrace + 统一 panic。
>
> C 档:这一弧没有新用户能力,加的是**调试/可观测**能力。验证靠触发 panic 看符号化栈。

## 这章咱们要点亮什么

五件事,围绕"崩了能看清是什么、在哪、怎么到这的":

1. **frame pointer 全局开**(`-fno-omit-frame-pointer`):Release `-O2` 下 RBP 链也完整,backtrace 在生产内核总可用。对齐 Linux `CONFIG_FRAME_POINTER=y`。
2. **kallsyms 符号查找**:`kallsyms_lookup(addr)` 二分查 `{addr,name}` 表,把裸地址翻成函数名。
3. **backtrace**:沿 RBP 链走栈,带防御(栈范围检查 + 深度上限 + 单调性)。
4. **统一 panic handler**:msg + 寄存器 + backtrace + 当前 task + 内存汇总 + halt,所有 fatal 异常收编到一处。
5. **`dump_memory_stats`**:PMM/Slab/PageCache 汇总,panic 时自动打。

## frame pointer:backtrace 的地基

x86-64 默认优化时把 RBP 当通用寄存器用(-O2 下),栈帧链就断了,backtrace 没法走。这一弧全局开 `-fno-omit-frame-pointer`,每个函数都老老实实 `push %rbp; mov %rsp,%rbp`,RBP 链在 `-O2` 下也完整。代价是少一个通用寄存器 + 一点开销——对教学/调试优先的内核完全值得。这一弧也是项目**首次在 `-O2` Release 下验证全绿**(之前一直 `-O0`),证明没有优化暴露的 UB 崩溃。

## kallsyms:地址 → 函数名

`kernel/lib/kallsyms.hpp`:

```cpp
// kallsyms.hpp:44 —— 注入符号表(按 addr 升序)
void kallsyms_set_table(const KallsymEntry* entries, size_t count);
// kallsyms.hpp:65 —— 二分查 addr 对应的函数名
bool kallsyms_lookup(uint64_t addr, char* buf, size_t len);
```

对齐 Linux 的 `kallsyms_lookup_name`。一个**关键解耦**:lookup 模块只认"注入进来的表",**不管表从哪来**。生产里真正的符号表靠 build 时 `nm` 抽出来嵌入(那是一步 follow-up,叫 1b);在 1b 就绪之前,`kallsyms_lookup` 找不到就返 false,backtrace 显示裸地址。因为模块和注入源解耦了,1b 可以**后加,不阻塞** backtrace/panic。

## backtrace:沿 RBP 链走,但别用 translate

`kernel/arch/x86_64/backtrace.hpp`,核心是 `backtrace_capture()`(纯函数,填一个地址数组,可单测)+ `backtrace_from()`(符号化打印)。

走 RBP 链有讲究:**终止条件用栈范围检查,不是 `VMM::translate()`**。原因是 `translate()` 有两个坑——它不支持 huge 页(返 0,direct-map/boot 栈这些 huge 映射区直接误判),而且它取锁,panic 路径里再取锁有死锁风险。所以改成:检查 RBP 是否落在"当前 task 栈 / boot 栈"范围内,加深度上限,加单调性(`next_rbp > cur_rbp`,链不能往回走)。这样无 lock、对 huge 有效、IF=0 安全。

> 教训:凡依赖 `translate()` 判页 present 的代码,对 huge 映射区(boot 栈、direct-map)都会失效。backtrace 这种 panic 路径的东西更得避开它(取锁死锁)。

## 统一 panic:崩了一站式交代

`panic(frame, name, vec, fmt, ...)` 收编了原来散落的 `dump_registers`/`kpanic`/`fatal_halt`。一次 panic 交代五样:消息(你给的 fmt)、寄存器 dump、backtrace(经 kallsyms 符号化)、当前 task、`dump_memory_stats`(PMM/Slab/PageCache 汇总),然后 halt。所有 fatal 异常(`#DE..#SS`/`#GP`/致命 `#PF`)和 `kpanic` 都收编到这一处。双 sink(serial + framebuffer)由 kprintf 多 sink 天然提供——VNC 和串口日志都能看到。

## 几个小坑

- **`kprintf` 不支持 `%zu`**(freestanding 子集):backtrace 里写 `(%zu frames)` 会输出字面 "%zu"。统一改 `%u` + `(unsigned)` cast。`%x` 是 uint64、`%u` 是 unsigned int,没有 `%l`/`%z` 变体。
- **backtrace 单测要防 tail-call**:`-O2` 会把 `middle(){leaf();}` 优化成 `jmp`(tail-call,不留帧),RBP 链帧数不足。测试函数加 `__attribute__((noinline))` + asm barrier(`__asm__ volatile("":::"memory")`)强制留帧。

## 验证

```bash
# kallsyms + backtrace + panic + memstats
grep -n 'kallsyms_set_table\|kallsyms_lookup' kernel/lib/kallsyms.hpp
grep -n 'backtrace_capture\|backtrace_from' kernel/arch/x86_64/backtrace.hpp
grep -rn 'dump_memory_stats' kernel/mm/diagnostics.hpp
grep -n 'fno-omit-frame-pointer' cmake/*.cmake CMakeLists.txt kernel/CMakeLists.txt 2>/dev/null
```

构建 + 测试:

```bash
cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

C 档的端到端验证是**触发一次 panic**:在内核里故意 `kpanic("test")` 或访问非法地址,看输出里有没有符号化的 backtrace(每帧 `函数名+偏移`)、寄存器 dump、内存汇总。在 1b 符号注入就绪前,backtrace 显示的是裸地址(配合 host 端 `addr2line -e build/kernel/... <addr>` 符号化)。

## 小结与下一站

可观测性补齐了:崩了能看清调用链、寄存器、内存状态。这是进 F3(信号/clone/futex/SMP 强并发)前必须先夯的——那之后 bug 不再是"安静地错了",而是"崩出一屏可读的栈"。

下一站 **045** 进 F3 进程弧:POSIX 信号(投递 + sigreturn)。那是 A 档(用户能捕获 SIGSEGV),也是和这一弧 frame pointer / panic 耦合的地方——信号栈帧要建在用户栈上。
