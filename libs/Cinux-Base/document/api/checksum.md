这是完整的中文 API 文档，以 Markdown 格式呈现：

```markdown
# Checksum

> Internet 校验和（RFC 1071），用于 IP/TCP/UDP/ICMP 校验

## 头文件

`#include <cinux/checksum.hpp>`

## 依赖

Span, Endian

## 概述

本模块实现了 RFC 1071 所定义的 Internet 校验和算法。Internet 校验和是 IP、TCP、UDP、ICMP 等网络协议共用的校验机制，其核心思想是：将数据按 16 位字累加，将进位折叠回低 16 位，最后取反得到校验值。该算法简单高效，适合在协议栈中逐跳验证数据完整性。

`cinux::lib` 命名空间提供了从底层字节流计算校验和、验证已含校验字段的数据报、构建 TCP/UDP 伪头部部分和以及将 32 位部分和折叠为 16 位校验和的完整工具链。用户既可以直接传入原始指针和长度进行计算，也可以使用 `ConstByteSpan` 进行类型安全的调用。

典型使用场景包括：构造或解析 IP 报文头部时计算/验证校验和、在实现用户态 TCP/UDP 协议栈时生成伪头部部分和并叠加负载数据完成校验、以及对任意字节流进行完整性校验。所有函数均为无状态纯函数，线程安全且可重入。

## API 参考

### `internet_checksum(const void* data, size_t len)` — 指针版本

```cpp
namespace cinux::lib {

uint16_t internet_checksum(const void* data, size_t len);

}
```

**功能：** 对给定的原始字节缓冲区计算 Internet 校验和（RFC 1071）。

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `data` | `const void*` | 指向待校验数据的指针 |
| `len` | `size_t` | 数据的字节长度 |

**返回值：** `uint16_t`，计算所得的 16 位 Internet 校验和。对零长度输入返回 `0xFFFF`。

**算法细节：**

1. 将数据按大端序组合为 16 位字并累加到 32 位和变量中。
2. 若数据长度为奇数，末尾补零字节（即末尾字节左移 8 位后加入累加和）。
3. 调用 `finalize_checksum` 将 32 位部分和折叠并取反，得到最终校验和。

**示例：**

```cpp
#include <cinux/checksum.hpp>

using namespace cinux::lib;

// 计算 IP 头部校验和（校验和字段先置零）
uint8_t ip_header[] = {
    0x45, 0x00, 0x00, 0x14, 0x00, 0x00,
    0x00, 0x00, 0x40, 0x06, 0x00, 0x00,  // checksum 字段 = 0
    0x7F, 0x00, 0x00, 0x01,              // src = 127.0.0.1
    0x7F, 0x00, 0x00, 0x01               // dst = 127.0.0.1
};

uint16_t cs = internet_checksum(ip_header, sizeof(ip_header));

// 将校验和填入头部字节 10-11（大端序）
ip_header[10] = static_cast<uint8_t>(cs >> 8);
ip_header[11] = static_cast<uint8_t>(cs & 0xFF);
```

---

### `internet_checksum(ConstByteSpan data)` — Span 版本

```cpp
namespace cinux::lib {

uint16_t internet_checksum(ConstByteSpan data);

}
```

**功能：** 与指针版本功能完全相同，接受 `ConstByteSpan` 类型参数，提供类型安全的接口。指针版本内部即委托给此重载。

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `data` | `ConstByteSpan` | 包含待校验数据的字节视图 |

**返回值：** `uint16_t`，计算所得的 16 位 Internet 校验和。

**示例：**

```cpp
#include <cinux/checksum.hpp>
#include <cinux/span.hpp>

using namespace cinux::lib;

uint8_t buffer[] = {0x01, 0x02, 0x03, 0x04};
ConstByteSpan span(buffer, sizeof(buffer));

uint16_t cs = internet_checksum(span);
```

---

### `verify_internet_checksum(const void* data, size_t len)`

```cpp
namespace cinux::lib {

bool verify_internet_checksum(const void* data, size_t len);

}
```

**功能：** 验证已嵌入校验和字段的数据报是否有效。对包含校验和字段的完整数据重新计算 Internet 校验和，若结果为 `0x0000` 则数据有效。

**原理：** 根据 RFC 1071，若数据中的校验和字段已被正确填入，则对整个数据报重新计算校验和的结果应为零（因为正确校验和的补再与自身累加后取反等于零）。

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `data` | `const void*` | 指向包含校验和字段的完整数据报的指针 |
| `len` | `size_t` | 数据报的字节长度 |

**返回值：** `bool`，`true` 表示校验通过（数据有效），`false` 表示校验失败（数据损坏）。

**示例：**

```cpp
#include <cinux/checksum.hpp>

using namespace cinux::lib;

// ip_header 中已填入正确的校验和字段
uint8_t ip_header[20] = { /* 含校验和的 IP 头部 */ };

if (verify_internet_checksum(ip_header, sizeof(ip_header))) {
    // 校验通过，数据有效
} else {
    // 校验失败，数据已损坏
}
```

---

### `pseudo_header_partial(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol, uint16_t payload_len)`

```cpp
namespace cinux::lib {

uint32_t pseudo_header_partial(uint32_t src_ip, uint32_t dst_ip,
                               uint8_t protocol, uint16_t payload_len);

}
```

**功能：** 计算 TCP/UDP 伪头部（pseudo header）的部分累加和。伪头部是 TCP/UDP 校验和计算中的关键组成部分，包含源 IP、目的 IP、协议号和负载长度，用于防止误投数据报。

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `src_ip` | `uint32_t` | 源 IPv4 地址（网络字节序存储为 32 位无符号整数） |
| `dst_ip` | `uint32_t` | 目的 IPv4 地址（网络字节序存储为 32 位无符号整数） |
| `protocol` | `uint8_t` | 上层协议号（如 `6` = TCP，`17` = UDP） |
| `payload_len` | `uint16_t` | TCP/UDP 负载（含头部）的总长度 |

**返回值：** `uint32_t`，伪头部的部分累加和。该值需与 TCP/UDP 头部及数据的校验和部分和相加后，再通过 `finalize_checksum` 折叠为最终校验和。

**计算过程：**

1. 将源 IP 的高 16 位和低 16 位分别加入累加和。
2. 将目的 IP 的高 16 位和低 16 位分别加入累加和。
3. 将协议号（零扩展至 16 位）加入累加和。
4. 将负载长度加入累加和。

**示例：**

```cpp
#include <cinux/checksum.hpp>

using namespace cinux::lib;

// 构建 TCP 伪头部部分和
// 源地址 192.168.1.1，目的地址 10.0.0.1，协议 TCP(6)，TCP 报文总长 40
uint32_t src = 0xC0A80101;  // 192.168.1.1
uint32_t dst = 0x0A000001;  // 10.0.0.1
uint32_t partial = pseudo_header_partial(src, dst, 6, 40);

// 将部分和与 TCP 头部+数据的校验和合并
uint32_t tcp_partial = internet_checksum(/* 指针方式无法直接返回 32 位，此处需自定义或分段累加 */);
// 实际使用中可分别累加再调用 finalize_checksum
```

---

### `finalize_checksum(uint32_t partial_sum)`

```cpp
namespace cinux::lib {

uint16_t finalize_checksum(uint32_t partial_sum);

}
```

**功能：** 将一个 32 位部分累加和折叠并取反，得到最终的 16 位 Internet 校验和。此函数是校验和计算的收尾步骤，可独立调用以支持分段累加场景。

**算法：**

1. **进位折叠：** 反复将高 16 位的进位加回低 16 位，直到结果不超过 16 位（即 `partial_sum >> 16 == 0`）。
2. **取反：** 对折叠后的 16 位值按位取反（one's complement），得到最终校验和。

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `partial_sum` | `uint32_t` | 待折叠的 32 位部分累加和 |

**返回值：** `uint16_t`，折叠并取反后的 16 位校验和。

**示例：**

```cpp
#include <cinux/checksum.hpp>

using namespace cinux::lib;

// 手动分段累加后折叠
uint32_t partial = pseudo_header_partial(src_ip, dst_ip, 6, tcp_total_len);
// ... 加上 TCP 头部和数据的 16 位累加和
partial += some_other_partial_sum;

uint16_t final_cs = finalize_checksum(partial);
```

**验证折叠逻辑：**

```cpp
// 0x1FFFF -> 折叠: 0xFFFF + 0x1 = 0x10000 -> 再折叠: 0x0000 + 0x1 = 0x0001
// 取反: ~0x0001 = 0xFFFE
REQUIRE(finalize_checksum(0x1FFFF) == 0xFFFE);
```

---

## 注意事项

1. **字节序：** 本实现按大端序（网络字节序）组合 16 位字。传入的数据应当已是网络字节序，IP 地址参数也应以网络字节序表示为 `uint32_t`。
2. **校验和字段须置零：** 在计算新的校验和时，数据中的校验和字段必须先置为零，计算完成后再将结果填入。若校验和字段非零，计算结果将不正确。`verify_internet_checksum` 则要求校验和字段已正确填入。
3. **奇数长度处理：** 当数据长度为奇数时，末尾字节被视为高 8 位，低 8 位补零（符合 RFC 1071 规范）。
4. **零长度输入：** 对零长度数据调用 `internet_checksum` 将返回 `0xFFFF`（零和取反的结果）。
5. **伪头部与 `internet_checksum` 的配合：** TCP/UDP 校验和的计算需要先调用 `pseudo_header_partial` 获取伪头部部分和，再与负载数据的累加和相加，最后通过 `finalize_checksum` 得到最终校验和。不能直接将伪头部部分和传给 `internet_checksum`。
6. **线程安全：** 所有函数均为纯函数，无共享状态，天然线程安全且可重入。
7. **性能：** 当前实现为逐字节组合 16 位字的朴素算法。对于大块数据或高性能场景，可考虑使用 SIMD 优化的并行累加实现替代。

## 另见

- [RFC 1071 — Computing the Internet Checksum](https://tools.ietf.org/html/rfc1071)
- `cinux::span.hpp` — `ConstByteSpan` 类型定义
- IP/TCP/UDP/ICMP 协议头部校验和字段定义
```