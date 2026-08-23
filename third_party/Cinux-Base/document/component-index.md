# 组件速查表

> 所有 Cinux-Base 组件的快速参考。

## Phase 1 — 叶子组件

| 组件 | 头文件 | 分发 | 依赖 | 说明 |
|------|--------|------|------|------|
| ErrorOr\<T\> | `cinux/expected.hpp` | header-only | 无 | Value/Error 判别式联合体，替代裸错误码 |
| Numeric | `cinux/numeric.hpp` | header-only | 无 | 对齐、整除向上取整、2 的幂判断、内存字面量 |
| ScopeGuard | `cinux/scope_guard.hpp` | header-only | 无 | RAII 作用域清理，scope exit 时执行回调 |
| BitOps | `cinux/bit_ops.hpp` | header-only | 无 | 位计数/旋转/单 bit 操作/位域提取插入 |
| Endian | `cinux/endian.hpp` | header-only | 无 | 字节序转换（htobe/htole/bswap 等） |
| CRC32 | `cinux/crc32.hpp` + `src/crc32.cpp` | .hpp+.cpp | 无 | CRC32 校验和计算 |

## Phase 2 — 基础类型

| 组件 | 头文件 | 分发 | 依赖 | 说明 |
|------|--------|------|------|------|
| StringView | `cinux/string_view.hpp` | header-only | 无 | 零分配、不假设 \\0 终止的字符串视图 |
| Span\<T\> | `cinux/span.hpp` | header-only | 无 | 非拥有连续内存视图（std::span 的 C++17 替代） |
| Optional\<T\> | `cinux/optional.hpp` | header-only | 无 | 可选值容器，"找到/未找到" 语义 |
| StaticString\<N\> | `cinux/static_string.hpp` | header-only | StringView | 固定容量 \\0 终止字符串，用于路径/文件名 |
| BitMap\<N\> | `cinux/bitmap.hpp` | header-only | BitOps | 固定大小位图，用于页分配器和 buddy 系统 |

## Phase 3 — 容器与工具

| 组件 | 头文件 | 分发 | 依赖 | 说明 |
|------|--------|------|------|------|
| Buffer / StaticBuffer\<N\> | `cinux/buffer.hpp` | header-only | StringView, Span | 类型安全字节缓冲区（只读视图 + 固定容量容器） |
| RingBuffer\<T,N\> | `cinux/ring_buffer.hpp` | header-only | 无 | SPSC 环形缓冲区，用于日志和管道 |
| IntrusiveList\<T\> | `cinux/intrusive_list.hpp` | header-only | 无 | 侵入式双向链表，零分配，链指针嵌入元素 |
| StaticHashMap\<K,V,N\> | `cinux/static_hash_map.hpp` | header-only | Optional, StringView | 固定容量开放寻址哈希表，线性探测 |
| Function\<Sig,N\> | `cinux/function.hpp` | header-only | 无 | 类型擦除 callable，小缓冲优化，无堆分配 |
| Checksum | `cinux/checksum.hpp` + `src/checksum.cpp` | .hpp+.cpp | Span, Endian | Internet 校验和（RFC 1071），用于 IP/TCP/UDP/ICMP |

## Phase 4 — 高层组件

| 组件 | 头文件 | 分发 | 依赖 | 说明 |
|------|--------|------|------|------|
| Logger | `cinux/logger.hpp` + `src/logger.cpp` | .hpp+.cpp | Function, vformat | 轻量日志框架：级别过滤 + 格式化 + Sink 分发 |
| StaticRandomSource | `cinux/random.hpp` + `src/random.cpp` | .hpp+.cpp | Span | xoshiro256** 伪随机数生成器 |
| Algorithm | `cinux/algorithm.hpp` + `src/algorithm.cpp` | .hpp+.cpp | Span | 极简算法库（min/max/clamp/sort/search） |

## Phase 5 — 内部组件

| 组件 | 头文件 | 分发 | 依赖 | 说明 |
|------|--------|------|------|------|
| Format Engine | `cinux/detail/vformat.hpp` + `src/detail/vformat.cpp` | .hpp+.cpp | \<cstdarg\> | 硬件无关格式化引擎（Logger 内部使用，非公开 API） |

## 直接使用标准库（不纳入 CinuxBase）

| 标准库组件 | 说明 |
|-----------|------|
| `std::array<T,N>` | 固定大小容器，header-only，无堆分配 |
| `std::atomic<T>` | freestanding 可用（对 trivial types） |
| `<cstring>` | memset/memcpy 等声明 |
| `<utility>` | std::move / std::forward / std::swap |

## 统计

- **Header-only 组件**: 15 个
- **需要编译的组件**: 5 个（CRC32, Checksum, Logger, Random, Algorithm）+ 1 内部（vformat）
- **总头文件**: 20 个（含 1 个 detail/）
- **总源文件**: 4 个（含 1 个 detail/）
- **总代码量**: ~3,000 行（不含测试）

## 另见

- [API 文档](api/) — 各组件详细 API 参考和示例
- [架构设计](architecture.md) — 依赖图和设计约束
