# 架构设计

> Cinux-Base 的设计哲学、约束体系和组件依赖关系。

## 设计哲学

Cinux-Base 的核心目标是**零 OS 耦合**——所有组件可以在 freestanding 环境（无标准库、无操作系统）中编译和使用。这使得同一套代码可以同时服务于：

- **内核态** — CinuxOS 内核的内存管理、驱动、网络协议栈
- **引导加载** — bootloader 阶段的工具函数
- **用户态** — 嵌入式应用或测试环境

## C++17 约束

### 禁止使用的 C++ 特性

| 禁止 | 替代方案 |
|------|---------|
| `throw` / `try` / `catch` | `ErrorOr<T>` 错误传播 |
| `dynamic_cast` / `typeid` | `static_cast`，访问者模式 |
| `new` / `malloc` / `::operator new` | 固定容量容器、placement new |
| `<vector>` `<string>` `<iostream>` | 自实现的容器和视图类型 |
| `<algorithm>` `<optional>` `<string_view>` | 自实现的 Algorithm / Optional / StringView |
| `<memory>` `<functional>` | 自实现的 Function（SBO） |
| `#define` 常量 | `constexpr` |
| C 风格转换 `(Type)value` | `static_cast<>` / `reinterpret_cast<>` |
| `typedef` | `using` |

### 允许使用的标准头文件

| 头文件 | 用途 |
|--------|------|
| `<cstddef>` `<cstdint>` | 基础类型（size_t, uint32_t 等） |
| `<cstdarg>` | 可变参数（Logger/vformat） |
| `<cstring>` | memset/memcpy 声明 |
| `<type_traits>` | 类型特征（is_trivially_destructible 等） |
| `<utility>` | std::move / std::forward / std::swap |
| `<array>` | std::array（替代自定义 Array） |
| `<atomic>` | std::atomic（替代自定义 Atomic） |
| `<new>` | placement new |
| `<cassert>` | assert 宏（仅测试和 debug 构建） |

## 命名空间

所有公共 API 位于 `cinux::lib` 命名空间：

```cpp
cinux::lib::ErrorOr<int> result = some_fn();
cinux::lib::StringView name("hello");
auto bytes = cinux::lib::4_KB;  // 字面量在 cinux::lib::literals 中
```

### 命名规范

| 元素 | 风格 | 示例 |
|------|------|------|
| 头文件 | `snake_case.hpp` | `string_view.hpp` |
| 类/结构/枚举 | `PascalCase` | `StringView`, `Error` |
| 函数/变量 | `snake_case` | `align_up()`, `find()` |
| 成员变量 | `snake_case_`（尾下划线） | `data_`, `size_` |
| 常量 | `kPascalCase` | `kPageSize` |
| 宏 | `UPPER_SNAKE_CASE` | `CINUX_LITTLE_ENDIAN` |

## 分发策略

### Header-Only（15 个组件）

模板和 constexpr 工具函数直接定义在 `.hpp` 中，无需编译：

```
expected.hpp, numeric.hpp, scope_guard.hpp, bit_ops.hpp, endian.hpp,
string_view.hpp, span.hpp, optional.hpp, static_string.hpp, bitmap.hpp,
buffer.hpp, ring_buffer.hpp, intrusive_list.hpp, static_hash_map.hpp, function.hpp
```

### .hpp + .cpp 拆分（4 个组件）

包含运行时代码（查找表、va_list 格式化、状态管理）的组件拆分为声明和实现，避免每个翻译单元实例化大量代码：

```
crc32.hpp + crc32.cpp          查找表（256 × uint32_t）
checksum.hpp + checksum.cpp    RFC 1071 循环实现
logger.hpp + logger.cpp        va_list 格式化 + sink 管理
random.hpp + random.cpp        xoshiro256** 状态机
algorithm.hpp + algorithm.cpp  insertion_sort 实现
detail/vformat.hpp + detail/vformat.cpp  格式化引擎（内部 API）
```

## 错误处理模型

Cinux-Base 使用 `ErrorOr<T>` 作为统一的错误传播机制：

```cpp
// 返回值或错误
ErrorOr<void> init_device();
ErrorOr<void*> map_memory(uint64_t addr, size_t len);
ErrorOr<int> parse_int(StringView str);

// 使用方式
auto result = parse_int(str);
if (!result.ok()) {
    // 处理错误
    handle_error(result.error());
    return;
}
// 使用值
use(result.value());
```

`Error` 枚举覆盖了 13 种常见错误码（OutOfMemory、NotFound、IOError 等）。对于"找到/未找到"语义，使用更轻量的 `Optional<T>`。

## 内存模型

所有容器在编译期确定容量，零堆分配：

| 容器 | 容量指定方式 | 内存来源 |
|------|------------|---------|
| `StaticBuffer<N>` | 模板参数 N | 栈/全局数组 |
| `RingBuffer<T,N>` | 模板参数 N | 栈/全局数组 |
| `StaticHashMap<K,V,N>` | 模板参数 N | 栈/全局数组 |
| `StaticString<N>` | 模板参数 N | 栈/全局数组 |
| `BitMap<N>` | 模板参数 N | 栈/全局数组 |
| `Function<Sig,InlineSize>` | 默认 32 字节 | 栈内联缓冲 |
| `IntrusiveList<T>` | 由调用方管理 | 嵌入式节点 |

## 依赖图

```
Phase 1 — 叶子组件（无内部依赖）
  ErrorOr    Numeric    ScopeGuard    BitOps    Endian    CRC32
    │          │                       │          │
    ▼          ▼                       ▼          ▼
Phase 2 — 基础类型
  StringView   Span   Optional   StaticString(→StringView)   BitMap(→BitOps)
    │  ╲         │       │
    │   ╲        │       │
    ▼    ▼       ▼       ▼
Phase 3 — 容器与工具
  Buffer(→StringView,Span)   RingBuffer(→Span)   IntrusiveList
  StaticHashMap(→Optional,StringView)   Function   Checksum(→Span,Endian)
    │          │            │
    ▼          ▼            ▼
Phase 4 — 高层组件
  Logger(→Function,vformat)   StaticRandomSource(→Span)   Algorithm(→Span)

Phase 5 — 内部组件
  detail/vformat（Logger 内部使用）
```

## 编译标志

```bash
-std=c++17                  # C++17 标准
-Wall -Wextra -Wpedantic    # 全面警告
-Werror                      # 警告视为错误
-Wold-style-cast -Wshadow   # 禁止 C 转换和变量遮蔽
-fno-exceptions              # 禁用异常
-fno-rtti                    # 禁用 RTTI
```

## 另见

- [构建指南](build-guide.md) — 如何编译和集成
- [组件速查表](component-index.md) — 所有组件一览
- [API 文档](api/) — 各组件详细 API 参考
