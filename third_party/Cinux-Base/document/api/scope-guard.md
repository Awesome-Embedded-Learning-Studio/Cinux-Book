以下是完整的中文 API 文档文件：

---

# ScopeGuard

> RAII 作用域清理，scope exit 时执行回调

## 头文件

`#include <cinux/scope_guard.hpp>`

## 依赖

无（叶子组件）

## 概述

`ScopeGuard` 是一个轻量级的 RAII 工具，用于在作用域退出时自动执行一段清理代码。它的设计灵感来源于 Go 语言中的 `defer` 语句和 C++ 社区中广泛讨论的 scope guard 惯用法。在系统编程中，资源管理（如 `munmap`、文件描述符关闭、锁释放等）往往需要手动配对分配与释放操作，容易遗漏或因异常路径导致资源泄漏。`ScopeGuard` 通过将清理动作绑定到对象生命周期，确保无论作用域以何种方式退出（正常返回、提前 `return`、异常抛出），清理代码都会被执行。

该组件仅依赖标准类型特征头文件 `<type_traits>` 和 `<utility>`，属于叶子组件，无任何项目内依赖。模板参数接受任意可调用对象（lambda、函数指针、函数对象），构造函数和 `dismiss()` 方法均标记为 `constexpr`，允许在常量求值上下文中使用。

典型应用场景包括：管理 C 风格资源（`malloc/free`、`mmap/munmap`、`fopen/fclose`）、事务回滚、临时状态恢复、以及任何需要"分配即释放"配对保证的场合。配合 `SCOPE_EXIT` 宏可以进一步简化代码书写，实现声明式的作用域清理。

## API 参考

### 类模板 `ScopeGuard<F>`

```cpp
namespace cinux::lib {

template <typename F>
class ScopeGuard {
public:
    explicit constexpr ScopeGuard(F&& fn);
    constexpr ScopeGuard(ScopeGuard&& other) noexcept;

    ScopeGuard(const ScopeGuard&)            = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard& operator=(ScopeGuard&&)      = delete;

    ~ScopeGuard();

    constexpr void dismiss() noexcept;
};

}  // namespace cinux::lib
```

**模板参数：**
- `F` — 可调用类型（lambda、函数指针、函数对象）。该类型必须可移动构造且支持无参调用 `fn_()`。

---

#### 构造函数

```cpp
explicit constexpr ScopeGuard(F&& fn);
```

从可调用对象构造守卫。接受右值引用，将 `fn` 移动至内部存储。构造后守卫处于 **活跃** 状态，析构时将调用回调。

**参数：**
- `fn` — 在作用域退出时要执行的可调用对象，通过 `std::move` 转移所有权。

**示例：**

```cpp
#include <cinux/scope_guard.hpp>
using namespace cinux::lib;

void example() {
    int* p = new int(42);
    ScopeGuard guard([&] { delete p; });

    // 使用 p ...
    // 作用域退出时自动 delete p
}
```

---

#### 移动构造函数

```cpp
constexpr ScopeGuard(ScopeGuard&& other) noexcept;
```

移动构造，转移守卫所有权。移动后源对象 `other` 变为 **非活跃** 状态（`active_ = false`），其析构时不会执行回调。新对象继承源对象的活跃状态与回调。

**参数：**
- `other` — 源守卫对象。

**注意：** 标记为 `noexcept`，保证移动操作不会抛出异常。

**示例：**

```cpp
int counter = 0;
{
    ScopeGuard first([&] { counter++; });
    ScopeGuard second(std::move(first));
    // first 已变为非活跃，析构时不会调用回调
    // second 将在析构时调用回调
}
// counter == 1
```

---

#### 析构函数

```cpp
~ScopeGuard();
```

若守卫处于活跃状态（未被 `dismiss()`），则调用存储的回调 `fn_()`。

**注意：** 析构函数 **不是** `constexpr` 或 `noexcept`。如果回调抛出异常，异常将在析构过程中传播，这通常应避免（在析构函数中抛出异常会导致 `std::terminate` 若同时有另一个异常处于活跃状态）。

---

#### `dismiss()`

```cpp
constexpr void dismiss() noexcept;
```

解除守卫。调用后，析构时将 **不再** 执行回调。此操作不可逆。

**`noexcept` 保证：** 调用永远不会抛出异常。

**示例：**

```cpp
int counter = 0;
{
    ScopeGuard guard([&] { counter++; });
    guard.dismiss();  // 解除守卫
}
// counter == 0，回调未被执行
```

---

### 宏 `SCOPE_EXIT`

```cpp
#define SCOPE_EXIT(expr)
```

声明一个匿名 `ScopeGuard` 对象，在当前作用域退出时执行 `expr`。内部通过 `__LINE__` 和 `##` 运算符生成唯一变量名，因此同一行只能使用一次 `SCOPE_EXIT`。

**参数：**
- `expr` — 要执行的语句（无需加分号，宏内部已包含）。

**展开结果：**

```cpp
auto _scope_guard_##__LINE__ = ::cinux::lib::ScopeGuard([&]() { expr; })
```

**示例：**

```cpp
#include <cinux/scope_guard.hpp>
using namespace cinux::lib;

void handle_mapping() {
    void* mapping = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return;

    SCOPE_EXIT(munmap(mapping, 4096));

    // 使用 mapping ...
    // 作用域退出时自动调用 munmap(mapping, 4096)
}
```

多个 `SCOPE_EXIT` 以声明的 **逆序** 执行（遵循 C++ 栈展开语义）：

```cpp
int trace = 0;
{
    SCOPE_EXIT(trace = trace * 10 + 1);  // 最后执行
    SCOPE_EXIT(trace = trace * 10 + 2);  // 第二个执行
    SCOPE_EXIT(trace = trace * 10 + 3);  // 第一个执行
}
// trace == 321
```

---

### 辅助宏（内部实现细节）

```cpp
#define CINUX_SCOPE_CONCAT2_(a, b) a##b
#define CINUX_SCOPE_CONCAT_(a, b)  CINUX_SCOPE_CONCAT2_(a, b)
```

这两个宏为 `SCOPE_EXIT` 的内部实现细节，用于在预处理阶段将 `_scope_guard_` 与 `__LINE__` 拼接为唯一标识符。用户代码不应直接使用。

## 注意事项

1. **`constexpr` 限制：** 构造函数和 `dismiss()` 标记为 `constexpr`，但析构函数不是（因为回调中可能包含运行时操作）。因此 `ScopeGuard` 无法作为 `constexpr` 变量的生命周期管理工具使用。

2. **异常安全：** 析构函数中调用回调时若回调抛出异常，且同时已有异常在栈展开过程中传播，将导致 `std::terminate`。建议回调中避免抛出异常。

3. **线程安全：** `ScopeGuard` 本身不提供任何线程同步机制。`dismiss()` 和析构的调用应限于拥有该对象的线程。

4. **拷贝语义：** `ScopeGuard` 是仅移动类型，禁止拷贝构造和所有赋值操作。这是有意为之的设计：拷贝语义在 scope guard 语义下不明确。

5. **`SCOPE_EXIT` 宏在同行的唯一性：** 由于 `SCOPE_EXIT` 使用 `__LINE__` 生成变量名，同一源代码行中不能放置两个 `SCOPE_EXIT` 宏调用，否则会产生变量重定义错误。

6. **回调捕获：** `SCOPE_EXIT` 宏内部使用 `[&]` 按引用捕获。注意所引用的局部变量必须在守卫析构时仍然有效。若需要按值捕获，应直接构造 `ScopeGuard` 对象。

7. **资源管理优先级：** 对于堆内存管理，优先使用智能指针（`std::unique_ptr`、`std::shared_ptr`）。`ScopeGuard` 更适合管理不具备 RAII 包装的 C 风格资源或需要执行任意清理逻辑的场景。

## 另见

- [expected](expected.md) — 可携带错误信息的返回值类型
- [optional](optional.md) — 可选值包装
- [buffer](buffer.md) — 缓冲区管理