---
title: "09 · 一张跟着任务走的文件册"
description: "描述符是客人手里的号牌,内核的册子每任务一本,按值住在 Task 里。0、1、2 是固定座,柜台那层就把它们接走,真发号从 3 起,一个任务至多五份文件。每格记着后端、节点与念到哪了——文件按字节,目录按条目。领号在关中断下占引用,任务退场析构链挨格放还,机内测试对上了 retain 与 release 各两笔。"
chapter: 14
order: 9
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - syscall
  - filesystem
---

# 一张跟着任务走的文件册

open 回给客人的那个数字,是号牌。号牌得有人核对,内核手里的册子,每个任务一本:Task 身上按值揣着一张文件册,任务生它就生,任务走它跟着析构。您拿着号来办业务,内核翻的就是您那本册子。

册子有个古怪的起点:头三个号不归它管。咱们把三个固定座认一认:0 是键盘,1 是控制台。2 这一号,上一卷的柜台还不认,write 只收 1,这一卷把它也认了下来,与 1 同座,都通控制台。标准流的接活,柜台那一层就办完了,册子从头到尾没见过它们。真正的发号从 3 起:`kFirstDescriptor = 3`。这是 Unix 世界人人认的老惯例,头一个真文件的号就是 3,哪天真 C 库进门的卷里,人家程序从小背熟了的号码表,跟咱们立的对得上。册子的宽度是 8,头三格永远给标准流留着,真正发给文件的是五个号——一个任务同时至多五份文件开着。教学内核的诚实预算,紧是紧了点,唱这一章的戏绰绰有余。

每一格记什么?咱们看结构:

```cpp
struct FileSlot {
    const FsOps*  ops     = nullptr;  ///< The backend's erased face.
    void*         backend = nullptr;  ///< The backend instance.
    FsNode        node{};             ///< Which node of that backend.
    unsigned long offset = 0;         ///< Bytes read so far, or the directory entry ordinal.
};
```

咱们看门面 resolve 递回来的那一对:函数表加后端指针,原样存着,业务来一笔就转一笔。node 是开的是哪个节点。offset 是念到哪了,文件身上它是字节偏移,目录身上它是条目的序号,同一格,两种刻度,下一章 getdents 吃的就是这口。同一个 fd,读文件念的是第几字节,读目录念的是第几条,册子不用分家。

领号的那一下,咱们把引用和并发一起办了:

```cpp
[[nodiscard]] base::Result<unsigned long> alloc(const FsOps* ops, void* backend, FsNode node) {
    const arch::IrqGuard kGuard;
    const unsigned long  kFd = slots_.insert(
        FileSlot{.ops = ops, .backend = backend, .node = node, .offset = 0}, kFirstDescriptor);
    if (kFd == kFileTableMax) {
        return base::KernelError::kOutOfMemory;
    }
    if (ops != nullptr) {
        ops->retain(backend, node);
    }
    return kFd;
}
```

关中断的卫兵罩着全程:查表、插格、占引用,一气呵成,中间插不进一记时钟。发号发的是最低的空格,关掉的号下一回接着用——号是稀缺品,咱们不兴跳着发。表真满了,回 `kOutOfMemory`,而且一份引用都不占:占不到格子的业务,不许在册子外头留孤儿。retain 占住的,就是第四章 ramfs 里那个数数的小家伙——名字哪天没了,占下的引用还替开着的文件撑着命。

任务退场,册子跟着析构,析构的路线是把每一格挨个 free:擦格子、放引用,一件不少。第四章那桩命案,在这一章收尾——开着 fd 的已删文件,任务活着时靠描述符的引用撑着,任务一走,引用还干净了,ramfs 那边归零的节点才敢消失。链路真的通不通,机内测试专门立了一案:起一个任务,开两个文件,一个不关就地退场,咱们数一数 retain 与 release 各自的次数——各两笔,分毫不差。收尸的路,是通的。

close 那头也有讲究:客人递来 0、1、2,回 0——关一个标准流不算错,它压根没从册子里开过什么,客客气气回个 0,各自安好。递来 8 往上的,回 `-EBADF`,册子就八格,再大的号查无此格。中间的走 free。号牌、册子、引用,三样都齐了。可目录也是文件,念起来什么动静?下一章,咱们念目录。
