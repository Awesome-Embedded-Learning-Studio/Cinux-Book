# StaticString\<N\>

> 固定容量 \0 终止字符串，用于路径/文件名

## 头文件

`#include <cinux/static_string.hpp>`

## 依赖

- `StringView`（`#include <cinux/string_view.hpp>`）
- `<cstddef>`
- `<cstring>`

## 概述

`StaticString<N>` 是一个模板化的固定容量字符串类，内部使用 `char data_[N]` 数组存储数据并以 `\0` 结尾。模板参数 `N` 为总容量（含终止符），因此最多可存储 `N - 1` 个有效字符。整个结构体不涉及任何堆分配，所有数据均位于栈上，非常适合嵌入式内核、驱动程序以及对内存分配有严格约束的场景。

该组件主要面向文件系统路径、设备名称、挂载点等短字符串的存储与操作。类中内置了 `parent_path()` 和 `filename()` 两个路径工具函数，可直接从路径字符串中提取父目录或文件名部分，无需引入额外的字符串分割库。

当容量不足时，追加操作不会抛出异常，而是返回 `false` 以通知调用方操作失败，原有数据保持不变。这种设计在系统编程中尤为实用——调用者可以安全地检查返回值，而无需担心异常带来的控制流不确定性。

## API 参考

### 类模板

```cpp
template <size_t N>
class StaticString;
```

- **N** — 总容量（含 `\0` 终止符）。例如 `StaticString<16>` 最多存储 15 个字符。

---

### 构造函数

#### 默认构造

```cpp
constexpr StaticString() : size_(0) { data_[0] = '\0'; }
```

构造一个空字符串，长度为 0，`c_str()` 返回 `""`。

```cpp
StaticString<32> s;          // s.size() == 0, s.empty() == true
```

#### 从 C 字符串构造

```cpp
constexpr StaticString(const char* str);
```

- **str** — 指向以 `\0` 结尾的 C 字符串指针。若传入 `nullptr`，等同于默认构造。
- 截断行为：若 `str` 长度超过 `N - 1`，仅复制前 `N - 1` 个字符，多余部分被静默丢弃。

```cpp
StaticString<32> s("hello"); // s.size() == 5
StaticString<4>  t("abcde"); // 截断，t.size() == 3，内容为 "abc"
StaticString<8>  n(nullptr); // n.size() == 0，等同于默认构造
```

#### 从字符指针和指定长度构造

```cpp
constexpr StaticString(const char* str, size_t len);
```

- **str** — 源字符数据指针。
- **len** — 期望复制的字符数。实际复制量取 `len` 与 `N - 1` 的较小值。

```cpp
StaticString<16> s("hello world", 5); // s.size() == 5，内容为 "hello"
```

#### 从 StringView 构造

```cpp
constexpr StaticString(StringView sv);
```

等价于 `StaticString(sv.data(), sv.size())`，支持从 `StringView` 隐式构造。

```cpp
StringView sv("world");
StaticString<16> s(sv);  // s.size() == 5，内容为 "world"
```

---

### 访问器

#### capacity

```cpp
static constexpr size_t capacity();
```

返回模板参数 `N`，即总容量（含终止符）。此方法为 `static`。

```cpp
StaticString<64> s;
size_t cap = s.capacity(); // cap == 64
```

#### size

```cpp
constexpr size_t size() const;
```

返回当前有效字符数（不含终止符）。范围 `[0, N - 1]`。

```cpp
StaticString<16> s("hello");
s.size(); // 5
```

#### empty

```cpp
constexpr bool empty() const;
```

当字符串长度为 0 时返回 `true`。

```cpp
StaticString<16> s;
s.empty(); // true
```

#### full

```cpp
constexpr bool full() const;
```

当字符串已达到最大容量（`size_ == N - 1`）时返回 `true`，此时无法再追加任何字符。

```cpp
StaticString<5> s("abcd"); // 4 个字符 + 终止符 = 满容
s.full(); // true
```

#### c_str

```cpp
constexpr const char* c_str() const;
```

返回内部 `data_` 数组的首地址，保证以 `\0` 结尾，可直接用于 C 风格 API。

```cpp
StaticString<32> s("hello");
const char* p = s.c_str(); // "hello"
```

#### operator[]

```cpp
constexpr char  operator[](size_t i) const; // 只读越界安全：越界返回 '\0'
constexpr char& operator[](size_t i);       // 可读写，不检查越界
```

- **i** — 下标。`const` 版本在 `i >= N` 时返回 `\0`；非 `const` 版本不进行越界检查。

```cpp
StaticString<16> s("abc");
char c = s[0];     // 'a'
char x = s[100];   // '\0'（const 版本安全返回）
s[0] = 'A';        // 修改为 "Abc"
```

---

### 修改操作

#### clear

```cpp
constexpr void clear();
```

清空字符串，长度重置为 0，`c_str()` 返回 `""`。

```cpp
StaticString<16> s("hello");
s.clear();
s.empty(); // true
```

#### append(char)

```cpp
constexpr bool append(char c);
```

- **c** — 要追加的字符。
- **返回值** — 成功返回 `true`；若已满（`size_ >= N - 1`）返回 `false`，字符串不变。

```cpp
StaticString<8> s;
s.append('a'); // true
s.append('b'); // true
// s == "ab"
```

#### append(StringView)

```cpp
bool append(StringView sv);
```

逐字符追加 `sv` 中的内容。当容量不足时在失败处停止，返回 `false`；全部成功则返回 `true`。

```cpp
StaticString<8> s;
s.append(StringView("hello")); // true，s == "hello"
```

#### append(const char*)

```cpp
bool append(const char* str);
```

等价于 `append(StringView(str))`。

```cpp
StaticString<8> s("ab");
s.append("cd"); // true，s == "abcd"
```

#### truncate

```cpp
constexpr void truncate(size_t new_size);
```

- **new_size** — 截断后的目标长度。若 `new_size >= size_`，则不做任何操作。

```cpp
StaticString<16> s("hello world");
s.truncate(5);
// s == "hello"
```

---

### 比较操作

#### equals

```cpp
constexpr bool equals(StringView sv) const;
```

将当前字符串以 `StringView` 形式与 `sv` 比较，长度和内容均相同则返回 `true`。

```cpp
StaticString<16> s("hello");
s.equals(StringView("hello")); // true
s.equals(StringView("world")); // false
```

#### operator==

```cpp
constexpr bool operator==(StringView sv) const;
constexpr bool operator==(const StaticString& o) const;
```

支持与 `StringView` 以及同类型 `StaticString` 的相等比较。

```cpp
StaticString<16> a("foo");
StaticString<32> b("foo");
a == StringView("foo"); // true
a == b;                 // true（不同容量但内容相同）
```

---

### 转换操作

#### view

```cpp
constexpr StringView view() const;
```

返回指向内部数据的 `StringView` 对象。

```cpp
StaticString<16> s("test");
StringView sv = s.view(); // sv.size() == 4
```

#### operator StringView

```cpp
constexpr operator StringView() const;
```

隐式转换为 `StringView`，等价于 `view()`。

```cpp
StaticString<16> s("test");
StringView sv = s; // 隐式转换
```

---

### 路径工具

#### parent_path

```cpp
constexpr StaticString parent_path() const;
```

提取父路径。从末尾向前查找最后一个 `/`，返回其之前的内容。

| 输入 | 输出 | 说明 |
|------|------|------|
| `"/foo/bar"` | `"/foo"` | 正常路径 |
| `"/"` | `"/"` | 根目录保持不变 |
| `"file"` | `""` | 无 `/` 则无父路径 |

```cpp
StaticString<64> p("/foo/bar");
auto parent = p.parent_path(); // "/foo"

StaticString<64> root("/");
root.parent_path();            // "/"

StaticString<64> plain("file");
plain.parent_path();           // ""
```

#### filename

```cpp
constexpr StringView filename() const;
```

提取文件名部分。从末尾向前查找最后一个 `/`，返回其后的子串。

| 输入 | 输出 | 说明 |
|------|------|------|
| `"/foo/bar.txt"` | `"bar.txt"` | 正常路径 |
| `"/"` | `""` | 根目录无文件名 |
| `"name"` | `"name"` | 无 `/` 则整体为文件名 |

```cpp
StaticString<64> p("/foo/bar.txt");
p.filename();                  // "bar.txt"

StaticString<64> root("/");
root.filename();               // ""

StaticString<64> noSlash("name");
noSlash.filename();            // "name"
```

---

### 类型别名

```cpp
using PathString = StaticString<256>;  // 路径字符串，最多 255 个字符
using NameString = StaticString<64>;   // 文件名/设备名，最多 63 个字符
```

```cpp
using namespace cinux::lib;

PathString path("/usr/local/bin");  // size() == 14
NameString name("test.txt");        // size() == 8
```

---

## 注意事项

1. **容量包含终止符**：模板参数 `N` 包含 `\0`，因此 `StaticString<16>` 最多存储 15 个有效字符。
2. **截断行为**：从 C 字符串或 `StringView` 构造时，超出容量的部分被静默截断，不会报错。而 `append` 系列方法在容量不足时返回 `false`，调用者应当检查返回值。
3. **constexpr 限制**：大部分方法标注为 `constexpr`，可在编译期求值。但 `append(StringView)` 和 `append(const char*)` 两个重载不是 `constexpr`，因为其内部循环调用了非 `constexpr` 的 `append(char)` 链式调用。
4. **线程安全**：`StaticString` 不内置任何同步机制。多个线程同时读同一个实例是安全的，但并发读写或并发写均需要外部同步。
5. **operator[] 越界**：`const` 版本对越界访问返回 `\0`，是安全的；非 `const` 版本不进行越界检查，越界写入为未定义行为。
6. **非堆分配保证**：所有数据存储在对象内部的固定数组中，适用于禁止动态内存分配的环境（如内核态、中断上下文）。
7. **比较操作**：`operator==` 仅提供相等比较，未提供 `operator!=`、`operator<` 等关系运算符。如需字典序比较，可通过 `.view()` 获取 `StringView` 后进行比较。

## 另见

- **StringView** — 只读字符串视图，`StaticString` 的主要交互类型
- **PathString** / **NameString** — `StaticString` 的常用类型别名