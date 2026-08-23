# 构建指南

> 如何编译、测试和集成 Cinux-Base 到你的项目。

## 前置条件

| 工具 | 最低版本 |
|------|---------|
| CMake | 3.16+ |
| GCC | 7+（推荐 9+） |
| Clang | 5+（推荐 10+） |
| Make / Ninja | 任意 |

## 基本构建

```bash
# 配置（默认开启测试）
cmake -B build

# 编译
cmake --build build

# 运行测试
ctest --test-dir build --output-on-failure
```

## CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `CINUX_BUILD_TESTS` | `ON` | 构建测试（需要网络下载 Catch2） |
| `CMAKE_CXX_STANDARD` | `17` | C++ 标准（不要改） |

关闭测试构建（无需下载 Catch2）：

```bash
cmake -B build -DCINUX_BUILD_TESTS=OFF
```

## CMake 目标

Cinux-Base 提供两个 CMake 目标：

### `cinux_base_headers`（INTERFACE 库）

仅包含头文件搜索路径和编译选项。适用于只使用 header-only 组件（ErrorOr、StringView、Span 等）的项目。

```cmake
target_link_libraries(your_target PRIVATE cinux_base_headers)
```

### `cinux_base`（STATIC 库）

包含所有头文件 + 编译后的 .cpp 组件（CRC32、Checksum、Logger、Random、Algorithm）。**当使用任何 .cpp 组件时必须链接此目标**。

```cmake
target_link_libraries(your_target PRIVATE cinux_base)
```

> 当 `src/` 中没有 .cpp 文件时，`cinux_base` 自动别名为 `cinux_base_headers`。

## 集成方式

### 方式一：add_subdirectory

将 Cinux-Base 作为子目录包含：

```
your_project/
├── CMakeLists.txt
├── external/
│   └── cinux-base/     ← git submodule
└── src/
    └── main.cpp
```

```cmake
# your_project/CMakeLists.txt
cmake_minimum_required(VERSION 3.16)
project(your_project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

add_subdirectory(external/cinux-base)

add_executable(your_app src/main.cpp)
target_link_libraries(your_app PRIVATE cinux_base)
```

### 方式二：FetchContent

无需手动克隆：

```cmake
cmake_minimum_required(VERSION 3.16)
project(your_project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(
    cinux_base
    GIT_REPOSITORY https://github.com/CinuxOS/Cinux-Base.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(cinux_base)

add_executable(your_app src/main.cpp)
target_link_libraries(your_app PRIVATE cinux_base)
```

### 方式三：安装后 find_package

```bash
# 先安装 Cinux-Base
cd cinux-base
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build
cmake --install build
```

```cmake
# 然后在你的项目中
find_package(cinux_base REQUIRED)
target_link_libraries(your_app PRIVATE cinux_base)
```

## 编译标志说明

链接 `cinux_base` 或 `cinux_base_headers` 会自动继承以下编译标志：

| 标志 | 说明 |
|------|------|
| `-std=c++17` | C++17 标准 |
| `-Wall -Wextra -Wpedantic` | 全面警告 |
| `-Werror` | 警告视为错误 |
| `-Wold-style-cast` | 禁止 C 风格转换 |
| `-Wshadow` | 禁止变量遮蔽 |
| `-fno-exceptions` | 禁用 C++ 异常 |
| `-fno-rtti` | 禁用 RTTI |

如果你的项目使用异常或 RTTI，可以用 `target_compile_options` 在你的目标上覆盖：

```cmake
target_link_libraries(your_app PRIVATE cinux_base)
# 如果你的项目需要异常，覆盖继承的标志
target_compile_options(your_app PRIVATE -fexceptions)
```

## 仅使用 header-only 组件

如果你的项目只用到 header-only 的组件（ErrorOr、StringView、Span、Optional 等），可以只链接 `cinux_base_headers`，不需要编译任何 .cpp 文件：

```cmake
# 不链接 cinux_base，只链接 headers
target_link_libraries(your_app PRIVATE cinux_base_headers)
```

## 测试

测试使用 Catch2 v3（通过 FetchContent 自动下载）。

```bash
# 运行全部测试
ctest --test-dir build --output-on-failure

# 运行特定组件的测试
ctest --test-dir build -R "numeric"

# 详细输出
cmake --build build --target test_verbose
```

## 另见

- [快速上手](quick-start.md) — 5 分钟完整示例
- [架构设计](architecture.md) — 设计哲学和约束
- [组件速查表](component-index.md) — 哪些组件需要 .cpp 编译
