---
title: 03 · 代码路线续:free / expand / crt_stub
---

# 代码路线续:free / expand / crt_stub

## free:magic 校验、双释放检测、合并

`free` 短得多,但每一句都在防错:

```cpp
void Heap::free(void* ptr) {
    if (ptr == nullptr) return;                       // free(nullptr) 安全, 照搬 C 语义
    auto* block = header_from_ptr(ptr);               // ptr - 32
    if (block->magic != HEAP_MAGIC) {                 // 头被踩 / free 了野指针
        kprintf("[HEAP] Double-free or corruption at %p (magic=0x%x, expected 0x%x)\n", ...);
        return;
    }
    if (block->free) {                                // 已经是空闲块 → 双释放
        kprintf("[HEAP] Double-free detected at %p\n", ptr);
        return;
    }
    used_ -= HEADER_SIZE + block->size;
    block->free = 1;
    block->next = free_list_;                         // 塞回链头
    free_list_  = block;
    coalesce(block);                                  // 和相邻空闲块合并
}
```

两道校验。第一道 magic:如果 `ptr - 32` 处不是 `0xDEADBEEF`,说明这个指针压根不是堆分配出来的(野指针),或者这块的头已经被越界写坏了。第二道 `free` 标志:如果这个块**已经**标记为空闲,你又 free 一次,就是双释放——双释放会把同一块塞进链两次,后续分配可能把同一块返回给两个调用者,是堆腐败的头号元凶,这里直接拦下。

拦下之后是记账(`used_` 减)、回链(前插)、合并。前插(O(1))而不是排序插入,因为合并逻辑不依赖链的顺序——它按**地址**找邻居,不按链序。

`coalesce` 的逻辑值得看一眼,因为它暴露了这套实现的代价:

```cpp
void Heap::coalesce(BlockHeader* block) {
    bool changed = true;
    while (changed) {                                 // 合并可能级联, 多趟直到稳定
        changed = false;
        for (BlockHeader* curr = free_list_; curr; curr = curr->next) {
            // curr 的尾正好贴着 block 的头 → 把 block 并进 curr
            // 或 block 的尾正好贴着 curr 的头 → 把 curr 并进 block
            ...遇到相邻就改 size、从链上摘掉被并掉的那块、changed=true、break...
        }
    }
}
```

关键点:它判断「相邻」靠的是**地址**——`curr 的地址 + 头 + curr.size == block 的地址`。但 free 链**不是按地址排序的**(alloc 把 remainder 往链头塞,free 也往链头塞),所以要找地址邻居,只能**遍历整条链**;而且合并后可能产生新的相邻,所以外面套一层 `while (changed)` 多趟扫,直到没有新合并。复杂度是 O(块数²)。

对内核堆来说这通常无所谓——内核动态分配的块数量远达不到让 O(n²) 成为瓶颈的程度。但这是个明确的取舍:用「简单的前插链 + 全扫描合并」换「不用维护地址排序 / 边界标签」。工业级分配器(dlmalloc、Linux 的 slab/buddy)各有更精巧的办法(比如在块头里同时记物理前后邻居的「边界标签 footer」),这套实现没走那条路——够用、可读、好讲,是这一章的选择。这里只是点明它的上限,不是说它错了。

## expand:free 链空了就向 VMM 要页

`expand` 是「容量不够时自动长个」:

```cpp
void Heap::expand(size_t min_bytes) {
    uint64_t needed_bytes = align_up(min_bytes + HEADER_SIZE, PAGE_SIZE);
    uint64_t needed_pages = needed_bytes / PAGE_SIZE;
    if (needed_pages < EXPAND_PAGES) needed_pages = EXPAND_PAGES;   // 至少续 4 页(16 KB)
    uint64_t expand_size = needed_pages * PAGE_SIZE;

    for (uint64_t off = 0; off < expand_size; off += PAGE_SIZE) {   // 在 base_+size_ 处续映页
        uint64_t phys = g_pmm.alloc_page();
        if (phys == 0) { kprintf("[HEAP] OOM during expansion ...\n"); return; }
        g_vmm.map(base_ + size_ + off, phys, PAGE_FLAGS);
    }
    memzero((void*)(base_ + size_), expand_size);                   // 清零
    auto* nb = (BlockHeader*)(base_ + size_);                       // 新区摆一个 free 块
    nb->magic = HEAP_MAGIC; nb->size = expand_size - HEADER_SIZE; nb->free = 1; nb->next = free_list_;
    free_list_ = nb;
    size_ += expand_size;                                           // 总长增长
}
```

它和 `init` 高度同构:都是「要页 → 映射 → 清零 → 摆 free 块」,区别只在 `init` 是从头建、`expand` 是在 `base_ + size_`(当前末尾)续上。续完之后 `size_` 增长,新 free 块塞进链头,控制权回到 `alloc` 的递归调用——下一次 first-fit 就能扫到这块新区。

要诚实说明的是:**这套扩容只在内核里能真正跑起来**(它依赖 `g_pmm` / `g_vmm`),而且我们这一章自带的测试,分配量都落在初始 64 KB 之内,未必会触发 `expand`。它是一段「写好、接好、等被用到」的代码——逻辑正确性靠代码审查 + 下游真正压满时验证,而不是靠本 tag 的测试用例直接覆盖。这不算缺陷,但读的时候心里要有数:别以为「测试过了」就等于「扩容路径被实测过」。

## crt_stub:让 new/delete 落到堆上

最后一块拼图在 [crt_stub.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/crt_stub.cpp)。内核是 `-ffreestanding -nostdlib` 编译的,标准库的 `operator new` / `delete` 不存在,得自己提供。这一章把所有重载都接到堆上:

```cpp
void* operator new(unsigned long size)                          { return cinux::mm::g_heap.alloc(size); }
void* operator new[](unsigned long size)                        { return cinux::mm::g_heap.alloc(size); }
void* operator new(unsigned long size, std::align_val_t a)      { return cinux::mm::g_heap.alloc(size, (size_t)a); }
void* operator new[](unsigned long size, std::align_val_t a)    { return cinux::mm::g_heap.alloc(size, (size_t)a); }
void  operator delete(void* p) noexcept                         { cinux::mm::g_heap.free(p); }
void  operator delete[](void* p) noexcept                       { cinux::mm::g_heap.free(p); }
// ... 还有 sized / aligned 的 delete 变体, 都落到 free ...
```

两件事。一是覆盖了**对齐版**的 `operator new(size, std::align_val_t)`——这是 C++17 加进来的带对齐分配,当你 `new (std::align_val_t(64)) T` 或对齐要求超过 `__STDCPP_DEFAULT_NEW_ALIGNMENT__`(通常 16)的类型 `new` 时,编译器会调这个重载。它把对齐值透传给 `g_heap.alloc` 的第二个参数——这正是前面那一大段「front padding」机制存在的意义:堆得能按调用者要的任意对齐摆 payload。二是 `operator delete` 全部忽略尺寸 / 对齐参数(`free` 用不上,尺寸从头里读),不管编译器调哪个 delete 重载,都安全落到 `g_heap.free`。

这个文件里还住着别的 freestanding 桩(`__cxa_pure_virtual`、`__stack_chk_fail`、`__cxa_atexit`、`_init_global_ctors`),它们和堆没有直接关系,但和「让 C++ 在裸机上跑起来」是一体的——这一章顺手把 `new`/`delete` 也补齐了,内核的 C++ 运行时支持至此基本成形。
