---
title: 04 · 调试现场与收尾
---

# 调试现场与收尾

## 调试现场

这一章和 016 一样没有 notes 文件,但堆分配器有几个「写错就极难查」的隐患,值得当调试现场讲。

**一是 front padding 没摆正——头不在 payload 前 32 字节处。** 这是这一章最深的坑,测试里 [test_heap.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_heap.cpp) 专门标了 `CRITICAL: alloc with front padding` 的用例来盯它(kernel 侧那条 `test_odd_sizes` 的注释更直白,管它叫「the alignment padding bug」)。症状是这样的:如果你实现 alloc 时图省事,把块头固定摆在块首 `curr_addr`,payload 摆到中间某个对齐位置,却没有保证「payload - 32 正好是头」——那么 `free(p)` 调 `header_from_ptr` 算出来的头地址,落到了 payload 和块首之间的填充区,读出来的 `magic` 是随机字节。于是你会看到一个稳定的 `[HEAP] Double-free or corruption at ... (magic=0x...)`——明明只 free 了一次,却报「损坏」。更阴的是,如果那段填充区恰好某些字节拼出来等于 `0xDEADBEEF`(概率低但非零),magic 校验放过去了,`free` 就会改写一个错误位置的块头,静默踩坏邻居,等症状爆发时离根因十万八千里。根因就一句:**payload 的头必须写在 `aligned_payload - HEADER_SIZE`,`header_from_ptr` 才能找回来**。测试里先 `alloc(64)` 占点位置、再 `alloc(64, 4096)`(强对齐,逼出 front padding),然后 free 它、再验 `used_` 归零——这条链路要是断了,十有八九就是头摆错了。

**二是 magic 能抓什么、抓不了什么,要心里有数。** magic 是个「校验哨兵」,不是 fence(篱笆)。它能抓:free 一个非堆指针、free 一个头被改写的块、双释放(靠 `free` 标志,第二道)。它**抓不了**:你往自己分到的 buffer 末尾多写了几个字节,踩坏了**下一块**的头——这种越界,只有等那块下一块被 `free`(或被 alloc 扫到)时,magic 校验才会引爆,中间有一段延迟,而且报错位置(`[HEAP] ... corruption at <下一块的指针>`)离真正越界的地方(`上一块的末尾`)隔了一个块,容易误导排查。工业级分配器会在每块末尾再放一个「canary」/fence 字节,写越界会立刻破坏 canary、free 时立刻发现;这套实现没做,所以越界抓得晚。知道这个边界,调试时就不会一头扎进「为什么报错的块明明没动过」。

**三是 `used_` 和 `dump_stats` 不计 front-pad 碎片,账目对不齐别慌。** `used_` 只累加 `HEADER_SIZE + size`,不含被丢弃的 front padding;`dump_stats` 的 `free` 统计只遍历 free 链上的块,那些 `< 48` 字节的死缝也不在链上。所以 `used_ + free_total + 头开销` 未必等于总长,差出来的就是内部碎片。host 测试里有一条 `size accounting invariant`,断言用的是 `free_total + block_count * HEADER_SIZE <= total` 且 `free_total > total * 90%`——留了 10% 的碎片 slack,正是为了容纳这种 front-pad 损耗。你要是看见账目差了几十字节,先别怀疑实现错了,想想是不是奇数大小的连续分配攒下的死缝。

**四是 coalesce 的 O(n²) 在块特别多时会慢,但不会错。** 前面讲过,合并靠全链扫描 + 多趟。它的正确性没问题(地址相邻判断是精确的),但如果你某天发现内核某个分配密集的路径变慢,而 profile 指向 `coalesce`,不用怀疑逻辑——就是这个全扫描的设计到上限了。这时候该想的是「换数据结构(边界标签/按地址排序)」,而不是「调试合并逻辑」。这是已知取舍,不是 bug。

## 验证

堆的核心逻辑(first-fit、对齐、分裂、合并、双释放、账目)绝大部分能在 host 上镜像测。[test_heap.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_heap.cpp) 把堆算法用一块 host `calloc` 出来的缓冲(代替 VMM 映射的页)抄了一份,mock 掉 PMM/VMM,`-O2` 编、`CINUX_HOST_TEST` 门控,覆盖:默认 / 自定义对齐(16、64、4096)、分裂后 remainder 合法、free 降低 used、相邻块合并(正序/逆序/三块各种顺序)、双释放不腐败 used、magic 校验、200 轮交错 + 100 块填满再排空的应力、4096 对齐下的 front padding(header_from_ptr 仍能找回头)、耗尽后返回 nullptr、多块不重叠、账目不变量。

跑它们:

```bash
ctest --test-dir build -R heap --output-on-failure
```

这里要诚实标注一条边界:host 镜像用**固定**缓冲,**不测 `expand`**(它没法 mock 出 VMM 续页),耗尽就直接返回 nullptr。所以「自动扩容」这条路径,host 单测是验不到的——它的正确性靠代码审查和下游实测。

「真 VMM、真 PMM、真 new/delete」则在 QEMU 里验。[test_heap.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_heap.cpp) 在机内跑真正的 `g_heap`,9 个场景:基础分配 + 读写回测、默认 / 4096 对齐、`alloc(0)`→nullptr、`free(nullptr)` 安全、多块互不重叠、free 三小块后再分配大块(coalesce + 复用)、奇数大小(37/23/41)仍对齐、50 块应力(隔一块 free 一块、再回填、验存活块的标记)、`dump_stats` 不崩:

```bash
cmake --build build --target run-big-kernel-test
```

`g_heap.init` 会打一行 `[HEAP] Initialised at 0x..., size 64 KB`,看到它就说明堆建起来了;test section `Heap Tests (017)` 全过、末尾 `ALL TESTS PASSED`,说明 alloc/free/对齐/合并/双释放这一套在真硬件语义下成立。配合 host 单测焊死算法、机内测跑真后端,两层缺一不可——尤其对齐那一段,host 和机内都盯得很紧。

## 下一站

内存子系统的三块拼图现在齐了:PMM 管物理页的分配,VMM 管虚拟↔物理的映射(还顺手给了 demand paging),堆在两者之上做细粒度的切块回收,还接管了 `new`/`delete`。内核从「只能整页拿内存」变成了「像用户态一样按需 new」。

但你会注意到一个还没收口的细节:这一章堆的基址是硬编码的 `0xFFFF800000000000`——我们只是「把堆映射到 high-half 起点」,内核并没有一个「地址空间」的概念:哪段虚拟地址归内核、哪段留给将来用户态进程、堆区和别的区域会不会撞、每个进程要不要有自己的视图……这些问题,堆本身回答不了,它只是个「在一块已映射内存上切蛋糕」的工具。

下一站就是把这些收口:把「地址空间」正式抽象出来,让内核有一套统一的区域划分与映射管理。那是 018 的事——我们先享受一下「内核能 `new` 了」这个里程碑,地址空间下一章再见。

---

### 参考

- cppreference — [C++ `operator new` / `std::align_val_t`](https://en.cppreference.com/w/cpp/memory/new/operator_new):C++17 引入的带对齐 `operator new(size, std::align_val_t)` 重载语义,支持 `crt_stub.cpp` 里那组对齐版重定向、以及 `alloc` 第二参数 `align` 的来历。
- 015 章 · [给物理内存建账本:bitmap PMM](../001/):堆的 `init` / `expand` 每页都靠 `g_pmm.alloc_page` 提供物理页,三块基石的第一块。
- 016 章 · [把物理页挂进虚拟地址:VMM](../002/):堆的页靠 `g_vmm.map` 挂到虚拟地址;`0xFFFF800000000000` 这个 high-half 基址的由来、`phys_to_virt` 的自举约定,都在这一章打过底。
- 本 tag 源码:[heap.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/heap.hpp) / [heap.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/heap.cpp)、[crt_stub.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/crt_stub.cpp)(`operator new`/`delete` 重定向)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(Step 9 `g_heap.init`,生产基址 `0xFFFF800000000000`);测试 [test_heap.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_heap.cpp)(host 镜像,不含 expand)、[test_heap.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_heap.cpp)(QEMU 真 `g_heap`)、[main_test.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/main_test.cpp)(测试 harness 基址 `0xFFFFFFFF80100000`,与生产不同)。
