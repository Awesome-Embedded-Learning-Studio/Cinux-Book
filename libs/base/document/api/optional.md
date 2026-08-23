# Optional\<T\>

> 可选值容器，"找到/未找到" 语义

## 头文件

`#include <cinux/optional.hpp>`

## 依赖

无

## 概述

`Optional<T>` 是一个轻量级的可选值容器，用于表示一个值"可能存在，也可能不存在"的场景。相比于传统的错误码或返回指针的方式，`Optional<T>` 以值语义清晰地表达"找到/未找到"的意图，使 API 更安全、更易读。

在 Cinux 基础库中，`Optional<T>` 的定位比 `ErrorOr` 更轻量。当操作失败的原因无关紧要，只需区分"有结果"与"无结果"时，应优先使用 `Optional<T>`。典型的使用场景包括：在容器中查找元素、从映射表中查询键、解析可能缺失的配置项等。

该实现使用联合体（union）存储避免不必要的堆分配，所有值均内联保存在 `Optional` 对象自身内部。同时，移动构造函数标注了 `noexcept`（当 `T` 的移动构造为 `noexcept` 时），使其可以在对异常安全有要求的上下文中放心使用。

## API 参考

### 命名空间

所有符号位于 `cinux::lib` 命名空间下。

---

### `NulloptT` 结构体

```cpp
struct NulloptT {
    explicit constexpr NulloptT(int) {}
};
```

用于表示"空值"的标记类型。构造函数为 `explicit`，不会与普通整数发生隐式转换。

---

### `nullopt` 常量

```cpp
inline constexpr NulloptT nullopt{0};
```

全局常量，作为 `Optional<T>` 的空状态哨兵值。可将其赋值给 `Optional<T>` 以清除所持有的值，也可在构造时直接使用。

**用法示例：**

```cpp
using namespace cinux::lib;

Optional<int> a = nullopt;   // 构造一个空的 Optional
Optional<int> b(42);
b = nullopt;                  // 清除 b 中持有的值
```

---

### `Optional<T>` 类模板

```cpp
template <typename T>
class Optional;
```

可能持有也可能不持有类型为 `T` 的值的容器。

**模板参数：**
- `T` — 被包装的值类型。需可复制构造或可移动构造。

---

#### 默认构造函数

```cpp
constexpr Optional();
```

构造一个不持有任何值的空 `Optional` 对象。

**用法示例：**

```cpp
Optional<int> o;
// o.has_value() == false
```

---

#### `NulloptT` 构造函数

```cpp
constexpr Optional(NulloptT);
```

以 `nullopt` 构造一个空的 `Optional` 对象。语义与默认构造相同，但代码意图更加明确。

**用法示例：**

```cpp
Optional<int> o = nullopt;
```

---

#### 拷贝构造函数（从 `T&`）

```cpp
Optional(const T& value);
```

通过拷贝给定值来构造一个持有该值的 `Optional`。

**参数：**
- `value` — 要持有的值的常量引用。

**用法示例：**

```cpp
int x = 42;
Optional<int> o(x);  // o 持有 42 的拷贝
```

---

#### 移动构造函数（从 `T&&`）

```cpp
Optional(T&& value);
```

通过移动给定值来构造一个持有该值的 `Optional`。

**参数：**
- `value` — 要移动的值的右值引用。

**用法示例：**

```cpp
Optional<std::string> o(std::string("hello"));
```

---

#### 拷贝构造函数（从 `Optional&`）

```cpp
Optional(const Optional& other);
```

拷贝另一个 `Optional`。若 `other` 持有值，则拷贝该值；否则构造空对象。

**参数：**
- `other` — 要拷贝的另一个 `Optional`。

**用法示例：**

```cpp
Optional<int> a(42);
Optional<int> b = a;  // b 也持有 42
```

---

#### 移动构造函数（从 `Optional&&`）

```cpp
Optional(Optional&& other) noexcept(std::is_nothrow_move_constructible<T>::value);
```

移动另一个 `Optional`。若 `other` 持有值，则移动该值；否则构造空对象。

**参数：**
- `other` — 要移动的另一个 `Optional`。

**`noexcept` 说明：** 当且仅当 `T` 的移动构造函数为 `noexcept` 时，本函数为 `noexcept`。

**用法示例：**

```cpp
Optional<std::string> a(std::string("hello"));
Optional<std::string> b = std::move(a);  // b 持有 "hello"，a 进入有效但未指定的状态
```

---

#### 拷贝赋值运算符

```cpp
Optional& operator=(const Optional& other);
```

拷贝赋值。先销毁当前持有的值（若有），再根据 `other` 是否持有值决定是否构造新值。

**参数：**
- `other` — 要拷贝的源对象。

**返回值：** `*this` 的引用。

**用法示例：**

```cpp
Optional<int> a(10);
Optional<int> b;
b = a;  // b 现在持有 10
```

---

#### 移动赋值运算符

```cpp
Optional& operator=(Optional&& other) noexcept(std::is_nothrow_move_constructible<T>::value);
```

移动赋值。先销毁当前持有的值（若有），再根据 `other` 是否持有值决定是否移动构造新值。

**参数：**
- `other` — 要移动的源对象。

**返回值：** `*this` 的引用。

**`noexcept` 说明：** 当且仅当 `T` 的移动构造函数为 `noexcept` 时，本函数为 `noexcept`。

**用法示例：**

```cpp
Optional<std::string> a(std::string("world"));
Optional<std::string> b;
b = std::move(a);
```

---

#### `nullopt` 赋值运算符

```cpp
Optional& operator=(NulloptT);
```

将 `Optional` 重置为空状态，等效于调用 `reset()`。

**返回值：** `*this` 的引用。

**用法示例：**

```cpp
Optional<int> o(42);
o = nullopt;  // o 不再持有任何值
```

---

#### 析构函数

```cpp
~Optional();
```

若当前持有值，则调用该值的析构函数；否则不做任何操作。

---

#### `has_value()`

```cpp
constexpr bool has_value() const;
```

查询 `Optional` 是否持有值。

**返回值：** 若持有值则返回 `true`，否则返回 `false`。

**用法示例：**

```cpp
Optional<int> a(42);
Optional<int> b;

a.has_value();  // true
b.has_value();  // false
```

---

#### `operator bool()`

```cpp
constexpr explicit operator bool() const;
```

向 `bool` 的显式转换，等效于 `has_value()`。由于为 `explicit`，不会在隐式上下文中触发转换。

**返回值：** 若持有值则返回 `true`，否则返回 `false`。

**用法示例：**

```cpp
Optional<int> o(42);
if (o) {
    // o 持有值，可以安全访问
}
```

---

#### `value()`

```cpp
T& value();
const T& value() const;
```

获取所持有值的引用。若 `Optional` 为空，则触发断言失败（`assert`）。

**返回值：** 所持有值的引用（左值版本返回左值引用，常量版本返回常量引用）。

**前置条件：** `has_value() == true`。在空 `Optional` 上调用为未定义行为（断言失败）。

**用法示例：**

```cpp
Optional<int> o(42);
int x = o.value();  // x == 42
```

---

#### `operator*()`

```cpp
T& operator*();
const T& operator*() const;
```

解引用运算符，等效于调用 `value()`。若 `Optional` 为空，则触发断言失败。

**返回值：** 所持有值的引用。

**前置条件：** `has_value() == true`。

**用法示例：**

```cpp
Optional<int> o(42);
int x = *o;  // x == 42
```

---

#### `operator->()`

```cpp
T* operator->();
const T* operator->() const;
```

成员访问运算符，返回指向所持有值的指针。若 `Optional` 为空，则触发断言失败。

**返回值：** 指向所持有值的指针。

**前置条件：** `has_value() == true`。

**用法示例：**

```cpp
struct Point { int x, y; };
Optional<Point> p(Point{3, 4});
int px = p->x;  // px == 3
int py = p->y;  // py == 4
```

---

#### `value_or()`

```cpp
constexpr T value_or(const T& default_val) const;
```

若 `Optional` 持有值则返回该值的拷贝，否则返回给定的默认值。

**参数：**
- `default_val` — 当 `Optional` 为空时返回的默认值。

**返回值：** 持有值的拷贝或 `default_val` 的拷贝。

**用法示例：**

```cpp
Optional<int> a(10);
Optional<int> b;

int x = a.value_or(99);  // x == 10
int y = b.value_or(99);  // y == 99
```

---

#### `reset()`

```cpp
void reset();
```

销毁当前持有的值（若有），将 `Optional` 重置为空状态。

**用法示例：**

```cpp
Optional<int> o(42);
o.reset();  // o 不再持有任何值
// o.has_value() == false
```

---

#### `emplace()`

```cpp
template <typename... Args>
T& emplace(Args&&... args);
```

原地销毁当前持有的值（若有），并在 `Optional` 内部直接构造一个新值。适用于非拷贝、非移动的场景，或需要传递多个构造参数的情况。

**参数：**
- `args...` — 传递给 `T` 构造函数的参数包。

**返回值：** 新构造值的引用。

**用法示例：**

```cpp
Optional<int> o;
o.emplace(77);    // o 现在持有 77
o.emplace(88);    // 旧值被销毁，o 现在持有 88

Optional<std::string> s;
s.emplace(5, 'x');  // s 现在持有 "xxxxx"
```

---

## 注意事项

1. **空值访问为未定义行为：** 在空的 `Optional` 上调用 `value()`、`operator*()` 或 `operator->()` 会触发 `assert` 断言失败。在 `NDEBUG` 宏定义下（Release 构建），断言被移除，此时访问为真正的未定义行为。使用前务必检查 `has_value()` 或 `operator bool()`。

2. **constexpr 限制：** `Optional` 的默认构造函数、`NulloptT` 构造函数、`has_value()`、`operator bool()` 和 `value_or()` 被标记为 `constexpr`。但涉及 placement `new` 的构造函数（从 `T` 构造、拷贝/移动构造、`emplace`）不是 `constexpr`，因此 `Optional<T>` 不能用作编译期常量来持有非平凡值。

3. **拷贝语义开销：** 拷贝构造和拷贝赋值会对 `T` 进行拷贝。对于拷贝开销较大的类型（如 `std::vector`、`std::string`），应优先使用移动操作。

4. **赋值操作不回收旧值内存：** 拷贝赋值和移动赋值会先调用旧值的析构函数再构造新值（先析构后构造），而非使用拷贝/移动赋值运算符。这意味着如果 `T` 内部分配了内存，赋值操作会释放旧内存再分配新内存，而非复用。

5. **线程安全：** `Optional<T>` 本身不提供任何线程同步机制。对同一个 `Optional` 实例的并发访问（其中至少有一个为写操作）需要外部同步。

6. **自赋值安全：** 拷贝赋值和移动赋值运算符通过 `this != &other` 检查保证自赋值安全。

7. **与 `std::optional` 的差异：** 本实现刻意保持精简，不提供 `map`、`flat_map`、`and_then` 等函数式组合子，也不支持与 `std::optional` 互操作。如需完整的 `optional` 功能，请使用标准库的 `std::optional`（C++17 起）。

## 另见

- `std::optional` — C++17 标准库的可选值类型，功能更完整
- `ErrorOr` — Cinux 基础库中带错误信息的返回值类型，适用于需要区分多种失败原因的场景