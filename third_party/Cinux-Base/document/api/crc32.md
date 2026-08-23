这是完整的中文 API 文档文件内容：

```markdown
# CRC32

> CRC32 校验和计算

## 头文件

`#include <cinux/crc32.hpp>`

## 依赖

无（叶子组件）

## 概述

CRC32（循环冗余校验，32 位）是一种广泛使用的校验和算法，能够快速检测数据在传输或存储过程中是否发生意外改变。该组件提供了标准多项式（0xEDB88320，即 ISO 3309 / ITU-T V.42 标准多项式的按位反转形式）的 CRC32 计算实现，使用预计算查找表加速运算，适用于数据完整性校验、帧校验序列生成等场景。

本组件从 `kernel/mini/lib/crc32.h` 迁移而来，命名空间调整为 `cinux::lib`。实现采用经典查表法：在编译期生成 256 项查找表，运行时对每个输入字节进行一次表查找和两次异或运算即可完成累加，兼顾了计算速度与代码简洁性。查找表被放置在 `.cpp` 文件内的匿名命名空间中，具有内部链接性，避免了跨翻译单元的符号重复。

## API 参考

### `cinux::lib::crc32`

```cpp
namespace cinux::lib {

uint32_t crc32(const void* data, size_t len);

}  // namespace cinux::lib
```

计算给定数据块的 CRC32 校验和。

#### 参数

| 参数 | 类型 | 说明 |
|------|------|------|
| `data` | `const void*` | 指向输入数据缓冲区的指针。当 `len` 为 0 时，`data` 可以为 `nullptr`。 |
| `len` | `size_t` | 输入数据的字节数。 |

#### 返回值

返回 `uint32_t` 类型的 CRC32 校验和。对于空输入（`len == 0`），返回 `0x00000000`。

#### 算法细节

- 多项式：`0xEDB88320`（标准 CRC-32 反射多项式）
- 初始值：`0xFFFFFFFF`
- 最终异或：`0xFFFFFFFF`
- 与 zlib / Ethernet / PNG 等使用的 CRC32 完全一致

#### 用法示例

```cpp
#include <cinux/crc32.hpp>
#include <cstdint>
#include <cstdio>

int main() {
    // 标准测试向量："123456789" 的 CRC32 应为 0xCBF43926
    const char* sample = "123456789";
    uint32_t checksum = cinux::lib::crc32(sample, 9);

    if (checksum == 0xCBF43926u) {
        std::printf("CRC32 校验通过: 0x%08X\n", checksum);
    }

    // 校验任意二进制数据
    uint8_t buffer[1024];
    // ... 填充 buffer ...
    uint32_t crc = cinux::lib::crc32(buffer, sizeof(buffer));

    return 0;
}
```

#### 分块计算示例

当数据分多次到达时（例如从流或文件中逐块读取），可以通过手动维护 CRC 中间状态来实现分块计算：

```cpp
#include <cinux/crc32.hpp>
#include <cstdint>

// 注意：当前 API 不直接支持分块计算。
// 若需分块计算，可在应用层自行实现累加逻辑，
// 或将所有数据收集完毕后一次性调用 crc32()。
```

> 提示：当前 API 仅提供单次整块计算接口。如需增量式（streaming）CRC32 计算，建议在应用层封装或在后续版本中扩展。

## 测试覆盖

组件提供以下测试用例（基于 Catch2），位于 `test/unit/test_crc32.cpp`：

| 测试用例 | 说明 |
|----------|------|
| `crc32: known answer '123456789'` | 验证标准测试向量，输入 `"123456789"` 应得到 `0xCBF43926` |
| `crc32: empty input returns 0` | 空输入（`len == 0`）应返回 `0x00000000` |
| `crc32: single byte` | 单字节输入应返回非零校验和 |
| `crc32: deterministic` | 相同输入多次调用应返回相同结果 |
| `crc32: different inputs give different checksums` | 不同输入应产生不同校验和 |

## 注意事项

- **空输入行为**：当 `len` 为 `0` 时，`data` 允许为 `nullptr`，函数将返回 `0x00000000`。若 `len > 0` 且 `data` 为 `nullptr`，属于未定义行为。
- **线程安全性**：该函数是纯函数（pure function），无副作用，天然线程安全，可从多个线程并发调用。
- **查找表位置**：256 项查找表定义在 `.cpp` 文件的匿名命名空间中，具有内部链接性，不会污染全局命名空间，也不会因多个翻译单元包含头文件而产生重复符号。
- **非加密哈希**：CRC32 是校验和算法，不是加密哈希函数。它仅适用于检测随机错误（如传输噪声），不能抵御恶意篡改。请不要将其用于安全敏感场景。
- **性能**：查表法的时间复杂度为 O(n)，其中 n 为输入字节数。每次迭代仅需一次表查找和少量位运算，性能优异。
- **跨平台一致性**：使用固定多项式和标准算法，保证在所有平台上对相同输入产生相同输出，不依赖平台字节序。

## 另见

- [md5](md5.md) — MD5 消息摘要算法
- [sha256](sha256.md) — SHA-256 安全哈希算法
```