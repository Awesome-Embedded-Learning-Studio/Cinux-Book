---
title: Lab 012 · 给 kprintf 补全格式 + 让 Release 内核起得来
---

# Lab 012 · 给 kprintf 补全格式 + 让 Release 内核起得来

> 配套章节：[012 · kprintf 重构与引导期 SSE 初始化](../../book/03-big-kernel/012-kprintf-sse.md)。这一关不贴答案，给目标、约束和一个等着你修的 `-O2` 崩溃。

## 实验目标

两条线，都要拿下。

第一条，kprintf 要像个 printf：`%08x`、`%-10s`、`%p` 这些格式化能用，且 `nullptr` 字符串不会让内核崩。而且这套格式化逻辑得能在 host 上用单测验证，不用每次都启 QEMU 盯串口。

第二条，把一个潜伏的崩溃修掉：用 `-O2`（Release）把内核编出来跑，它会 Triple Fault。你要找到根因并修掉它，让 Release 构建也能稳稳跑到 idle loop。

## 前置条件

你得先过 Lab 011：内核能在 `sti` 之后稳定 tick、`int $3` 仍能正常返回。这一关会重新动 kprintf（内核的眼睛）和 boot 入口（内核最早的指令），没把 011 的中断链走稳，这里排错会混乱。

另外提醒一句：这一关的 tag 叫 `012_driver_serial`，但实际没有串口驱动的活——serial 早就有了。别被名字带偏去找串口的事，真正的活是 kprintf 和 SSE。

## 任务分解

**第一步**，把 kprintf 的格式化引擎从串口里拆出来，目标是得到一个不依赖任何硬件的格式化核心：它吃一个「输出单个字符」的回调，产出的每个字符都交给这个回调，引擎本身不碰串口。想想用什么语言机制能让它在 big kernel 和 host 测试里复用同一份代码。

**第二步**，给引擎补全格式能力，至少要覆盖 `%d %u %x %X %p %c %s %%`，外加宽度 `%Nd`、零补 `%0Nd`、左对齐 `%-Nd`、字符串左对齐 `%-Ns`。`%p` 要固定 16 位十六进制带 `0x`。注意两个坑：负数零补时符号要顶在最前面（`%06d` 打 `-42` 得 `-00042`），以及最小负整数取反会溢出。

**第三步**，给引擎写 host 单测，既然引擎能脱离硬件，就喂它一个往 `std::string` 追加字符的回调，把每个 specifier、每种宽度对齐、`nullptr`、负数零补、混合格式、未知 specifier 的兜底，都测一遍。这批测试要用 `-O2` 编。

**第四步**，修掉 `-O2` 的 Triple Fault，第三步用 `-O2` 编测试时，内核多半会崩在 IDT 初始化。你要定位它（想想在没有可靠输出的早期崩溃里，怎么用 debugcon 打标记）、找到根因（对比 `-O0` 和 `-O2` 生成的指令，读一读 CR0/CR4），然后在内核最早能执行的地方把那个没设好的控制位设上。

## 接口约束

你要实现出来的东西，对外长这样（只给职责，不给实现）：

- `vkprintf_impl<OutputFn>(OutputFn&& out, const char* fmt, va_list args)`：硬件无关的格式化引擎，模板，header-only。每个产出的字符通过 `out(c)` 交出去。
- `kprintf(fmt, ...)` / `kvprintf(fmt, va_list)` / `kpanic(fmt, ...)`：三个薄包装，内部都委托 `vkprintf_impl`，回调喂给串口。`kpanic` 打完进 `cli; hlt` 永不返回。
- 引擎支持的格式清单见上面第二步；明确**不**支持 `%f`、`%lld`、精度——把边界划清楚。

SSE 那部分，你要在 mini kernel 的 boot 入口、`cli` 之后，把控制寄存器拨到「SSE 可用」的状态。具体哪些位，去 Intel SDM 查 CR4 和 CR0（提示：一个位管「OS 支持 FXSAVE/FXRSTOR」，一个位管「SIMD 浮点异常」，CR0 里还有一个「任务切换」位要用 `clts` 清掉）。

## 验证步骤

kprintf 的格式逻辑，跑 host 单测，不依赖 QEMU：

```bash
ctest --test-dir build -R kprintf --output-on-failure
```

SSE 的修复，反过来——必须用 `-O2` 把内核编出来跑，确认它不再崩：

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release <其余参数>
cmake --build build-release --target run-kernel-test
```

修复前这会在 IDT 阶段 `QEMU unexpected exit code: 0`；修复后能跑完全部内核测试并正常退出。想看 kprintf 的实际输出，`make run`（或对应 CMake target）起来后留意 main 里那段格式回归打印。

## 常见故障

- **`-O2` 在 IDT 阶段 Triple Fault**：这是这一关的核心。先别怀疑自己的逻辑——优化级别一变就崩，先往「编译器在 `-O2` 下用上了某种 `-O0` 不用的指令，而它依赖某个没初始化的硬件状态」这个方向查。SSE 是头号嫌疑。
- **崩溃没声音**：因为崩在 IDT 之前/之中，异常处理还没就绪。用 debugcon（port `0xE9`）在关键步骤打标记，读 debug 日志定位崩在哪一步。
- **`%p` 位数不对**：64 位下指针该是 16 位十六进制，少了前导零容易看错地址。检查你是不是按「固定宽度」处理的，而不是「够用就行」。
- **负数零补成 `000-42`**：符号没顶到最前面。单独处理「负数 + 零补」这一支：先吐符号，再零补，最后数字。
- **`%s` 传 nullptr 崩**：引擎没防 nullptr。加一个兜底，把它打成 `(null)`。

## 通过标准

1. kprintf 的 host 单测全绿，覆盖所有 specifier、宽度对齐、`nullptr`、负数零补、混合格式、未知 specifier 兜底。
2. `CMAKE_BUILD_TYPE=Release`（`-O2`）构建的内核能跑过 IDT 初始化、进到 idle loop、稳定 tick，不再 Triple Fault。
3. main 里那段 kprintf 格式回归输出，`%08x`、`%-10s`、`%p` 等格式都对。
4. 引擎是 header-only + 回调式，big kernel 和 host 单测共用同一份代码（不是抄了两份）。

做到这四条，你的内核就既有了趁手的诊断工具，又能在发布优化级别下起得来。下一关，我们让它的输出不止走串口——上屏幕。
