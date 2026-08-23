Here is the complete Chinese API documentation for the `IntrusiveList<T>` component.

```markdown
# IntrusiveList<T>

> 侵入式双向链表，零分配，链指针嵌入元素

## 头文件

`#include <cinux/intrusive_list.hpp>`

## 命名空间

`cinux::lib`

## 依赖

无

## 概述

`IntrusiveList` 是一个侵入式双向链表容器。与 `std::list` 等传统容器不同，侵入式容器将链接指针（`prev` / `next`）直接嵌入到用户定义的元素类型内部，而非在堆上单独分配节点。这意味着链表的所有插入、删除操作都不会触发任何动态内存分配，适用于内核开发、嵌入式系统以及任何对内存分配有严格约束的场景。

使用侵入式链表时，用户需要在自己的结构体中声明一个 `IntrusiveListNode<T>` 类型的成员，并（可选地）通过 Traits 类指定该成员的位置。默认 Traits 假定该成员名为 `list_node`。若同一元素需要同时存在于多个链表中，只需嵌入多个 `IntrusiveListNode<T>` 成员，并为每个链表提供不同的 Traits 即可。

由于链接指针与业务数据共存于同一对象中，侵入式链表还具有指针稳定性——元素的地址在其生命周期内不会改变，因此可以在遍历过程中安全地移除元素，且不存在迭代器失效的风险。

## 类和结构体

### IntrusiveListNode\<T\>

```cpp
template <typename T>
struct IntrusiveListNode {
    T* prev = nullptr;
    T* next = nullptr;
};
```

链表链接节点，需要嵌入到用户定义的结构体中。

**模板参数：**

| 参数 | 说明 |
|------|------|
| `T` | 包含该节点的宿主类型 |

**成员：**

| 成员 | 类型 | 说明 |
|------|------|------|
| `prev` | `T*` | 指向前一个元素的指针，默认为 `nullptr` |
| `next` | `T*` | 指向后一个元素的指针，默认为 `nullptr` |

**使用示例：**

```cpp
struct VMA {
    uint64_t start, end;
    IntrusiveListNode<VMA> list_node;
};
```

---

### IntrusiveListTraits\<T\>

```cpp
template <typename T>
struct IntrusiveListTraits {
    static constexpr IntrusiveListNode<T>& node(T& obj);
};
```

默认 Traits 类，用于将 `T&` 映射到其内部的 `IntrusiveListNode<T>&`。默认实现假定元素中包含名为 `list_node` 的成员。

**模板参数：**

| 参数 | 说明 |
|------|------|
| `T` | 元素类型 |

**静态成员函数：**

#### node

```cpp
static constexpr IntrusiveListNode<T>& node(T& obj);
```

返回 `obj` 中嵌入的链接节点引用。

**参数：**

| 参数 | 说明 |
|------|------|
| `obj` | 对元素的引用 |

**返回值：** 对 `obj` 内 `list_node` 成员的引用。

---

### IntrusiveList\<T, Traits\>

```cpp
template <typename T, typename Traits = IntrusiveListTraits<T>>
class IntrusiveList;
```

侵入式双向链表主类。

**模板参数：**

| 参数 | 说明 |
|------|------|
| `T` | 元素类型 |
| `Traits` | Traits 类型，用于将 `T&` 映射到 `IntrusiveListNode<T>&`，默认为 `IntrusiveListTraits<T>` |

## API 参考

### 构造函数

```cpp
constexpr IntrusiveList() = default;
```

构造一个空的侵入式链表。`head_`、`tail_` 初始化为 `nullptr`，`size_` 初始化为 0。声明为 `constexpr`，可在编译期使用。

---

### 容量查询

#### empty

```cpp
constexpr bool empty() const;
```

判断链表是否为空。

**返回值：** 若链表中无元素则返回 `true`，否则返回 `false`。

```cpp
IntrusiveList<Item> list;
REQUIRE(list.empty());  // true

Item a{1, {}};
list.push_back(a);
REQUIRE(!list.empty()); // false
```

#### size

```cpp
constexpr size_t size() const;
```

返回链表中元素的数量。

**返回值：** 链表中的元素个数，类型为 `size_t`。

```cpp
IntrusiveList<Item> list;
REQUIRE(list.size() == 0);

Item a{1, {}}, b{2, {}};
list.push_back(a);
list.push_back(b);
REQUIRE(list.size() == 2);
```

---

### 元素访问

#### front

```cpp
constexpr T* front();
```

返回指向链表头部元素的指针。

**返回值：** 指向头部元素的指针；若链表为空则返回 `nullptr`。

```cpp
Item a{1, {}}, b{2, {}};
list.push_back(a);
list.push_back(b);
REQUIRE(list.front()->value == 1);
```

#### back

```cpp
constexpr T* back();
```

返回指向链表尾部元素的指针。

**返回值：** 指向尾部元素的指针；若链表为空则返回 `nullptr`。

```cpp
Item a{1, {}}, b{2, {}};
list.push_back(a);
list.push_back(b);
REQUIRE(list.back()->value == 2);
```

---

### 插入操作

#### push_front

```cpp
constexpr void push_front(T& obj);
```

将元素插入链表头部。

**参数：**

| 参数 | 说明 |
|------|------|
| `obj` | 要插入的元素的引用 |

**行为：** `obj` 成为新的头部元素。若链表原来为空，`obj` 同时成为头部和尾部。`obj` 的 `prev` 被设为 `nullptr`，`next` 指向原来的头部元素。

```cpp
IntrusiveList<Item> list;
Item a{1, {}}, b{2, {}}, c{3, {}};

list.push_front(a);  // 链表: a
list.push_front(b);  // 链表: b -> a
list.push_front(c);  // 链表: c -> b -> a

REQUIRE(list.front()->value == 3);
REQUIRE(list.back()->value == 1);
```

#### push_back

```cpp
constexpr void push_back(T& obj);
```

将元素插入链表尾部。

**参数：**

| 参数 | 说明 |
|------|------|
| `obj` | 要插入的元素的引用 |

**行为：** `obj` 成为新的尾部元素。若链表原来为空，`obj` 同时成为头部和尾部。`obj` 的 `next` 被设为 `nullptr`，`prev` 指向原来的尾部元素。

```cpp
IntrusiveList<Item> list;
Item a{1, {}}, b{2, {}}, c{3, {}};

list.push_back(a);  // 链表: a
list.push_back(b);  // 链表: a -> b
list.push_back(c);  // 链表: a -> b -> c

REQUIRE(list.front()->value == 1);
REQUIRE(list.back()->value == 3);
```

#### insert_before

```cpp
constexpr void insert_before(T& before, T& obj);
```

将元素 `obj` 插入到元素 `before` 之前。

**参数：**

| 参数 | 说明 |
|------|------|
| `before` | 目标位置元素的引用，新元素将插入到该元素之前 |
| `obj` | 要插入的元素的引用 |

**行为：** 若 `before` 是头部元素，则 `obj` 成为新的头部元素。链表大小增加 1。

```cpp
IntrusiveList<Item> list;
Item a{1, {}}, b{2, {}}, c{3, {}};
list.push_back(a);
list.push_back(c);     // 链表: a -> c
list.insert_before(c, b); // 链表: a -> b -> c
```

#### insert_after

```cpp
constexpr void insert_after(T& after, T& obj);
```

将元素 `obj` 插入到元素 `after` 之后。

**参数：**

| 参数 | 说明 |
|------|------|
| `after` | 目标位置元素的引用，新元素将插入到该元素之后 |
| `obj` | 要插入的元素的引用 |

**行为：** 若 `after` 是尾部元素，则 `obj` 成为新的尾部元素。链表大小增加 1。

```cpp
IntrusiveList<Item> list;
Item a{1, {}}, b{2, {}}, c{3, {}};
list.push_back(a);
list.push_back(c);     // 链表: a -> c
list.insert_after(a, b); // 链表: a -> b -> c
```

---

### 删除操作

#### remove

```cpp
constexpr void remove(T& obj);
```

从链表中移除指定元素。

**参数：**

| 参数 | 说明 |
|------|------|
| `obj` | 要移除的元素的引用 |

**行为：** 更新前后相邻元素的链接指针以绕过 `obj`。若 `obj` 是头部元素，则更新 `head_`；若 `obj` 是尾部元素，则更新 `tail_`。移除后，`obj` 的 `prev` 和 `next` 均被重置为 `nullptr`。链表大小减少 1。

**注意：** 调用此函数前，`obj` 必须在当前链表中。对不在链表中的元素调用此函数是未定义行为。

```cpp
IntrusiveList<Item> list;
Item a{1, {}}, b{2, {}}, c{3, {}};
list.push_back(a);
list.push_back(b);
list.push_back(c);

list.remove(b);       // 链表: a -> c
REQUIRE(list.size() == 2);
REQUIRE(list.front()->value == 1);
REQUIRE(list.back()->value == 3);
```

#### pop_front

```cpp
constexpr T* pop_front();
```

移除并返回链表头部元素。

**返回值：** 指向原头部元素的指针；若链表为空则返回 `nullptr`。

```cpp
Item a{1, {}}, b{2, {}}, c{3, {}};
list.push_back(a);
list.push_back(b);
list.push_back(c);

REQUIRE(list.pop_front()->value == 1);  // 移除 a
REQUIRE(list.pop_back()->value == 3);   // 移除 c
REQUIRE(list.size() == 1);
```

#### pop_back

```cpp
constexpr T* pop_back();
```

移除并返回链表尾部元素。

**返回值：** 指向原尾部元素的指针；若链表为空则返回 `nullptr`。

```cpp
Item a{1, {}}, b{2, {}};
list.push_back(a);
list.push_back(b);

T* last = list.pop_back();
REQUIRE(last->value == 2);
REQUIRE(list.size() == 1);
```

#### clear

```cpp
constexpr void clear();
```

清空链表。将 `head_` 和 `tail_` 设为 `nullptr`，`size_` 设为 0。

**注意：** 此函数仅重置链表的内部状态，不会修改被移除元素的链接节点指针，也不会销毁任何元素。

```cpp
Item a{1, {}}, b{2, {}};
list.push_back(a);
list.push_back(b);
list.clear();

REQUIRE(list.empty());
REQUIRE(list.size() == 0);
REQUIRE(list.front() == nullptr);
REQUIRE(list.back() == nullptr);
```

---

### 迭代

#### begin

```cpp
constexpr T* begin();
```

返回指向链表头部元素的指针，用于迭代。空链表时返回 `nullptr`。

#### end

```cpp
constexpr T* end();
```

返回 `nullptr`，作为迭代终止哨兵。

**迭代示例（range-for）：**

```cpp
IntrusiveList<Item> list;
Item a{10, {}}, b{20, {}}, c{30, {}};
list.push_back(a);
list.push_back(b);
list.push_back(c);

for (auto* cur = list.begin(); cur != list.end();
     cur = IntrusiveListTraits<Item>::node(*cur).next) {
    // 使用 cur->value ...
}
```

**注意：** 由于 `T*` 同时作为迭代器，当前设计更适合使用显式指针遍历而非 range-for 循环。

---

## 高级用法：同一元素存在于多个链表

通过自定义 Traits，可以将同一个元素同时挂载到多个不同的链表中。只需在元素中嵌入多个 `IntrusiveListNode<T>` 成员，并为每个链表定义不同的 Traits 即可。

```cpp
struct DualNode {
    int v;
    IntrusiveListNode<DualNode> list_a;
    IntrusiveListNode<DualNode> list_b;
};

struct TraitsA {
    static constexpr IntrusiveListNode<DualNode>& node(DualNode& o) {
        return o.list_a;
    }
};

struct TraitsB {
    static constexpr IntrusiveListNode<DualNode>& node(DualNode& o) {
        return o.list_b;
    }
};

DualNode x{42, {}, {}};

IntrusiveList<DualNode, TraitsA> la;
IntrusiveList<DualNode, TraitsB> lb;

la.push_back(x);   // 通过 list_a 链接
lb.push_back(x);   // 通过 list_b 链接

// la 和 lb 独立管理 x，互不干扰
REQUIRE(la.size() == 1);
REQUIRE(lb.size() == 1);
REQUIRE(la.front()->v == 42);
REQUIRE(lb.front()->v == 42);
```

---

## 注意事项

1. **元素生命周期管理**：`IntrusiveList` 不拥有元素的所有权，不会在析构时销毁元素。用户必须确保元素的生命周期长于其在链表中的存活时间。将栈上或被销毁容器中的元素留在链表中会导致悬垂指针。

2. **元素不可重复入同一链表**：同一个元素不能在同一链表中出现多次（即不能对同一链表连续调用两次 `push_back` 传入同一元素），否则会破坏链表的内部结构。

3. **clear 不清理链接节点**：`clear()` 仅重置链表的 `head_`、`tail_` 和 `size_`，不会将各元素的 `prev`/`next` 置为 `nullptr`。若需要完全清理元素的链接状态，需手动遍历并逐个调用 `remove()`。

4. **remove 的前置条件**：被移除的元素必须处于当前链表中。对不在链表中的元素调用 `remove()` 是未定义行为。

5. **非线程安全**：本组件不提供任何线程安全保证。在多线程环境中使用时，需要外部同步机制。

6. **constexpr 支持**：所有成员函数均声明为 `constexpr`，可用于编译期计算（受限于 C++ 编译器对 constexpr 的支持程度）。

## 另见

- `IntrusiveListNode<T>` — 链接节点结构
- `IntrusiveListTraits<T>` — 默认 Traits，假定成员名为 `list_node`
- Linux 内核 `list_head` — 经典的侵入式链表实现
- Boost.Intrusive — Boost 库中的侵入式容器实现
```