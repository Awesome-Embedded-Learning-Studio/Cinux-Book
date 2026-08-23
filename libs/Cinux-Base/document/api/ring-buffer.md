# RingBuffer\<T,N\>

> SPSC 环形缓冲区，用于日志和管道

## 头文件

`#include <cinux/ring_buffer.hpp>`

## 依赖

无

## 概述

`RingBuffer<T, N>` 是一个固定容量的单生产者-单消费者（SPSC）环形缓冲区实现，全部数据存储在栈上预分配的数组中，不涉及任何堆内存分配。它通过显式的元素计数 `count_` 来精确区分"缓冲区已满"和"缓冲区为空"两种状态，避免了传统环形缓冲区中需要牺牲一个槽位的做法。

该组件的设计动机来源于嵌入式和系统编程场景中对低延迟、零分配数据管道的需求。典型应用包括：日志子系统中的消息暂存队列、中断处理与主循环之间的数据传输管道、串口/网络数据的字节流缓冲，以及线程间（配合适当同步原语）的单向数据传递。由于所有操作均为 `constexpr` 友好，该组件也可在编译期常量求值上下文中使用。

`RingBuffer` 同时提供单元素和批量两种操作接口。批量操作 `push_batch` / `pop_batch` 能够正确处理索引回绕，适用于需要高效搬运连续数据块的场景。头文件还定义了便捷别名 `ByteRingBuffer<N>`，即 `RingBuffer<uint8_t, N>`，专门面向字节流缓冲。

## API 参考

### 类模板 `RingBuffer<T, N>`

```cpp
namespace cinux::lib {

template <typename T, size_t N>
class RingBuffer;
```

**模板参数：**

| 参数 | 说明 |
|------|------|
| `T` | 元素类型，需满足可复制赋值（CopyAssignable）要求 |
| `N` | 缓冲区容量（最多可存储的元素数量），必须为编译期常量且大于 0 |

---

#### 默认构造函数

```cpp
constexpr RingBuffer() = default;
```

构造一个空的环形缓冲区，所有元素值初始化，`head_`、`tail_`、`count_` 均为 0。

**示例：**

```cpp
#include <cinux/ring_buffer.hpp>

using namespace cinux::lib;

RingBuffer<int, 64> rb;       // 构造一个容量为 64 的 int 环形缓冲区
```

---

#### `empty`

```cpp
constexpr bool empty() const;
```

判断缓冲区是否为空。

**返回值：** 当缓冲区中没有元素时返回 `true`，否则返回 `false`。

**示例：**

```cpp
RingBuffer<int, 4> rb;
assert(rb.empty());   // 初始状态为空

rb.push(1);
assert(!rb.empty());  // 插入元素后不为空
```

---

#### `full`

```cpp
constexpr bool full() const;
```

判断缓冲区是否已满。

**返回值：** 当缓冲区中已存储 `N` 个元素时返回 `true`，否则返回 `false`。

**示例：**

```cpp
RingBuffer<int, 3> rb;
rb.push(1);
rb.push(2);
rb.push(3);
assert(rb.full());    // 已存满 3 个元素

assert(!rb.push(4));  // 满时 push 返回 false
```

---

#### `size`

```cpp
constexpr size_t size() const;
```

获取当前缓冲区中已存储的元素数量。

**返回值：** 当前元素数量，类型为 `size_t`，范围 `[0, N]`。

**示例：**

```cpp
RingBuffer<int, 8> rb;
rb.push(10);
rb.push(20);
assert(rb.size() == 2);
```

---

#### `capacity`

```cpp
constexpr size_t capacity() const;
```

获取缓冲区的总容量。

**返回值：** 模板参数 `N`，即缓冲区最多可存储的元素数量。

**示例：**

```cpp
RingBuffer<int, 16> rb;
assert(rb.capacity() == 16);
```

---

#### `push`

```cpp
constexpr bool push(const T& item);
```

向缓冲区尾部推入一个元素。当缓冲区已满时，操作失败且不修改缓冲区内容。

**参数：**

| 参数 | 说明 |
|------|------|
| `item` | 要插入的元素，以 `const` 引用方式传入 |

**返回值：** 成功插入返回 `true`；缓冲区已满返回 `false`。

**示例：**

```cpp
RingBuffer<int, 8> rb;

bool ok = rb.push(42);
assert(ok);           // 成功插入

// 填满缓冲区
for (int i = 0; i < 8; ++i) {
    rb.push(i);
}
ok = rb.push(99);
assert(!ok);          // 缓冲区已满，插入失败
```

---

#### `pop`

```cpp
constexpr bool pop(T& out);
```

从缓冲区头部弹出一个元素。当缓冲区为空时，操作失败且不修改 `out` 的值。

**参数：**

| 参数 | 说明 |
|------|------|
| `out` | 输出参数，用于接收弹出的元素值 |

**返回值：** 成功弹出返回 `true`；缓冲区为空返回 `false`。

**示例：**

```cpp
RingBuffer<int, 8> rb;
rb.push(1);
rb.push(2);
rb.push(3);

int val = 0;
assert(rb.pop(val) && val == 1);   // FIFO：先入先出
assert(rb.pop(val) && val == 2);
assert(rb.pop(val) && val == 3);
assert(!rb.pop(val));               // 缓冲区已空
```

---

#### `clear`

```cpp
constexpr void clear();
```

清空缓冲区，重置头指针、尾指针和元素计数为 0。不销毁缓冲区中的元素对象。

**示例：**

```cpp
RingBuffer<int, 4> rb;
rb.push(1);
rb.push(2);
rb.clear();

assert(rb.empty());
assert(rb.size() == 0);
```

---

#### `peek_front`

```cpp
constexpr const T& peek_front() const;
```

查看缓冲区头部元素（下一个将被 `pop` 弹出的元素），但不将其移除。

**前置条件：** 缓冲区不为空（`!empty()`）。在空缓冲区上调用此函数属于未定义行为。

**返回值：** 头部元素的 `const` 引用。

**示例：**

```cpp
RingBuffer<int, 8> rb;
rb.push(10);
rb.push(20);

assert(rb.peek_front() == 10);  // 下一个将被 pop 出来的值
assert(rb.size() == 2);         // peek 不改变缓冲区状态
```

---

#### `peek_back`

```cpp
constexpr const T& peek_back() const;
```

查看缓冲区尾部元素（最近一次 `push` 插入的元素），但不将其移除。

**前置条件：** 缓冲区不为空（`!empty()`）。在空缓冲区上调用此函数属于未定义行为。

**返回值：** 尾部元素的 `const` 引用。

**示例：**

```cpp
RingBuffer<int, 8> rb;
rb.push(10);
rb.push(20);

assert(rb.peek_back() == 20);   // 最近插入的元素
```

---

#### `push_batch`

```cpp
size_t push_batch(const T* items, size_t count);
```

批量推入多个元素。当缓冲区剩余空间不足时，会推入尽可能多的元素后停止。

**参数：**

| 参数 | 说明 |
|------|------|
| `items` | 指向待推入元素数组的指针 |
| `count` | 期望推入的元素数量 |

**返回值：** 实际成功推入的元素数量，范围 `[0, min(count, N - size())]`。

**示例：**

```cpp
RingBuffer<int, 8> rb;

int src[] = {1, 2, 3, 4, 5};
size_t n = rb.push_batch(src, 5);
assert(n == 5);          // 全部推入成功
assert(rb.size() == 5);

int more[] = {10, 20, 30, 40};
n = rb.push_batch(more, 4);
assert(n == 3);          // 仅剩 3 个空位，推入 3 个
assert(rb.full());
```

---

#### `pop_batch`

```cpp
size_t pop_batch(T* items, size_t count);
```

批量弹出多个元素。当缓冲区中元素不足时，会弹出尽可能多的元素后停止。

**参数：**

| 参数 | 说明 |
|------|------|
| `items` | 指向输出数组的指针，用于存放弹出的元素 |
| `count` | 期望弹出的元素数量 |

**返回值：** 实际成功弹出的元素数量，范围 `[0, min(count, size())]`。

**示例：**

```cpp
RingBuffer<int, 8> rb;
int src[] = {1, 2, 3, 4, 5};
rb.push_batch(src, 5);

int dst[5] = {};
size_t n = rb.pop_batch(dst, 5);
assert(n == 5);
assert(dst[0] == 1);
assert(dst[4] == 5);
assert(rb.empty());
```

---

### 别名模板 `ByteRingBuffer<N>`

```cpp
template <size_t N>
using ByteRingBuffer = RingBuffer<uint8_t, N>;
```

面向字节流场景的便捷别名，等价于 `RingBuffer<uint8_t, N>`。

**示例：**

```cpp
#include <cinux/ring_buffer.hpp>

using namespace cinux::lib;

ByteRingBuffer<256> byte_buf;

// 模拟串口接收数据
uint8_t uart_data[] = {0xAA, 0x55, 0x01, 0x02};
byte_buf.push_batch(uart_data, 4);

// 主循环中取出处理
uint8_t out[4];
size_t n = byte_buf.pop_batch(out, 4);
```

---

## 注意事项

1. **非线程安全：** `RingBuffer` 本身不包含任何同步机制。在多线程环境中用作 SPSC 队列时，调用方需自行确保生产者端和消费者端的操作正确同步（例如配合 `std::atomic` 标志或内存屏障）。

2. **容量固定：** 容量 `N` 在编译期确定，运行时无法动态扩容。请根据实际场景选择合适的容量值。`N` 应大于 0，`N = 1` 是合法的最小容量。

3. **元素类型要求：** 元素类型 `T` 必须支持默认构造和复制赋值。对于大型或不可复制的对象，建议使用 `RingBuffer<std::unique_ptr<T>, N>` 或存储指针/索引。

4. **peek 前置条件：** `peek_front()` 和 `peek_back()` 不进行空检查。在空缓冲区上调用属于未定义行为，调用方应先通过 `empty()` 确认缓冲区非空。

5. **clear 不析构元素：** `clear()` 仅重置索引和计数，不会调用缓冲区中元素的析构函数。若 `T` 持有需要显式释放的资源，请注意此行为。

6. **constexpr 限制：** 单元素操作（`push`、`pop`、`peek_front`、`peek_back`、`clear`）为 `constexpr` 函数，可在编译期求值。批量操作（`push_batch`、`pop_batch`）由于实现中使用了 `while` 循环，在 C++23 之前的编译期上下文中可能不可用。

7. **溢出安全：** 当 `push` 失败（缓冲区满）时，数据不会被修改或丢弃；当 `pop` 失败（缓冲区空）时，输出参数不被修改。`push_batch` / `pop_batch` 返回实际操作的元素数量，调用方可据此判断是否所有数据都已处理完毕。

## 另见

- `cinux::lib` 命名空间下的其他数据结构组件
- C++ `std::queue` / `std::deque` — 标准库动态队列（支持堆分配，容量可变）
- SPSC 队列设计模式 — 单生产者单消费者无锁队列的常见实现方法