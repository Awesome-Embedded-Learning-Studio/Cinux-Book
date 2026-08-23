# Cinux-Base

> 零 OS 耦合的 C++17 freestanding 基础类型库，为 CinuxOS 内核提供 constexpr、无堆分配的核心组件。

## 特性

- **C++17** — 不使用 C++20 feature，最大兼容性
- **Freestanding** — 仅依赖 `<cstdint>` `<cstddef>` `<type_traits>` `<utility>` `<new>` `<cstring>` `<array>` `<atomic>` 等自由头文件
- **零堆分配** — 禁止 `new` / `malloc` / `::operator new`，所有容器固定容量
- **零异常** — 禁止 `throw` / `try` / `catch`，通过 `ErrorOr<T>` 传播错误
- **零 RTTI** — 禁止 `dynamic_cast` / `typeid`
- **constexpr-first** — 所有可在编译期计算的函数标记 `constexpr`
- **严格编译** — `-Wall -Wextra -Wpedantic -Werror`，无警告通过

## 组件一览

| 类别 | 组件数 | 示例 |
|------|--------|------|
| 错误处理 | 1 | `ErrorOr<T>` |
| 数值/位运算 | 3 | `Numeric`, `BitOps`, `Endian` |
| 字符串/视图 | 3 | `StringView`, `Span<T>`, `StaticString<N>` |
| 容器 | 4 | `StaticBuffer<N>`, `RingBuffer<T,N>`, `IntrusiveList<T>`, `StaticHashMap<K,V,N>` |
| 工具 | 4 | `Optional<T>`, `BitMap<N>`, `Function<Sig>`, `ScopeGuard` |
| 校验 | 2 | `CRC32`, `Checksum` |
| 高层 | 3 | `Logger`, `StaticRandomSource`, `Algorithm` |
| **总计** | **20** | |

→ 完整列表见 [document/component-index.md](document/component-index.md)

## 快速构建

```bash
# 配置 + 编译 + 测试（三步）
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

> 需要CMake 3.16+ 及支持 C++17 的编译器（GCC 7+ / Clang 5+）。

## 集成到你的项目

```cmake
# 方式一：add_subdirectory
add_subdirectory(cinux-base)
target_link_libraries(your_target PRIVATE cinux_base)

# 方式二：FetchContent
include(FetchContent)
FetchContent_Declare(cinux_base GIT_REPOSITORY https://github.com/CinuxOS/Cinux-Base.git)
FetchContent_MakeAvailable(cinux_base)
target_link_libraries(your_target PRIVATE cinux_base)
```

→ 完整说明见 [document/build-guide.md](document/build-guide.md)

## 代码示例

```cpp
#include <cinux/expected.hpp>
#include <cinux/string_view.hpp>
#include <cinux/scope_guard.hpp>

using namespace cinux::lib;

ErrorOr<int> parse_port(StringView str) {
    if (str.empty()) return Error::InvalidArgument;
    int port = 0;
    for (size_t i = 0; i < str.size(); ++i) {
        char c = str[i];
        if (c < '0' || c > '9') return Error::InvalidArgument;
        port = port * 10 + (c - '0');
    }
    return port;
}
```

→ 更多示例见 [document/quick-start.md](document/quick-start.md) 和 [document/api/](document/api/)

## 项目结构

```
include/cinux/        20 个头文件（15 header-only + 1 detail/）
src/                  4 个编译单元（CRC32, Checksum, Logger, vformat）
test/                 22 个测试文件，Catch2 v3，170 个测试用例
document/             架构文档、构建指南、组件 API 文档
```

## 文档

| 文档 | 说明 |
|------|------|
| [architecture.md](document/architecture.md) | 设计哲学、约束、依赖图 |
| [build-guide.md](document/build-guide.md) | CMake 配置、编译选项、集成方式 |
| [component-index.md](document/component-index.md) | 组件速查表 |
| [quick-start.md](document/quick-start.md) | 5 分钟上手教程 |
| [api/](document/api/) | 20 个组件独立 API 文档 |

## 许可证

MIT License — 详见 [LICENSE](LICENSE)
