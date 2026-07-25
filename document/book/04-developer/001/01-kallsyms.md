---
title: 01 · kallsyms 与 panic backtrace
---

# kallsyms 与 panic backtrace

> 前面内存弧那一串难缠的 bug(direct-map 走错、嵌套虚拟化的可见性问题、缓存陈旧命中),基本是靠临时到处塞 `kprintf` 一行行定位的。可接下来要做信号、线程、多核——这些强并发、容易死锁的东西,光靠 `kprintf` 不够:**内核崩了,得能看清它崩在哪、怎么走到那的、当时内存什么状态**。这一章补的就是这套可观测性:frame pointer 打底、符号查找、栈回溯、统一的 panic 处理。

## 地基:frame pointer

x86-64 在优化时(`-O2`)默认把 `rbp` 当通用寄存器用,函数的栈帧链就断了,没法回溯。这一章全局打开 `-fno-omit-frame-pointer`,让每个函数都老老实实 `push %rbp; mov %rsp,%rbp`,栈帧链在 `Release` 构建下也完整。代价是少一个通用寄存器、一点点开销——对一个"可调试优先于性能"的教学内核,完全值得。有了完整的栈帧链,后续即便开 `-O2` 优化也不会因为未定义行为把回溯打断。

## kallsyms:地址翻成函数名

内核崩溃时,栈回溯给出的是一串**裸地址**。光看 `0xFFFFFFFF80123ABC` 你不知道是哪个函数。`kernel/lib/kallsyms.hpp` 提供地址到符号的查找,对齐 Linux 的 `kallsyms_lookup_name`:

```cpp
// kallsyms.hpp —— 注入一张按地址升序排好的 {addr, name} 表
void kallsyms_set_table(const KallsymEntry* entries, size_t count);
// 二分查找:地址 → 函数名
bool kallsyms_lookup(uint64_t addr, char* buf, size_t len);
```

一个**关键的解耦**:查找模块只认"注入进来的表",不管表从哪来。生产内核的表,是构建时用 `nm` 从内核镜像抽出来的符号,嵌入进去(这一步是后续工作);在它就绪之前,`kallsyms_lookup` 找不到就返 false,回溯显示裸地址。因为模块和注入源解耦了,真正的符号注入可以**后加,不阻塞**回溯/panic——回溯框架先能用起来。

## 栈回溯:沿 rbp 链走,但别用 translate

`kernel/arch/x86_64/backtrace.hpp` 的核心是 `backtrace_capture()`(纯函数,填一个地址数组,能单测)+ `backtrace_from()`(符号化打印)。它沿着 rbp 链一路走,但**终止条件用栈范围检查,不是 `VMM::translate()`**。

为什么避开 `translate`?两个坑:它**不支持大页**(direct-map、boot 栈这些大页映射区直接返 0,误判);而且它**取锁**,panic 路径里再取锁有死锁风险。所以改成检查 rbp 是否落在"当前任务栈 / boot 栈"的范围内,加深度上限、加单调性(链不能往回走)。这样无锁、对大页有效、在中断关闭的状态下也安全。

> 教训:凡是用 `translate()` 判"页在不在"的代码,对大页映射区都会失效。而 panic 路径的东西更得避开它(取锁死锁)。

## 统一的 panic:一站式交代

`panic(...)` 把原来散落的几个处理函数(`dump_registers`/`kpanic`/`fatal_halt`)收编到一处。一次 panic 交代五样:**消息**(调用方给的格式串)、**寄存器转储**、**栈回溯**(经 kallsyms 符号化)、**当前任务**、**内存汇总**(`dump_memory_stats`:物理分配器/slab/Page Cache 的占用概况),然后停机。所有致命异常(除零、一般保护错、致命缺页)和 `kpanic` 都汇到这里。串口和屏幕两个输出由 `kprintf` 的多通道天然覆盖——VNC 和串口日志都能看到。

## 几个小坑

- **`kprintf` 不支持 `%zu`**(它是 freestanding 子集)。回溯里写 `(%zu frames)` 会原样输出字面 `%zu`。统一改 `%u` + `(unsigned)` 强转。它只有 `%x`(64 位)、`%u`(unsigned)、`%d`,没有 `%l`/`%z` 变体。
- **回溯的单测要防尾调用**。`-O2` 会把 `middle(){ leaf(); }` 优化成 `jmp leaf`(尾调用,不留栈帧),栈帧链帧数不足。测试函数加 `__attribute__((noinline))` + 一个 asm 内存屏障,强制留帧。

## 验证

```bash
grep -n 'fno-omit-frame-pointer' CMakeLists.txt cmake/*.cmake 2>/dev/null
grep -n 'kallsyms_set_table\|kallsyms_lookup' kernel/lib/kallsyms.hpp
grep -n 'backtrace_capture\|backtrace_from' kernel/arch/x86_64/backtrace.hpp
grep -rn 'dump_memory_stats' kernel/mm/diagnostics.hpp
```

构建:

```bash
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
```

想亲眼看这套东西的价值:在内核里临时加一句 `kpanic("test")`(或访问一个非法地址),看输出里有没有符号化的栈回溯、寄存器转储、内存汇总。符号注入就绪前,回溯里是裸地址——拿去 host 端 `addr2line -e build/kernel/.../big_kernel <地址>` 翻译。**看完记得删掉触发代码。**
