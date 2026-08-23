Now I have all the information needed. Let me generate the complete Chinese API documentation.

# BufferView + StaticBuffer\<N\>

> 类型安全字节缓冲区（只读视图 + 固定容量容器）

## 头文件

`#include <cinux/buffer.hpp>`

## 依赖

- **StringView** (`#include <cinux/string_view.hpp>`) — 非拥有字符序列视图，`BufferView::as_string()` 的返回类型。
- **Span** (`#include <cinux/span.hpp>`) — 非拥有连续内存视图，`StaticBuffer<N>::as_span()` 的返回类型。头文件中定义了类型别名 `ByteSpan`（即 `Span<uint8_t>`）和 `ConstByteSpan`（即 `Span<const uint8_t>`）。

## 概述

在系统级编程和网络协议解析等场景中，我们频繁地需要对原始字节序列进行读取、切片和传递。传统做法使用 `void*` 配合 `size_t` 长度来表达一段内存区域，但这缺乏类型安全性，且无法利用编译期检查来防止悬空指针或越界访问。`BufferView` 通过将 `const uint8_t*` 与 `size_t` 封装为不可变视图类型来解决这个问题——它不拥有数据，只负责安全地引用一段已有的字节序列。

`StaticBuffer<N>` 是一个编译期确定容量上限的字节容器，适合在嵌入式、内核模块或任何不允许动态堆分配的环境中用作临时缓冲区。它将存储直接内嵌于对象内部（`uint8_t data_[N]`），支持通过 `copy_from` / `copy_to` 进行批量数据搬移，通过 `fill` 进行填充，并可通过 `view()` 和 `as_span()` 轻松转换为对应的只读视图或可写跨度。两者组合使用，即可在"拥有数据"与"观察数据"之间建立清晰的边界。

## API 参考

---

### BufferView 类

非拥有只读字节序列视图。所有成员函数均为 `constexpr`（除 `as_string()` 外），可在编译期求值。

```cpp
namespace cinux::lib {
class BufferView {
public:
    constexpr BufferView() = default;
    constexpr BufferView(const void* data, size_t size);

    constexpr const uint8_t* data() const;
    constexpr size_t         size() const;
    constexpr bool           empty() const;

    constexpr const uint8_t& operator[](size_t i) const;

    constexpr BufferView slice(size_t offset, size_t len) const;

    StringView as_string() const;
};
}
```

---

#### `BufferView::BufferView()`

```cpp
constexpr BufferView() = default;
```

**说明**：构造一个空视图，`data()` 返回 `nullptr`，`size()` 返回 `0`，`empty()` 返回 `true`。

**示例**：

```cpp
cinux::lib::BufferView bv;
assert(bv.empty());
assert(bv.size() == 0);
assert(bv.data() == nullptr);
```

---

#### `BufferView::BufferView(const void* data, size_t size)`

```cpp
constexpr BufferView(const void* data, size_t size);
```

**说明**：从任意内存指针和字节长度构造视图。`data` 的类型为 `const void*`，构造时被 `static_cast` 为 `const uint8_t*` 存储。

**参数**：
- `data` — 指向被观察字节序列起始地址的指针，可为 `nullptr`。
- `size` — 字节序列的字节长度。

**示例**：

```cpp
const char* msg = "hello";
cinux::lib::BufferView bv(msg, 5);
assert(bv.size() == 5);
assert(bv[0] == 'h');
```

---

#### `BufferView::data()`

```cpp
constexpr const uint8_t* data() const;
```

**返回值**：指向底层字节序列起始位置的 `const uint8_t*` 指针。若视图为空，返回 `nullptr`。

**示例**：

```cpp
uint8_t bytes[] = {0x10, 0x20};
cinux::lib::BufferView bv(bytes, 2);
const uint8_t* p = bv.data();
assert(p[0] == 0x10);
```

---

#### `BufferView::size()`

```cpp
constexpr size_t size() const;
```

**返回值**：视图所引用的字节数。

---

#### `BufferView::empty()`

```cpp
constexpr bool empty() const;
```

**返回值**：若 `size() == 0` 则返回 `true`，否则返回 `false`。

---

#### `BufferView::operator[](size_t i)`

```cpp
constexpr const uint8_t& operator[](size_t i) const;
```

**说明**：按下标访问第 `i` 个字节。**不进行边界检查**，调用方需确保 `i < size()`。

**参数**：
- `i` — 字节下标。

**返回值**：对应位置的 `const uint8_t&` 引用。

**示例**：

```cpp
uint8_t data[] = {0x01, 0x02, 0x03};
cinux::lib::BufferView bv(data, 3);
assert(bv[1] == 0x02);
```

---

#### `BufferView::slice(size_t offset, size_t len)`

```cpp
constexpr BufferView slice(size_t offset, size_t len) const;
```

**说明**：返回从 `offset` 开始、至多 `len` 字节的子视图。函数具有安全截断语义：
- 若 `offset >= size()`，返回空视图 `{}`。
- 若 `offset + len` 超出已有数据范围，则 `len` 自动被截断为 `size() - offset`。

**参数**：
- `offset` — 子视图在原视图中的起始偏移量。
- `len` — 期望的子视图长度。

**返回值**：新的 `BufferView` 实例。

**示例**：

```cpp
uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
cinux::lib::BufferView bv(data, 5);

auto s1 = bv.slice(2, 2);   // {0x03, 0x04}
assert(s1.size() == 2 && s1[0] == 0x03);

auto s2 = bv.slice(10, 2);  // 越界 → 空视图
assert(s2.empty());

auto s3 = bv.slice(3, 10);  // 部分截断 → {0x04, 0x05}
assert(s3.size() == 2);
```

---

#### `BufferView::as_string()`

```cpp
StringView as_string() const;
```

**说明**：将字节视图重新解释为字符视图（`StringView`）。内部通过 `reinterpret_cast<const char*>(data_)` 实现，因此不是 `constexpr`。适用于将二进制缓冲区中已知的文本片段提取为字符串视图。

**返回值**：引用相同内存区域的 `StringView` 实例。

**示例**：

```cpp
const char* msg = "hello";
cinux::lib::BufferView bv(msg, 5);
cinux::lib::StringView sv = bv.as_string();
assert(sv == cinux::lib::StringView("hello"));
```

---

### StaticBuffer\<N\> 类模板

编译期确定容量的固定大小字节缓冲区。所有数据内嵌存储，无动态内存分配。

```cpp
namespace cinux::lib {
template <size_t N>
class StaticBuffer {
public:
    constexpr StaticBuffer() = default;

    constexpr uint8_t*       data();
    constexpr const uint8_t* data() const;
    static constexpr size_t  capacity();
    constexpr size_t         size() const;
    constexpr bool           empty() const;

    constexpr void resize(size_t new_size);
    constexpr void fill(uint8_t value);

    void copy_from(const void* src, size_t len);
    void copy_to(void* dst, size_t len) const;

    constexpr BufferView view() const;

    constexpr ByteSpan      as_span();
    constexpr ConstByteSpan as_span() const;
};
}
```

---

#### `StaticBuffer<N>::StaticBuffer()`

```cpp
constexpr StaticBuffer() = default;
```

**说明**：构造一个空的缓冲区。内部数组 `data_[N]` 被值初始化为零，`size_` 为 `0`。

**示例**：

```cpp
cinux::lib::StaticBuffer<64> buf;
assert(buf.empty());
assert(buf.size() == 0);
assert(buf.capacity() == 64);
```

---

#### `StaticBuffer<N>::data()`

```cpp
constexpr uint8_t*       data();        // (1) 可写重载
constexpr const uint8_t* data() const;  // (2) 只读重载
```

**返回值**：指向内部存储数组起始地址的指针。可写重载返回 `uint8_t*`，只读重载返回 `const uint8_t*`。

**示例**：

```cpp
cinux::lib::StaticBuffer<8> buf;
buf.data()[0] = 0x42;  // 直接写入
```

---

#### `StaticBuffer<N>::capacity()`

```cpp
static constexpr size_t capacity();
```

**返回值**：模板参数 `N`，即缓冲区的最大字节数。此函数为 `static` 和 `constexpr`，可在编译期使用。

---

#### `StaticBuffer<N>::size()`

```cpp
constexpr size_t size() const;
```

**返回值**：当前缓冲区中有效数据的字节数（注意：不是容量 `N`，而是逻辑长度）。

---

#### `StaticBuffer<N>::empty()`

```cpp
constexpr bool empty() const;
```

**返回值**：若 `size() == 0` 返回 `true`，否则返回 `false`。

---

#### `StaticBuffer<N>::resize(size_t new_size)`

```cpp
constexpr void resize(size_t new_size);
```

**说明**：设置缓冲区的逻辑长度。若 `new_size > N`，请求被忽略（安全降级），`size_` 不变。注意此函数不会初始化新增区域的内容。

**参数**：
- `new_size` — 期望的新逻辑长度，必须小于等于 `N`。

**示例**：

```cpp
cinux::lib::StaticBuffer<16> buf;
buf.resize(8);
assert(buf.size() == 8);

buf.resize(100);  // 超过容量，被忽略
assert(buf.size() == 8);  // 仍然为 8
```

---

#### `StaticBuffer<N>::fill(uint8_t value)`

```cpp
constexpr void fill(uint8_t value);
```

**说明**：将整个内部数组（共 `N` 字节）填充为指定值，并将逻辑长度 `size_` 设为 `N`（即满载）。

**参数**：
- `value` — 填充字节值。

**示例**：

```cpp
cinux::lib::StaticBuffer<4> buf;
buf.fill(0xFF);
assert(buf.size() == 4);
for (size_t i = 0; i < 4; ++i) {
    assert(buf.data()[i] == 0xFF);
}
```

---

#### `StaticBuffer<N>::copy_from(const void* src, size_t len)`

```cpp
void copy_from(const void* src, size_t len);
```

**说明**：从外部内存 `src` 拷贝至多 `min(len, N)` 字节到内部存储中，并将逻辑长度设为实际拷贝的字节数。内部使用 `memcpy`，因此不是 `constexpr`。

**参数**：
- `src` — 源数据指针。
- `len` — 期望拷贝的字节数。若 `len > N`，仅拷贝前 `N` 字节。

**示例**：

```cpp
cinux::lib::StaticBuffer<16> buf;
const char* src = "ABCDE";
buf.copy_from(src, 5);
assert(buf.size() == 5);
assert(buf.data()[0] == 'A');
assert(buf.data()[4] == 'E');
```

---

#### `StaticBuffer<N>::copy_to(void* dst, size_t len) const`

```cpp
void copy_to(void* dst, size_t len) const;
```

**说明**：将缓冲区中至多 `min(len, size())` 字节拷贝到外部目标 `dst`。拷贝量由当前逻辑长度 `size()` 限制，而非容量 `N`。内部使用 `memcpy`。

**参数**：
- `dst` — 目标内存指针。
- `len` — 期望拷贝的字节数。实际拷贝量为 `min(len, size())`。

**示例**：

```cpp
cinux::lib::StaticBuffer<16> buf;
buf.copy_from("HELLO", 5);

char dst[16] = {};
buf.copy_to(dst, sizeof(dst));
assert(dst[0] == 'H');
assert(dst[4] == 'O');
```

---

#### `StaticBuffer<N>::view()`

```cpp
constexpr BufferView view() const;
```

**返回值**：覆盖当前有效数据范围 `[data_, data_ + size_)` 的 `BufferView` 只读视图。

**示例**：

```cpp
cinux::lib::StaticBuffer<8> buf;
uint8_t data[] = {0xAA, 0xBB, 0xCC};
buf.copy_from(data, 3);

cinux::lib::BufferView v = buf.view();
assert(v.size() == 3);
assert(v[0] == 0xAA);
```

---

#### `StaticBuffer<N>::as_span()`

```cpp
constexpr ByteSpan      as_span();        // (1) 可写跨度
constexpr ConstByteSpan as_span() const;  // (2) 只读跨度
```

**说明**：返回覆盖当前有效数据范围的 `Span` 对象。`ByteSpan` 即 `Span<uint8_t>`，`ConstByteSpan` 即 `Span<const uint8_t>`。可写重载适用于需要逐字节修改缓冲区内容的场景；只读重载用于只读访问。

**返回值**：
- 可写重载：`ByteSpan{data(), size()}`。
- 只读重载：`ConstByteSpan{data(), size()}`。

**示例**：

```cpp
cinux::lib::StaticBuffer<8> buf;
uint8_t data[] = {0xAA, 0xBB, 0xCC};
buf.copy_from(data, 3);

// 只读跨度
auto cspan = static_cast<const cinux::lib::StaticBuffer<8>&>(buf).as_span();
assert(cspan.size() == 3);
assert(cspan[2] == 0xCC);

// 可写跨度
auto span = buf.as_span();
span[1] = 0xDD;  // 通过 span 修改缓冲区
```

---

## 注意事项

1. **BufferView 不拥有数据**：`BufferView` 是非拥有的轻量视图。调用方必须保证被引用的内存在视图的整个生命周期内保持有效。将 `BufferView` 指向临时对象或已释放的内存会导致未定义行为。
2. **`operator[]` 无边界检查**：`BufferView::operator[]` 和 `Span::operator[]` 均不进行运行时边界检查。在 debug 构建中，建议通过 `slice()` 等安全 API 访问数据，或在调用前显式检查下标范围。
3. **`StaticBuffer<N>::resize()` 静默忽略越界请求**：当 `new_size > N` 时，`resize()` 不抛出异常也不改变内部状态。这是面向 freestanding 环境的设计决策，调用方应自行确保参数合法性。
4. **`copy_from` / `copy_to` 非 constexpr**：这两个函数内部调用 `memcpy`，因此不能在编译期使用。其余成员函数均为 `constexpr`。
5. **`as_string()` 非 constexpr**：由于内部使用了 `reinterpret_cast`，`as_string()` 不具备 `constexpr` 资格。
6. **容量与长度的区分**：`StaticBuffer<N>::capacity()` 返回编译期固定的存储容量 `N`，而 `size()` 返回当前有效数据的逻辑长度。`fill()` 会将 `size()` 设为 `N`，但 `copy_from()` 仅将 `size()` 设为实际拷贝的字节数。
7. **零初始化保证**：`StaticBuffer<N>` 的内部数组 `data_[N]` 使用值初始化（`uint8_t data_[N]{}`），因此默认构造后所有字节均为零。

## 另见

- **Span\<T\>** (`#include <cinux/span.hpp>`) — 通用的非拥有连续内存视图，支持迭代器和子视图操作。`ByteSpan` 和 `ConstByteSpan` 是针对字节类型的别名。
- **StringView** (`#include <cinux/string_view.hpp>`) — 非拥有字符序列视图，不假设以空字符结尾，支持字符串搜索、比较和子串操作。