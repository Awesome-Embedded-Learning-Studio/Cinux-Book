以下是算法组件的完整中文 API 文档文件：

---

# Algorithm

> 极简 freestanding 算法库

## 头文件

`#include <cinux/algorithm.hpp>`

## 依赖

Span

## 概述

`cinux::lib` 中的 Algorithm 组件是一个最小化的 freestanding 算法库，旨在替代标准库 `<algorithm>` 头文件。它提供了一组常用的基础算法函数，包括数值比较（`min`、`max`、`clamp`）、元素交换（`swap`）、线性查找（`find`、`find_if`）、批量操作（`fill`、`copy`）、排序（`insertion_sort`）以及二分查找（`binary_search`）。所有函数均基于原始指针区间 `[begin, end)` 进行操作，不依赖任何 STL 容器或迭代器抽象，非常适合嵌入式、内核或其它无法使用完整标准库的 freestanding 环境。

该组件的设计遵循以下原则：**零堆分配**——所有算法仅通过指针运算和原地操作完成工作，不引入任何动态内存分配；**constexpr 友好**——`min`、`max`、`clamp`、`swap`、`find`、`find_if`、`fill`、`copy` 以及 `binary_search` 均标记为 `constexpr`，可在编译期求值；**头文件内联**——全部实现均为函数模板，定义在头文件中，无需额外的编译单元（`algorithm.cpp` 不存在，所有逻辑均在 `algorithm.hpp` 中），使用时只需包含头文件即可。

典型使用场景包括：内核启动阶段的简单数组排序与查找、驱动程序中的缓冲区初始化与拷贝、以及嵌入式固件中对小型数据集合的基本操作。对于大型数据集，建议使用更高效的排序算法（如快速排序或归并排序），本库提供的插入排序主要面向小规模数据或近乎有序的数据。

## API 参考

### `min`

```cpp
template <typename T>
constexpr const T& min(const T& a, const T& b);
```

返回两个值中的较小者。

**参数：**
- `a` —— 第一个值
- `b` —— 第二个值

**返回值：** `const T&`，指向 `a` 和 `b` 中较小者的常量引用。当两者相等时返回 `b`。

**示例：**

```cpp
#include <cinux/algorithm.hpp>
using namespace cinux::lib;

int x = min(3, 5);   // x == 3
int y = min(10, 2);  // y == 2
```

---

### `max`

```cpp
template <typename T>
constexpr const T& max(const T& a, const T& b);
```

返回两个值中的较大者。

**参数：**
- `a` —— 第一个值
- `b` —— 第二个值

**返回值：** `const T&`，指向 `a` 和 `b` 中较大者的常量引用。当两者相等时返回 `b`。

**示例：**

```cpp
#include <cinux/algorithm.hpp>
using namespace cinux::lib;

int x = max(3, 5);   // x == 5
int y = max(10, 2);  // y == 10
```

---

### `clamp`

```cpp
template <typename T>
constexpr T clamp(const T& v, const T& lo, const T& hi);
```

将值 `v` 限制在 `[lo, hi]` 区间内。若 `v < lo` 则返回 `lo`，若 `v > hi` 则返回 `hi`，否则返回 `v` 本身。

**参数：**
- `v` —— 待限制的值
- `lo` —— 下界
- `hi` —— 上界

**返回值：** `T`，限制后的值。

**示例：**

```cpp
#include <cinux/algorithm.hpp>
using namespace cinux::lib;

int a = clamp(7, 1, 10);   // a == 7（在区间内，不变）
int b = clamp(-1, 0, 10);  // b == 0（低于下界，取下界）
int c = clamp(99, 0, 10);  // c == 10（超过上界，取上界）
```

---

### `swap`

```cpp
template <typename T>
constexpr void swap(T& a, T& b);
```

交换两个值。内部使用 `std::move` 实现高效移动语义。

**参数：**
- `a` —— 第一个值的引用
- `b` —— 第二个值的引用

**返回值：** 无

**示例：**

```cpp
#include <cinux/algorithm.hpp>
using namespace cinux::lib;

int a = 10, b = 20;
swap(a, b);
// a == 20, b == 10
```

---

### `find`

```cpp
template <typename T>
constexpr T* find(T* begin, T* end, const T& value);
```

在 `[begin, end)` 区间内线性查找等于 `value` 的第一个元素。

**参数：**
- `begin` —— 区间起始指针（包含）
- `end` —— 区间末尾指针（不包含）
- `value` —— 待查找的值

**返回值：** `T*`，指向第一个匹配元素的指针；若未找到则返回 `end`。

**示例：**

```cpp
#include <cinux/algorithm.hpp>
using namespace cinux::lib;

int arr[] = {10, 20, 30, 40};
int* end  = arr + 4;

int* p = find(arr, end, 30);  // p == &arr[2]
int* q = find(arr, end, 99);  // q == end（未找到）
```

---

### `find_if`

```cpp
template <typename T, typename Pred>
constexpr T* find_if(T* begin, T* end, Pred pred);
```

在 `[begin, end)` 区间内线性查找第一个满足谓词 `pred` 的元素。

**参数：**
- `begin` —— 区间起始指针（包含）
- `end` —— 区间末尾指针（不包含）
- `pred` —— 一元谓词，签名为 `bool(const T&)`，返回 `true` 表示匹配

**返回值：** `T*`，指向第一个满足谓词的元素的指针；若未找到则返回 `end`。

**示例：**

```cpp
#include <cinux/algorithm.hpp>
using namespace cinux::lib;

int arr[] = {1, 4, 7, 2};
int* end  = arr + 4;

int* p = find_if(arr, end, [](int v) { return v > 5; });
// p == &arr[2], *p == 7
```

---

### `fill`

```cpp
template <typename T>
constexpr void fill(T* begin, T* end, const T& value);
```

将 `[begin, end)` 区间内的所有元素赋值为 `value`。

**参数：**
- `begin` —— 区间起始指针（包含）
- `end` —— 区间末尾指针（不包含）
- `value` —— 要填充的值

**返回值：** 无

**示例：**

```cpp
#include <cinux/algorithm.hpp>
using namespace cinux::lib;

int buf[4];
fill(buf, buf + 4, 42);
// buf == {42, 42, 42, 42}
```

---

### `copy`

```cpp
template <typename T>
constexpr T* copy(const T* src_begin, const T* src_end, T* dst);
```

将 `[src_begin, src_end)` 区间内的元素逐一复制到以 `dst` 为起点的目标区域。

**参数：**
- `src_begin` —— 源区间起始指针（包含）
- `src_end` —— 源区间末尾指针（不包含）
- `dst` —— 目标区域起始指针

**返回值：** `T*`，指向目标区域中最后一个被复制元素之后的位置（即 `dst + (src_end - src_begin)`）。

**注意：** 源区间与目标区域不得重叠。

**示例：**

```cpp
#include <cinux/algorithm.hpp>
using namespace cinux::lib;

int src[]  = {1, 2, 3};
int dst[3] = {};

T* result = copy(src, src + 3, dst);
// dst == {1, 2, 3}
// result == dst + 3
```

---

### `insertion_sort`（默认比较）

```cpp
template <typename T>
void insertion_sort(T* begin, T* end);
```

对 `[begin, end)` 区间执行插入排序，使用 `operator<` 进行升序比较。时间复杂度为 O(n^2)，空间复杂度为 O(1)。该算法是稳定的（stable），且为非递归实现。

**参数：**
- `begin` —— 区间起始指针（包含）
- `end` —— 区间末尾指针（不包含）

**返回值：** 无

**示例：**

```cpp
#include <cinux/algorithm.hpp>
using namespace cinux::lib;

int arr[] = {5, 2, 8, 1, 9, 3};
insertion_sort(arr, arr + 6);
// arr == {1, 2, 3, 5, 8, 9}
```

---

### `insertion_sort`（自定义比较器）

```cpp
template <typename T, typename Comp>
void insertion_sort(T* begin, T* end, Comp comp);
```

对 `[begin, end)` 区间执行插入排序，使用自定义比较器 `comp`。时间复杂度为 O(n^2)，空间复杂度为 O(1)。该算法是稳定的（stable），且为非递归实现。

**参数：**
- `begin` —— 区间起始指针（包含）
- `end` —— 区间末尾指针（不包含）
- `comp` —— 比较器，签名为 `bool(const T& a, const T& b)`，当 `a` 应排在 `b` 之前时返回 `true`

**返回值：** 无

**示例：**

```cpp
#include <cinux/algorithm.hpp>
using namespace cinux::lib;

int arr[] = {3, 1, 4, 1, 5};
insertion_sort(arr, arr + 5, [](int a, int b) { return a > b; });
// arr == {5, 4, 3, 1, 1}（降序排列）
```

---

### `binary_search`

```cpp
template <typename T>
constexpr T* binary_search(T* begin, T* end, const T& value);
```

在已排序的 `[begin, end)` 区间内执行二分查找，寻找等于 `value` 的元素。时间复杂度为 O(log n)。要求数据已按升序排列。

**参数：**
- `begin` —— 区间起始指针（包含）
- `end` —— 区间末尾指针（不包含）
- `value` —— 待查找的值

**返回值：** `T*`，指向匹配元素的指针；若未找到则返回 `nullptr`（注意：不是 `end`）。

**示例：**

```cpp
#include <cinux/algorithm.hpp>
using namespace cinux::lib;

int arr[] = {1, 3, 5, 7, 9, 11};

int* p = binary_search(arr, arr + 6, 5);   // p == &arr[2]
int* q = binary_search(arr, arr + 6, 6);   // q == nullptr
int* r = binary_search(arr, arr + 6, 99);  // r == nullptr
```

## 注意事项

1. **指针区间约定：** 所有函数均采用半开区间 `[begin, end)` 的 C++ 惯例。`begin` 指向第一个元素，`end` 指向最后一个元素之后的位置。调用者需确保指针合法且区间有效。
2. **`binary_search` 返回值：** 与标准库 `std::binary_search` 不同，本函数返回指向元素的指针（找到时）或 `nullptr`（未找到时），而非 `bool`。这使得调用者可以直接访问找到的元素。
3. **`copy` 不处理重叠区间：** `copy` 函数要求源区间与目标区域不重叠。若存在重叠，请另行处理或确保安全性。
4. **排序算法选择：** `insertion_sort` 的时间复杂度为 O(n^2)，适合小规模数据（通常 n < 50）或近乎有序的数据。对于大规模或随机数据，应考虑更高效的排序算法。
5. **类型要求：** 使用默认比较的函数（`min`、`max`、`find`、`insertion_sort`、`binary_search`）要求类型 `T` 支持 `operator<` 和 `operator==`。`swap` 要求类型支持移动语义。
6. **constexpr 限制：** 标记为 `constexpr` 的函数在 C++20 及以上标准中可在编译期求值。`insertion_sort` 不是 `constexpr` 函数，不可用于编译期计算。

## 另见

- [Span](./span.md) —— 本组件依赖的 Span 视图类型
- `<algorithm>` —— C++ 标准库算法头文件（本组件的替代目标）