这是我基于头文件和测试文件生成的完整 API 文档。

---

# StringView

> 零分配字符串视图，不假设 `\0` 终止

## 头文件

`#include <cinux/string_view.hpp>`

## 依赖

无

## 概述

`cinux::lib::StringView` 是一个非拥有 (non-owning)、零分配的字符串视图类。它仅持有一个指向外部字符序列的 `const char*` 指针和一个长度值，不复制、不管理底层内存，因此在拷贝和传递时开销极低。与 C 风格字符串不同，`StringView` 不假设底层数据以 `\0` 结尾，因此可以安全地引用更大缓冲区中的子串或任意内存片段。

该组件适用于需要频繁传递、比较、查找子串但不需要修改字符串内容的场景，例如命令行参数解析、协议头部字段提取、日志消息格式化等。所有公开方法均标记为 `constexpr`，可在编译期求值，这使得 `StringView` 也非常适合用于模板元编程和静态断言中的字符串处理。

`StringView` 的设计目标是轻量与可控：越界访问 `operator[]` 返回 `'\0'` 而非触发未定义行为；`substr` 在参数越界时返回空视图而非导致崩溃。这种防御性语义使其在嵌入式和内核等不容忍异常的环境中尤为适用。

## API 参考

### 类 `cinux::lib::StringView`

```cpp
namespace cinux::lib {
class StringView { /* ... */ };
}
```

非拥有字符序列视图，所有成员函数均为 `constexpr`。

---

### 静态常量

#### `npos`

```cpp
static constexpr size_t npos = static_cast<size_t>(-1);
```

"未找到"哨兵值，等价于 `size_t` 的最大值。`find`、`rfind` 在查找失败时返回此值；`substr` 将其作为 `count` 的默认值以表示"取到末尾"。

**示例**

```cpp
if (sv.find('x') == StringView::npos) {
    // 字符 'x' 不存在
}
```

---

### 构造函数

#### 默认构造

```cpp
constexpr StringView() = default;
```

构造一个空视图：`data()` 为 `nullptr`，`size()` 为 `0`。

**示例**

```cpp
cinux::lib::StringView sv;   // 空
assert(sv.empty());
```

#### 从 C 字符串构造

```cpp
constexpr StringView(const char* str);
```

从以 `\0` 结尾的 C 字符串构造视图。若 `str` 为 `nullptr`，则构造空视图。内部通过编译期 `strlen` 计算长度。

| 参数     | 说明                                    |
| -------- | --------------------------------------- |
| `str`    | 指向以 `\0` 结尾的字符数组的指针，可为 `nullptr` |

**示例**

```cpp
cinux::lib::StringView sv("hello");
assert(sv.size() == 5);
```

#### 从指针 + 长度构造

```cpp
constexpr StringView(const char* str, size_t len);
```

从原始指针和显式长度构造视图。不假设 `\0` 终止，调用者需保证 `[str, str+len)` 范围内的内存有效。

| 参数  | 说明                   |
| ----- | ---------------------- |
| `str` | 指向字符序列的指针     |
| `len` | 字符序列的字节长度     |

**示例**

```cpp
const char* buf = "hello world";
cinux::lib::StringView sv(buf, 5);  // "hello"
assert(sv == cinux::lib::StringView("hello"));
```

---

### 访问器

#### `size`

```cpp
constexpr size_t size() const;
```

返回视图所引用的字符数。

**返回值**：字符数量，类型 `size_t`。

**示例**

```cpp
cinux::lib::StringView sv("abc");
assert(sv.size() == 3);
```

#### `empty`

```cpp
constexpr bool empty() const;
```

判断视图是否为空（长度为 0）。

**返回值**：`true` 表示为空。

**示例**

```cpp
cinux::lib::StringView sv;
assert(sv.empty());
```

#### `data`

```cpp
constexpr const char* data() const;
```

返回指向底层字符序列的指针。不保证以 `\0` 结尾。

**返回值**：`const char*`，空视图时为 `nullptr`。

**示例**

```cpp
cinux::lib::StringView sv("hello");
// data() 指向 'h'，但不保证 data()[5] == '\0'（虽然本例碰巧如此）
```

#### `operator[]`

```cpp
constexpr char operator[](size_t i) const;
```

按下标访问字符。越界访问返回 `'\0'` 而非触发未定义行为。

| 参数 | 说明       |
| ---- | ---------- |
| `i`  | 字符索引   |

**返回值**：对应位置的字符，越界时返回 `'\0'`。

**示例**

```cpp
cinux::lib::StringView sv("ab");
assert(sv[0] == 'a');
assert(sv[5] == '\0');  // 越界安全
```

#### `front`

```cpp
constexpr char front() const;
```

返回第一个字符。空视图返回 `'\0'`。

**返回值**：首字符或 `'\0'`。

**示例**

```cpp
cinux::lib::StringView sv("abc");
assert(sv.front() == 'a');
```

#### `back`

```cpp
constexpr char back() const;
```

返回最后一个字符。空视图返回 `'\0'`。

**返回值**：末字符或 `'\0'`。

**示例**

```cpp
cinux::lib::StringView sv("abc");
assert(sv.back() == 'c');
```

---

### 比较

#### `compare`

```cpp
constexpr int compare(StringView o) const;
```

按字典序（逐字节，按 `unsigned char` 比较）与另一个视图比较。

| 参数 | 说明         |
| ---- | ------------ |
| `o`  | 待比较的视图 |

**返回值**：负数表示 `*this < o`，零表示相等，正数表示 `*this > o`。

**示例**

```cpp
cinux::lib::StringView a("abc"), b("abd");
assert(a.compare(b) < 0);
assert(a.compare(a) == 0);
```

#### `equals`

```cpp
constexpr bool equals(StringView o) const;
```

判断是否与另一个视图内容完全相同。

**返回值**：相同返回 `true`。

**示例**

```cpp
cinux::lib::StringView a("abc"), b("abc");
assert(a.equals(b));
```

#### 比较运算符

```cpp
constexpr bool operator==(StringView o) const;
constexpr bool operator!=(StringView o) const;
constexpr bool operator< (StringView o) const;
constexpr bool operator<=(StringView o) const;
constexpr bool operator> (StringView o) const;
constexpr bool operator>=(StringView o) const;
```

全部基于 `compare` 实现。由于构造函数接受 `const char*`，右侧可直接传入字符串字面量。

**示例**

```cpp
using cinux::lib::StringView;

StringView a("abc");
StringView b("abc");
assert(a == b);
assert(a == "abc");       // 隐式构造
assert(a != "xyz");
assert(a < "abd");
assert(a <= "abc");
assert("abd" > a);
assert(a >= "abc");
```

---

### 前缀/后缀判断

#### `starts_with`

```cpp
constexpr bool starts_with(StringView prefix) const;
```

判断当前视图是否以给定前缀开头。空前缀始终匹配。

| 参数      | 说明     |
| --------- | -------- |
| `prefix`  | 前缀视图 |

**返回值**：以该前缀开头返回 `true`。

**示例**

```cpp
cinux::lib::StringView sv("hello world");
assert(sv.starts_with("hello"));
assert(sv.starts_with(""));          // 空前缀始终匹配
assert(!sv.starts_with("world"));
```

#### `ends_with`

```cpp
constexpr bool ends_with(StringView suffix) const;
```

判断当前视图是否以给定后缀结尾。空后缀始终匹配。

| 参数      | 说明     |
| --------- | -------- |
| `suffix`  | 后缀视图 |

**返回值**：以该后缀结尾返回 `true`。

**示例**

```cpp
cinux::lib::StringView sv("hello world");
assert(sv.ends_with("world"));
assert(sv.ends_with(""));
assert(!sv.ends_with("hello"));
```

---

### 搜索

#### `find`（单字符）

```cpp
constexpr size_t find(char c, size_t pos = 0) const;
```

从指定位置开始正向查找字符。

| 参数  | 说明                         |
| ----- | ---------------------------- |
| `c`   | 待查找的字符                 |
| `pos` | 起始位置，默认为 0           |

**返回值**：首次出现的下标；未找到返回 `npos`。

**示例**

```cpp
cinux::lib::StringView sv("hello world");
assert(sv.find('o') == 4);
assert(sv.find('o', 5) == 7);
assert(sv.find('z') == StringView::npos);
```

#### `find`（子串）

```cpp
constexpr size_t find(StringView needle, size_t pos = 0) const;
```

从指定位置开始正向查找子串。查找空串时返回 `pos`（若 `pos <= size()`）。

| 参数     | 说明                   |
| -------- | ---------------------- |
| `needle` | 待查找的子串视图       |
| `pos`    | 起始位置，默认为 0     |

**返回值**：子串首次出现的起始下标；未找到返回 `npos`。

**示例**

```cpp
cinux::lib::StringView sv("hello world");
assert(sv.find("world") == 6);
assert(sv.find("hello") == 0);
assert(sv.find("xyz") == StringView::npos);
assert(sv.find("") == 0);           // 空串在位置 0 找到
```

#### `rfind`（单字符反向查找）

```cpp
constexpr size_t rfind(char c) const;
```

反向查找字符最后一次出现的位置。

| 参数 | 说明           |
| ---- | -------------- |
| `c`  | 待查找的字符   |

**返回值**：最后一次出现的下标；未找到返回 `npos`。

**示例**

```cpp
cinux::lib::StringView sv("hello");
assert(sv.rfind('l') == 3);
assert(sv.rfind('z') == StringView::npos);
```

---

### 子串

#### `substr`

```cpp
constexpr StringView substr(size_t pos, size_t count = npos) const;
```

返回从 `pos` 开始、长度为 `count` 的子视图。若 `pos >= size()` 则返回空视图；若 `count` 超出剩余长度则截断到末尾。默认 `count = npos` 表示取到末尾。

| 参数    | 说明                                          |
| ------- | --------------------------------------------- |
| `pos`   | 起始位置                                      |
| `count` | 子串长度，默认 `npos` 表示取到视图末尾        |

**返回值**：新的 `StringView` 对象。

**示例**

```cpp
cinux::lib::StringView sv("hello world");

assert(sv.substr(6) == cinux::lib::StringView("world"));
assert(sv.substr(0, 5) == cinux::lib::StringView("hello"));
assert(sv.substr(100) == cinux::lib::StringView());         // 越界返回空
assert(sv.substr(6, 100) == cinux::lib::StringView("world")); // count 截断
```

---

## 注意事项

1.  **生命周期**：`StringView` 不拥有底层内存。调用者必须确保 `StringView` 所引用的字符序列在其使用期间保持有效。引用临时 `std::string` 的 `c_str()` 或已释放的缓冲区会导致悬空指针。
2.  **constexpr 限制**：所有公开成员函数均为 `constexpr`，在 C++20 及以上标准中可作为编译期常量使用。在 C++17 下，`constexpr` 的 `StringView` 构造和操作在运行时同样有效。
3.  **线程安全**：`StringView` 本身是只读视图，多个线程可同时读取同一个 `StringView` 对象而无需同步。但如果底层缓冲区被其他线程修改，则行为未定义。
4.  **无 `\0` 保证**：`data()` 返回的指针不一定指向以 `\0` 结尾的字符串。将 `data()` 传递给期望 C 字符串的函数（如 `printf("%s", sv.data())`）前，必须确保底层缓冲区以 `\0` 结尾。
5.  **越界安全性**：`operator[]`、`front()`、`back()` 在越界或空视图时返回 `'\0'`，不触发未定义行为。这是一种防御性设计，但调用者不应依赖此行为来检测错误。
6.  **字符编码**：`StringView` 按字节操作，不理解 UTF-8 或其他多字节编码。包含多字节字符的字符串在使用下标操作时可能产生不完整的结果。
7.  **与 `std::string_view` 的区别**：`cinux::lib::StringView` 的设计类似于 `std::string_view`（C++17），但有意不依赖标准库，适用于嵌入式或内核等无法使用 STL 的环境。接口子集较小，不提供迭代器、流输出等特性。

## 另见

- `std::string_view` — C++17 标准库中的等价组件
- `cinux::lib::String` — 如果存在，为拥有所有权的字符串类
- `cinux::lib::Vector` — 如果存在，可用于构建动态字符缓冲区