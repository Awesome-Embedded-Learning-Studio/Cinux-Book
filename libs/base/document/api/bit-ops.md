这是 BitOps 组件的完整中文 API 文档文件。

---

```markdown
# BitOps

> 位操作原语 — 位计数/旋转/单 bit 操作/位域提取插入

## 头文件

`#include <cinux/bit_ops.hpp>`

## 依赖

无（叶子组件）

## 概述

`cinux::lib::bit_ops` 提供一组完整的位操作原语，涵盖位计数（popcount / clz / ctz）、循环移位（rotl / rotr）、单 bit 置位/清零/翻转/测试，以及硬件寄存器级别的位域提取与插入。所有函数均声明为 `constexpr`，可在编译期求值，适用于 `static_assert`、模板参数及常量初始化等场景。

在内核与底层系统编程中，频繁需要操控硬件寄存器中的特定位域——例如解析页表项标志位、设置中断控制器掩码、提取状态寄存器字段等。本组件以纯 C++ 可移植实现提供这些能力，不依赖编译器内建函数（`__builtin_*`）或平台特定指令。内核构建在运行时可选择使用编译器内建函数以获得更佳性能，而本实现同时充当编译期求值路径与跨平台回退方案。

所有函数均针对 `uint32_t` 和 `uint64_t`（位计数函数）或 `uint64_t`（旋转/单 bit/位域函数）提供重载，覆盖 32 位与 64 位系统的常见需求。接口设计力求最小化、零开销，适合在性能敏感的热路径中内联使用。

## API 参考

### 命名空间

所有函数位于命名空间 `cinux::lib` 中。

---

### 位计数（Bit Counting）

#### `popcount`

```cpp
constexpr int popcount(uint32_t v);
constexpr int popcount(uint64_t v);
```

**功能**：计算值 `v` 中置位（为 1）的 bit 数量，即 population count。

**参数**：
- `v`：待计算的整数值。

**返回值**：`int`，`v` 中值为 1 的 bit 个数，范围 `[0, 32]`（32 位重载）或 `[0, 64]`（64 位重载）。

**示例**：

```cpp
using namespace cinux::lib;

static_assert(popcount(uint32_t{0xFF}) == 8);
static_assert(popcount(uint64_t{0xFFFFFFFFFFFFFFFFULL}) == 64);

int n = popcount(uint32_t{0b10101010}); // n == 4
```

---

#### `clz`

```cpp
constexpr int clz(uint32_t v);
constexpr int clz(uint64_t v);
```

**功能**：计算值 `v` 中从最高有效位开始的前导零（leading zeros）数量。

**参数**：
- `v`：待计算的整数值。

**返回值**：`int`，从 MSB 开始连续零 bit 的数量。当 `v == 0` 时返回类型的位宽（32 位重载返回 `32`，64 位重载返回 `64`）。

**注意**：与某些平台 `__builtin_clz` 在 `v == 0` 时行为未定义不同，本实现给出明确定义值。

**示例**：

```cpp
using namespace cinux::lib;

static_assert(clz(uint32_t{1}) == 31);
static_assert(clz(uint64_t{1}) == 63);
static_assert(clz(uint32_t{0x80000000u}) == 0);
static_assert(clz(uint32_t{0}) == 32);
```

---

#### `ctz`

```cpp
constexpr int ctz(uint32_t v);
constexpr int ctz(uint64_t v);
```

**功能**：计算值 `v` 中从最低有效位开始的尾随零（trailing zeros）数量。

**参数**：
- `v`：待计算的整数值。

**返回值**：`int`，从 LSB 开始连续零 bit 的数量。当 `v == 0` 时返回类型的位宽（32 位重载返回 `32`，64 位重载返回 `64`）。

**示例**：

```cpp
using namespace cinux::lib;

static_assert(ctz(uint32_t{8}) == 3);
static_assert(ctz(uint64_t{256}) == 8);
static_assert(ctz(uint32_t{1}) == 0);
static_assert(ctz(uint32_t{0}) == 32);
```

---

### 循环移位（Rotation）

#### `rotl`

```cpp
constexpr uint64_t rotl(uint64_t v, int n);
```

**功能**：将 `v` 向左循环移位 `n` 位。移出的高位回绕至低位。

**参数**：
- `v`：待旋转的 64 位值。
- `n`：左旋位数。自动取模 64，因此 `n >= 64` 等价于 `n % 64`。

**返回值**：`uint64_t`，旋转后的结果。`n == 0` 或 `n == 64` 时返回 `v` 本身。

**示例**：

```cpp
using namespace cinux::lib;

uint64_t v = 0x0123456789ABCDEFULL;
uint64_t rotated = rotl(v, 13);
// rotr(rotated, 13) == v  // 往返一致
static_assert(rotl(v, 0) == v);
static_assert(rotl(v, 64) == v);
```

---

#### `rotr`

```cpp
constexpr uint64_t rotr(uint64_t v, int n);
```

**功能**：将 `v` 向右循环移位 `n` 位。移出的低位回绕至高位。

**参数**：
- `v`：待旋转的 64 位值。
- `n`：右旋位数。自动取模 64，因此 `n >= 64` 等价于 `n % 64`。

**返回值**：`uint64_t`，旋转后的结果。`n == 0` 或 `n == 64` 时返回 `v` 本身。

**示例**：

```cpp
using namespace cinux::lib;

uint64_t v = 0x0123456789ABCDEFULL;
uint64_t rotated = rotr(v, 37);
// rotl(rotated, 37) == v  // 往返一致
static_assert(rotr(v, 0) == v);
```

---

### 单 bit 操作（Single-bit Operations）

#### `bit`

```cpp
constexpr uint64_t bit(int n);
```

**功能**：生成一个仅第 `n` 位为 1 的掩码值。

**参数**：
- `n`：目标 bit 索引（0 = LSB，63 = MSB）。

**返回值**：`uint64_t`，值为 `1ULL << n`。

**示例**：

```cpp
using namespace cinux::lib;

static_assert(bit(0) == 1);
static_assert(bit(5) == 32);
static_assert(bit(63) == (1ULL << 63));
```

---

#### `set_bit`

```cpp
constexpr uint64_t set_bit(uint64_t v, int n);
```

**功能**：将 `v` 的第 `n` 位置为 1。

**参数**：
- `v`：原始值。
- `n`：目标 bit 索引。

**返回值**：`uint64_t`，`v` 的第 `n` 位被置 1 后的结果。

**示例**：

```cpp
using namespace cinux::lib;

static_assert(set_bit(uint64_t{0}, 5) == 32);
// set_bit(0b1010, 0) == 0b1011
```

---

#### `clear_bit`

```cpp
constexpr uint64_t clear_bit(uint64_t v, int n);
```

**功能**：将 `v` 的第 `n` 位清零（置为 0）。

**参数**：
- `v`：原始值。
- `n`：目标 bit 索引。

**返回值**：`uint64_t`，`v` 的第 `n` 位被清零后的结果。

**示例**：

```cpp
using namespace cinux::lib;

static_assert(clear_bit(uint64_t{32}, 5) == 0);
// clear_bit(0b1011, 0) == 0b1010
```

---

#### `toggle_bit`

```cpp
constexpr uint64_t toggle_bit(uint64_t v, int n);
```

**功能**：翻转 `v` 的第 `n` 位（0 变 1，1 变 0）。

**参数**：
- `v`：原始值。
- `n`：目标 bit 索引。

**返回值**：`uint64_t`，`v` 的第 `n` 位被翻转后的结果。

**示例**：

```cpp
using namespace cinux::lib;

static_assert(toggle_bit(uint64_t{0}, 3) == 8);
static_assert(toggle_bit(uint64_t{8}, 3) == 0);
```

---

#### `test_bit`

```cpp
constexpr bool test_bit(uint64_t v, int n);
```

**功能**：测试 `v` 的第 `n` 位是否为 1。

**参数**：
- `v`：待测试的值。
- `n`：目标 bit 索引。

**返回值**：`bool`，若第 `n` 位为 1 则返回 `true`，否则返回 `false`。

**示例**：

```cpp
using namespace cinux::lib;

static_assert(test_bit(uint64_t{32}, 5));
static_assert(!test_bit(uint64_t{32}, 0));

if (test_bit(status_register, 7)) {
    // 第 7 位已置位，处理对应事件
}
```

---

### 位域提取与插入（Bit-field Extract / Insert）

#### `extract_bits`

```cpp
constexpr uint64_t extract_bits(uint64_t v, int high, int low);
```

**功能**：从 `v` 中提取闭区间 `[high:low]` 的位域，返回右对齐（从第 0 位开始）的提取结果。常用于读取硬件寄存器中的特定字段。

**参数**：
- `v`：源操作数。
- `high`：位域的最高位索引（包含）。
- `low`：位域的最低位索引（包含）。

**返回值**：`uint64_t`，提取的位域值，已右对齐至 bit 0。位域宽度为 `high - low + 1`。

**前置条件**：`high >= low`，`high - low + 1 <= 64`，`high < 64`，`low >= 0`。

**示例**：

```cpp
using namespace cinux::lib;

// 0xABCD = 0b1010_1011_1100_1101
// bits [11:8] = 0b1011 = 0xB
static_assert(extract_bits(0xABCD, 11, 8) == 0xB);

// bits [7:4] = 0b1100 = 0xC
static_assert(extract_bits(0xABCD, 7, 4) == 0xC);

// 提取完整的 8 位
static_assert(extract_bits(0xFF, 7, 0) == 0xFF);

// 单 bit 提取
uint64_t v = 1ULL << 63;
static_assert(extract_bits(v, 63, 63) == 1);
```

---

#### `insert_bits`

```cpp
constexpr uint64_t insert_bits(uint64_t v, int high, int low, uint64_t val);
```

**功能**：将 `val` 的低 `(high - low + 1)` 位插入到 `v` 的闭区间 `[high:low]` 位置，原位域被覆盖。常用于写入硬件寄存器中的特定字段。

**参数**：
- `v`：目标操作数。
- `high`：位域的最高位索引（包含）。
- `low`：位域的最低位索引（包含）。
- `val`：待插入的值。仅其低 `(high - low + 1)` 位被使用，超出部分被屏蔽。

**返回值**：`uint64_t`，`v` 中 `[high:low]` 位域被 `val` 替换后的结果。

**前置条件**：`high >= low`，`high - low + 1 <= 64`，`high < 64`，`low >= 0`。

**示例**：

```cpp
using namespace cinux::lib;

// 将 0x5 插入 0xABCD 的 bits [11:8]
// 结果: 0b1010_0101_1100_1101 = 0xA5CD
static_assert(insert_bits(0xABCD, 11, 8, 0x5) == 0xA5CD);

// 单 bit 插入
static_assert(insert_bits(0ULL, 63, 63, 1) == (1ULL << 63));
```

---

## 注意事项

1. **constexpr 限制**：所有函数均为 `constexpr`，可在编译期求值。实现未使用 `__builtin_*` 内建函数，以确保在所有标准兼容的 C++ 编译器上均可作为常量表达式求值。若运行时性能为首要考量，建议在内核构建中使用编译器内建函数替代。

2. **零值行为**：`clz(0)` 返回类型位宽（32 或 64），`ctz(0)` 同理。此行为与 `__builtin_clz` / `__builtin_ctz` 在零值时未定义的行为不同，本组件提供了明确定义。

3. **位索引范围**：单 bit 操作（`bit`、`set_bit`、`clear_bit`、`toggle_bit`、`test_bit`）的 `n` 参数应在 `[0, 63]` 范围内。传入负数或超过 63 的值属于未定义行为（移位量超出类型位宽）。

4. **旋转参数取模**：`rotl` / `rotr` 对移位量 `n` 自动执行 `n & 63`，因此 `n` 可以大于 64，也可以为负数（负数经过取模后等价于反向旋转）。

5. **位域参数合法性**：`extract_bits` / `insert_bits` 要求 `high >= low >= 0` 且 `high < 64`。位域宽度 `high - low + 1` 可达 64（此时提取/插入整个值）。调用者需确保参数合法，本组件不做运行时检查。

6. **线程安全**：所有函数均为纯函数（pure function），无共享可变状态，天然线程安全。

7. **无符号类型限定**：所有输入输出均为无符号整数类型（`uint32_t` / `uint64_t`），避免了有符号整数右移的实现定义行为和符号扩展问题。

## 另见

- [类型定义与固定宽度整数](cstdint.md) — `<cstdint>` 标准头文件提供 `uint32_t`、`uint64_t` 等类型
- 位操作在页表解析、中断控制器、设备寄存器访问等子系统中的具体应用
```