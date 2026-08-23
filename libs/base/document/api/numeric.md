我现在已获取所有必要信息。这是完整的文档文件：

---

# Numeric

> 编译期数值工具 — 对齐、整除、2 的幂、内存字面量

## 头文件

`#include <cinux/numeric.hpp>`

## 依赖

无（叶子组件）

## 概述

`cinux::lib::numeric` 提供一组纯 `constexpr` 的数值工具函数，涵盖内存地址对齐、向上/向下取整除法、2 的幂次判定与上取整、以 2 为底的对数以及基于二进制前缀（KiB/MiB/GiB/TiB）的用户自定义字面量。所有函数均不依赖任何运行时库，适合在 `static_assert`、模板参数或常量表达式中使用。

在操作系统内核与底层系统编程场景中，大量逻辑需要对齐操作（例如页对齐 4096 字节）、将字节数向上取整到最近的 2 的幂（用于分配器 `buddy` 设计），以及可读的内存大小表示。该组件将这些高频需求抽取为轻量、零依赖、编译期可求值的工具集，避免各模块重复实现。

函数签名统一使用 `uint64_t` 作为地址与大小类型，与 64 位地址空间兼容；内部对 2 的幂对齐路径使用位运算加速，非 2 的幂对齐则回退到通用算术路径，保证正确性的同时兼顾性能。

## API 参考

### 对齐操作

#### `align_up`

```cpp
constexpr uint64_t align_up(uint64_t addr, uint64_t align);
```

将 `addr` 向上取整到 `align` 的最近整数倍。

**参数**
- `addr` — 待对齐的地址或数值。
- `align` — 对齐粒度，可以为任意正整数。当 `align` 为 2 的幂时，内部走位运算快速路径。

**返回值**
返回大于等于 `addr` 且为 `align` 整数倍的最小 `uint64_t` 值。若 `addr` 已经对齐，则返回 `addr` 本身。

**示例**

```cpp
using namespace cinux::lib;

static_assert(align_up(0, 4096) == 0);
static_assert(align_up(1, 4096) == 4096);
static_assert(align_up(4097, 4096) == 8192);

// 非 2 的幂对齐
static_assert(align_up(1, 3) == 3);
static_assert(align_up(4, 3) == 6);
```

---

#### `align_down`

```cpp
constexpr uint64_t align_down(uint64_t addr, uint64_t align);
```

将 `addr` 向下取整到 `align` 的最近整数倍。

**参数**
- `addr` — 待对齐的地址或数值。
- `align` — 对齐粒度，可以为任意正整数。当 `align` 为 2 的幂时，内部走位运算快速路径。

**返回值**
返回小于等于 `addr` 且为 `align` 整数倍的最大 `uint64_t` 值。若 `addr` 已经对齐，则返回 `addr` 本身。

**示例**

```cpp
using namespace cinux::lib;

static_assert(align_down(4095, 4096) == 0);
static_assert(align_down(4096, 4096) == 4096);
static_assert(align_down(4097, 4096) == 4096);
static_assert(align_down(8191, 4096) == 4096);
```

---

#### `is_aligned`

```cpp
constexpr bool is_aligned(uint64_t addr, uint64_t align);
```

判断 `addr` 是否为 `align` 的整数倍。

**参数**
- `addr` — 待检测的地址或数值。
- `align` — 对齐粒度。

**返回值**
若 `addr` 是 `align` 的整数倍，返回 `true`；否则返回 `false`。

**示例**

```cpp
using namespace cinux::lib;

static_assert(is_aligned(0, 4096));
static_assert(is_aligned(4096, 4096));
static_assert(!is_aligned(1, 4096));
static_assert(!is_aligned(100, 4096));
```

---

### 2 的幂操作

#### `is_power_of_two`

```cpp
constexpr bool is_power_of_two(uint64_t v);
```

判断 `v` 是否为 2 的幂（且 `v > 0`）。

**参数**
- `v` — 待检测的无符号 64 位整数。

**返回值**
若 `v` 是 2 的幂（即存在某个非负整数 `k` 使得 `v == 1 << k`），返回 `true`。注意 `v == 0` 返回 `false`。

**示例**

```cpp
using namespace cinux::lib;

static_assert(!is_power_of_two(0));
static_assert(is_power_of_two(1));
static_assert(is_power_of_two(256));
static_assert(is_power_of_two(4096));
static_assert(!is_power_of_two(3));
static_assert(!is_power_of_two(255));
```

---

#### `round_up_to_power_of_two`

```cpp
constexpr uint64_t round_up_to_power_of_two(uint64_t v);
```

将 `v` 向上取整到最近的 2 的幂。若 `v` 本身已经是 2 的幂，则返回 `v`。若 `v == 0`，返回 `1`。

**参数**
- `v` — 待取整的无符号 64 位整数。

**返回值**
大于等于 `v` 的最小 2 的幂。对于 `v == 0` 返回 `1`。

**实现说明**
内部使用经典的位传播（bit smear）算法，通过连续右移并按位或，将最高有效位以下的所有位填 1，最后加 1 得到结果。时间复杂度为 O(log W)，其中 W 为字宽（此处 W = 64）。

**示例**

```cpp
using namespace cinux::lib;

static_assert(round_up_to_power_of_two(0) == 1);
static_assert(round_up_to_power_of_two(1) == 1);
static_assert(round_up_to_power_of_two(3) == 4);
static_assert(round_up_to_power_of_two(5) == 8);
static_assert(round_up_to_power_of_two(1024) == 1024);
static_assert(round_up_to_power_of_two(1025) == 2048);
```

---

### 整除操作

#### `div_ceil`

```cpp
constexpr uint64_t div_ceil(uint64_t a, uint64_t b);
```

向上取整整除，即 `ceil(a / b)`。

**参数**
- `a` — 被除数。
- `b` — 除数，必须大于 0。当 `b == 0` 时行为未定义。

**返回值**
大于等于 `a / b` 的最小整数。等价于 `(a + b - 1) / b`。

**示例**

```cpp
using namespace cinux::lib;

static_assert(div_ceil(0, 4) == 0);
static_assert(div_ceil(1, 4) == 1);
static_assert(div_ceil(4, 4) == 1);
static_assert(div_ceil(5, 4) == 2);
static_assert(div_ceil(9, 4) == 3);
```

---

### 对数操作

#### `log2_int`

```cpp
constexpr int log2_int(uint64_t v);
```

计算 `floor(log2(v))`，即 `v` 的以 2 为底的整数对数（向下取整）。

**参数**
- `v` — 待计算的无符号 64 位整数。

**返回值**
返回 `v` 的最高有效位的位索引。例如 `log2_int(1) == 0`，`log2_int(4096) == 12`。若 `v == 0`，返回 `-1` 表示无意义值。

**示例**

```cpp
using namespace cinux::lib;

static_assert(log2_int(0) == -1);
static_assert(log2_int(1) == 0);
static_assert(log2_int(2) == 1);
static_assert(log2_int(3) == 1);   // floor(log2(3)) = 1
static_assert(log2_int(4) == 2);
static_assert(log2_int(4096) == 12);
static_assert(log2_int(65536) == 16);
```

---

### 内存字面量

内存字面量定义在子命名空间 `cinux::lib::literals` 中，使用前需通过 `using namespace cinux::lib::literals;` 引入，或通过 `using cinux::lib::literals::operator""_KB;` 等方式选择性引入。

所有字面量基于二进制前缀（IEC 标准）：1 KB = 1024 字节（即 1 KiB）。

#### `operator""_KB`

```cpp
constexpr uint64_t operator""_KB(unsigned long long v);
```

将 `v` 转换为 KiB（千字节）对应的字节数。

**返回值**
`v * 1024`

**示例**

```cpp
using namespace cinux::lib::literals;

static_assert(1_KB == 1024ULL);
static_assert(4_KB == 4096ULL);   // 一个标准内存页
```

---

#### `operator""_MB`

```cpp
constexpr uint64_t operator""_MB(unsigned long long v);
```

将 `v` 转换为 MiB（兆字节）对应的字节数。

**返回值**
`v * 1024 * 1024`

**示例**

```cpp
using namespace cinux::lib::literals;

static_assert(1_MB == 1048576ULL);
```

---

#### `operator""_GB`

```cpp
constexpr uint64_t operator""_GB(unsigned long long v);
```

将 `v` 转换为 GiB（吉字节）对应的字节数。

**返回值**
`v * 1024 * 1024 * 1024`

**示例**

```cpp
using namespace cinux::lib::literals;

static_assert(1_GB == 1073741624ULL);
```

---

#### `operator""_TB`

```cpp
constexpr uint64_t operator""_TB(unsigned long long v);
```

将 `v` 转换为 TiB（太字节）对应的字节数。

**返回值**
`v * 1024 * 1024 * 1024 * 1024`

**示例**

```cpp
using namespace cinux::lib::literals;

static_assert(1_TB == 1099511627776ULL);
```

---

## 注意事项

- **全 constexpr**：本组件中所有函数与字面量运算符均为 `constexpr`，可在编译期求值，适合在 `static_assert`、非类型模板参数以及常量初始化中使用。
- **除零未定义**：`div_ceil(a, b)`、`align_up(addr, align)`、`align_down(addr, align)` 以及 `is_aligned(addr, align)` 均要求除数/对齐值不为零。传入零值将导致除零行为（编译期触发编译错误，运行期为未定义行为）。
- **无溢出保护**：`align_up` 与 `div_ceil` 内部执行 `addr + align - 1` 或 `a + b - 1` 加法，当输入值接近 `UINT64_MAX` 时可能溢出。调用方应确保输入范围合理。
- **字面量语义**：`_KB`/`_MB`/`_GB`/`_TB` 使用二进制前缀（1K = 1024），而非十进制前缀（1K = 1000）。名称采用传统缩写（KB/MB/GB/TB）而非 IEC 推荐的 KiB/MiB/GiB/TiB，使用时请注意区分。
- **线程安全**：所有函数为纯函数（pure function），无共享可变状态，天然线程安全。
- **`round_up_to_power_of_two` 上限**：当 `v` 超过 `2^63` 时，结果将溢出到 `0`（因为无符号 64 位整数无法表示 `2^64`）。对于 `v > 2^63` 的场景需调用方自行处理。
- **头文件自包含**：本头文件仅依赖 `<cstdint>`，无其他内部或外部依赖。

## 另见

- [bit_ops](bit_ops.md) — 位操作工具（位提取、位翻转等）
- [buffer](buffer.md) — 缓冲区管理（内部使用对齐函数）
- [bitmap](bitmap.md) — 位图数据结构（内部使用 `log2_int` 与 2 的幂操作）