# StaticHashMap<K,V,N>

> 固定容量开放寻址哈希表，线性探测

## 头文件

`#include <cinux/static_hash_map.hpp>`

## 依赖

Optional, StringView

## 概述

`StaticHashMap` 是一个完全在栈上分配的固定容量哈希表实现，采用开放寻址（open addressing）策略与线性探测（linear probing）解决冲突。所有槽位在编译期确定，整个容器不产生任何堆分配（heap allocation），适用于嵌入式、内核或其它对动态内存有严格限制的场景。

该组件通过模板参数 `N` 指定槽位数量，配合可定制的哈希函子 `HashT` 提供键的散列计算。内置的 `Hash` 特化覆盖了 `uint32_t`、`int`、`uint64_t` 以及 `StringView` 四种常见键类型，其中整数类型使用 Knuth 乘法哈希，字符串类型使用 FNV-1a 算法。删除操作采用墓碑（tombstone）标记法，已被标记为墓碑的槽位可在后续插入时被复用，从而在保持开放寻址结构的同时避免过多的伪占用。

`StaticHashMap` 的所有公共方法均标记为 `constexpr`，允许在常量求值上下文中使用。典型用例包括：编译期字符串映射、嵌入式固件中的小型查找表、操作系统内核启动阶段的数据结构、以及需要确定性内存布局的实时系统。

## API 参考

### 类模板

```cpp
template <typename K, typename V, size_t N, typename HashT = Hash<K>>
class StaticHashMap;
```

| 模板参数 | 说明 |
|----------|------|
| `K` | 键类型，需支持 `==` 比较和拷贝赋值 |
| `V` | 值类型，需支持默认构造和拷贝赋值 |
| `N` | 槽位数量（容量），编译期常量 |
| `HashT` | 哈希函子类型，默认为 `Hash<K>` |

---

### 默认构造函数

```cpp
constexpr StaticHashMap() = default;
```

构造一个空的哈希表，所有槽位初始化为未占用状态，`size()` 为 0。

```cpp
cinux::lib::StaticHashMap<int, int, 16> map;
// map.size() == 0, map.empty() == true
```

---

### capacity()

```cpp
static constexpr size_t capacity();
```

返回哈希表的总槽位数 `N`。此方法为 `static constexpr`，可在编译期求值。

- **返回值**：`size_t`，等于模板参数 `N`。

```cpp
cinux::lib::StaticHashMap<int, int, 16> map;
static_assert(map.capacity() == 16);
```

---

### size()

```cpp
constexpr size_t size() const;
```

返回当前哈希表中实际存储的键值对数量（不包括已删除的墓碑条目）。

- **返回值**：`size_t`，当前元素个数。

```cpp
cinux::lib::StaticHashMap<int, int, 16> map;
map.insert(1, 10);
map.insert(2, 20);
// map.size() == 2
```

---

### empty()

```cpp
constexpr bool empty() const;
```

判断哈希表是否为空。

- **返回值**：`bool`，当 `size() == 0` 时返回 `true`。

```cpp
cinux::lib::StaticHashMap<int, int, 16> map;
// map.empty() == true
map.insert(1, 10);
// map.empty() == false
```

---

### full()

```cpp
constexpr bool full() const;
```

判断哈希表是否已满，即 `size() >= N`。当表满时，`insert` 将无法插入新键。

- **返回值**：`bool`，当元素数量达到容量上限时返回 `true`。

```cpp
cinux::lib::StaticHashMap<int, int, 4> map;
map.insert(1, 10);
map.insert(2, 20);
map.insert(3, 30);
map.insert(4, 40);
// map.full() == true
// map.insert(5, 50) 返回 false
```

---

### insert()

```cpp
constexpr bool insert(const K& key, const V& value);
```

插入一个键值对。如果键已存在或表已满，则插入失败。

- **参数**：
  - `key`：要插入的键。
  - `value`：与键关联的值。
- **返回值**：`bool`，插入成功返回 `true`；键已存在或表满返回 `false`。

```cpp
cinux::lib::StaticHashMap<int, int, 16> map;
bool ok = map.insert(1, 10);   // ok == true
bool ok2 = map.insert(1, 99);  // ok2 == false，键已存在
// map.find(1).value() 仍为 10
```

---

### insert_or_assign()

```cpp
constexpr bool insert_or_assign(const K& key, const V& value);
```

插入键值对，若键已存在则覆盖其值。仅在表满且键不存在时失败。

- **参数**：
  - `key`：要插入或更新的键。
  - `value`：与键关联的新值。
- **返回值**：`bool`，操作成功返回 `true`；仅在表满且键不存在时返回 `false`。

```cpp
cinux::lib::StaticHashMap<int, int, 16> map;
map.insert(1, 10);
bool ok = map.insert_or_assign(1, 99);  // ok == true，值被覆盖
// map.find(1).value() == 99
```

---

### find()

```cpp
constexpr Optional<V> find(const K& key) const;
```

根据键查找对应的值。

- **参数**：
  - `key`：要查找的键。
- **返回值**：`Optional<V>`，若键存在则包含对应值，否则为空 `Optional`。

```cpp
cinux::lib::StaticHashMap<int, int, 16> map;
map.insert(1, 10);

auto v = map.find(1);
if (v.has_value()) {
    // v.value() == 10
}

auto v2 = map.find(999);
// v2.has_value() == false
```

使用 `StringView` 作为键的示例：

```cpp
cinux::lib::StaticHashMap<cinux::lib::StringView, int, 16> map;
map.insert("hello", 1);
map.insert("world", 2);

auto v = map.find("hello");  // v.value() == 1
auto v2 = map.find("foo");   // v2.has_value() == false
```

---

### contains()

```cpp
constexpr bool contains(const K& key) const;
```

判断哈希表中是否包含指定的键。

- **参数**：
  - `key`：要检查的键。
- **返回值**：`bool`，键存在返回 `true`，否则返回 `false`。

```cpp
cinux::lib::StaticHashMap<int, int, 16> map;
map.insert(5, 50);
// map.contains(5) == true
// map.contains(6) == false
```

---

### remove()

```cpp
constexpr bool remove(const K& key);
```

删除指定键对应的条目。删除时采用墓碑（tombstone）标记，被标记的槽位可在后续插入中被复用。

- **参数**：
  - `key`：要删除的键。
- **返回值**：`bool`，删除成功返回 `true`；键不存在返回 `false`。

```cpp
cinux::lib::StaticHashMap<int, int, 16> map;
map.insert(1, 10);
bool ok = map.remove(1);   // ok == true, size() 变为 0
bool ok2 = map.remove(1);  // ok2 == false，键已不存在

// 墓碑复用：重新插入同一键会成功
map.insert(1, 20);
// map.find(1).value() == 20
```

---

### operator[]()

```cpp
constexpr V& operator[](const K& key);
```

通过键直接访问对应的值引用。若键不存在，则自动以默认构造的 `V{}` 插入该键。

- **参数**：
  - `key`：要访问或插入的键。
- **返回值**：`V&`，对应值的引用。若键不存在，则先插入默认值再返回引用。

> **注意**：此运算符为非 `const` 方法。当键不存在时会触发默认插入，导致 `size()` 增加。如果表已满且键不存在，行为未定义。

```cpp
cinux::lib::StaticHashMap<int, int, 8> map;
map[42] = 100;        // 自动插入键 42，值为 100
// map.find(42).value() == 100

map[42] = 200;        // 覆盖已有值
// map[42] == 200
```

---

### clear()

```cpp
constexpr void clear();
```

清空哈希表，将所有槽位重置为未占用状态，`size()` 归零。

```cpp
cinux::lib::StaticHashMap<int, int, 8> map;
map.insert(1, 10);
map.insert(2, 20);
map.clear();
// map.empty() == true
// map.find(1).has_value() == false
```

---

### 哈希函子 Hash<T>

`Hash` 是默认的哈希函子模板，为常见类型提供了特化实现：

```cpp
template <typename T>
struct Hash;
```

#### 内置特化

| 特化类型 | 算法 |
|----------|------|
| `Hash<uint32_t>` | Knuth 乘法哈希：`v * 2654435761u` |
| `Hash<int>` | 转换为 `uint32_t` 后调用 `Hash<uint32_t>` |
| `Hash<uint64_t>` | 乘法哈希：`v * 14695981039346656037ULL` |
| `Hash<StringView>` | FNV-1a 哈希 |

用户可通过模板参数 `HashT` 传入自定义哈希函子，只需实现 `constexpr size_t operator()(const K&) const` 即可：

```cpp
struct MyKey { int id; };
struct MyHash {
    constexpr size_t operator()(const MyKey& k) const {
        return cinux::lib::Hash<int>{}(k.id);
    }
};

cinux::lib::StaticHashMap<MyKey, const char*, 32, MyHash> map;
map.insert(MyKey{1}, "hello");
```

---

## 注意事项

1. **容量固定**：槽位数 `N` 在编译期确定，运行时无法扩容。当表满后 `insert` 将返回 `false`。建议根据实际使用场景选择合适的 `N`，并保持负载因子不超过 70% 以获得较好的探测性能。
2. **墓碑累积**：频繁的插入-删除循环会在表中留下墓碑标记。虽然墓碑槽位可在后续插入中被复用，但它们在查找时仍会被跳过，可能降低查找效率。如果出现大量墓碑，可调用 `clear()` 后重新插入所有元素。
3. **线性探测最坏情况**：当哈希冲突集中时，线性探测可能导致连续占用区域较长，查找退化为 O(n)。选择合适的容量（略大于实际元素数）和良好的哈希函子可以缓解此问题。
4. **`operator[]` 的非 const 语义**：`operator[]` 在键不存在时会自动插入默认值，因此不提供 `const` 重载。如果仅需查找而不希望产生副作用，请使用 `find()` 或 `contains()`。
5. **键和值类型要求**：键类型 `K` 需支持 `operator==` 和拷贝赋值；值类型 `V` 需支持默认构造和拷贝赋值。
6. **`constexpr` 支持**：所有公共方法均为 `constexpr`，可在编译期常量表达式中使用。这使得在编译期构建静态查找表成为可能。
7. **线程安全**：`StaticHashMap` 不提供任何内部同步机制。在多线程环境下使用时，调用方需自行保证访问安全。

## 另见

- `cinux::lib::Optional` — 用于 `find()` 返回值的可选值包装类型
- `cinux::lib::StringView` — 字符串视图类型，可作为哈希表的键
- `cinux::lib::Hash<T>` — 默认哈希函子，提供内置类型的散列特化
