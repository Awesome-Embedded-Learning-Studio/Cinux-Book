---
title: 02 · kprintf 重构:回调解耦与格式能力
---

# kprintf 重构:回调解耦与格式能力

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
