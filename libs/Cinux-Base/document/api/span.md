以下是 `Span<T>` 组件的完整中文 API 文档：

---

# Span\<T\>

> 非拥有连续内存视图（std::span 的 C++17 替代）

## 头文件

`#include <cinux/span.hpp>`

## 依赖

无

## 概述

`cinux::lib::Span<T>` 是一个轻量级的、非拥有的连续内存视图类模板。它不管理所指向内存的生命周期，仅持有一个原始指针和一个长度计数，用于安全且高效地引用已有的一段连续元素序列。该组件为 C++17 环境提供了与 C++20 `std::span` 类似的核心功能，使项目无需升级语言标准即可获得非拥有视图的便利。

典型使用场景包括：函数参数中替代原始指针加长度的组合（`T* ptr, size_t len`），从而提供边界信息与范围迭代能力；对 C 风格数组、缓冲区或容器内部存储进行只读或读写视图的封装；在网络协议解析、二进制数据处理等场景中以 `ByteSpan` / `ConstByteSpan` 类型别名便捷地操作字节序列。

`Span<T>` 的所有公开方法均标记为 `constexpr`，支持在编译期常量表达式中使用。它不执行任何堆分配，拷贝和赋值的开销等同于拷贝两个原生变量（一个指针与一个 `size_t`），适合以值方式传递。

## API 参考

### 类模板 `Span<T>`

```cpp
namespace cinux::lib {

template <typename T>
class Span {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);

    // 构造函数（见下文）
    constexpr Span() = default;
    constexpr Span(T* data, size_t size);
    constexpr Span(T* begin, T* end);
    template <size_t N>
    constexpr Span(T (&arr)[N]);

    // 访问器
    constexpr size_t size() const;
    constexpr bool   empty() const;
    constexpr T*     data() const;
    constexpr T&     operator[](size_t i) const;
    constexpr T&     front() const;
    constexpr T&     back() const;

    // 子视图
    constexpr Span first(size_t count) const;
    constexpr Span last(size_t count) const;
    constexpr Span subspan(size_t pos, size_t count = npos) const;

    // 迭代器
    constexpr T* begin() const;
    constexpr T* end() const;
};

} // namespace cinux::lib
```

---

### 静态常量

#### `npos`

```cpp
static constexpr size_t npos = static_cast<size_t>(-1);
```

表示"无限制"的特殊长度值，用于 `subspan` 的 `count` 参数默认值，含义为"取到末尾"。

---

### 构造函数

#### 默认构造函数

```cpp
constexpr Span() = default;
```

构造一个空视图。`data()` 返回 `nullptr`，`size()` 返回 `0`。

**示例：**

```cpp
cinux::lib::Span<int> s;
assert(s.empty());
assert(s.data() == nullptr);
```

#### 指针 + 长度构造

```cpp
constexpr Span(T* data, size_t size);
```

- **data**：指向连续元素序列首元素的指针。
- **size**：序列中元素的个数。

**示例：**

```cpp
int arr[] = {10, 20, 30};
cinux::lib::Span<int> s(arr, 3);
assert(s.size() == 3);
assert(s[0] == 10);
```

#### 指针对构造

```cpp
constexpr Span(T* begin, T* end);
```

- **begin**：指向序列首元素的指针。
- **end**：指向序列末尾之后一个位置的指针（past-the-end）。长度由 `end - begin` 计算得出。

**示例：**

```cpp
int arr[] = {1, 2, 3, 4};
cinux::lib::Span<int> s(arr + 1, arr + 3); // 指向 {2, 3}
assert(s.size() == 2);
assert(s[0] == 2);
```

#### C 数组构造

```cpp
template <size_t N>
constexpr Span(T (&arr)[N]);
```

从固定大小的 C 风格数组自动推导指针和长度。无需手动传入大小。

**示例：**

```cpp
int arr[] = {5, 6, 7};
cinux::lib::Span<int> s(arr);
assert(s.size() == 3);
assert(s.front() == 5);
```

---

### 访问器

#### `size()`

```cpp
constexpr size_t size() const;
```

返回视图中元素的个数。

#### `empty()`

```cpp
constexpr bool empty() const;
```

若视图为空（`size() == 0`）则返回 `true`，否则返回 `false`。

#### `data()`

```cpp
constexpr T* data() const;
```

返回指向底层元素序列首元素的指针。对空视图返回 `nullptr`。

#### `operator[]`

```cpp
constexpr T& operator[](size_t i) const;
```

按下标 `i` 访问元素，不执行边界检查（release 模式下）。返回元素的引用，因此可用于读写。

**示例：**

```cpp
int arr[] = {10, 20, 30};
cinux::lib::Span<int> s(arr);
s[1] = 99;
assert(arr[1] == 99);
```

#### `front()`

```cpp
constexpr T& front() const;
```

返回对第一个元素的引用。在空视图上调用属于未定义行为。

#### `back()`

```cpp
constexpr T& back() const;
```

返回对最后一个元素的引用。在空视图上调用属于未定义行为。

---

### 子视图

#### `first(count)`

```cpp
constexpr Span first(size_t count) const;
```

返回一个新 `Span`，包含当前视图的前 `count` 个元素。若 `count > size()`，则返回整个视图。

- **count**：期望截取的元素数量。
- **返回值**：`Span`，指向从头部开始的子序列。

**示例：**

```cpp
int arr[] = {1, 2, 3, 4, 5};
cinux::lib::Span<int> s(arr);
auto f = s.first(3); // {1, 2, 3}
```

#### `last(count)`

```cpp
constexpr Span last(size_t count) const;
```

返回一个新 `Span`，包含当前视图的最后 `count` 个元素。若 `count > size()`，则返回整个视图。

- **count**：期望截取的元素数量。
- **返回值**：`Span`，指向从尾部截取的子序列。

**示例：**

```cpp
int arr[] = {1, 2, 3, 4, 5};
cinux::lib::Span<int> s(arr);
auto l = s.last(2); // {4, 5}
```

#### `subspan(pos, count = npos)`

```cpp
constexpr Span subspan(size_t pos, size_t count = npos) const;
```

从位置 `pos` 开始，截取 `count` 个元素构成新视图。若 `pos >= size()`，返回空视图。若 `count` 为 `npos` 或超过剩余长度，则截取到末尾。

- **pos**：起始偏移（从 0 开始）。
- **count**：截取长度，默认为 `npos`（取到末尾）。
- **返回值**：`Span`，指向指定子序列。

**示例：**

```cpp
int arr[] = {1, 2, 3, 4, 5};
cinux::lib::Span<int> s(arr);
auto sub = s.subspan(1, 3); // {2, 3, 4}
auto tail = s.subspan(2);   // {3, 4, 5}
```

---

### 迭代器

#### `begin()` / `end()`

```cpp
constexpr T* begin() const;
constexpr T* end() const;
```

分别返回指向首元素和 past-the-end 位置的指针，支持 range-for 循环和标准算法。

**示例：**

```cpp
int arr[] = {10, 20, 30};
cinux::lib::Span<int> s(arr);
int sum = 0;
for (auto& v : s) {
    sum += v;
}
// sum == 60
```

---

### 类型别名

```cpp
using ByteSpan      = Span<uint8_t>;
using ConstByteSpan = Span<const uint8_t>;
```

- **ByteSpan**：可变字节序列视图，适用于二进制缓冲区的读写操作。
- **ConstByteSpan**：只读字节序列视图，适用于协议解析等只需读取的场景。

**示例：**

```cpp
uint8_t buf[] = {0x01, 0x02, 0x03};
cinux::lib::ByteSpan bs(buf);
assert(bs.size() == 3);
assert(bs[1] == 0x02);

const uint8_t cbuf[] = {0xAA, 0xBB};
cinux::lib::ConstByteSpan cbs(cbuf);
assert(cbs.size() == 2);
```

---

## 注意事项

1. **非拥有语义**：`Span<T>` 不管理底层内存的生命周期。调用方必须确保 `Span` 所引用的内存在 `Span` 的整个使用期间保持有效。将 `Span` 悬空（dangling）会导致未定义行为。
2. **无边界检查**：`operator[]`、`front()`、`back()` 在 release 模式下不执行边界检查。越界访问属于未定义行为。如需安全访问，可在调试阶段自行添加断言。
3. **`const Span<T>` 与 `Span<const T>` 的区别**：`const Span<T>` 表示视图本身不可修改（不可重新绑定），但元素仍可写入；`Span<const T>` 表示元素为只读。若需要只读视图，应使用 `Span<const T>`。
4. **编译期支持**：所有公开方法均为 `constexpr`，可在常量表达式中使用。但注意 `constexpr` 构造时指向的存储需具有静态存储期。
5. **线程安全**：`Span` 本身不提供任何线程安全保证。多个线程同时通过同一个 `Span` 读取是安全的，但并发读写需要外部同步。
6. **`subspan` / `first` / `last` 的截断行为**：当请求的长度超过可用元素时，这些方法会自动截断到实际可用范围，而非触发未定义行为。
7. **与 `std::span` 的差异**：本组件不提供固定长度模板参数（即无 `Span<T, N>` 静态扩展变体），也不提供从 `std::array` 或 `std::vector` 的隐式转换构造。需要时可手动传入 `.data()` 和 `.size()`。

## 另见

- `std::span`（C++20）—— 标准库中的等效组件
- `cinux::lib::ByteSpan` / `cinux::lib::ConstByteSpan` —— 字节序列类型别名

---

相关源文件：
- 头文件：`/home/charliechen/Cinux-Base/include/cinux/span.hpp`
- 测试文件：`/home/charliechen/Cinux-Base/test/test_span.cpp`