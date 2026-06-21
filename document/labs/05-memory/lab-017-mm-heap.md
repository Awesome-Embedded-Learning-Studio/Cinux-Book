---
title: Lab 017 · 在页上切块:内核堆分配器
---

# Lab 017 · 在页上切块:内核堆分配器

> 配套章节:[017 · 在页上切块:内核堆分配器](../../book/05-memory/017-mm-heap.md)。这一关给你目标和约束,不贴 first-fit 的实现,也不贴对齐 / 合并 / 扩容的算法——那些得你自己写。

## 实验目标

让内核能按**任意大小**动态分配内存,而不是只会整页整页地拿。拆成几个能独立验证的子目标:

1. 能 init:从 VMM 借一串页,把它变成一个有一整块空闲区的堆。
2. 能 alloc / free:first-fit 切块、回收,带 16 字节默认对齐,且 `alloc(0)` 返回 nullptr、`free(nullptr)` 安全。
3. 能分裂 / 合并:分配大块时把尾部多余的切成新空闲块;释放时和地址相邻的空闲块合并,避免碎片。
4. 能按调用者要求对齐:支持 4096 字节这类大对齐,且对齐后的块头仍能被 `free` 找回。
5. 能扩容:空闲链放不下时,自动向 VMM 续页、重试。
6. 让 `new` / `delete` 落到堆上:接管全局 `operator new` / `delete`(含对齐变体)。

做完这几条,内核就有了自己的 `malloc` 后端,后面动态数组、链表、缓存都有了落脚处。

## 前置条件

你得先过 Lab 015 + Lab 016:PMM 的 `alloc_page` / `free_page` 可用(VMM 续页、demand paging 都靠它),VMM 的 `map(virt, phys, flags)` 可用(堆的每一页都要靠它挂到虚拟地址)。这一关所有「要一页」的需求都转成 `g_pmm.alloc_page()` + `g_vmm.map(...)`。

另外要接受一个前提:内核是 `-ffreestanding -nostdlib` 编译的,标准库的 `operator new` / `delete` 不存在,得自己在 crt 桩里提供——这一关的第 6 个子目标就是补上它们。

## 任务分解

**第一步:定块头。** 给每个块设计一个固定大小的头,字段至少包含:一个校验用的 magic(随便选个固定值)、payload 大小(不含头本身)、是否空闲的标志、空闲链的 next 指针。把头的总长凑成一个 2 的幂(为什么?让默认对齐天然成立,且 next 指针位置稳定),用 `static_assert` 焊死。想清楚「块的物理总长 = 头 + payload」这个口径,后面所有计算都按它来。

**第二步:init。** 入参是虚拟基址和初始大小。先把大小页对齐;然后逐页 `alloc_page` + `map`(每页都要检查物理页是否拿到手,拿不到要打日志并安全返回);整块**清零**;在区首摆一个覆盖全段的空闲块(magic、size = 整段 − 头、free=1、next=null);记下基址、总长、used=0、free 链头。最后打一行 init 日志。

**第三步:alloc(first-fit + 分裂)。** 拒绝 `size==0`;最低 16 字节对齐;按「请求大小 + (对齐−1)」预算余量。沿空闲链找**第一个**放得下的空闲块,找不到就走第六步。找到后,先算对齐后的 payload 地址、再算头地址(头紧贴 payload 之前)、算「对齐后真正可用空间」够不够;然后处理两段缝——块首到新头之间的「前填充」若够大(至少能放下头 + 16 字节)就回收成小块塞回链、否则丢弃;尾部多余的切成新空闲块塞回链。最后在新头位置写 in-use 头、payload 清零、used 加、返回 payload 指针。这一步最容易写错的就是「头到底摆哪」——关键约定见接口约束。

**第四步:free(校验 + 合并)。** `nullptr` 直接返回。把 payload 指针往回数一个头的大小还原块头;核对 magic(不对就是野指针或头被踩坏,打日志返回);核对 free 标志(已空闲就是双释放,打日志返回);标 free、used 减、塞回链头;调用合并。合并按**地址相邻**判断(某块的尾贴着另一块的头就并),注意空闲链**不按地址排**,所以找邻居得遍历整条链,而且合并可能级联,要反复扫到没有新合并为止。

**第五步:扩容。** 入参是「至少还要多少字节」。算出页数(至少续若干页,别一次只续一页),在当前堆末尾续映新页、清零、摆一个覆盖新区的空闲块塞回链、总长增长。注意它和 init 高度同构,只是从「末尾续」而不是「从头建」。

**第六步:接管 new/delete。** 在 freestanding 桩文件里提供全局 `operator new` / `delete` 的全部常用重定向:单对象 / 数组、普通版 / 带 `std::align_val_t` 的对齐版、`delete` 的无参 / 带尺寸 / 带对齐变体——全部落到堆的 alloc / free(delete 用不上尺寸和对齐,忽略即可)。对齐版的 new 要把对齐值透传给 alloc 的对齐参数。

## 接口约束

你要实现出来的东西,对外长这样(职责和签名,不给算法实现):

- `BlockHeader`(结构体,固定大小):magic / size(不含头)/ free / next。用 `static_assert` 锁大小。
- `Heap::init(uint64_t virt_base, uint64_t initial_size)`:借页、清零、摆初始空闲块。
- `Heap::alloc(size_t size, size_t align = 16) -> void*`:first-fit,带对齐和分裂;`size==0` 返回 nullptr;链空则扩容后重试。
- `Heap::free(void* ptr)`:nullptr 安全;magic + 双释放校验;回链 + 合并。
- `Heap::expand(size_t min_bytes)`:向 VMM 续页、摆新空闲块。
- `Heap::dump_stats() const`:打印 total / used / free / 块数。
- 全局 `operator new` / `delete`(含 `std::align_val_t` 对齐变体)→ 堆。
- `extern Heap g_heap;`:全局堆实例,`main.cpp` 里 init。

最关键的约束(违反就出 bug):**任意 payload 指针 `p`,`p − 头大小` 处必须是它自己的块头**。`free` 就靠这条把指针还原成头、读出大小。所以 alloc 写头时,头必须摆在「对齐后 payload − 头大小」的位置,绝不能图省事固定摆在块首。

magic、头大小、最小可分裂块阈值这些常量你自己定,这关不提供。

## 验证步骤

纯逻辑(first-fit、对齐、分裂、合并、双释放、账目)在 host 上镜像着测。把堆算法用一块 host `malloc` 出来的缓冲抄一份,mock 掉 PMM/VMM,`-O2` 编、`CINUX_HOST_TEST` 门控:

```bash
ctest --test-dir build -R heap --output-on-failure
```

建议覆盖:默认 / 自定义(64、4096)对齐、分裂后仍有空闲块、free 降低 used、相邻块合并(正序 / 逆序 / 三块各种顺序)、双释放不腐败 used、magic 校验、多轮交错 + 填满排空的应力、**大对齐下的前填充(header_from_ptr 仍能找回头)**、耗尽返回 nullptr、多块不重叠、账目不变量(`free_total + 块数×头大小 <= total`,并留点碎片 slack)。

注意:host 镜像用**固定**缓冲,**没法测扩容**(mock 不了 VMM 续页),耗尽就返回 nullptr。扩容这条路径留给机内测。

真 PMM、真 VMM、真 new/delete,在 QEMU 里验。机内测跑真正的全局堆:基础分配 + 读写回测、默认 / 4096 对齐、`alloc(0)`→nullptr、`free(nullptr)` 安全、多块不重叠、free 三小块后再分配大块(合并 + 复用)、奇数大小仍对齐、若干块应力(隔一块 free 一块、回填、验存活标记)、`dump_stats` 不崩:

```bash
cmake --build build --target run-big-kernel-test
```

init 会打印 `[HEAP] Initialised at 0x..., size ... KB`,test section 全过、末尾 `ALL TESTS PASSED`,就说明堆走通了。

## 常见故障

- **明明只 free 一次,却稳定报 `Double-free or corruption (magic=0x...)`**:块头没摆在「对齐后 payload − 头大小」处,`free` 用 `p − 头大小` 还原头时读到了填充区的随机字节。把头写回 `aligned_payload − HEADER_SIZE` 的位置。
- **越界写了自己 buffer 末尾,当时没事,过一会儿 free / alloc 别的块才报损坏**:magic 是哨兵不是 fence,踩坏的是**下一块**的头,要等那块被 free / 扫到才引爆。报错位置离真正越界处隔了一个块,排查时往「上一块的末尾」看。
- **账目 `used + free` 对不上总长,差了几十字节**:奇数大小连续分配攒下的前填充死缝(`< 最小可分裂块`),既不在空闲链里、也不计入 used,是内部碎片。属于设计内的损耗,不是 bug;账目断言要留碎片 slack。
- **合并没生效,free 后再分配大块失败**:合并按「地址相邻」判断,但你的空闲链可能漏了某种相邻方向,或没套「反复扫到稳定」的外层循环(合并可能级联)。检查两个方向都判、且多趟扫描。
- **双释放把同一块塞进链两次,后续分配返回了同一块给两个人**:free 时没检查 `free` 标志就回链。先核对 magic,再核对 free 标志,任一不过就打日志返回、不回链。
- **对齐版 `new (std::align_val_t(N)) T` 给出的指针不满足 N 对齐**:alloc 没把第二个参数 `align` 真的用来上对齐 payload,或对齐预算余量算少了。alloc 里要 `align_up(payload 候选, align)`,且预算按 `size + (align−1)`。
- **扩容写了但「测试过了」其实没被覆盖到**:host 单测用固定缓冲测不到扩容,机内测试分配量也可能落在初始区内。扩容路径靠代码审查 + 真正压满时验证,别误以为已被实测。

## 通过标准

1. host 单测全绿:各种对齐、分裂、合并(多顺序)、双释放、magic、应力、前填充下 header_from_ptr 正确、耗尽→nullptr、账目不变量。
2. QEMU 机内测通过:init 打出 `[HEAP] Initialised`、各场景全过、`ALL TESTS PASSED`。
3. 任意 payload 指针 `p − 头大小` 处必是其块头;alloc 写头落在「对齐 payload − 头大小」——这条铁律守住,对齐和 free 才都成立。
4. free 走 magic + 双释放双校验;合并按地址相邻、多趟扫描;扩容向 VMM 续页。
5. 全局 `operator new` / `delete`(含对齐变体)落到堆上,内核 C++ 代码能直接 `new` / `delete`。

做到这五条,内核就有了细粒度动态分配的能力,PMM + VMM + 堆三块基石就位。但堆基址还是硬编码在 high-half 某地址,内核没有「地址空间」的抽象——那是下一关 018 的事。
