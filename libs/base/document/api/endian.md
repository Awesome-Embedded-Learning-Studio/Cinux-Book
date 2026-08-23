以下是 Endian 组件的完整中文 API 文档 markdown 文件：

```markdown
# Endian

> 字节序转换 — bswap、htobe/htole、POSIX 别名

## 头文件

`#include <cinux/endian.hpp>`

## 依赖

无（叶子组件）

## 概述

`cinux::lib::endian` 模块提供了一套完整的编译期字节序检测与转换工具函数。在现代系统编程中，处理网络协议、文件格式、硬件寄存器等场景时，经常需要在主机字节序与大端序（Big-Endian）或小端序（Little-Endian）之间进行转换。本组件以 `constexpr` 函数的形式提供了 `bswap16`/`bswap32`/`bswap64` 底层字节反转操作，以及 `htobe`、`htole`、`betoh`、`letoh` 等语义清晰的主机序转换函数，覆盖 16 位、32 位、64 位三种宽度。

本组件利用编译器预定义宏 `__BYTE_ORDER__` 在编译期自动检测当前主机的字节序，所有转换函数均为 `constexpr`，既可用于运行时，也可用于编译期常量表达式（如 `static_assert` 或模板参数）。此外，模块在文件头部主动 `#undef` 了 glibc `<endian.h>` 中与函数名冲突的预处理器宏（如 `htobe16`、`ntohs` 等），确保在同时引入系统头文件时不会产生宏展开冲突。

模块还提供了 `ntohs`、`ntohl`、`htons`、`htonl` 四个 POSIX 风格别名，它们分别等价于 `betoh`/`htobe` 系列函数（网络字节序即大端序），方便已有 POSIX 代码的迁移。整个组件不依赖任何外部库，是一个零依赖的叶子组件。

## API 参考

### 字节序检测

#### `is_little_endian`

```cpp
namespace cinux::lib {

constexpr bool is_little_endian();

}  // namespace cinux::lib
```

在编译期检测当前主机是否为小端序。

**返回值**：若主机为小端序返回 `true`，否则返回 `false`。函数为 `constexpr`，可用于 `static_assert` 或 `if constexpr` 等编译期上下文。

**示例**

```cpp
#include <cinux/endian.hpp>

static_assert(cinux::lib::is_little_endian(), "此构建假设小端序平台");

if constexpr (cinux::lib::is_little_endian()) {
    // 小端序特有的代码路径
}
```

---

#### `is_big_endian`

```cpp
namespace cinux::lib {

constexpr bool is_big_endian();

}  // namespace cinux::lib
```

在编译期检测当前主机是否为大端序。逻辑上等价于 `!is_little_endian()`。

**返回值**：若主机为大端序返回 `true`，否则返回 `false`。

**示例**

```cpp
#include <cinux/endian.hpp>

static_assert(!cinux::lib::is_big_endian(), "此平台预期为大端序");
```

---

### 字节反转（bswap）

#### `bswap16`

```cpp
namespace cinux::lib {

constexpr uint16_t bswap16(uint16_t v);

}  // namespace cinux::lib
```

将 16 位无符号整数的两个字节反转。

**参数**：
- `v` — 待反转的 16 位无符号整数。

**返回值**：字节反转后的 16 位无符号整数。

**示例**

```cpp
#include <cinux/endian.hpp>

constexpr auto result = cinux::lib::bswap16(uint16_t{0x1234});
// result == 0x3412

static_assert(cinux::lib::bswap16(uint16_t{0x0001}) == 0x0100);
static_assert(cinux::lib::bswap16(uint16_t{0xFF00}) == 0x00FF);
static_assert(cinux::lib::bswap16(uint16_t{0x0000}) == 0x0000);
```

---

#### `bswap32`

```cpp
namespace cinux::lib {

constexpr uint32_t bswap32(uint32_t v);

}  // namespace cinux::lib
```

将 32 位无符号整数的四个字节反转。

**参数**：
- `v` — 待反转的 32 位无符号整数。

**返回值**：字节反转后的 32 位无符号整数。

**示例**

```cpp
#include <cinux/endian.hpp>

constexpr auto result = cinux::lib::bswap32(uint32_t{0x12345678});
// result == 0x78563412

static_assert(cinux::lib::bswap32(uint32_t{0x00000001}) == 0x01000000);
static_assert(cinux::lib::bswap32(uint32_t{0xFF000000}) == 0x000000FF);
```

---

#### `bswap64`

```cpp
namespace cinux::lib {

constexpr uint64_t bswap64(uint64_t v);

}  // namespace cinux::lib
```

将 64 位无符号整数的八个字节反转。

**参数**：
- `v` — 待反转的 64 位无符号整数。

**返回值**：字节反转后的 64 位无符号整数。

**示例**

```cpp
#include <cinux/endian.hpp>

constexpr auto result = cinux::lib::bswap64(uint64_t{0x0123456789ABCDEFULL});
// result == 0xEFCDAB8967452301ULL
```

---

### 主机序与大端序互转（htobe / betoh）

#### `htobe16` / `htobe32` / `htobe64`

```cpp
namespace cinux::lib {

constexpr uint16_t htobe16(uint16_t v);
constexpr uint32_t htobe32(uint32_t v);
constexpr uint64_t htobe64(uint64_t v);

}  // namespace cinux::lib
```

将主机字节序的无符号整数转换为大端序（Big-Endian）。若主机本身为大端序，则原样返回；若为小端序，则执行字节反转。

**参数**：
- `v` — 主机字节序的无符号整数值。

**返回值**：大端序表示的无符号整数。

**示例**

```cpp
#include <cinux/endian.hpp>

using cinux::lib::htobe16;
using cinux::lib::htobe32;
using cinux::lib::htobe64;

// 在小端序主机上：
// htobe16(0x0001) == 0x0100
// htobe32(0x12345678) == 0x78563412

uint32_t big_endian_value = htobe32(0xDEADBEEFu);
```

---

#### `betoh16` / `betoh32` / `betoh64`

```cpp
namespace cinux::lib {

constexpr uint16_t betoh16(uint16_t v);
constexpr uint32_t betoh32(uint32_t v);
constexpr uint64_t betoh64(uint64_t v);

}  // namespace cinux::lib
```

将大端序的无符号整数转换为主机字节序。在小端序主机上执行字节反转，在大端序主机上原样返回。由于转换是对称的，`betoh` 与 `htobe` 在实现上完全相同。

**参数**：
- `v` — 大端序表示的无符号整数值。

**返回值**：主机字节序的无符号整数。

**示例**

```cpp
#include <cinux/endian.hpp>

using cinux::lib::htobe32;
using cinux::lib::betoh32;

// 往返转换保证还原原值：
uint32_t original = 0xDEADBEEFu;
assert(betoh32(htobe32(original)) == original);
```

---

### 主机序与小端序互转（htole / letoh）

#### `htole16` / `htole32` / `htole64`

```cpp
namespace cinux::lib {

constexpr uint16_t htole16(uint16_t v);
constexpr uint32_t htole32(uint32_t v);
constexpr uint64_t htole64(uint64_t v);

}  // namespace cinux::lib
```

将主机字节序的无符号整数转换为小端序（Little-Endian）。若主机本身为小端序，则原样返回；若为大端序，则执行字节反转。

**参数**：
- `v` — 主机字节序的无符号整数值。

**返回值**：小端序表示的无符号整数。

**示例**

```cpp
#include <cinux/endian.hpp>

using cinux::lib::htole16;
using cinux::lib::htole32;

// 在小端序主机上，htole 为恒等操作：
// htole16(0x0001) == 0x0001

// 将主机值写入小端序文件格式：
uint32_t le_value = htole32(file_header_size);
```

---

#### `letoh16` / `letoh32` / `letoh64`

```cpp
namespace cinux::lib {

constexpr uint16_t letoh16(uint16_t v);
constexpr uint32_t letoh32(uint32_t v);
constexpr uint64_t letoh64(uint64_t v);

}  // namespace cinux::lib
```

将小端序的无符号整数转换为主机字节序。在大端序主机上执行字节反转，在小端序主机上原样返回。`letoh` 与 `htole` 在实现上完全相同。

**参数**：
- `v` — 小端序表示的无符号整数值。

**返回值**：主机字节序的无符号整数。

**示例**

```cpp
#include <cinux/endian.hpp>

using cinux::lib::htole32;
using cinux::lib::letoh32;

// 往返转换保证还原原值：
uint32_t original = 0xDEADBEEFu;
assert(letoh32(htole32(original)) == original);
```

---

### POSIX 风格别名（网络字节序）

#### `htons` / `htonl`

```cpp
namespace cinux::lib {

constexpr uint16_t htons(uint16_t v);
constexpr uint32_t htonl(uint32_t v);

}  // namespace cinux::lib
```

"Host to Network Short" / "Host to Network Long"。将主机字节序的值转换为网络字节序（即大端序）。分别等价于 `htobe16` 和 `htobe32`。

**参数**：
- `v` — 主机字节序的无符号整数值。

**返回值**：网络字节序（大端序）表示的无符号整数。

---

#### `ntohs` / `ntohl`

```cpp
namespace cinux::lib {

constexpr uint16_t ntohs(uint16_t v);
constexpr uint32_t ntohl(uint32_t v);

}  // namespace cinux::lib
```

"Network to Host Short" / "Network to Host Long"。将网络字节序（大端序）的值转换为主机字节序。分别等价于 `betoh16` 和 `betoh32`。

**参数**：
- `v` — 网络字节序（大端序）表示的无符号整数值。

**返回值**：主机字节序的无符号整数。

**示例**

```cpp
#include <cinux/endian.hpp>

using cinux::lib::htons;
using cinux::lib::htonl;
using cinux::lib::ntohs;
using cinux::lib::ntohl;

// 往返转换：
uint16_t port = 8080;
uint16_t net_port = htons(port);
assert(ntohs(net_port) == port);

uint32_t addr = 0x0A000001u;  // 10.0.0.1
uint32_t net_addr = htonl(addr);
assert(ntohl(net_addr) == addr);
```

---

## 注意事项

- **`constexpr` 限制**：所有函数均为 `constexpr`，可在编译期常量表达式中使用。但在 C++11/14 模式下，`constexpr` 函数体仅允许单条 `return` 语句；本实现完全兼容 C++11。
- **宏冲突处理**：glibc 的 `<endian.h>` 头文件将 `htobe16`、`ntohs` 等名称定义为预处理器宏。本组件在文件头部主动 `#undef` 了这些宏，以避免命名冲突。如果在使用中遇到"宏未定义"之类的编译警告，请确保 `<cinux/endian.hpp>` 的引入顺序正确。
- **命名空间隔离**：由于 `htons`/`ntohs` 等名称与 GCC 内置函数及系统宏存在潜在冲突，建议使用时通过完全限定名 `cinux::lib::htons(...)` 或 `using cinux::lib::htons;` 的方式调用，**切勿**使用 `using namespace cinux::lib;`，否则可能引发歧义。
- **仅支持无符号整数**：所有转换函数仅接受 `uint16_t`、`uint32_t`、`uint64_t` 类型的参数。如有符号类型或 `uint8_t` 的转换需求，需由调用方自行处理。
- **线程安全**：所有函数均为无状态纯函数（pure function），天然线程安全。
- **字节序检测**：字节序检测依赖编译器预定义宏 `__BYTE_ORDER__`（GCC/Clang 均支持）。若编译器未提供该宏，组件默认假设为小端序（即 `CINUX_LITTLE_ENDIAN` 为 `1`），适用于 x86\_64 及主流 ARM 平台。

## 另见

（待补充：其他相关组件的文档链接，如网络字节序使用场景相关的组件）
```