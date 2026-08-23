# 快速上手

> 5 分钟集成 Cinux-Base 到你的项目。

## 第一步：获取代码

```bash
# 方式 A：git clone
git clone https://github.com/CinuxOS/Cinux-Base.git
# 方式 B：作为 submodule
git submodule add https://github.com/CinuxOS/Cinux-Base.git external/cinux-base
```

## 第二步：配置 CMake

创建或修改你的 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

# 引入 Cinux-Base
add_subdirectory(external/cinux-base)

add_executable(my_app src/main.cpp)
target_link_libraries(my_app PRIVATE cinux_base)
```

## 第三步：编写代码

```cpp
// src/main.cpp
#include <cinux/expected.hpp>
#include <cinux/numeric.hpp>
#include <cinux/string_view.hpp>
#include <cinux/scope_guard.hpp>
#include <cstdio>

using namespace cinux::lib;
using namespace cinux::lib::literals;

// ErrorOr 用于函数返回值错误传播
ErrorOr<int> parse_decimal(StringView str) {
    if (str.empty()) {
        return Error::InvalidArgument;
    }

    int value = 0;
    for (size_t i = 0; i < str.size(); ++i) {
        char c = str[i];
        if (c < '0' || c > '9') {
            return Error::InvalidArgument;
        }
        value = value * 10 + (c - '0');
    }
    return value;
}

int main() {
    // 使用 ScopeGuard 做清理
    FILE* f = std::fopen("test.txt", "w");
    if (!f) return 1;
    SCOPE_EXIT(std::fclose(f));

    // 使用 ErrorOr 处理错误
    auto result = parse_decimal("12345");
    if (!result.ok()) {
        std::fprintf(f, "Parse failed: %s\n", error_string(result.error()));
        return 1;
    }
    std::fprintf(f, "Parsed: %d\n", result.value());

    // 使用 constexpr 数值工具
    constexpr auto page_size = 4_KB;
    constexpr auto aligned = align_up(5000, page_size);
    static_assert(aligned == 8192);
    std::fprintf(f, "5000 aligned to 4KB = %lu\n", aligned);

    // 使用 StringView 做字符串操作
    StringView path = "/usr/local/bin/app";
    auto slash = path.rfind('/');
    if (slash != StringView::npos) {
        StringView filename = path.substr(slash + 1);
        std::fprintf(f, "Filename: %.*s\n",
                     static_cast<int>(filename.size()), filename.data());
    }

    return 0;
}
```

## 第四步：编译运行

```bash
cmake -B build
cmake --build build
./build/my_app
```

输出：
```
Parsed: 12345
5000 aligned to 4KB = 8192
Filename: app
```

## 常见使用模式

### 仅使用 header-only 组件

如果你的项目只用到 ErrorOr、StringView、Span、Numeric、BitOps、Endian、Optional、ScopeGuard 等 header-only 组件，可以只链接 header 目标：

```cmake
target_link_libraries(my_app PRIVATE cinux_base_headers)
```

这不会编译任何 .cpp 文件，只会添加头文件搜索路径和编译选项。

### 错误处理

```cpp
ErrorOr<void> do_something() {
    auto result = might_fail();
    if (!result.ok()) {
        return result.error();  // 传播错误
    }
    use(result.value());
    return {};  // 成功（ErrorOr<void>）
}
```

### 使用容器

```cpp
#include <cinux/static_string.hpp>
#include <cinux/bitmap.hpp>
#include <cinux/ring_buffer.hpp>

cinux::lib::StaticString<256> path;
path.append("/dev/");
path.append("sda");

cinux::lib::BitMap<4096> page_bitmap;
page_bitmap.set_range(0, 1024);  // 标记前 1024 页已用

cinux::lib::RingBuffer<uint8_t, 256> uart_buf;
uart_buf.push(0xAA);
uint8_t byte;
if (uart_buf.pop(byte)) {
    // 成功取出
}
```

## 下一步

- [组件速查表](component-index.md) — 查看所有可用组件
- [API 文档](api/) — 各组件完整 API 参考和示例
- [架构设计](architecture.md) — 深入了解设计约束和依赖关系
