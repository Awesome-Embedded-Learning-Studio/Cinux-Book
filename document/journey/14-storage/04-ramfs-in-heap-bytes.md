---
title: "04 · 长在内存里的盘"
description: "头一个后端把字节种在堆里:RamNode 是树的半边加 unique_ptr 的字节缓冲,引用计数管生死,孩子边与开着的描述符各占一份。一把自旋锁守全部操作,扩容按页粒度整块搬,空洞补零。unlink 摘边不销毁,开着 fd 的已删文件能读到最后一放手,根目录生来领 1 号,发号单调递增。"
chapter: 14
order: 4
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - filesystem
---

# 长在内存里的盘

树有了,该有肉。头一个后端,咱们叫它 ramfs:整座盘就是一堆堆上的字节,断电就没了,可机器本来也不打算让它活过一次开机。名字的世界要的,是个能读写、能建删的架子,它全都有。

后端的节点,是树的半边加货的半边,咱们把它摆出来:

```cpp
struct RamNode : DirNode<RamNode> {
    std::unique_ptr<unsigned char[]> data;
    unsigned long                    capacity = 0;
    unsigned long                    size     = 0;
    base::RefCount<>                 references;

    void release_ref() {
        if (references.release()) {
            delete this;
        }
    }
};
```

`DirNode<RamNode>` 把树那一半接了上来,字节装在 `unique_ptr` 里,缓冲没了,节点也就到头了。这里头那个小小的 `RefCount` 值得咱们停一拍:它就是个数数的,`retain` 加一,`release` 减一,减完回报一句“归零了没有”。判断给调用者,动手删的也是调用者——上面那四行 `release_ref` 就是动手的地方。这个数数的小家伙,撑起了本卷最讲究的一条命:目录的孩子边占一份引用,开着的描述符也占一份。名字从树上摘掉,摘掉的只是孩子边的引用。只要还有描述符开着它,节点就活着,字节照读,最后一放手,节点就地消失。已删还能读,听上去不讲理,可您想想三个月后排查问题的您自己:进程还开着这个文件呢,读到一半它没了,这罪可怎么查。

身份的发放,咱们也立了定规:根目录一出生就领 1 号,后到的节点从 2 起一个一个往上发,发号的单调递增,回头绝不重发。这个号眼下还没有谁来查它,册子上的记录照留,格式留给将来。

并发这块,咱们给整座后端配了一把锁:一把自旋锁,每个操作从头持到尾。您要是皱眉了——对,粒度粗得很,一座盘一把锁,说出去不好意思。可咱们把道理摊开:这是单核的教学内核,元数据的临界区都短得可怜,自旋锁进门关中断、出门还原的脾气,进程那一卷就交代过了,短活计交给它正合适。哪天核多了、操作长了,再谈把锁变细,今天不做明天的烦恼。

文件的字节怎么长?咱们看写的路子:

```cpp
const unsigned long kNeeded = offset + count;
if (kNeeded > kFile->capacity) {
    const unsigned long kFresh =
        base::math::Ceil(kNeeded, kRamfsGrowthAlign) * kRamfsGrowthAlign;
    auto* const kGrown = new unsigned char[kFresh]{};
    if (kFile->size > 0) {
        base::CopyBytes(kGrown, kFile->data.get(), kFile->size);
    }
    kFile->data.reset(kGrown);
    kFile->capacity = kFresh;
}
```

装不下了,按 4096 的粒度整块换新:需要多少,向上凑整到整页数,新缓冲开出来是零填的,老内容抄过去,旧的一手交出去。粒度取一页的道理,咱们算一算就明白:一个一 MB 的文件,您一个字节一个字节往上写,按页长的话它一共搬二百五十六次家。要是来一次写就搬一次,同样的活它得搬一百万次。搬家的次数省了,新开的缓冲还是零填的,所以 offset 故意跳到文件末尾外头再写,中间那段洞读回来也是零,绝不是堆上的陈年老字节。读的那头是老实的:offset 过了文件尾,回一个 0,没得读。没过尾呢,想读多少给多少,尾巴夹到文件的真实长度为止。

删名字的路子,有一处咱们得演给您看。`unlink` 顺着父路摸到目录,把叶子从链上摘下来,摘下来之后要查一遍:这是个目录、屋里还有孩子?那这笔删除不办,回一声 `kDirectoryNotEmpty`。有意思的是那个不办的姿势——节点已经摘下来了,咱们把它塞回链上,塞的位置是链头。您多想一层就乐了:头插的世界里,新来的排最前,被塞回去的老目录,名次反倒往前挪了。原本就居链头的呢,摘了再插,名次原地不动。它不算冤:清单的次序从没承诺过什么,ls 也没有说谎,这笔咱们如实记下。

还有一手咱们得交代:堆尽的脾气。内核的 `new` 在堆见底的时候是大声终止的,不返空——所以创建文件、建目录的活儿,压根没有静默失败可走,创建要么成,要么整台机器响着停。后端就这样了,可客人不认识它,柜台也还没接它的活。下一章,给整层楼装一座大门面。
