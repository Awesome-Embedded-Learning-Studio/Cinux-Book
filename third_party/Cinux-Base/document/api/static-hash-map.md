# StaticHashMap<K,V,N>

> 固定容量开放寻址哈希表，线性探测

## 头文件

`#include <cinux/static_hash_map.hpp>`

## 依赖

[Optional](optional.md)、[StringView](string-view.md)

## 概述

`StaticHashMap` 是一个零堆分配、固定容量的开放寻址哈希表实现，采用线性探测（linear probing）策略解决冲突。其所有存储均内嵌于对象自身之中（一个 `Slot slots_[N]` 数组），因而不依赖任何动态内存分配器。这一特性使其非常适合在内核启动早期、中断上下文、嵌入式裸机环境等无法使用 `malloc` / `new` 的场景中充当小型查找表。

在操作系统内核中，`StaticHashMap` 可用于构建 **dentry 缓存**（文件路径到 inode 编号的映射）、**ARP 表**（IP 地址到 MAC 地址的映射）、**页缓存索引**（文件偏移到物理页帧的映射）以及 **PID 到 `task_struct`** 的快速索引等关键数据结构。其模板参数 `N` 在编译期确定槽位数，用户可根据具体子系统的内存预算精确定量控制表的大小。

该实现支持 **墓碑（tombstone）删除** 机制：删除元素时仅标记对应槽位为"已死亡"，不移动其他元素，从而保证线性探测链的正确性。在后续插入时，墓碑槽位会被优先复用，避免空间浪费。全部公开方法均标注为 `constexpr`，允许在编译期求值。

## API 参考

### 类模板 `StaticHashMap`

```cpp
template <typename K, typename V, size_t N, typename HashT = Hash<K>>
class StaticHashMap;
```

**模板参数：**

| 参数    | 说明 |
|---------|------|
| `K`     | 键的类型。必须可默认构造、可拷贝，且支持 `operator==` 比较。 |
| `V`     | 值的类型。必须可默认构造、可拷贝。 |
| `N`     | 哈希表的槽位数量（容量），为编译期常量。 |
| `HashT` | 哈希函子类型，默认为 `Hash<K>`。自定义函子须提供 `constexpr size_t operator()(const K&) const`。 |

---

### 构造函数

```cpp
constexpr StaticHashMap() = default;
```

默认构造函数。创建一个所有槽位均为空、大小为零的空哈希表。

**示例：**

```cpp
cinux::lib::StaticHashMap<int, int, 64> map;
```

---

### `capacity`

```cpp
static constexpr size_t capacity();
```

返回哈希表的槽位总数 `N`。此方法为 `static` 且 `constexpr`。

**返回值：** 编译期常量 `N`。

---

### `size`

```cpp
constexpr size_t size() const;
```

返回当前哈希表中存活的键值对数量。不包含已删除（墓碑）的槽位。

**返回值：** 当前有效元素个数。

---

### `empty`

```cpp
constexpr bool empty() const;
```

判断哈希表是否为空（即 `size() == 0`）。

---

### `full`

```cpp
constexpr bool full() const;
```

判断哈希表是否已满（即 `size() >= N`）。

---

### `insert`

```cpp
constexpr bool insert(const K& key, const V& value);
```

向哈希表中插入一个键值对。若键已存在或表已满，则插入失败。

**参数：**

| 参数    | 说明 |
|---------|------|
| `key`   | 要插入的键。 |
| `value` | 要插入的值。 |

**返回值：** 插入成功返回 `true`；键已存在或表已满返回 `false`。

**示例：**

```cpp
StaticHashMap<int, int, 16> map;

assert(map.insert(1, 10));    // true
assert(!map.insert(1, 99));   // false，键已存在

StaticHashMap<int, int, 2> small;
assert(small.insert(1, 10));
assert(small.insert(2, 20));
assert(small.full());
assert(!small.insert(3, 30)); // false，表满
```

---

### `insert_or_assign`

```cpp
constexpr bool insert_or_assign(const K& key, const V& value);
```

插入键值对，若键已存在则覆盖其值。仅当表已满且键不存在时才失败。

**示例：**

```cpp
StaticHashMap<int, int, 16> map;
map.insert(1, 10);

assert(map.insert_or_assign(1, 99));  // true，覆盖
assert(map.find(1).value() == 99);
```

---

### `find`

```cpp
constexpr Optional<V> find(const K& key) const;
```

按键查找对应的值。

**返回值：** 找到时返回 `Optional<V>`（包含值），未找到时返回空 `Optional`。

**示例：**

```cpp
StaticHashMap<int, int, 16> map;
map.insert(1, 10);

auto v = map.find(1);
assert(v.has_value());
assert(v.value() == 10);

auto v2 = map.find(999);
assert(!v2.has_value());

// StringView 作为键
StaticHashMap<cinux::lib::StringView, int, 16> smap;
smap.insert("hello", 1);
assert(smap.find("hello").value() == 1);
```

---

### `contains`

```cpp
constexpr bool contains(const K& key) const;
```

判断表中是否存在指定的键。

---

### `remove`

```cpp
constexpr bool remove(const K& key);
```

从哈希表中删除指定键对应的条目。采用墓碑标记法（tombstone），不移动其他元素。后续插入时可复用墓碑槽位。

**示例：**

```cpp
StaticHashMap<int, int, 16> map;
map.insert(1, 10);

assert(map.remove(1));          // true
assert(!map.find(1).has_value());  // 键已不可见

assert(map.insert(1, 20));     // true，复用墓碑槽位
assert(map.find(1).value() == 20);
```

---

### `operator[]`

```cpp
constexpr V& operator[](const K& key);
```

按键直接访问对应值的引用。若键不存在，则自动以默认值 `V{}` 插入新条目。

**注意：** 若表已满且键不存在，将导致越界访问（未定义行为）。调用方须确保表未满或键已存在。

**示例：**

```cpp
StaticHashMap<int, int, 8> map;

map[42] = 100;
assert(map.find(42).value() == 100);

int val = map[7];  // 自动插入，val == 0（int 默认值）
```

---

### `clear`

```cpp
constexpr void clear();
```

清空哈希表，将所有槽位重置为初始状态。时间复杂度 O(N)。

---

### `Hash<K>` 特化

库为以下类型提供了 `Hash` 模板特化：

#### `Hash<uint32_t>`

```cpp
template <> struct Hash<uint32_t> {
    constexpr size_t operator()(uint32_t v) const;
};
```

使用 Knuth 乘法哈希：`v * 2654435761u`。

#### `Hash<int>`

将 `int` 转为 `uint32_t` 后委托给 `Hash<uint32_t>`。

#### `Hash<uint64_t>`

```cpp
template <> struct Hash<uint64_t> {
    constexpr size_t operator()(uint64_t v) const;
};
```

使用 FNV 风格乘法哈希：`v * 14695981039346656037ULL`。

#### `Hash<StringView>`

```cpp
template <> struct Hash<StringView> {
    constexpr size_t operator()(StringView sv) const;
};
```

使用 FNV-1a 哈希算法，逐字节处理 `StringView` 的内容。

**自定义哈希函子示例：**

```cpp
struct MyKey {
    int id;
    int sub;
};

struct MyKeyHash {
    constexpr size_t operator()(const MyKey& k) const {
        return cinux::lib::Hash<int>{}(k.id * 31 + k.sub);
    }
};

cinux::lib::StaticHashMap<MyKey, int, 64, MyKeyHash> map;
```

---

## 注意事项

1. **容量限制**：`N` 为编译期固定的槽位数，不存在动态扩容。建议负载因子不超过 70%。
2. **墓碑积累**：删除操作通过墓碑标记实现。高频删除后墓碑会增加探测路径长度，可周期性调用 `clear()` 重建。
3. **线性探测最坏情况**：哈希函数分布不均或负载过高时可能产生主群集，导致 O(N) 退化。
4. **线程安全**：所有方法均非线程安全，多线程并发需外部同步。
5. **`operator[]` 的 UB 风险**：表满且键不存在时为越界访问，调用方须确保安全。
6. **constexpr 支持**：所有公开方法均为 `constexpr`，支持编译期求值。

## 另见

- [Optional](optional.md) — `find()` 返回的值包装类型
- [StringView](string-view.md) — 字符串视图，可用作哈希表键
