以下是为 Format Engine 组件编写的完整中文 API 文档：

---

# Format Engine

> 硬件无关格式化引擎（Logger 内部使用）

## 头文件

`#include <cinux/detail/vformat.hpp>`

## 依赖

`<cstdarg>` `<cstddef>` `<cstdint>`

## 概述

`cinux::lib::detail::vformat` 是一套轻量级的、不依赖标准库 `printf` 系列函数的格式化引擎。它最初从内核代码 `kernel/lib/private/vkprintf_impl.hpp` 迁移而来，设计目标是提供一个硬件无关、可安全运行在裸机（bare-metal）及内核环境中的格式化实现。

该引擎采用模板化架构：核心格式化逻辑以模板函数 `vformat_impl` 实现，接受一个用户提供的逐字符输出回调（`OutputFn`）。这使得底层输出目标完全可定制——既可以写入固定大小的字符缓冲区，也可以直接输出到 UART 等硬件设备。在此基础上，`vformat.cpp` 提供了一个缓冲区写入封装 `vformat_to_buf`，供 Logger 组件直接使用。

引擎支持的格式说明符涵盖日常内核/嵌入式开发所需的绝大部分场景：`%%`、`%c`、`%s`、`%d`、`%u`、`%x`、`%X`、`%p`，并支持 `l` / `ll` 长度修饰符、宽度字段以及 `-` 左对齐和 `0` 零填充标志。

## API 参考

### 命名空间

所有符号位于命名空间 `cinux::lib::detail` 中。

---

### `format_decimal`

将一个有符号 64 位整数格式化为十进制字符串。

```cpp
inline int format_decimal(int64_t value, char* buffer, int buffer_size);
```

**参数**

| 参数 | 类型 | 说明 |
|------|------|------|
| `value` | `int64_t` | 待格式化的有符号整数 |
| `buffer` | `char*` | 输出字符缓冲区 |
| `buffer_size` | `int` | 缓冲区大小（字节） |

**返回值**

`int` —— 实际写入缓冲区的字符数（不含末尾 `\0`）。若 `buffer_size < 1`，返回 `0`。

**说明**

- 对 `INT64_MIN`（`-9223372036854775808`）做了特殊处理，直接拷贝字面量字符串，避免取负溢出。
- 输出始终以 `\0` 结尾。

**示例**

```cpp
char buf[32];
int n = cinux::lib::detail::format_decimal(-42, buf, sizeof(buf));
// buf == "-42", n == 3
```

---

### `format_unsigned`

将一个无符号 64 位整数格式化为十进制字符串。

```cpp
inline int format_unsigned(uint64_t value, char* buffer, int buffer_size);
```

**参数**

| 参数 | 类型 | 说明 |
|------|------|------|
| `value` | `uint64_t` | 待格式化的无符号整数 |
| `buffer` | `char*` | 输出字符缓冲区 |
| `buffer_size` | `int` | 缓冲区大小（字节） |

**返回值**

`int` —— 实际写入缓冲区的字符数（不含末尾 `\0`）。若 `buffer_size < 1`，返回 `0`。

**示例**

```cpp
char buf[32];
int n = cinux::lib::detail::format_unsigned(12345u, buf, sizeof(buf));
// buf == "12345", n == 5
```

---

### `format_hex`

将一个无符号 64 位整数格式化为十六进制字符串。

```cpp
inline int format_hex(uint64_t value, char* buffer, int buffer_size, bool lowercase);
```

**参数**

| 参数 | 类型 | 说明 |
|------|------|------|
| `value` | `uint64_t` | 待格式化的无符号整数 |
| `buffer` | `char*` | 输出字符缓冲区 |
| `buffer_size` | `int` | 缓冲区大小（字节） |
| `lowercase` | `bool` | `true` 使用小写字母 `a-f`，`false` 使用大写字母 `A-F` |

**返回值**

`int` —— 实际写入缓冲区的字符数（不含末尾 `\0`）。若 `buffer_size < 1`，返回 `0`。

**示例**

```cpp
char buf[32];
int n1 = cinux::lib::detail::format_hex(0xFF, buf, sizeof(buf), true);
// buf == "ff", n1 == 2

int n2 = cinux::lib::detail::format_hex(0xAB, buf, sizeof(buf), false);
// buf == "AB", n2 == 2
```

---

### `vformat_impl`

核心模板格式化引擎，逐一将格式化结果字符通过回调输出。

```cpp
template <typename OutputFn>
void vformat_impl(OutputFn&& putc_fn, const char* fmt, va_list args);
```

**参数**

| 参数 | 类型 | 说明 |
|------|------|------|
| `putc_fn` | `OutputFn&&` | 逐字符输出回调，签名为 `void(char)`。每产生一个输出字符即调用一次 |
| `fmt` | `const char*` | 格式化字符串（printf 风格，见下方支持的说明符） |
| `args` | `va_list` | 可变参数列表 |

**支持的格式说明符**

| 说明符 | 说明 | 示例 |
|--------|------|------|
| `%%` | 输出百分号字符 `%` | `"100%%"` → `"100%"` |
| `%c` | 输出单个字符 | `"%c", 'Z'` → `"Z"` |
| `%s` | 输出 C 字符串（`const char*`）；若传入 `nullptr` 则输出 `"(null)"` | `"hello %s", "world"` → `"hello world"` |
| `%d` | 输出有符号十进制整数 | `"%d", -7` → `"-7"` |
| `%u` | 输出无符号十进制整数 | `"%u", 12345u` → `"12345"` |
| `%x` | 输出无符号十六进制（小写） | `"%x", 0xFF` → `"ff"` |
| `%X` | 输出无符号十六进制（大写） | `"%X", 0xAB` → `"AB"` |
| `%p` | 输出指针值，固定前缀 `0x`，零填充至 16 位十六进制数字 | `"%p", (uint64_t)0x1234` → `"0x0000000000001234"` |

**长度修饰符**

| 修饰符 | 说明 | 适用说明符 |
|--------|------|------------|
| `l` | `long` / `unsigned long`（按 64 位读取） | `%ld` `%lu` `%lx` `%lX` |
| `ll` | `long long` / `unsigned long long`（按 64 位读取） | `%lld` `%llu` `%llx` `%llX` |

不带长度修饰符时，`%d` 读取 `int`，`%u`/`%x`/`%X` 读取 `unsigned int`。

**宽度与对齐标志**

| 语法 | 说明 |
|------|------|
| `%Nd` | 最小宽度为 `N`，右对齐，左侧填充空格 |
| `%0Nd` | 最小宽度为 `N`，右对齐，左侧填充 `0` |
| `%-Nd` | 最小宽度为 `N`，左对齐，右侧填充空格 |
| `%-Ns` | 字符串左对齐，右侧填充空格 |

宽度字段对 `%d`、`%u`、`%x`、`%X`、`%s` 均有效。

**示例**

```cpp
// 输出到 UART（伪代码）
vformat_impl([](char c) { uart_putc(c); }, "value=%d", args);

// 输出到自定义缓冲区
std::string result;
vformat_impl([&](char c) { result.push_back(c); }, "hex=%X num=%d", args);
```

---

### `vformat_to_buf`

将格式化结果写入固定大小的字符缓冲区，自动截断溢出部分并添加 `\0` 终止符。此函数在 `src/detail/vformat.cpp` 中定义。

```cpp
void vformat_to_buf(char* buf, size_t buf_size, const char* fmt, va_list args);
```

**参数**

| 参数 | 类型 | 说明 |
|------|------|------|
| `buf` | `char*` | 输出缓冲区指针 |
| `buf_size` | `size_t` | 缓冲区大小（字节），含 `\0` 终止符的空间 |
| `fmt` | `const char*` | 格式化字符串 |
| `args` | `va_list` | 可变参数列表 |

**返回值**

无。格式化结果直接写入 `buf`，始终以 `\0` 终止。

**说明**

- 若 `buf` 为 `nullptr` 或 `buf_size` 为 `0`，函数立即返回，不做任何操作。
- 当格式化结果超过 `buf_size - 1` 个字符时，超出部分被截断，缓冲区末尾保证为 `\0`。
- 内部使用 `va_copy` 复制 `va_list`，不消耗调用者的 `args`。
- 这是 Logger 组件实际使用的入口函数。

**示例**

```cpp
char buf[64];
va_list args;
va_start(args, fmt);
cinux::lib::detail::vformat_to_buf(buf, sizeof(buf), "count=%d name=%s", args);
va_end(args);
// 若 args 对应 (42, "test")，则 buf == "count=42 name=test"

// 缓冲区溢出时自动截断
char small[5];
va_start(args, fmt);
cinux::lib::detail::vformat_to_buf(small, sizeof(small), "abcdefgh", args);
va_end(args);
// small == "abcd"（4 字符 + '\0'）
```

**典型封装用法**

```cpp
// 通常封装为可变参数便捷函数
int my_snprintf(char* buf, size_t size, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    cinux::lib::detail::vformat_to_buf(buf, size, fmt, args);
    va_end(args);
    return static_cast<int>(std::strlen(buf));
}
```

---

## 注意事项

1. **非公开 API**：本组件位于 `cinux::lib::detail` 命名空间，属于内部实现细节，不保证跨版本 ABI / API 兼容性。外部代码不应直接使用。
2. **不依赖标准 I/O**：整个引擎不调用 `printf`、`sprintf`、`malloc` 或任何标准库 I/O 函数，适合在内核态和裸机环境中使用。
3. **缓冲区截断**：`vformat_to_buf` 在缓冲区不足时静默截断，不报告截断发生。调用方应确保缓冲区足够大，或自行检查返回长度。
4. **`%p` 固定宽度**：指针格式化始终输出 `0x` 前缀 + 16 位十六进制数字（零填充），适用于 64 位地址空间。不读取长度修饰符。
5. **浮点不支持**：引擎不支持 `%f`、`%e`、`%g` 等浮点格式说明符，以保持实现的轻量性和内核适用性。
6. **未识别的说明符**：遇到不支持的格式说明符时，引擎将原样输出 `%` 及该说明符字符。例如 `%f` 将输出 `%f`。
7. **`va_list` 所有权**：`vformat_impl` 不调用 `va_end`，调用者负责管理 `va_list` 的生命周期。`vformat_to_buf` 内部使用 `va_copy`，因此也不会消耗传入的 `args`。
8. **线程安全**：所有函数均操作调用者提供的缓冲区，不使用全局或静态可变状态，本身是线程安全的。但如同标准 `printf`，可变参数的正确传递由调用者负责。

## 另见

- Logger 组件 —— 本格式化引擎的上层消费者
- `<cstdarg>` —— C 可变参数机制
- `printf(3)` / `sprintf(3)` —— 标准 C 格式化函数（本引擎的语义参考）
- `kernel/lib/private/vkprintf_impl.hpp` —— 本引擎的内核原始版本