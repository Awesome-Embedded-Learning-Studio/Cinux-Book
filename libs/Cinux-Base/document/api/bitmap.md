我已经掌握了所有必要的信息。这是完整的 markdown 文档：

---

# BitMap\<N\>

> 固定大小位图，用于页分配器和 buddy 系统

## 头文件

`#include <cinux/bitmap.hpp>`

## 依赖

BitOps（提供 `popcount` 函数，用于统计置位数量）

## 概述

`BitMap<N>` 是一个编译期确定大小的位图容器，底层以 `uint64_t` 数组存储。模板参数 `N` 指定位的总数，内部自动计算所需字数 `kWords = (N + 63) / 64`。第 `i` 位存放在 `words_[i / 64]` 的第 `i % 64` 个比特处。

该组件面向操作系统内核场景设计，典型用途包括：物理页帧的占用/空闲追踪、buddy 分配器中的阶(order)管理、CPU 亲和性掩码以及资源位域分配。由于大小在编译期固定，不涉及动态内存分配，适合在内核数据结构中直接嵌入。

大多数单比特操作和查询方法被标记为 `constexpr`，可在编译期求值。批量操作 `set_all` 和 `clear_all` 使用 `memset` 以获得运行时性能优势。所有位索引均从 0 开始，当传入越界索引时，单比特操作静默忽略（`test` 返回 `false`），查询操作返回 `N` 作为"未找到"哨兵值。

## API 参考

### 类模板

```cpp
namespace cinux::lib {

template <size_t N>
class BitMap { /* ... */ };

}  // namespace cinux::lib
```

**模板参数**

| 参数 | 类型 | 说明 |
|------|------|------|
| `N` | `size_t` | 位图容纳的比特总数，必须为编译期常量 |

### 构造函数

```cpp
constexpr BitMap() = default;
```

默认构造函数，所有比特初始化为 0（清零状态）。标记为 `constexpr`，允许编译期构造。

**示例**

```cpp
cinux::lib::BitMap<64> bm;        // 64 位位图，全部为 0
cinux::lib::BitMap<1024> pages;    // 1024 位位图，可追踪 1024 个页帧
```

### 单比特操作

#### `set`

```cpp
constexpr void set(size_t pos);
```

将第 `pos` 位置 1。若 `pos >= N` 则静默忽略。

**参数**

| 参数 | 说明 |
|------|------|
| `pos` | 要置位的索引，范围 `[0, N)` |

**示例**

```cpp
BitMap<64> bm;
bm.set(5);
// 此时 bm.test(5) == true
```

#### `clear`

```cpp
constexpr void clear(size_t pos);
```

将第 `pos` 位清 0。若 `pos >= N` 则静默忽略。

**参数**

| 参数 | 说明 |
|------|------|
| `pos` | 要清零的索引，范围 `[0, N)` |

**示例**

```cpp
BitMap<64> bm;
bm.set(5);
bm.clear(5);
// 此时 bm.test(5) == false
```

#### `toggle`

```cpp
constexpr void toggle(size_t pos);
```

翻转第 `pos` 位的值（0 变 1，1 变 0）。若 `pos >= N` 则静默忽略。

**参数**

| 参数 | 说明 |
|------|------|
| `pos` | 要翻转的索引，范围 `[0, N)` |

**示例**

```cpp
BitMap<32> bm;
bm.toggle(3);   // bit 3: 0 -> 1
bm.toggle(3);   // bit 3: 1 -> 0
```

#### `test`

```cpp
constexpr bool test(size_t pos) const;
```

检测第 `pos` 位是否为 1。若 `pos >= N` 返回 `false`。

**参数**

| 参数 | 说明 |
|------|------|
| `pos` | 要检测的索引，范围 `[0, N)` |

**返回值**

| 类型 | 说明 |
|------|------|
| `bool` | 该位为 1 返回 `true`，否则返回 `false`；越界返回 `false` |

**示例**

```cpp
BitMap<64> bm;
bm.set(42);
bool used = bm.test(42);  // true
bool free = bm.test(0);   // false
bool oob  = bm.test(99);  // false（越界）
```

### 批量操作

#### `set_all`

```cpp
void set_all();
```

将所有 N 位置 1。内部使用 `memset` 填充 `0xFF`，随后清除最后一个字中超出 N 的尾部比特（若 N 不是 64 的整数倍）。此方法**不是** `constexpr`。

**示例**

```cpp
BitMap<65> bm;
bm.set_all();
// bm.count_set() == 65
// bm.test(64) == true，尾部冗余位被正确屏蔽
```

#### `clear_all`

```cpp
void clear_all();
```

将所有 N 位清 0。内部使用 `memset` 清零。此方法**不是** `constexpr`。

**示例**

```cpp
BitMap<64> bm;
bm.set_all();
bm.clear_all();
// bm.count_set() == 0
```

#### `set_range`

```cpp
constexpr void set_range(size_t begin, size_t end);
```

将 `[begin, end)` 范围内的所有位置 1。范围被自动截断至 `[0, N)`，不会越界。

**参数**

| 参数 | 说明 |
|------|------|
| `begin` | 起始索引（含） |
| `end` | 结束索引（不含） |

**示例**

```cpp
BitMap<128> bm;
bm.set_range(5, 15);
// bm.test(4)  == false
// bm.test(5)  == true
// bm.test(14) == true
// bm.test(15) == false
// bm.count_set() == 10
```

#### `clear_range`

```cpp
constexpr void clear_range(size_t begin, size_t end);
```

将 `[begin, end)` 范围内的所有位清 0。范围被自动截断至 `[0, N)`。

**参数**

| 参数 | 说明 |
|------|------|
| `begin` | 起始索引（含） |
| `end` | 结束索引（不含） |

**示例**

```cpp
BitMap<128> bm;
bm.set_range(0, 20);
bm.clear_range(5, 15);
// bm.count_set() == 10（位 0-4 和 15-19 仍为 1）
```

### 查询操作

#### `size`

```cpp
constexpr size_t size() const;
```

返回位图的总位数。

**返回值**

| 类型 | 说明 |
|------|------|
| `size_t` | 模板参数 `N` |

**示例**

```cpp
BitMap<256> bm;
size_t cap = bm.size();  // 256
```

#### `count_set`

```cpp
constexpr size_t count_set() const;
```

统计所有值为 1 的位的数量。遍历内部所有字并调用 `popcount` 计算。

**返回值**

| 类型 | 说明 |
|------|------|
| `size_t` | 置位数量，范围 `[0, N]` |

**示例**

```cpp
BitMap<64> bm;
bm.set(0);
bm.set(63);
size_t n = bm.count_set();  // 2
```

#### `count_clear`

```cpp
constexpr size_t count_clear() const;
```

统计所有值为 0 的位的数量。等价于 `N - count_set()`。

**返回值**

| 类型 | 说明 |
|------|------|
| `size_t` | 清零数量，范围 `[0, N]` |

**示例**

```cpp
BitMap<64> bm;
bm.set_all();
size_t free = bm.count_clear();  // 0
```

#### `find_first_set`

```cpp
constexpr size_t find_first_set() const;
```

从低位到高位查找第一个值为 1 的位。

**返回值**

| 类型 | 说明 |
|------|------|
| `size_t` | 第一个置位的位置；若全为 0 则返回 `N` |

**示例**

```cpp
BitMap<64> bm;
bm.set(42);
size_t pos = bm.find_first_set();  // 42

bm.clear_all();
size_t none = bm.find_first_set(); // 64（即 N，表示未找到）
```

#### `find_first_clear`

```cpp
constexpr size_t find_first_clear() const;
```

从低位到高位查找第一个值为 0 的位。

**返回值**

| 类型 | 说明 |
|------|------|
| `size_t` | 第一个清零位的位置；若全为 1 则返回 `N` |

**示例**

```cpp
BitMap<64> bm;
for (size_t i = 0; i < 10; ++i) {
    bm.set(i);
}
size_t pos = bm.find_first_clear();  // 10
```

#### `find_next_clear`

```cpp
constexpr size_t find_next_clear(size_t pos) const;
```

从 `pos` 位置开始（含），查找下一个值为 0 的位。

**参数**

| 参数 | 说明 |
|------|------|
| `pos` | 搜索起始位置（含） |

**返回值**

| 类型 | 说明 |
|------|------|
| `size_t` | 从 `pos` 开始第一个清零位的位置；若不存在则返回 `N` |

**示例**

```cpp
BitMap<128> bm;
bm.set_range(0, 50);
size_t p1 = bm.find_next_clear(0);   // 50
size_t p2 = bm.find_next_clear(50);  // 50
```

### 数据访问

#### `data`

```cpp
constexpr const uint64_t* data() const;
```

返回底层 `uint64_t` 字数组的只读指针。

**返回值**

| 类型 | 说明 |
|------|------|
| `const uint64_t*` | 指向内部 `words_` 数组的指针 |

**示例**

```cpp
BitMap<128> bm;
bm.set(0);
const uint64_t* raw = bm.data();
// raw[0] 的最低位为 1
```

#### `data_size`

```cpp
constexpr size_t data_size() const;
```

返回底层存储占用的字节数，即 `sizeof(words_)`。

**返回值**

| 类型 | 说明 |
|------|------|
| `size_t` | 字节数，等于 `kWords * sizeof(uint64_t)` |

**示例**

```cpp
BitMap<65> bm;
size_t bytes = bm.data_size();  // 2 * 8 = 16 字节（kWords = 2）
```

## 注意事项

- **`constexpr` 限制**：`set_all` 和 `clear_all` 使用 `memset`，不可在编译期上下文中调用。其余单比特操作、范围操作和查询方法均为 `constexpr`，支持编译期求值。
- **越界行为**：`set`、`clear`、`toggle` 在 `pos >= N` 时静默忽略，不触发未定义行为。`test` 在越界时返回 `false`。`find_*` 系列方法以 `N` 作为"未找到"哨兵值。
- **线程安全**：`BitMap` 不提供任何内部同步机制。在多线程环境中并发读写同一个 `BitMap` 实例需要外部加锁保护。
- **容量与内存**：内部存储为 `uint64_t` 字数组，实际占用的比特数 `kWords * 64` 可能大于 `N`。当 `N` 不是 64 的整数倍时，`set_all` 会正确屏蔽最后一个字中的冗余尾部比特，确保 `count_set()` 最多返回 `N`。
- **最小容量**：`N = 0` 在语法上合法但语义上无意义，建议 `N >= 1`。
- **`popcount` 依赖**：`count_set()` 依赖 `cinux::lib::popcount(uint64_t)`。当前实现为逐位计数可移植版本；若需运行时性能优化，可在 `bit_ops.hpp` 中替换为编译器内建函数（如 `__builtin_popcountll`）。

## 另见

- **BitOps** (`cinux/bit_ops.hpp`) — 位操作原语集合（`popcount`、`clz`、`ctz`、`rotl`、`rotr`、`bit`、`set_bit`、`clear_bit`、`toggle_bit`、`test_bit`、`extract_bits`、`insert_bits`）
- **Buddy 分配器**（规划中）— 将使用 `BitMap` 管理各阶空闲块
- **页帧分配器**（规划中）— 将使用 `BitMap` 追踪物理页的占用状态