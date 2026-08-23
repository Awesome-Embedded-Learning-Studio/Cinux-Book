以下是 `Function<Sig,N>` 组件的完整中文 API 文档：

---

# Function\<Sig, N\>

> 类型擦除 callable，小缓冲优化，无堆分配

## 头文件

`#include <cinux/function.hpp>`

## 依赖

无（仅依赖标准库头文件 `<cstddef>`、`<cstdint>`、`<new>`、`<type_traits>`、`<utility>`）

## 概述

`Function<Sig, N>` 是一个轻量级的类型擦除可调用对象包装器，设计灵感来源于 `std::function`，但与传统实现存在关键差异：**绝不进行堆分配**。所有被包装的可调用对象均存储在对象内部的内联缓冲区中，缓冲区大小由模板参数 `N`（默认 32 字节）控制。当被包装对象的大小超过 `InlineSize` 时，会在编译期通过 `static_assert` 报错，而非在运行时回退到堆分配。

该组件适用于嵌入式系统、实时系统以及对内存分配行为有严格约束的场景。通过函数指针跳板（trampoline）机制实现类型擦除，存储了三个函数指针分别负责调用、销毁和拷贝操作，避免了虚函数开销和动态分配。被包装的可调用对象必须满足 trivially copyable 或 nothrow move constructible 约束。

典型用例包括：回调函数存储、事件处理器注册、策略模式的运行时切换等。由于其零堆分配特性，它特别适合在确定性的内存环境中使用。

## API 参考

### 类模板 `Function`

```cpp
namespace cinux::lib {

template <typename Sig, size_t InlineSize = 32>
class Function;  // 主模板，未定义

template <typename R, typename... Args, size_t InlineSize>
class Function<R(Args...), InlineSize>;
}
```

**模板参数：**

| 参数 | 说明 |
|------|------|
| `R` | 返回值类型 |
| `Args...` | 参数类型列表 |
| `InlineSize` | 内联缓冲区大小（字节数），默认为 `32`。所有被包装的可调用对象的大小不得超过此值，否则触发编译错误 |

---

### 默认构造函数

```cpp
constexpr Function() = default;
```

构造一个空的、无效的 `Function` 对象。调用 `valid()` 返回 `false`。

**示例：**

```cpp
Function<int(int)> f;
assert(!f.valid());
```

---

### `nullptr_t` 构造函数

```cpp
constexpr Function(std::nullptr_t);
```

构造一个空的、无效的 `Function` 对象。语义上等价于默认构造。

**示例：**

```cpp
Function<int(int)> f = nullptr;
assert(!f.valid());
```

---

### 函数指针构造函数

```cpp
constexpr Function(R (*fn)(Args...));
```

从自由函数指针构造。若 `fn` 非空，将其存入内联缓冲区并设置相应的跳板函数；若为空，行为等同于默认构造。

**参数：**

| 参数 | 说明 |
|------|------|
| `fn` | 指向自由函数的指针，签名须为 `R(Args...)` |

**示例：**

```cpp
static int double_it(int x) { return x * 2; }

Function<int(int)> f = double_it;
assert(f(21) == 42);
```

---

### 通用可调用对象构造函数

```cpp
template <typename F,
          typename = std::enable_if_t<!std::is_same<std::decay_t<F>, Function>::value>>
constexpr Function(F&& f);
```

从任意可调用对象（lambda、仿函数等）构造。通过完美转发将对象存入内联缓冲区。

**模板约束：**

- `sizeof(std::decay_t<F>) <= InlineSize`：可调用对象的大小不得超过内联缓冲区大小，否则触发 `static_assert` 编译错误，提示信息为 `"Function: callable exceeds InlineSize — capture less data or increase N"`
- `std::is_trivially_copyable<Decayed>::value || std::is_nothrow_move_constructible<Decayed>::value`：可调用对象必须是 trivially copyable 或 nothrow move constructible，否则触发 `static_assert` 编译错误

**参数：**

| 参数 | 说明 |
|------|------|
| `f` | 可调用对象（lambda、仿函数、函数对象等） |

**示例：**

```cpp
// 无状态 lambda
Function<int(int)> f1 = [](int x) { return x + 1; };
assert(f1(9) == 10);

// 有状态 lambda（捕获指针，大小不超过 32 字节）
int value = 100;
int* ptr = &value;
Function<int()> f2 = [ptr]() { return *ptr; };
assert(f2() == 100);
```

---

### 拷贝构造函数

```cpp
constexpr Function(const Function& other);
```

深拷贝。将被包装的可调用对象从 `other` 的内联缓冲区复制到当前对象。拷贝通过内部存储的 `copy_` 跳板函数完成。

**参数：**

| 参数 | 说明 |
|------|------|
| `other` | 要拷贝的源 `Function` 对象 |

**示例：**

```cpp
Function<int(int)> a = [](int x) { return x * 3; };
Function<int(int)> b = a;            // 深拷贝
assert(b(5) == 15);
assert(a.valid());                   // 源对象仍然有效
```

---

### 移动构造函数

```cpp
constexpr Function(Function&& other) noexcept;
```

移动构造。将被包装对象从 `other` 的内联缓冲区复制到当前对象，随后将 `other` 重置为无效状态。

**参数：**

| 参数 | 说明 |
|------|------|
| `other` | 要移动的源 `Function` 对象。移动完成后 `other.valid() == false` |

** noexcept 保证：** 是

**示例：**

```cpp
Function<int(int)> a = [](int x) { return x * 3; };
Function<int(int)> b = std::move(a);
assert(b(5) == 15);
assert(!a.valid());                  // 移动源变为无效
```

---

### 拷贝赋值运算符

```cpp
constexpr Function& operator=(const Function& other);
```

先销毁当前持有的可调用对象（`reset()`），再从 `other` 深拷贝。处理自赋值情况。

**参数：**

| 参数 | 说明 |
|------|------|
| `other` | 要拷贝赋值的源对象 |

**返回值：** `*this` 的引用

---

### 移动赋值运算符

```cpp
constexpr Function& operator=(Function&& other) noexcept;
```

先销毁当前持有的可调用对象（`reset()`），再从 `other` 移动，随后将 `other` 重置为无效状态。处理自赋值情况。

**参数：**

| 参数 | 说明 |
|------|------|
| `other` | 要移动赋值的源对象 |

**返回值：** `*this` 的引用

** noexcept 保证：** 是

---

### 析构函数

```cpp
~Function();
```

若当前持有有效的可调用对象，通过内部 `destroy_` 跳板函数调用其析构函数。

---

### `operator()`

```cpp
R operator()(Args... args) const;
```

调用被包装的可调用对象，转发所有参数。若对象无效（`invoke_ == nullptr`），行为未定义（解引用空函数指针）。

**参数：**

| 参数 | 说明 |
|------|------|
| `args` | 转发给被包装可调用对象的参数 |

**返回值：** 被包装可调用对象的返回值，类型为 `R`

**示例：**

```cpp
Function<int(int)> f = [](int x) { return x * 2; };
int result = f(21);  // result == 42
```

---

### `valid()`

```cpp
constexpr bool valid() const;
```

查询当前对象是否持有一个有效的可调用对象。

**返回值：** 若持有有效可调用对象返回 `true`，否则返回 `false`

**示例：**

```cpp
Function<int(int)> f;
assert(!f.valid());

f = [](int x) { return x; };
assert(f.valid());

f.reset();
assert(!f.valid());
```

---

### `operator bool()`

```cpp
constexpr explicit operator bool() const;
```

等价于 `valid()` 的显式布尔转换运算符。

**返回值：** 与 `valid()` 相同

**示例：**

```cpp
Function<int(int)> f = [](int x) { return x; };
if (f) {
    // f 有效，可安全调用
    f(10);
}
```

---

### `reset()`

```cpp
constexpr void reset();
```

销毁当前持有的可调用对象（若有），并将对象重置为无效状态。之后 `valid()` 返回 `false`。

**示例：**

```cpp
Function<int(int)> f = [](int x) { return x; };
assert(f.valid());
f.reset();
assert(!f.valid());
```

---

## 注意事项

1. **无堆分配**：与 `std::function` 不同，`Function` 永远不会在堆上分配内存。超出 `InlineSize` 的可调用对象会在编译期报错，而非静默回退到堆分配。

2. **大小限制**：默认内联缓冲区为 32 字节。捕获大量数据的 lambda 可能超过此限制。解决方法有两种：减少捕获量，或增大模板参数 `N`：
   ```cpp
   // 使用 64 字节缓冲区
   Function<int(int), 64> f = [big_capture]() { /* ... */ };
   ```

3. **类型约束**：被包装的可调用对象必须满足 `std::is_trivially_copyable` 或 `std::is_nothrow_move_constructible`。不满足这两个约束的对象将导致编译错误。

4. **调用无效对象的后果**：对无效的 `Function`（未赋值、已 `reset()`、或已被移动）调用 `operator()` 属于未定义行为。调用前应通过 `valid()` 或 `operator bool()` 检查。

5. **对齐保证**：内联缓冲区使用 `alignas(max_align_t)` 对齐，确保能满足所有标量类型的对齐要求。

6. **与 `std::function` 的兼容性**：`Function` 不提供 `target_type()`、`target<>()` 等类型查询接口，也不支持 `std::function` 的 allocator 支持。它是一个极简化的替代方案。

7. **void 返回类型**：完全支持 `void` 返回类型的函数签名：
   ```cpp
   int counter = 0;
   Function<void()> f = [&counter]() { counter++; };
   f();
   ```

## 另见

- `std::function` — 标准库类型擦除可调用对象包装器（允许堆分配）
- `std::move_only_function` (C++23) — 仅移动的可调用对象包装器
- `llvm::function_ref` — 轻量级非拥有函数引用