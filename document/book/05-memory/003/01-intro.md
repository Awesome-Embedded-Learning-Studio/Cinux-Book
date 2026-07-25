---
title: 01 · 点亮什么、为什么、设计图
---

# 点亮什么、为什么、设计图

## 这一章我们要点亮什么

三件事,一件比一件实在。

第一件,堆给了内核**任意大小**的动态分配能力。`g_heap.alloc(64)` 切一块 64 字节出来,`g_heap.free(p)` 还回去。不再被迫以页为单位,64 字节就只占 64 字节(外加一个小小的块头)。

第二件,这套分配器不只是「能分」,它还管「不浪费、不泄漏」。分配时如果切出来的块比需要的大很多,它会把尾部多余的**分裂**成一个新的空闲块留给下次;释放时如果发现旁边刚好也是空闲块,它会把两者**合并**成一块大的,避免碎片把内存切成渣。

第三件,稍微超出「分配器」本身的范围:我们把全局的 `operator new` / `delete` 全部重定向到这个堆上。于是在内核里写 `new Foo`、`new uint8_t[n]`、甚至带对齐要求的 `new (std::align_val_t(64)) T`,落到的都是 `g_heap.alloc`。C++ 的标准分配语法,在 freestanding 内核里第一次有了真实后端。

合起来,内核从「只能整页整页地拿内存」升级成「像用户态 `malloc` 一样按需切配」。后面几乎所有动态数据结构——动态数组、链表节点、各种缓存——都要建在这层之上。

## 为什么现在需要它

先接住 016 的尾巴。016 末尾我们留了一句话:现在最小单位是整页,「分配几十字节」还做不到——「那是下一章堆分配器的活」。这一章就是兑现这句承诺。PMM(015)解决了「物理页从哪来」,VMM(016)解决了「物理页怎么出现在虚拟地址空间」,两者配套,但粒度都停在页。堆就是在这两者之上加一层细粒度:它从 VMM 那儿**借**一串连续的页,自己在这串页里做字节级的切块回收。所以堆既不碰物理地址(那是 PMM 的事),也不碰页表映射(那是 VMM 的事),它只管「这一段已经映射好的虚拟内存里,哪些字节在用、哪些空着」。

再说说为什么是「借页」而不是「自己拥有固定一块内存」。内核启动时不知道以后要动态分配多少——文件系统的缓存、进程的内核栈、驱动里的各种缓冲,总量无法预知。所以堆被设计成**可扩张**:初始映射一小段(本章是 64 KB),不够了就再向 VMM 要几页续在末尾。这层「不够就扩」的设计,让堆不需要一上来就霸占一大块,也不需要在预测失误时推倒重来。它和 016 的 demand paging 是两种不同的「按需」:demand paging 是「访问到才补页」,堆的 expand 是「空闲链空了才扩页」,一个是被动缺页驱动,一个是主动容量驱动。

还有一笔账要交代清楚。`main.cpp` 里这一章的 milestone 注释写的是「Kernel heap allocator with **kmalloc/kfree**, new/delete takeover」。别被它带偏——翻遍这个 tag 的源码,**没有** `kmalloc` / `kfree` 这两个符号,那只是 milestone 目标里的叫法(沿袭了「内核 malloc」这个传统说法)。这一章实际落地的是 `Heap::alloc` / `Heap::free` 两个方法,加全局 `operator new` / `delete` 的接管。源码注释是线索,不是权威——这点我们在 010 章(GDT 注释把 SDM 图号抄错)就吃过亏,这里同理:以源码符号为准。

## 设计图

堆的核心数据结构是**空闲链表(free list)**:一块块连续的空闲区,用链表串起来。每个块前面顶着一个 `BlockHeader`,记录这块多大、是否空闲、下一块在哪。

```text
   一段 VMM 映射好的虚拟内存(初始 64 KB)
   base_                                                        base_ + size_
   ▼                                                                        ▼
   ┌────────────────────────────────────────────────────────────────────────┐
   │ BlockHeader │          可分配区(payload)                              │
   │ magic/size/ │   (初始时整段是一个大 free 块)                            │
   │ free/next   │                                                          │
   └────────────────────────────────────────────────────────────────────────┘
        │
        ▼
   free_list_ ──► (初始:就上面这一块, next=null)

   alloc(64) 之后:first-fit 找到这块, 它够大 → 从尾部切一块,
   原块缩成 remainder 留在链上, 切下来的块标 in-use 返回给调用者:

   ┌──────────────┬──────────┐┌──────────────┬──────────┐
   │ Header(rem)  │  free    ││ Header(in-use)│ 64 字节  │  ← 返回这块的 payload 指针
   │ size=大-开销 │ free=1   ││ size=64       │ free=0   │
   └──────────────┴──────────┘└──────────────┴──────────┘
        ▲
   free_list_ ──► remainder(remainder.next = null)

   free(p) 之后:把 in-use 块标回 free、塞回链头;若与相邻 free 块地址相连则 coalesce 合并
```

`BlockHeader` 本身长这样(32 字节):

```text
   ┌──────┬──────┬──────┬────────────┬──────────┐
   │magic │ size │ free │  _pad[12]  │   next   │   = 32 字节, [[gnu::packed]]
   │ 4B   │ 4B   │ 4B   │   12B      │   8B(x64)│
   └──────┴──────┴──────┴────────────┴──────────┘
     │      │      │                     │
     │      │      └─ 1=空闲 0=在用       └─ 空闲链的下一个块
     │      └─ payload 字节数(不含本头)
     └─ 0xDEADBEEF: 校验用, free() 时核对, 防 free 了野指针/双释放
```

alloc 的流程,关键是**对齐**——它让块头不再老实待在块的开头:

```text
   Heap::alloc(size, align=16):
     needed = size + (align-1)              ← 预留对齐填充
     沿 free 链 first-fit 找一块 free 且 size >= needed 的
     找到 curr(地址 curr_addr):
        block_end       = curr_addr + 32 + curr.size
        aligned_payload = align_up(curr_addr + 32, align)   ← 把 payload 摆到对齐处
        hdr_addr        = aligned_payload - 32              ← ★ 头紧贴 payload 之前
        usable          = block_end - aligned_payload       (须 >= size)
        front_pad       = hdr_addr - curr_addr              ← 块首到新头之间的缝
        tail_space      = block_end - (aligned_payload + size)
        从链上摘掉 curr
        若 front_pad >= 48(MIN_SPLIT): 把缝回收成小块塞回链; 否则: 这点缝丢了(内部碎片)
        若 tail_space >= 48: 把尾部 remainder 切成新 free 块塞回链
        在 hdr_addr 写 in-use 头, payload 清零, 返回 aligned_payload
     没找到 → expand() 向 VMM 续页, 递归重试 alloc
```

那个 `hdr_addr = aligned_payload - 32` 是这一章最容易写错、也最该讲透的地方——调试现场专门拎出来。
