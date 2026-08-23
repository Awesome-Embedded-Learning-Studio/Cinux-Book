The `src/random.cpp` file does not exist — the component is header-only. I now have all the information needed. Here is the complete Chinese API documentation:

---

# StaticRandomSource

> 基于 xoshiro256** 算法的伪随机数生成器

## 头文件

`#include <cinux/random.hpp>`

## 依赖

无外部依赖。仅依赖 C++ 标准库头文件 `<array>`、`<cstddef>`、`<cstdint>`。

## 概述

`StaticRandomSource` 是一个基于 xoshiro256** 算法的确定性伪随机数生成器（PRNG），位于 `cinux::lib` 命名空间下。该组件完全在头文件中实现，不进行任何堆内存分配，也不依赖外部熵源或操作系统系统调用，适合在嵌入式、内核态或资源受限环境中使用。

xoshiro256** 是 Blackman 与 Vigna 于 2018 年提出的高性能伪随机数生成算法，属于线性位移寄存器变换（linear transformation of a linear recurrence）家族。它具有 2^256 - 1 的超长周期和极佳的统计特性，在诸多非密码学场景下（模拟、测试、游戏、采样）均可提供高质量的随机序列。`StaticRandomSource` 使用 SplitMix64 算法将单个 64 位种子扩展为 256 位内部状态，确保即使种子值相近，生成的序列也截然不同。

**注意：** 该生成器不具密码学安全性。切勿将其用于密钥生成、令牌签发、密码学协议等安全敏感场景。在这些场景中应使用硬件熵源或内核提供的 `/dev/urandom` 等密码学安全随机源。

## API 参考

### 类 `cinux::lib::StaticRandomSource`

```cpp
class StaticRandomSource {
public:
    constexpr void seed(uint64_t seed_value);
    constexpr uint64_t next_u64();
    constexpr uint32_t next_u32();
    constexpr uint32_t next_bounded(uint32_t max);
    void fill(void* buf, size_t len);
    constexpr bool seeded() const;

private:
    static constexpr uint64_t rotl(uint64_t x, int k);
    std::array<uint64_t, 4> state_{};
    bool seeded_ = false;
};
```

---

### `seed`

```cpp
constexpr void seed(uint64_t seed_value);
```

使用 SplitMix64 扩展算法将单个 64 位种子值扩展为 256 位内部状态（4 个 `uint64_t`）。调用后 `seeded()` 返回 `true`。

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `seed_value` | `uint64_t` | 用于初始化内部状态的 64 位种子值 |

**注意：** 可重复调用以重新播种。播种后，生成器将产生全新的随机序列。

**示例：**

```cpp
#include <cinux/random.hpp>

using namespace cinux::lib;

StaticRandomSource rng;
rng.seed(0xDeadBeefCafeBabeULL);

// 现在可以产生随机数
uint64_t value = rng.next_u64();
```

---

### `next_u64`

```cpp
constexpr uint64_t next_u64();
```

生成下一个 64 位伪随机数，同时推进内部状态。该函数是 xoshiro256** 算法的核心输出。

**返回值：** 一个 `uint64_t` 伪随机数，值域为 `[0, 2^64)`。

**示例：**

```cpp
StaticRandomSource rng;
rng.seed(12345);

uint64_t a = rng.next_u64();
uint64_t b = rng.next_u64();
// a 和 b 是确定性的——相同种子总会得到相同序列
```

---

### `next_u32`

```cpp
constexpr uint32_t next_u32();
```

生成下一个 32 位伪随机数。实现上取 `next_u64()` 返回值的高 32 位，确保分布均匀。

**返回值：** 一个 `uint32_t` 伪随机数，值域为 `[0, 2^32)`。

**示例：**

```cpp
StaticRandomSource rng;
rng.seed(42);

uint32_t v = rng.next_u32();
```

---

### `next_bounded`

```cpp
constexpr uint32_t next_bounded(uint32_t max);
```

生成一个在 `[0, max)` 范围内的均匀分布伪随机整数。使用 `(value * bound) >> 32` 的乘法缩减法，避免模偏差（modulo bias）。当 `max` 为 0 时直接返回 0。

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `max` | `uint32_t` | 上界（不包含）。若为 0 则返回 0。 |

**返回值：** 一个 `uint32_t` 值，满足 `0 <= result < max`。

**示例：**

```cpp
StaticRandomSource rng;
rng.seed(42);

// 模拟掷骰子：生成 [0, 6) 的随机数
for (int i = 0; i < 10; ++i) {
    uint32_t dice = rng.next_bounded(6) + 1;  // 结果为 1-6
}

// 边界情况
rng.next_bounded(1);  // 总是返回 0
rng.next_bounded(0);  // 总是返回 0
```

---

### `fill`

```cpp
void fill(void* buf, size_t len);
```

用伪随机字节填充给定的缓冲区。内部按 8 字节（一个 `uint64_t`）块为单位生成数据以提高效率，末尾不足 8 字节的部分单独处理。

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `buf` | `void*` | 指向待填充缓冲区的指针 |
| `len` | `size_t` | 需要填充的字节数 |

**注意：** 此函数不是 `constexpr`（与其它成员函数不同），因为它通过指针进行字节级写入。

**示例：**

```cpp
#include <cinux/random.hpp>
#include <cstring>
#include <iostream>

using namespace cinux::lib;

int main() {
    StaticRandomSource rng;
    rng.seed(42);

    uint8_t buffer[64];
    rng.fill(buffer, sizeof(buffer));

    // 输出前 16 字节的十六进制值
    for (int i = 0; i < 16; ++i) {
        std::cout << std::hex << static_cast<int>(buffer[i]) << ' ';
    }
    std::cout << '\n';
}
```

---

### `seeded`

```cpp
constexpr bool seeded() const;
```

查询生成器是否已被播种。

**返回值：** 若已调用过 `seed()` 则返回 `true`，否则返回 `false`。新构造的 `StaticRandomSource` 对象处于未播种状态，内部状态全为零。

**示例：**

```cpp
StaticRandomSource rng;
assert(!rng.seeded());   // 构造后未播种

rng.seed(12345);
assert(rng.seeded());    // 播种后为 true
```

---

## 注意事项

1. **不适用于密码学场景：** `StaticRandomSource` 是一个确定性伪随机数生成器，不具密码学安全性。不要将其用于密钥生成、会话令牌、随机密码、加密盐值等安全敏感用途。密码学场景应使用内核熵源（如 `/dev/urandom`）或硬件随机数生成器。

2. **constexpr 支持：** 除 `fill()` 外，所有公开成员函数均为 `constexpr`，可在编译期常量求值中使用。

3. **线程安全：** 该类不提供任何内部同步机制。若多个线程需要随机数，每个线程应持有独立的 `StaticRandomSource` 实例，或由使用者自行加锁。

4. **播种是必需的：** 未播种的生成器内部状态全为零，直接调用生成函数不会产生断言或异常，但输出将不具有随机性。建议在构造后立即调用 `seed()`。

5. **确定性可复现：** 给定相同种子，生成器将产生完全相同的随机序列。这对于测试、模拟复现和调试非常有用。

6. **无堆分配：** 内部状态仅使用固定大小的 `std::array<uint64_t, 4>`（32 字节），不进行任何动态内存分配，适合内核态和嵌入式环境。

7. **`fill` 非 constexpr：** `fill()` 函数由于涉及 `void*` 指针操作，不可在编译期求值上下文中使用。

## 另见

- [xoshiro256** 原始论文与参考实现](https://prng.di.unimi.it/) — Sebastiano Vigna, David Blackman
- SplitMix64 算法 — 用于种子扩展的哈希函数
- `<random>` — C++ 标准库随机数设施（需要异常和 RTTI 支持，不适用于 `fno-exceptions` 环境）
- CinuxOS 项目中其它 `cinux::lib` 组件