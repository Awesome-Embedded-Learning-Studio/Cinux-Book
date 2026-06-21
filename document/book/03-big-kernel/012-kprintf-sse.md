---
title: 012 · kprintf 重构与引导期 SSE 初始化
---

# 012 · kprintf 重构与引导期 SSE:在大步前进前夯实两个地基

> 得先跟你交个底:这一章的 tag 在仓库里叫 `012_driver_serial`,名字带个 serial,但它的真实改动里**一行串口驱动的新代码都没有**。serial 的底层输出(IO 到 COM1)早在更早就就位了,kprintf 一直靠它吐字。这一章真正干的两件事,是把 kprintf 这个「内核唯一的诊断通道」从里到外重做了一遍,顺手修掉了一个只在 `-O2` 下才会让内核连 IDT 都加载不出来的崩溃。它是个插曲——在大步前进之前,先把两个地基夯实。

## 这一章我们要点亮什么

两件看得见的事。

第一件,kprintf 终于像个 printf 了。011 的 kprintf 其实已经能 `%d`/`%u`/`%x`/`%X`/`%p`,也有 `%Nd`/`%0Nd` 的宽度与零补;这一章真正补的是**左对齐**(`%-`)、把负数零补的坑修对(`-00042` 而不是 `000-42`),并顺手把引擎抽成 header-only 模板让它能在 host 上单测。于是你能写出 `kprintf("%-8x", 0xdead)` 这种带对齐的诊断输出,而不必自己手拼字符串。

第二件,稍微惊悚一点:`-O2`(Release)构建的内核,以前根本起不来。它在加载 IDT 的当口安静地 Triple Fault,QEMU 直接退出,连一句遗言都不留。这一章把它修了,从那以后无论 Debug 还是 Release,内核都能稳稳跑到 idle loop。

## 为什么现在需要它

先说为什么是现在。011 之后内核已经能响应时钟中断,接下来 013 就要往屏幕上画字、动 framebuffer,用户态、文件系统也在不远处。可一旦系统复杂起来,**调试就全压在 kprintf 一根线上**——它就是我们的眼睛。这时候如果 kprintf 连个对齐都打不好、连个指针都格式化不利索,后面每一处排查都会被它拖累。所以在往系统里堆新东西之前,先花一章把 kprintf 做扎实,是一笔稳赚的投资。

重构的过程中,一个潜伏的 bug 被顺带逼了出来。我们想把 kprintf 的格式化引擎抽出来做单元测试,测试要在 host 上用 `-O2` 编译(因为那才是发布内核会跑的优化级别)。结果一编——内核在 `idt_init` 里 Triple Fault 了。这不是 kprintf 的锅,但它像一面镜子,照出了一个从 010 起就一直藏在那儿、只是之前没被 `-O2` 照到的硬件初始化缺陷:引导期从没初始化 SSE。

这就是这一章为什么是「两个地基」:一个是软件地基(kprintf),一个是硬件地基(SSE),它俩凑在一个 tag 里,因为正是在给 kprintf 做 `-O2` 单测的时候,后者才暴露。

## 设计图

kprintf 重构后的形状,核心是「格式化引擎」和「输出后端」彻底分家,中间靠一个回调连起来:

```text
  ┌─────────────────────────────────────┐
  │  vkprintf_impl<OutputFn>(out,fmt,va) │  ← 硬件无关,header-only 模板
  │  纯逻辑:解析 % 宽度 对齐,格式化数字  │     kernel/lib/private/vkprintf_impl.hpp
  └───────────────┬─────────────────────┘
                  │ 每产出一个字符 → out(c)
                  ▼
  ┌─────────────────────────────────────┐
  │  回调 lambda                          │
  │  [&](char c){ g_serial.putc(c); }     │  ← big kernel:喂给串口
  └───────────────┬─────────────────────┘
                  ▼
  ┌─────────────────────────────────────┐
  │  输出后端                             │
  │  big kernel: Serial(COM1)            │     host 单测: std::string
  │  (013 之后还会多一路: framebuffer)   │
  └─────────────────────────────────────┘
```

同一个引擎,big kernel 喂给串口,host 单测喂给一个 `std::string`。013 之后还会多一路屏幕——到那时你就明白这层回调当初为什么要抽出来了。

SSE 那条线是另一回事,它埋在 boot 流程最开头:

```text
  mini kernel boot.S _start
     │  cli
     │  ★ 设 CR4.OSFXSR(bit9) + CR4.OSXMMEXCPT(bit10) + clts   ← 本章新增
     │  ... 进入长模式、加载 big kernel ...
     ▼
  big kernel main: kprintf_init → GDT → IDT(idt_init 内部清零,可能被 -O2 向量化为 SSE)
```

关键在于这个 ★ 出现在内核**最早的可执行指令**处。它的意义下面讲 SSE 那节展开。

## 代码路线

### kprintf:把格式化引擎抽出来,用回调解耦输出后端

重构前,kprintf.cpp 是一坨:格式解析、数字转换、串口输出,全挤在一个函数里。这有两个坏处——格式逻辑没法脱离串口硬件做测试,而且将来想往屏幕也输出一份,就得把整坨抄一遍。

重构把它们拆开了。引擎变成一个 header-only 的模板,藏在 `kernel/lib/private/vkprintf_impl.hpp`:

```cpp
template <typename OutputFn>
void vkprintf_impl(OutputFn&& putc_fn, const char* fmt, va_list args) {
    // 纯逻辑:遍历 fmt,遇 % 解析标志/宽度/类型,产出的每个字符都交给 putc_fn
    ...
}
```

它只认一个 `putc_fn` 回调,至于这个字符最终去串口还是去屏幕、去测试缓冲区,引擎一概不管。而 kprintf.cpp 瘦成了一层薄包装,就剩三个委托:

```cpp
static Serial g_serial(SERIAL_COM1);          // big kernel 的单例串口

void kprintf(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);   // 回调喂串口
    va_end(args);
}
```

`kvprintf`、`kpanic` 同理,都是「`vkprintf_impl` + 一个喂串口的 lambda」。你以后想加一份屏幕输出,不用碰引擎,只要再调一次 `vkprintf_impl`、换一个喂 framebuffer 的 lambda 就行。这就是回调解耦的红利。

### 格式能力清单:这次补全了哪些 specifier

引擎支持的格式,正好够一个内核诊断用,也明确地**不**假装支持那些它没有的:

```text
  %%        字面量百分号
  %c        字符
  %s        字符串(nullptr 会安全地打成 "(null)",不会崩)
  %d %u     有符号 / 无符号十进制
  %x %X     小写 / 大写十六进制(不带 0x 前缀)
  %p        指针,固定 16 位大写十六进制 + "0x" 前缀
  宽度修饰   %Nd 右对齐空格补 | %0Nd 零补 | %-Nd 左对齐 | %-Ns 字符串左对齐
```

`%p` 强制 16 位是因为在 64 位下,指针就该长那样,`0x000000000000dead` 一眼能对上位,比省略前导零更不容易看错。而 `%f` 浮点、`%lld` 长度修饰、`%.3f` 精度这些,引擎统统没有——内核里几乎用不到浮点,硬塞进去只会徒增体积和 bug 面。把边界划清楚,比假装无所不能有用得多。

### 零补与左对齐:负数那个小坑

宽度处理里有个容易写错的细节,值得拎出来讲。`%-10d` 左对齐、`%08x` 零补,这些直觉上没问题。坑在负数零补:`%06d` 格式化 `-42`,你要的结果是 `-00042`,而不是 `000-42`。也就是说,符号得待在最前面,后面才是零,再后面才是数字。引擎里专门为此分了一个支:

```cpp
bool has_sign = (len > 0 && buffer[0] == '-');
if (!left_align && zero_pad && has_sign) {
    // 先吐符号,再零补到宽度,最后吐数字
    putc_fn('-');
    for (int i = digits_len; i < width - 1; i++) putc_fn('0');
    putc_fn(/* 数字部分 */);
}
```

这种小地方不专门处理,出来的字符串就是错的,而它还不会报错——你只会在日志里看到一个诡异的 `000-42`,然后花半小时怀疑别处。把它写对、再用单测焊死,就省了这半小时。

顺带一提,数字转换里还有个 `INT64_MIN` 的特判:`0x8000000000000000` 取反会溢出,不能直接 `-value`,得单独走一条路径。这是写 itoa 类函数的老朋友了,但漏掉的话,打印最小负数就会得到一个正数。

### 为什么 host 单测能直接测内核格式化引擎

这大概是这次重构最值钱的一笔。因为引擎是 header-only 模板、且只依赖一个回调,host 侧的单测只要 include 它、喂一个往 `std::string` 里追加的回调,就能直接测内核的格式化逻辑——**完全不用模拟串口硬件**:

```cpp
std::string do_printf(const char* fmt, ...) {
    std::string out;
    va_list args; va_start(args, fmt);
    cinux::lib::detail::vkprintf_impl([&](char c) { out.push_back(c); }, fmt, args);
    va_end(args);
    return out;
}

TEST("kprintf: %08x zero-pad hex") {
    ASSERT_EQ(do_printf("%08x", 0xFFu), "000000ff");
}
```

`test_kprintf.cpp` 就这么写了四十来个用例,把每一个 specifier、每一种宽度对齐、`nullptr` 字符串、负数零补、混合格式、甚至未知 specifier 的兜底,全测了一遍。以前要验证 kprintf 对不对,只能把内核跑起来盯着串口看;现在它和普通库函数一样可以 `ctest` 一键验证。更妙的是,这批测试正是用 `-O2` 编的——而 SSE 的崩溃,就是在这一刻被照出来的。

### SSE:一个只在 -O2 才炸的 Triple Fault

故事来了。给 kprintf 写 host 单测时,顺带把整个内核测试也用 Release(`-O2`)编了一遍,结果小内核在 IDT 初始化阶段 Triple Fault,QEMU 直接退出。诡异的是 `-O0`(Debug)下一切正常,而且之前 GDT、kprintf、C++ 运行时的测试全过。

定位它靠的是两个老办法。第一个是 **debugcon 标记法**:在 `idt_init` 的各个步骤之间,插一句 `outb $0xE9, '某字符'`,把执行进度打到 QEMU 的 debug 日志。结果日志停在 `idt_init` 的清零循环那一步,之后再无标记——崩溃点被精确钉死在「第一个会用上 SSE 指令的地方」。

第二个是 **反汇编对比**。`-O2` 把那个把 IDT 结构体数组清零的循环,向量化成了一串 SSE 指令:

```asm
; -O2 生成的 idt_init 清零循环
pxor   %xmm0, %xmm0          ; ← 第一条 SSE 指令,崩在这
movaps %xmm0, (%rcx,%rdx,1)  ; 16 字节对齐写入
```

而 `-O0` 生成的是逐字节的普通 store,根本不碰 SSE。这就解释了为什么只有 `-O2` 崩。

根因是控制寄存器。在崩溃前把 CR0、CR4 读出来打到 debugcon,看到 `CR4 = 0x...20`——只有 PAE 位被置上,而 OSFXSR(bit 9)是 0。按 Intel SDM 的规定,SSE2 这类 128 位指令在 `CR4.OSFXSR = 0` 时会触发 `#UD`(非法操作码,向量 6)。于是链条就清楚了:

```text
boot 入口 → cli →(从未设 CR4.OSFXSR)→ ... → idt_init
                                              ↓
                                        pxor %xmm0
                                              ↓
                                  CR4.OSFXSR = 0  →  #UD (vector 6)
                                              ↓
              可此刻 IDT 还没 lidt(limit = 0),连 #UD 自己都找不到 handler
                                              ↓
                                        Triple Fault → QEMU -no-reboot → exit(0)
```

这里有个容易误解的地方:64 位长模式**硬件上**确实支持 SSE,这是架构强制要求的。但「硬件支持」不等于「OS 已启用」——CPU 仍然要检查 `CR4.OSFXSR`,这位不设,128 位 SSE 指令就一律当非法指令处理。两者不冲突,只是很多人(包括我们之前的 boot 代码)默认以为进了长模式 SSE 就自动能用了,这是个常见误区。

另一个细节:为什么是 Triple Fault 而不是看到一个 `#UD` 的异常输出?因为 `pxor` 崩在 `idt_init` **内部**——IDT 正在被清零、还没 `lidt` 加载,此时 IDT 的 limit 还是 0。#UD 找不到 handler,又没法进一步处理,直接一路升级成 Triple Fault。这也解释了它为什么崩得那么「安静」:连异常处理都还没就绪,自然吐不出任何东西。

### 修复:在内核最早的入口把 CR4 拨好

修法很直接,但位置讲究。在 `kernel/mini/arch/x86_64/boot.S` 的 `_start`,紧跟在 `cli` 之后,把 SSE 相关的控制位一次性设好:

```asm
_start:
    cli

    /* Enable SSE: set CR4.OSFXSR (bit 9) and CR4.OSXMMEXCPT (bit 10) */
    movq %cr4, %rax
    orq $(1 << 9), %rax          /* OSFXSR: enable FXSAVE/FXRSTOR 管理 SSE 状态 */
    orq $(1 << 10), %rax         /* OSXMMEXCPT: 允许 SIMD 浮点异常传递为 #XF */
    movq %rax, %cr4
    clts                          /* 清 CR0.TS,不依赖 BIOS/KVM 的初始值 */
```

为什么放在 mini kernel 的 `boot.S` 而不是 big kernel 的 `main`?因为 `boot.S` 是整个内核链上**最早**的可执行点。`-O2` 可能在任何函数里生成 SSE 指令,越早把这位置好,后面所有代码——包括 mini kernel 自己、包括它加载的 big kernel——就都安全了。`clts` 顺手清掉 `CR0.TS` 也是同样的道理:不依赖 QEMU/BIOS 给 CR0 的初始值,把状态握在自己手里。

(完整版的排查过程——debugcon 标记法的具体输出、CR0/CR4 的读出值、`-O0` vs `-O2` 的指令对比表——我们另起一篇 debug-notes 收着,这里只走主线。)

## 调试现场

这一章真正的「调试现场」就是上面那段 SSE 排查,它已经融在代码路线里讲了。这里只补两条这次沉淀下来的、以后会反复用到的经验。

一是 **debugcon**(port 0xE9)标记法值得当成看家手艺。内核崩得没声音的时候,在关键路径上插 `outb 0xE9` 打标记,再读 `debug.log`,是定位「崩在哪一步」最快的办法——比插 kprintf 强,因为 kprintf 本身可能还没初始化(这次就崩在 IDT 之前,串口虽然有,但更早的崩溃根本走不到 kprintf)。

二是「`-O0` 正常、`-O2` 崩」这种信号,要先怀疑某个硬件特性没被初始化。优化级别一变,代码就崩,根因往往是编译器在 `-O2` 下用上了某种 `-O0` 不用的指令(SSE/AVX、向量化内存操作、特定的寻址方式),而这些指令依赖某个没设好的控制位或状态。下次再撞上「换个优化级别就崩」,先往这个方向想,别急着怀疑自己的逻辑。

## 验证

kprintf 的格式化逻辑,直接跑那批 host 单测,不依赖 QEMU:

```bash
ctest --test-dir build -R kprintf --output-on-failure
```

四十来个用例覆盖了所有 specifier、宽度对齐、`nullptr`、负数零补、混合格式和未知 specifier 兜底。它们绿的,格式化引擎就是对的。

SSE 的修复,验证方式反过来——得用 `-O2` 把内核编出来跑,确认它不再 Triple Fault:

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release <其余参数>
cmake --build build-release --target run-kernel-test
```

修复前这会在 `idt_init` 阶段 `QEMU unexpected exit code: 0`;修复后能稳稳跑完全部 22 项内核测试并 `exit 0`(经 isa-debug-exit 的正常退出)。顺带,`run` 起来后你会先看到一段 kprintf 的格式回归输出——那是 main 里新加的那 19 行,把 `%08x`、`%-10s`、`%p` 这些实打实打了一遍:

```text
[KPRINTF] %08x: 0000dead
[KPRINTF] %-10d: 42        |
[KPRINTF] %p: 0x00001234ABCD5678
[KPRINTF] mix: test n=99 hex=cafebabe ptr=0x0000000000000001
```

## 下一站

地基夯实了,该往上盖了。到现在为止,内核所有的输出还只走串口一条路——你得开着 QEMU 的串口窗口才看得见它在说什么。下一个 tag 013 终于要把 framebuffer 接上,让内核能直接在屏幕上画字: framebuffer 驱动、字体、console。到那时,这一章给 kprintf 抽出来的那个回调式引擎,会迎来它的第二个输出后端——屏幕。我们早就为这一天留好了接口。

顺带,013 还会把 drivers 目录理一理(serial、pit 各自挪进自己的子目录),那个一直被 tag 名挂着、却没在这一章出现的「serial driver 化」,到那时才算真正落地。

---

### 参考

- Intel SDM Vol.3(System Programming,控制寄存器):`CR4.OSFXSR`(bit 9)、`CR4.OSXMMEXCPT`(bit 10)、`CR0.TS/EM/MP`。本地 PDF `document/reference/intel/SDM-Vol3A-*.pdf`,可用 `pdf-reader` 搜索 "OSFXSR"/"OSXMMEXCPT" 复核位号。
- Intel SDM Vol.2(指令参考):SSE2 指令(`PXOR`/`MOVAPS`)在 `CR4.OSFXSR = 0` 时触发 `#UD` 的规则;`CLTS`、`FXSAVE`/`FXRSTOR` 语义。
- OSDev — QEMU exit devices / isa-debug-exit:退出码 `(value<<1)|1` 恒为奇数,退出码 0 在 `-no-reboot` 下表示 Triple Fault。
- 本 tag 源码:[vkprintf_impl.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/lib/private/vkprintf_impl.hpp)、[kprintf.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/lib/kprintf.cpp)、[kprintf.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/lib/kprintf.hpp)、[boot.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/arch/x86_64/boot.S)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp);测试 [test_kprintf.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_kprintf.cpp);排查笔记 [012-01-sse-init-crash-o2.md](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/document/notes/012/012-01-sse-init-crash-o2.md)。
