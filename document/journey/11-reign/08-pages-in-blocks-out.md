---
title: 08 · 页进,块出
description: "heap_runtime 是懂页的那半边:保护页之上铺下 64KiB 开张,不够吃就经共享走查按页扩容、多带一页防头部挤兑,再 grow 重试。全局 operator new 四件薄转发进堆,失败走 Check 急停——内核里分配失败没有体面的恢复路。面板添了 heap 行。"
chapter: 11
order: 8
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - memory
  - heap
---

# 页进,块出

铺子的算术上一节验瓷实了,本节咱们进货。进货的活住在 heap_runtime 这一对文件里,它是堆两半里懂页的那半边。开张的头一步 BringUpHeap,咱们把它整个摆出来:

```cpp
bool BringUpHeap() {
    unsigned long const kBase = kHeapBase + kHeapLowerGuardBytes;
    if (!map_heap_pages(kBase, kHeapInitialBytes)) {
        return false;
    }
    return Heap::self().init(kBase, kHeapInitialBytes);
}
```

咱们拢共写了三行,可每一行都有它的讲究。头一行定店址:从堆窗口的基址往上让出一页再开工。让出来的那页咱们特意不映射,它的名字叫保护页,kHeapLowerGuardBytes 说的就是它那 4KiB,拦在第一块的前面。堆里的代码要是越界往下多写一步,踩进的就是这一页没映射的地界,缺页当场就炸了。炸的位置在自家门口,比炸到邻居家里的动静好查一百倍。第二行铺的是底货:经共享走查把 64KiB 映射进堆窗口,页从 Pmm 领——您看,接管那阵子的活,其实一次走查都没用上,如今连堆的进货都走它,当初把走查写成纯函数花的钱,就是在这里生息的。第三行把这段地界交给算术半边,铺子就挂了牌。64KiB 的底货是配置里的常量,写在了 heap_config.hpp,往后咱们要调也只去那儿调。

日常的进销存都在 HeapAllocate 里,咱们给它立的章程是“试分在前,不够就进货,进完重试”这么三条:

```cpp
void* HeapAllocate(unsigned long bytes) {
    if (void* const kBlock = Heap::self().allocate(bytes)) {
        return kBlock;
    }
    unsigned long const kPages  = cinux::base::math::Ceil(bytes, kSize) + 1;
    unsigned long const kGrow   = kPages * kSize;
    unsigned long const kOldTop = Heap::self().top();
    if (kOldTop == 0 || !map_heap_pages(kOldTop, kGrow) || !Heap::self().grow(kOldTop + kGrow)) {
        return nullptr;
    }
    return Heap::self().allocate(bytes);
}
```

咱们看 +1 的余量,整段里最藏心思的就是它:多带的就是一页,源码里就一个字符的事。一笔 100KiB 的请求,按页取整的答数是 25 页,进货偏偏进 26 页。多出来的一页不是浪费:块是有头有尾的,铺子把新头立在了顶上。更要紧的是,“整页装得下、加个头就装不下”的坎上要是正好卡着一个请求,咱们不多带一页,进了货还是分不出,结果还得再进一回。“头部挤兑”一类的尴尬,一页的余量就能兜住,顶多那页在下次合并时归了仓。注意这里 Ceil 的用法跟上一节那个 bug,恰好是一对教材:这里算的其实是页数,也就是单位的个数,咱们乘回页宽才得到字节数,一处轮子的两处用法,语义认对了它就听话。

铺子开张的真正意义在最后一块:接管 new 和 delete。全局的 operator new、new[]、delete、delete[](尺寸版也算上)四件薄转发,全数通到了 HeapAllocate 和 HeapFree 上。这一接管改写了两种日子。咱们从头一种说起,那是打内核出生那一卷起的日子:new 没有实现,链接期就把咱们拦下,咱们写链表得手搓节点数组,容器的事想都别想。如今开机路径里的 new 直接可用,后面的进程对象、调度队列,底座都是本节铺的。四件里的失败路径咱们得单独交代,因为它跟用户态的做法是反的。用户态的 new 失败会抛异常,咱们内核里没有这回事:没有栈可以展开,没有接得住的处理程序,分配失败意味着机器的下一步没有着落。所以咱们的章程是急停:HeapAllocate 交了空指针,operator new 里的 Check 当场把机器停住,遗言里写明了 kernel heap exhausted。而死在病发的地方,好过带着烂指针一路跑到认不出病因的地方。delete 那头对应地松一格:delete nullptr 是合法的 C++,放行也是静默的。真还回来的指针,要是 HeapFree 报了拒(双重释放或者野指针),咱们照样急停,停下的理由同上。HeapFree 脸上留着可查询的判定,是因为测试要的是报真话而不是处刑,处刑的活归 operator delete。

面板那头咱们也惦记着,堆亮出了自己的一行,咱们抄在下面:

```text
[kern] heap: new/delete live (probe 114514)
```

这行字不是摆设:开机的路径里真有一个 new unsigned long,写进去的是 0x114514,再把值打了出来。咱们每天开机,new 都实打实地跑一遍,114514 就是它交的作业。

机器里的验收是 ktest 的 heap 一台三案:new[] 往返、100KiB 大分配强制扩容且首尾可写,free 出的洞复用在前而顶不涨。头一案的模样平平无奇,后两案里藏着本卷最亮的一段公案——那个 100KiB 的扩容案,头一版根本测了个寂寞:断言明明写着,可它断言的世界压根没发生过。怎么回事,下一节咱们把汇编摆出来看。
