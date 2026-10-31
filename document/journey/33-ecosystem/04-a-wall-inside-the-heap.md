---
title: "04 · 堆里砌护墙的人"
description: "KVM 放行的门刚开,头一场开机冒烟就出案子:musl 静态的 busybox 开机连环段错误,十一号一发接一发,ash 崩了被拉起,拉起来又崩。老客人从开门迎客那一卷就在,静态动态两路都跑得,凭什么新地基一立,倒的偏偏是它。办案三步走:新近的改动一半一半摘出去开机试,定罪 mmap 的重写。断点落在缺页裁决者的处决点,抓到用户态读一页不存在的页,地址 0x1309000。再断 mmap 门口记参,开机头一笔就是 mmap(0xc49000, 0x1000, PROT_NONE, MAP_FIXED|MAP_PRIVATE|MAP_ANON),musl 的分配器 mallocng 往 brk 堆内部砌护墙的招数。真因:注销见护页跨堆,把堆尾砍到跨度起点,堆尾巴整段失管,musl 手里的指针悬空。当年主线拒 MAP_FIXED 回 EINVAL,musl 静默放弃护页,一切照旧,老语义错得安静,新语义错得响亮。修法:堆中打洞,洞以上的尾巴升格成独立的匿名段入链,堆本体收缩到洞口。长凳添打洞案,VMA 二十三案全过,机内 ktest 二十一案全过,musl 全链路 ash 与动态 hello 复绿。教训入册:MAP_FIXED 的每一个新消费者都拿真实调用序列考替换语义,占址重建得把单独立户的堆一起想全。悬案还开着一桩,glibc 的 hello 起动即死,下一案更响。"
chapter: 33
order: 4
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - memory
  - debugging
  - ecosystem
---

# 堆里砌护墙的人

KVM 一放行,头一场开机冒烟就给咱们出了桩案子。musl 静态的 busybox,开机就是连环的段错误,十一号,一发接一发,ash 崩了被拉起,拉起来又崩。

> 顺带记一句环境摩擦:KVM 的设备组被系统记岔了名字,机内的实弹压了好几天没跑成,咱们只能干等。理顺的那天,头一场冒烟就把这桩案子端了出来。

musl 是咱们的老客人。开门迎客那一卷咱们把它迎进门,静态的身子跑了那么久,动态的身子也在把后半场交给解释器那一卷跑通了,凭什么地基一翻新,倒的偏偏是它?

办案头一步是圈人。新近的改动不少,咱们用笨办法,土话叫二分,一半一半往外摘,摘出去开机试。摘到 mmap 的重写,机器就安生了。装回去,连环的段错误跟着回来。罪,定在 mmap 的重写头上。

定了罪,离破案还远。mmap 的重写长长一截,病根埋在哪一句,得让死者自己开口。咱们把断点请出来,落在一个要紧的位置,缺页裁决者的处决点。听得懂喊声那一卷立的章程,用户态来的缺页,裁决者不受理的,惹祸的任务以十一号的身份自己退场,resolve_current_fault 就是裁决者本尊。咱们断在它把信号号装进寄存器、要送当前任务退场的那一句前头,停住的那一拍,案情就躺在寄存器里:用户态,读,一页不存在的页,出错地址 0x1309000。

分配器死在一页不存在的页上,可它的营生是发页,页怎么会丢?咱们把断点挪到 mmap 的门口接着记,门面就这一扇,名字叫 HandleMmap,来一笔记一笔。开机记下的头一笔,就是它:

```text
mmap(0xc49000, 0x1000, PROT_NONE, MAP_FIXED|MAP_PRIVATE|MAP_ANON)
```

这笔单子咱们认得。musl 这一代的分配器叫 mallocng,它有砌墙的习惯,拿 mmap 要一页 PROT_NONE 的映射,读、写、执行一样都不给,砌在自己地盘的敏感边上。您要是还记得栈底的守卫页,这堵墙就是同一个主意,越界的读写一碰它,当场段错误,病留在墙上,不往邻居家蔓延。它的要求挨个读下来,地址 0xc49000,页对齐,长度一页,权限 PROT_NONE,旗子里明晃晃一面 MAP_FIXED,请的是上一章刚立的替换语义,占址重建。要紧的是那个地址,0xc49000 落在 brk 堆的跨度里。老客人递上来的考题是,在堆的身上,占址重建。砌墙的人,说的就是它。

替换语义接了单。拆旧的活顺着上一章开出来的口子,落到登记册的注销上,干活的函数叫 unregister_range。这里咱们得补一句户口的事。懒到家那一卷立册的时候,寻常的段在链上各占一节,唯独 brk 堆单独立户,heap_ 的起止两个字段就是它的全部身份,不进链。注销头一眼看见护页的跨度与堆重叠,好家伙,下的是最粗的一斧,把堆的末尾砍到跨度起点。0xc49000 之上,整段堆尾巴从册上销了户。musl 手里还捏着尾巴里的指针,悬空了,一读,读在一页不存在的页上,正是 0x1309000,裁决者翻册,这段地址册上已经没有,十一号,退场。真因落定:护页跨堆,注销砍尾,堆尾巴整段失管。

案子破了,还有当年的一个对照值得记进案卷。当年第一遍,笔者的主线对 MAP_FIXED 是拒的,一律回 EINVAL。musl 拿到 EINVAL,不吵不闹,静默放弃了这堵墙,堆里少一道当场翻车的保险,日子照过,谁也没察觉。老语义错得安静,新语义这一斧,错得响亮。响亮不算坏事,老语义底下墙从来没砌成,病也从来没机会露头。musl 的静默放弃,咱们写进案卷,当反面教员。

修法咱们照着病理下刀。堆里要打洞的时候,洞以上的尾巴不能再跟着洞一起消失,它升格成一段独立的匿名段,自己进链,堆本体收缩到洞口,护页那页在洞里,照拆。注销进门管堆的那几行,如今是这样:

```cpp
    if (!heap_recorded_ && heap_.start < heap_.end && overlaps_span(heap_, start, end)) {
        if (end < heap_.end) {
            auto* const kTail = new VmaRegion{};
            if (kTail == nullptr) {
                return base::KernelError::kOutOfMemory;
            }
            kTail->start      = end;
            kTail->end        = heap_.end;
            kTail->kind       = VmaKind::kAnonymous;
            kTail->readable   = heap_.readable;
            kTail->writable   = heap_.writable;
            kTail->executable = heap_.executable;
            regions_.push_back(*kTail);
        }
        if (start <= heap_.start) {
            heap_.end = heap_.start;
        } else {
            heap_.end = start;
        }
    }
```

这几行的活,咱们一件件数。洞口 end 还压在堆末之内的时候,尾巴就从 end 到 heap_.end 自己立一户,读、写、执行三项权限照着堆抄,进了链。随后堆的末尾收缩,跨度要是打堆头盖进来,堆就整户清空。护页那页在洞里,照拆。墙两边的堆各归各户,musl 的指针,两头都有了着落。

咱们把这桩案子的缩印钉进长凳,test_vma 添的打洞案就是它:

```cpp
TEST("vma: a hole punched in the heap leaves the tail as its own region") {
    cinux::mm::VmaStore store;
    store.seed_heap(0x100000, 0x200000);
    ASSERT_TRUE(store.move_heap_end(0x108000).ok());
    ASSERT_TRUE(store.unregister_range(0x102000, 0x104000).ok());
    ASSERT_EQ(store.heap_end(), 0x102000UL);
    ASSERT_TRUE(store.find(0x103000) == nullptr);
    const cinux::mm::VmaRegion* const kTail = store.find(0x105000);
    ASSERT_TRUE(kTail != nullptr);
    ASSERT_TRUE(kTail->writable);
    ASSERT_EQ(store.region_count(), 1UL);
}
```

堆从 0x100000 长到 0x108000,洞开在 0x102000 到 0x104000,洞里的 0x103000 查无此段,堆末收到 0x102000,洞上的 0x105000 还活着,是自己的一段,写的权限健在。长凳上 test_vma 那件咱们自己跑,二十三案全过,机内的 ktest,二十一案全过。实弹再跑一遍,musl 的全链路,静态的 ash 与动态的 hello,一起复绿。

结案的话,咱们写进册子。MAP_FIXED 的每一个新消费者,都会拿自己真实的调用序列来考替换语义,这回应考的是 libc 里的分配器守卫,考题就一页护页,排在后头的还有装库的 ld.so,它的考卷厚得多。占址重建四个字,拆与立两头都得想全,链上的段想全了,还不够,堆这类单独立户的特殊段,也得一起想全。

musl 这边的全链路复绿,咱们把案子归档。同一个地基上,悬案还开着一桩,glibc 的 hello,起动即死,一声不吭。那是下一案,比这桩更响。
