现在我已获取了所有必需的信息。以下是完整的中文 API 文档：

---

# ErrorOr\<T\>

> Value/Error 判别式联合体，替代裸错误码

## 头文件

`#include <cinux/expected.hpp>`

## 依赖

无（叶子组件）

## 概述

`ErrorOr<T>` 是一个类型安全的判别式联合体（discriminated union），用于在函数返回时同时携带"成功值"或"错误码"两种可能。它取代了 C/C++ 传统中通过返回裸 `int` 错误码、设置 `errno` 或抛出异常来表达失败的惯用法，要求调用者在编译期即可区分两条路径，从而减少遗漏错误处理的风险。

该组件专为操作系统内核及底层嵌入式场景设计：不使用异常，不依赖 RTTI，不进行动态内存分配。内部通过 placement new 管理联合体成员的生命周期，确保值类型 `T` 的构造与析构行为完全可控。当错误路径被误用时（例如在 `!ok()` 时调用 `value()`），会触发 `assert` 而非抛出异常，适合禁用异常的编译环境。

`ErrorOr<void>` 特化版本面向那些"可能失败但不返回有效载荷"的操作（如磁盘 flush、连接关闭等）。`Error` 枚举预定义了一组常见子系统错误码，并附带 `error_string()` 将其转换为人类可读字符串，方便日志输出。

## API 参考

---

### 枚举 `Error`

```cpp
enum class Error : uint32_t {
    Ok = 0,
    OutOfMemory,        // 内存不足
    InvalidArgument,    // 非法参数
    NotFound,           // 未找到
    IOError,            // I/O 错误
    AlreadyExists,      // 已存在
    PermissionDenied,   // 权限不足
    WouldBlock,         // 操作将阻塞
    BufferOverflow,     // 缓冲区溢出
    NotImplemented,     // 未实现
    BrokenPipe,         // 管道断裂
    ConnectionRefused,  // 连接被拒
    TimedOut,           // 超时
    Busy,               // 资源忙碌
};
```

Cinux 子系统通用错误码枚举。底层类型为 `uint32_t`，可安全用于 `switch` 与数值比较。

**枚举值说明**

| 枚举值 | 数值 | 含义 |
|---|---|---|
| `Ok` | 0 | 无错误（成功） |
| `OutOfMemory` | 1 | 内存分配失败 |
| `InvalidArgument` | 2 | 调用方传入非法参数 |
| `NotFound` | 3 | 请求的资源不存在 |
| `IOError` | 4 | 输入/输出操作失败 |
| `AlreadyExists` | 5 | 资源已存在（重复创建等） |
| `PermissionDenied` | 6 | 权限不足 |
| `WouldBlock` | 7 | 非阻塞模式下操作将阻塞 |
| `BufferOverflow` | 8 | 写入超出缓冲区容量 |
| `NotImplemented` | 9 | 功能尚未实现 |
| `BrokenPipe` | 10 | 管道已断裂 |
| `ConnectionRefused` | 11 | 远端拒绝连接 |
| `TimedOut` | 12 | 操作超时 |
| `Busy` | 13 | 设备或资源正忙 |

---

### 函数 `error_string`

```cpp
constexpr const char* error_string(Error e);
```

将 `Error` 枚举值转换为以空字符结尾的只读字符串字面量。对于未知的枚举值返回 `"Unknown"`。该函数标记为 `constexpr`，可在编译期求值。

**参数**

- `e` — 要转换的 `Error` 枚举值。

**返回值**

`const char*` — 对应错误的可读名称字符串指针，永不为 `nullptr`。

**示例**

```cpp
using namespace cinux::lib;

Error err = Error::TimedOut;
printf("operation failed: %s\n", error_string(err));
// 输出: operation failed: TimedOut
```

---

### 类模板 `ErrorOr<T>`

```cpp
template <typename T>
class ErrorOr;
```

持有类型 `T` 的值或一个 `Error` 错误码的判别式联合体。

#### 构造函数

```cpp
ErrorOr(T value);              // (1) 成功路径：存储值
constexpr ErrorOr(Error err);  // (2) 错误路径：存储错误码
ErrorOr(const ErrorOr& other); // (3) 拷贝构造
ErrorOr(ErrorOr&& other)       // (4) 移动构造
    noexcept(std::is_nothrow_move_constructible<T>::value);
```

**(1) 值构造**

从类型 `T` 的值构造一个成功的 `ErrorOr`。内部通过 `std::move` 将值移动到联合体存储中。非 `explicit`，允许隐式从 `T` 转换，便于函数直接 `return value;`。

**参数** — `value`: 要存储的成功值。

**(2) 错误构造**

从 `Error` 枚举构造一个失败的 `ErrorOr`。标记为 `constexpr`。非 `explicit`，允许隐式从 `Error` 转换，便于函数直接 `return Error::IOError;`。

**参数** — `err`: 错误码。

**(3) 拷贝构造**

深拷贝另一个 `ErrorOr`。若源持有值，通过拷贝构造 `T`；若源持有错误码，直接拷贝 `Error`。

**(4) 移动构造**

移动另一个 `ErrorOr` 的内容。若源持有值，通过移动构造 `T`；若源持有错误码，直接拷贝 `Error`。当 `T` 为 `noexcept` 移动构造时，此函数亦为 `noexcept`。

**示例**

```cpp
ErrorOr<int> success(42);          // 成功，持有值 42
ErrorOr<int> failure(Error::Busy); // 失败，持有错误码 Busy

// 函数中直接返回
ErrorOr<int> read_fd(int fd) {
    int n = sys_read(fd, buf, sizeof(buf));
    if (n < 0) return Error::IOError;
    return n;
}
```

---

#### 赋值运算符

```cpp
ErrorOr& operator=(const ErrorOr& other);  // (1) 拷贝赋值
ErrorOr& operator=(ErrorOr&& other)        // (2) 移动赋值
    noexcept(std::is_nothrow_move_constructible<T>::value);
```

**(1) 拷贝赋值** — 先销毁当前活动成员，再深拷贝 `other`。自赋值安全。

**(2) 移动赋值** — 先销毁当前活动成员，再移动 `other`。自赋值安全。`noexcept` 规格同移动构造。

---

#### 析构函数

```cpp
~ErrorOr();
```

若当前持有值（即 `ok() == true`），调用 `T` 的析构函数；若持有错误码，则无操作。

---

#### `ok`

```cpp
constexpr bool ok() const;
```

判断当前是否持有值（成功路径）。

**返回值** — `true` 表示持有值，`false` 表示持有错误码。标记为 `constexpr`。

---

#### `operator bool`

```cpp
constexpr explicit operator bool() const;
```

隐式转换为 `bool`，语义与 `ok()` 相同。标记为 `explicit`，防止意外的隐式数值转换。

**返回值** — `true` 表示持有值。

**示例**

```cpp
ErrorOr<int> result = some_fn();
if (result) {
    // 成功路径
    use(result.value());
} else {
    // 错误路径
    log(result.error());
}
```

---

#### `value`

```cpp
T& value();
const T& value() const;
```

获取存储的值的引用。**前提条件**: `ok() == true`。若在错误路径上调用，触发 `assert` 失败（调用 `assert(is_ok_ && "ErrorOr::value() called on error")`）。

**返回值** — 存储值的左值引用。

**示例**

```cpp
ErrorOr<int> result(42);
int x = result.value();  // x == 42
result.value() = 100;    // 可通过非 const 重载修改值
```

---

#### `operator*`

```cpp
T& operator*();
const T& operator*() const;
```

解引用运算符，等价于 `value()`。语义前提条件相同：调用时必须 `ok() == true`，否则触发 `assert`。

**返回值** — 存储值的引用。

**示例**

```cpp
ErrorOr<int> r(7);
int x = *r;  // x == 7
```

---

#### `operator->`

```cpp
T* operator->();
const T* operator->() const;
```

箭头运算符，返回指向存储值的指针。前提条件与 `value()` 相同。

**返回值** — 指向存储值的指针。

**示例**

```cpp
struct Point { int x, y; };

ErrorOr<Point> p(Point{3, 4});
int px = p->x;  // px == 3
int py = p->y;  // py == 4
```

---

#### `error`

```cpp
constexpr Error error() const;
```

获取存储的错误码。语义上仅在 `!ok()` 时调用才有意义，但由于 `Error` 为平凡类型且联合体布局共享，此函数在成功路径亦可调用但结果无意义。

**返回值** — 存储的 `Error` 枚举值。标记为 `constexpr`。

**示例**

```cpp
ErrorOr<int> result(Error::NotFound);
if (!result.ok()) {
    printf("failed: %s\n", error_string(result.error()));
    // 输出: failed: NotFound
}
```

---

### 特化 `ErrorOr<void>`

```cpp
template <>
class ErrorOr<void>;
```

面向无返回值操作的特化版本。不含值成员，仅跟踪成功/失败状态及错误码。

#### 构造函数

```cpp
constexpr ErrorOr();           // (1) 默认构造：成功状态 (Error::Ok)
constexpr ErrorOr(Error err);  // (2) 错误路径
```

**(1) 默认构造** — 构造一个成功状态的 `ErrorOr<void>`。

**(2) 错误构造** — 从 `Error` 构造失败状态。非 `explicit`。

#### 拷贝与移动

```cpp
constexpr ErrorOr(const ErrorOr&)            = default;
constexpr ErrorOr(ErrorOr&&)                 = default;
constexpr ErrorOr& operator=(const ErrorOr&) = default;
constexpr ErrorOr& operator=(ErrorOr&&)      = default;
```

全部使用默认实现（均为 trivial），标记为 `constexpr`。

#### `ok`

```cpp
constexpr bool ok() const;
```

**返回值** — `true` 表示成功。

#### `operator bool`

```cpp
constexpr explicit operator bool() const;
```

**返回值** — `true` 表示成功。

#### `error`

```cpp
constexpr Error error() const;
```

**返回值** — 存储的错误码。

**示例**

```cpp
ErrorOr<void> flush_buffer(Buffer& buf) {
    if (!buf.dirty()) return {};                  // 成功
    if (write_to_disk(buf) < 0) return Error::IOError;
    return {};                                     // 成功
}

auto result = flush_buffer(my_buf);
if (!result) {
    log("flush failed: %s", error_string(result.error()));
}
```

---

## 注意事项

1. **异常与 assert**：`value()`、`operator*()`、`operator->()` 在错误路径上调用时会触发 `assert` 而非抛出异常。在 `NDEBUG` 定义（Release 构建）下 `assert` 被移除，此时在错误路径访问值属于未定义行为。建议在调用前始终检查 `ok()` 或 `operator bool()`。

2. **constexpr 限制**：`ErrorOr` 的值构造函数和析构函数因涉及 placement new 与 `T` 的析构调用，不可标记为 `constexpr`。错误路径构造函数 `ErrorOr(Error)`、`ok()`、`operator bool()`、`error()` 及 `error_string()` 均为 `constexpr`，可在编译期使用。`ErrorOr<void>` 的所有成员函数均为 `constexpr`。

3. **线程安全**：`ErrorOr` 本身不提供任何线程同步机制。对同一个 `ErrorOr` 实例的并发读写属于数据竞争，需要外部同步。

4. **值类型要求**：`T` 须为可析构（`Destructible`）、可拷贝构造或可移动构造的类型。`T` 的移动构造 `noexcept` 性质会传播到 `ErrorOr` 的移动构造与移动赋值的 `noexcept` 规格。

5. **自赋值安全**：拷贝赋值与移动赋值运算符均正确处理自赋值场景（`if (this != &other)` 检查）。

6. **无动态分配**：`ErrorOr` 的存储完全内联于对象布局中（通过 `union Storage`），不引入任何堆分配，适用于内核及嵌入式环境。

7. **与 `Error::Ok` 的交互**：`Error::Ok` 虽然存在，但不应用于构造 `ErrorOr<T>` 的"成功"路径——应使用值构造函数。`ErrorOr<void>` 的成功路径应使用默认构造函数（`ErrorOr<void>{}`）。

## 另见

- [Error](error.md) — 错误码枚举定义
- [Optional\<T\>](optional.md) — 无错误码的可空值包装器
- [ScopeGuard](scope-guard.md) — 作用域守卫，常与 `ErrorOr` 配合进行资源清理