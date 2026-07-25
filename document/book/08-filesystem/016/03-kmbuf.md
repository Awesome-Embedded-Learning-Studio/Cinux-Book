---
title: 03 · 主线二:治法 1——KmBuf RAII
---

# 主线二:治法 1——KmBuf RAII

## 主线二:治法 1——`KmBuf` RAII,为什么堆不栈

per-call buffer 的容器选什么?第一反应是「栈上 `uint8_t buf[4096]` 不就完了」。这里不行,原因是**栈深度约束**。

demand page 的 `#PF` 跑在受限栈上(它可能嵌在中断上下文、甚至嵌在别的 ISR 里被再入,可用栈深本来就宝贵;而且文件读写调用链会深递归进 demand-page,16KB 任务栈也吃紧)。源码注释把这条约束写在 `KmBuf` 的类文档里:

```cpp
/// RAII kmalloc'd scratch buffer for ext2 SMP read/write paths (block_buf_ is
/// shared/non-thread-safe). Heap not stack: demand-page #PF runs on IST2 which
/// is only 4 KB (IRQ_STACK_PAGES=1). ~KmBuf kfrees; operator bool checks OOM.
class KmBuf {
    void* p_;
public:
    explicit KmBuf(uint64_t n) : p_(cinux::mm::kmalloc(n, 16)) {}
    ~KmBuf() {
        if (p_ != nullptr) {
            cinux::mm::kfree(p_);
        }
    }
    KmBuf(const KmBuf&)            = delete;
    KmBuf& operator=(const KmBuf&) = delete;
    explicit operator bool() const { return p_ != nullptr; }
    void*    get() const { return p_; }
    uint8_t* data() const { return static_cast<uint8_t*>(p_); }
};
```

([ext2_common.hpp:24-41](../../../libs/ext2/ext2_common.hpp#L24))

在 `#PF` 里再撑一个 4KB 的栈数组就贴着栈底跑了;就算不在 `#PF` 里,文件读写的调用链也会深递归进 demand-page 路径,把 8KB 任务栈(`TaskBuilder::STACK_PAGES = 2`、AP 内核栈 `kStackPages = 4`,都见 `task_builder.hpp` / `ap_main.cpp`)也吃紧。所以选堆。

> **关于那条源码注释的常量名**:注释里写的是「`#PF runs on IST2 which is only 4 KB (IRQ_STACK_PAGES=1)`」。这个写法在「`#PF` 走不走 IST」上要分清——`kernel/arch/x86_64/idt.cpp` 的 IDT 路由表里 `#PF`(`ExceptionVector::PF`)的 `ist = 0`,即 `#PF` 不走 IST、跑在主任务栈上;而 IST2 那 4KB(`IRQ_STACK_PAGES = 1`,见 `gdt.hpp:115-119`)是给**硬件 IRQ**用的,不是给 `#PF`。所以注释把 `#PF` 跟 IST2 混着说,措辞不准。但**结论本身成立**——`#PF` 跑在主任务栈上(`ist = 0`),栈深受限、调用链还深,4KB buffer 不该再压栈。咱们这里把注释当动机引用,选堆的真正理由是上面那段「调用链深 + 栈余量宝贵」,而不是字面上那 4KB 的 IST。

`KmBuf` 刻意做小,就这四个方法:

- 构造 `kmalloc(n, 16)`——按 16 对齐(ext2 块结构体里很多字段需要 `uint32_t` 对齐,块大小 4096 也是 16 的倍数)。
- 析构 `kfree`——这是 RAII 的核心:调用方在自己栈帧上 `KmBuf scratch(4096);`,出作用域编译器自动 `kfree`,既消除共享又不用记着 free。
- `operator bool` 查 OOM——`kmalloc` 在内存紧时可能失败,调用方**必须检查**,不能假设成功就往里写(否则就是空指针写)。整个库里每个 `KmBuf` 的调用点后面都跟着 `if (!scratch) return ...;`。
- `get()` / `data()` 取指针。

它**不是 smart pointer**,不引入所有权复杂度,不 share、不 move、不 count ref。它就是一个一次性 scratch 载体,构造、用、析构,完事。范围小是刻意的——这个场景不需要更多。
