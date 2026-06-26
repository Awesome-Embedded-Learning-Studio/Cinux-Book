---
title: 03 · 内核里 C++ 的克制用法
---

# 03 · 内核里 C++ 的克制用法:能用什么,绝不能用什么

> 一句话:把桌面 C++ 那套习惯收一收——内核里我们只取三个"零成本"的现代特性和一个 RAII 守卫,把 STL 容器、异常、RTTI、虚函数多态、智能指针统统挡在门外。

## 这一章干什么

读了前两章(03-cpp 的 C 核心、freestanding 子集),你已经知道内核里的 C++ 是一个被 `-ffreestanding`、`-fno-exceptions`、`-fno-rtti` 砍过的方言。这一章是个**速览**,只回答一个问题:**砍完之后,还剩下的那点现代 C++,我们在 Cinux 里到底用了哪几样、又是怎么用的**。目的是让你在读正文 [009 大内核入口](../../book/03-big-kernel/009-large-kernel-entry.md) 起、看到 `enum class`、`constexpr`、`using`、RAII 锁守卫、模板 `Atomic` 时,不会因为不熟悉而卡住,也不会因为太熟悉(桌面 C++ 习惯)而把不该搬进内核的东西搬进来。

这一章偏短。它只圈"边界"和"最小够读懂的写法",不讲 ABI、不讲运行时 stub(那在 02-freestanding-cpp 里),也不重复正文要讲透的实现细节。

## 一句话的取舍原则

内核 C++ 的取舍可以用一句话概括:**只用编译期就定死、运行期不引入隐式运行时依赖的特性**。

具体说,Cinux 允许的现代特性满足两条:

1. **零运行时成本**——`enum class`、`constexpr`、`using` 都是编译期的"重命名/常量折叠",不产生任何额外的运行期代码、不依赖 libstdc++、不依赖堆。
2. **不引入隐式控制流**——不抛异常、不做运行期类型查询、不在堆上偷偷分配。

RAII 锁守卫是个特例:它有运行期行为(构造时加锁、析构时解锁),但那是我们**主动写在头文件里、看得见摸得着**的几行,不是编译器偷偷塞进来的。模板 `Atomic` 同理——它只是 `__atomic_*` 内建的薄包装,没有虚表、没有分配。

> 外部依据:OSDev 的 [C++](https://wiki.osdev.org/C%2B%2B) 页明确建议内核 C++ 关掉异常与 RTTI、避免 STL,只保留"低层、可预测"的子集;GCC 手册的 `-ffreestanding` 条目说明该模式下实现无需提供完整标准库,只保证 `<stdint.h>`、`<stddef.h>`、`<stdarg.h>` 等少数 freestanding 头(`memcpy`/`memset` 仍要自供)。这两条正是 Cinux 取舍的外部依据。

## 允许的现代特性:`enum class`、`constexpr`、`using`

### `enum class`:带作用域的整数常量

正文里中断描述符表(IDT)用 `enum class` 来表达"异常向量号""门类型""特权级"这三类互不相同的整数。看 [idt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/idt.hpp):

```cpp
enum class ExceptionVector : uint8_t {
    DE  = 0,   // #DE: Divide Error
    BP  = 3,   // #BP: Breakpoint (INT3)
    PF  = 14,  // #PF: Page Fault (has error code)
};

enum class IDTGateType : uint8_t {
    Interrupt = 0x0E,  // 64-bit interrupt gate (clears IF)
    Trap      = 0x0F,  // 64-bit trap gate (preserves IF)
};

enum class IDTPrivilege : uint8_t {
    Kernel = 0x00,  // Ring 0 only
    User   = 0x60,  // Ring 3 (DPL=3)
};
```

为什么不直接用普通 `enum` 或裸 `#define`/`constexpr int`?

- **带作用域**:`IDTGateType::Interrupt` 不会和别的 `Interrupt` 撞名。普通 `enum` 的成员会泄漏到外层作用域,内核里符号一多就是灾难。
- **强类型**:你不能把 `ExceptionVector::PF`(向量号)随手赋给一个要 `IDTGateType` 的地方,编译器会挡。向量号、门类型、特权级三者都是 `uint8_t`,语义却完全不同——强类型让"把 14 当成门类型"这种低级错误在编译期就死掉。
- **底层类型显式钉死**:`: uint8_t` 保证它就是一个字节,塞进 IDT entry 的 `type_attr` 字段时不会有"枚举到底多大"的歧义。

代价几乎为零:编译完就是几个整数常量,没有虚表、没有分配。这是教科书级的"零成本抽象"。

> 详见正文 [010b · 大内核 IDT 与异常](../../book/03-big-kernel/010b-big-kernel-idt-exceptions.md)——那里会展开 IDT entry 的 16 字节布局、`type_attr` 字节怎么拼、IST 怎么用。

### `constexpr`:编译期常量与常量函数

`constexpr` 把"常量"从"一个不能改的变量"升级成"编译期就能算出来的值"。Cinux 用它干两件事。

**第一,编译期常量。**[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp) 里初始化内核堆时:

```cpp
// Step 12: Initialise kernel heap (64 KB initial region after kernel image)
constexpr uint64_t HEAP_VIRT_BASE    = cinux::arch::KMEM_HEAP_BASE;
constexpr uint64_t HEAP_INITIAL_SIZE = 64 * 1024;   // 64 KB
cinux::mm::g_heap.init(HEAP_VIRT_BASE, HEAP_INITIAL_SIZE);
```

`HEAP_INITIAL_SIZE = 64 * 1024` 在编译期就折叠成 `65536`,不占运行期任何开销。BootInfo 的物理地址也是这么钉死的:

```cpp
// BootInfo is placed at physical 0x7000 by the bootloader
static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;
```

`0x7000` 这个值是和引导阶段(stage2 把 BootInfo 写到物理 `0x7000`)约定死的,用 `constexpr` 钉死、配个注释,比散落在代码里的魔数 `0x7000` 可读得多。

**第二,编译期常量函数。**IDT 的 `type_attr` 字节由一个 `constexpr` 函数算出来:

```cpp
/// Build a type_attr byte from privilege and gate type
constexpr uint8_t make_idt_attr(IDTPrivilege priv, IDTGateType gate) {
    return 0x80 | static_cast<uint8_t>(priv) | static_cast<uint8_t>(gate);
}
```

调用处如果参数也都是编译期常量,整个 `make_idt_attr(...)` 会被折叠成一个字节;即便参数是运行期的,它也只是几条位运算,和手写 `0x80 | priv | gate` 没区别——但可读性强得多。`constexpr` 在这里的作用是"既能编译期算、又不亏运行期",典型的零成本。

> 详见正文 [009 · 大内核入口](../../book/03-big-kernel/009-large-kernel-entry.md)——`BOOT_INFO_PHYS` 怎么和引导阶段对接、`g_heap` 怎么初始化,那里讲透。

### `using`:类型别名,给函数指针起人话名字

IDT 类里有两个函数指针,裸写签名(`void (*)(InterruptFrame*)`)读起来很累。Cinux 用 `using` 给它们起人话名字:

```cpp
class IDT {
public:
    /// C handler function signature
    using Handler = void (*)(InterruptFrame*);

    /// Assembly ISR stub signature
    using Stub = void (*)();
    ...
};
```

之后 `set_handler(ExceptionVector vector, Stub stub, ...)` 的形参类型就是 `Stub`——一眼知道"这里要传一个汇编写的 ISR stub"。`using` 是 C++11 起的别名语法(等价于老的 `typedef`,但能模板化、读起来是从左到右的),纯编译期,运行期零成本。

这三样——`enum class`、`constexpr`、`using`——就是 Cinux 允许的现代特性的主力。它们的共同点:**全是编译期的名字/常量,不产生运行期负担**。正文里凡是不是这三样的"现代写法",多半就踩进了下面要划的禁区。

## RAII 锁守卫:运行期行为,但看得见

RAII(Resource Acquisition Is Initialization)是 C++ 区别于 C 的核心习惯:**把"获取资源"绑在构造函数上、"释放资源"绑在析构函数上,靠作用域自动释放**。桌面 C++ 用它管内存(`std::unique_ptr`)、管文件、管锁;内核里我们只用它管**锁**——而且是那种"看得见摸得着、就几行"的守卫。

看 [sync.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/sync.hpp) 里自旋锁的守卫:

```cpp
class Spinlock {
public:
    void acquire();
    void release();

    /** RAII guard -- acquires on construction, releases on destruction. */
    [[nodiscard]] auto guard() { return Guard(this); }

    /** IRQ-safe RAII guard -- disables interrupts then acquires. */
    [[nodiscard]] auto irq_guard() { return IrqGuard(this); }

private:
    volatile bool locked_ = false;

    class Guard {
    public:
        explicit Guard(Spinlock* lock) : lock_(lock) { lock_->acquire(); }
        ~Guard() { lock_->release(); }
        Guard(const Guard&)            = delete;   // 不可拷贝(否则双重释放)
        Guard& operator=(const Guard&) = delete;
    private:
        Spinlock* lock_;
    };
    ...
};
```

注意几个细节,它们是内核 RAII 和桌面 RAII 的关键差异:

- **`[[nodiscard]]`**:`guard()` 返回的守卫对象**绝不能被丢弃**。写成 `lock_.guard();`(忘了接住返回值),守卫是个临时对象,这条语句结束就析构、立刻 `release()`——锁等于没加。`[[nodiscard]]` 让编译器对这种"丢弃返回值"发出警告,这是内核里防手抖的一道闸。
- **`= delete` 拷贝/赋值**:守卫持有 `lock_` 指针,如果允许拷贝,两个守卫析构时会 `release()` 同一把锁两次,或一个先释放另一个还在"以为持有"。直接 `delete` 掉拷贝构造和赋值,从根上杜绝。
- **构造即加锁、析构即解锁**:`Guard(Spinlock* lock)` 在构造函数体里 `acquire()`,析构里 `release()`。没有异常(我们禁了异常),所以析构一定走、不会"构造到一半抛异常导致锁没加上却以为加了"。

真实调用现场在调度器里,每个临界区开头一行、靠作用域收尾:

```cpp
void RoundRobin::enqueue(Task* task) {
    auto g = lock_.irq_guard();   // 构造:关中断 + acquire()
    (void)g;                       // 仅"接住"守卫,防 [[nodiscard]] 警告
    if (count_ >= MAX_TASKS) { ... return; }   // 提前 return 也安全:g 析构会解锁
    ...
}   // 函数结束,g 离开作用域,析构:release() + 恢复中断
```

`(void)g;` 这行值得说一句:它纯粹是为了"消费"那个 `[[nodiscard]]` 返回值,告诉编译器"我知道这对象在这儿,我是故意让它靠作用域收尾的"。没有这行,有的编译器在开警告时会抱怨"未使用的变量 `g`";有了它,意图清晰——`g` 的价值不在被读取,而在它**活着**这件事本身。

这就是内核 RAII 的全部哲学:**靠作用域自动释放,所以提前 `return`、走异常(如果有的话)、漏写 `unlock` 都不会泄漏锁**。但它和桌面 RAII 的边界要画清——见下一节的禁区。

> 详见正文 [06 · 进程卷](../../book/06-process/021-proc-sync.md) 里的同步原语章——`Mutex` 的 `guard()`、`Semaphore`、`InterruptGuard`(关中断的 RAII)都在那里展开,包括"自旋锁绝不能跨阻塞操作持有"这条硬约束。

## 有限模板 `Atomic`:被允许的、很薄的一层模板

模板本身不犯禁——只要它实例化出来的代码是可预测的、不偷偷塞虚表或堆分配。Cinux 用模板做了个 `Atomic<T>`:

```cpp
template <typename T>
class Atomic {
    static_assert(__is_trivially_copyable(T), "Atomic requires a trivially copyable type");
    alignas(T) T value_;
public:
    T load(MemoryOrder order = MemoryOrder::SeqCst) const {
        return __atomic_load_n(&value_, static_cast<int>(order));
    }
    void store(T v, MemoryOrder order = MemoryOrder::SeqCst) {
        __atomic_store_n(&value_, v, static_cast<int>(order));
    }
    T fetch_add(T delta, MemoryOrder order = MemoryOrder::SeqCst) {
        return __atomic_fetch_add(&value_, delta, static_cast<int>(order));
    }
    ...
};
```

几个为什么:

- **`__atomic_*` 是 GCC 内建**,不是 `<atomic>`。我们没链 libstdc++,但 `__atomic_load_n` 这类内建由编译器直接生成 `lock` 前缀的指令(`lock xadd`、`lock cmpxchg`),不需要任何库。所以内核能用原子操作,靠的是编译器内建,不是 STL。
- **`static_assert(__is_trivially_copyable(T))`**:编译期把关,只允许平凡可拷贝类型(整数、指针)。你想 `Atomic<std::string>`?编译直接失败。这是模板"主动收紧实例化范围"的典型——把不安全的用法挡在编译期。
- **`MemoryOrder` 是 `enum class`**(映射到 `__ATOMIC_*` 宏),给内存序一个带作用域的名字,又是上面那套 `enum class` 的应用。

实例化后它就是个普通结构体加几个内联函数,没有虚表、没有分配。内核里它被用来做线程号自增、调度器 tick 计数、PIT tick 计数:

```cpp
extern cinux::lib::Atomic<uint64_t> next_tid;   // process_internal.hpp
// process_new.cpp:
cinux::lib::Atomic<uint64_t> next_tid{1};
```

这是 Cinux 允许模板的全部理由:**实例化结果可控、靠编译器内建而非库、有 `static_assert` 收口**。任何"模板 + 虚函数 + 堆分配"的组合(典型如 STL 容器)就被挡在禁区外了。

> 详见正文 [06 · 进程卷](../../book/06-process/020-proc-scheduler.md)——`next_tid` 怎么在创建进程时 `fetch_add`、调度器的 `Atomic<int> tick_count_` 怎么被时钟中断累加,那里有完整调用链。

## 禁区:桌面 C++ 习惯,内核里一个都别用

这一节是本章的重点。读者最容易栽跟头的,不是"不知道能用什么",而是"把桌面 C++ 的习惯顺手搬进来"。下面这些,**在 Cinux 内核里一个都不准用**。

**STL 容器(`std::vector`/`std::map`/`std::string`/…)**
这些容器会动态分配堆、会拷贝元素、依赖 libstdc++ 的运行时。内核的堆是我们自己手写的 first-fit 分配器(见 02-freestanding-cpp),不是 STL 那套带异常安全保证的分配器;而且 `-ffreestanding` 下根本链不进 `<vector>`。需要"一组同类对象"时,内核用**定长数组 + 计数**(如调度器的 `run_queue_[MAX_TASKS]`),或**侵入式链表**(如 `Mutex` 的等待队列靠 `Task::wait_next` 串起来,不分配节点)。需要"字符串"时,用 `char[]` + 自家 `kprintf`/`strncpy`。

**智能指针(`std::unique_ptr`/`std::shared_ptr`)**
`unique_ptr` 本身没有堆分配(它只是包装一个指针),但它依赖移动语义和标准库的 `default_delete`;`shared_ptr` 更糟,内部有原子引用计数块、会分配。内核里所有权管理是显式的:谁 `new` 谁 `delete`(全局 `new`/`delete` 已转 `g_heap`,见 02-freestanding-cpp),资源生命周期靠 RAII 守卫(锁)或手动配对管理。不要用智能指针"省心",它会引入你看不见的控制流。

**异常(`throw`/`try`/`catch`)**
Cinux 用 `-fno-exceptions` 编译,`throw` 直接编不过。更根本的是:**内核没有合理的异常处理策略**——一个未捕获的异常意味着内核崩溃,而"展开栈、调用析构链"这套机制本身又依赖运行时。内核的错误处理走**返回值 / 错误码 / 直接 `kprintf` + `hlt`**,不用异常。

**RTTI(`dynamic_cast`/`typeid`)**
`-fno-rtti` 关掉。运行期类型查询需要编译器为每个多态类型生成类型信息,那是隐式成本和隐式依赖。内核里要分类型,用 `enum class` 显式打 tag(像 `TaskState`),不用 `dynamic_cast`。

**虚函数多态(继承 + `virtual` + 运行期分派)**
这一条要小心区分:`virtual` 函数本身能编过(Cinux 里没有 `-fno-virtual`),但**我们刻意不用它做运行期多态**。原因:虚表是每个类一份的隐式数据结构、虚表指针塞进每个对象、虚调用是一次间接跳转——这些在性能敏感的内核热路径上不划算,更关键的是它"隐藏了控制流"(你看代码看不到实际调用的是哪个函数)。Cinux 的"多态"靠**函数指针 + `enum class` tag**显式做(像 IDT 的 `Stub`/`Handler`、驱动里的注册表),不靠继承体系。`__cxa_pure_virtual` 这个 stub 还是要提供(防纯虚调用),但那是防御性的,不是鼓励用虚函数(见 02-freestanding-cpp)。

> 外部依据:OSDev 的 [C++](https://wiki.osdev.org/C%2B%2B) 页对"为什么内核要避开 STL/异常/RTTI/虚函数"有逐条社区总结;GCC 手册 `-fno-exceptions`/`-fno-rtti` 条目说明这两个开关分别去掉异常运行时和类型信息生成。Cinux 的 `-ffreestanding` 等编译标志在 02-freestanding-cpp 里逐条展开。

把上面五条总结成一张对照表,贴在脑里:

| 桌面 C++ 习惯 | 内核里的替代 |
|---|---|
| `std::vector` / `std::string` | 定长数组 + 计数 / `char[]` + 自家 `kprintf` |
| `std::unique_ptr` / `shared_ptr` | 显式 `new`/`delete`(转 `g_heap`)+ RAII 守卫 |
| `throw` / `try` / `catch` | 返回值 / 错误码 / `kprintf` + `hlt` |
| `dynamic_cast` / `typeid` | `enum class` 显式 tag |
| 继承 + `virtual` 多态 | 函数指针 + `enum class` tag(IDT `Stub`/`Handler`) |

## 一句话收尾

内核里的 C++ 不是"少了 STL 的残废 C++",而是"被刻意收紧到一个可预测子集的 C++":编译期特性(`enum class`/`constexpr`/`using`)随便用,运行期特性只留看得见的 RAII 锁守卫和有 `static_assert` 把关的模板 `Atomic`,其余一概挡在门外。记住这条边界,你读正文的 C++ 代码就不会卡——也不会把桌面习惯误带进来。

---

### 参考

- OSDev — [C++](https://wiki.osdev.org/C%2B%2B)(内核里 C++ 该关什么、该避什么)、[Calling Global Constructors](https://wiki.osdev.org/Calling_Global_Constructors)(`.init_array` 与 `__cxa_*`,02-freestanding-cpp 详讲)。
- GCC 手册 — `-ffreestanding`(freestanding 实现只需提供少数头)、`-fno-exceptions` / `-fno-rtti`(分别去掉异常运行时与类型信息)、`__atomic_*` 内建(原子操作不依赖 `<atomic>` 库)。
- ISO C++ 标准草案 — `[enum]/enum class`(作用域枚举与底层类型)、`[dcl.constexpr]`(常量表达式)、`namespace.udecl`/`using`(类型别名)。
- 本仓库源码:[idt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/idt.hpp)(`enum class`/`constexpr`/`using`)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(`constexpr` 堆基址/BootInfo 地址)、[sync.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/sync.hpp)(RAII `Guard`/`IrqGuard`/`InterruptGuard`)、[atomic.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/lib/atomic.hpp)(模板 `Atomic<T>` + `static_assert`)、[scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.cpp)(`irq_guard()` 真实调用现场)。
- 边界依据:README §3(模块 3)划定的 out_of_scope——STL 容器 / 异常 / RTTI / 智能指针。

> 本章为速览,边界划清即达目的。freestanding 运行时 stub(`__cxa_*`/guard)、全局 `new`/`delete` 转 `g_heap`、内联汇编 `asm volatile`、`extern "C"` 接汇编与链接脚本符号,见同卷 [02-freestanding-cpp.md](02-freestanding-cpp.md);正文 C++ 内核实现细节见 [009 大内核入口](../../book/03-big-kernel/009-large-kernel-entry.md) 起。
