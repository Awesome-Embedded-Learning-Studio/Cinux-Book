---
title: "04 · 真正的 auxv"
description: "开门迎客那一卷栈页上的辅助向量只有一条裸 AT_NULL,这一卷摆成一桌真的。musl 1.2.5 的源码一条条核过:缺了就崩的硬需五味,缺了能活但可惜的营养组,有守卫可缺的散席。AT_RANDOM 的缺席这些年一直由 musl 的确定性回退硬扛,栈护甲从可预测升成真随机。十四枚号全用 static_assert 锁给宿主的 elf.h,画像拆成宿主可链的独立件,四笔测试的错记下。"
chapter: 25
order: 4
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - auxv
  - abi
---

# 真正的 auxv

栈页上的那幅画像,开门迎客那一卷咱们画完了 argc、argv、envp,可 ABI 还规定了第四样东西:辅助向量,行话的叫法是 auxv。它是紧跟在 envp 结尾的零词之后的一串配对,每对的两格,前格放的是类型号,后格放的是值,最后一对是类型零的 AT_NULL,收住全桌的尾。它是内核递给程序启动代码的一桌情报:你的入口在哪,你的程序头表落在哪,页是多大的,这些事你自己在用户态打探不着,内核不说的话,你就不知道。咱们那一卷摆的,是一桌空席:只摆了一条裸 AT_NULL,问了也不答。

空席为什么这些年没人掀桌?因为静态的客人不点菜。busybox 天天在空席上吃得饱饱的,咱们把 musl 的源码翻开找原因,找到了:musl 对缺的每一味都有自己的兜底。最惊险的一味是 AT_RANDOM,十六字节的随机数,栈护甲的种子。栈护甲的行话是 stack canary:函数返回地址前埋的一只金丝雀,进了函数抄一遍、出了函数对一遍,对不上说明栈被人冲了,当场自杀谢罪了。金丝雀的值得是随机的,不然攻击者照着公式算出来了,护甲就成了摆设。musl 拿不到 AT_RANDOM 的时候收零,走确定性回退:护甲值等于护甲自家地址乘 1103515245。地址是能打探的,数是能算的,金丝雀是人人能喂的。咱们这些年的静态实弹,背后靠的全是它在硬扛。这一课咱们记下了:没炸不等于没问题,可能只是没人来查而已。

解释器可就不一样了,它是真正点菜的客人,而且点的菜缺一味就掀桌。咱们把 musl 1.2.5 的 ld.so 启动代码从头翻到尾,启动代码住在 musl 源码树的 ldso/dynlink.c,每一处 auxv 的用法都核过,这一桌菜咱们不猜,是照着菜单数的。菜单分了三档。第一档是缺了就崩的硬需,五味里打头的是 AT_PHDR:主程序头表在内存的落点,ld.so 拿它与自家的头表比对,认出两张表里哪张是主程序的,缺了这一味,它拿零当地址去解引用,当场就死了。AT_PHENT 与 AT_PHNUM 说的是头表的几何,一条的大小与总的条数。AT_ENTRY 说的是主程序的入口,ld.so 忙完自己的事,最后一跳跳的就是它。AT_PAGESZ 说的是页大小,musl 的 libc 里页大小就吃这一味,缺了它收到的值是零,装载里一切按页取整的算式,从 mprotect 的边界到 mmap 的落点,跟着一起歪——崩是躲不掉的,只是崩得不在这味缺席的那一行,而是崩在头一回拿页大小干活的活上,炸点离病因隔着几手的路,查的时候要多绕一层,硬需硬的正是这里。

第二档是缺了能活但可惜的营养组:AT_RANDOM,真随机的金丝雀。AT_BASE 说的是解释器自家的基址,这一味缺席的话它有兜底,拿 AT_PHDR 的页对齐去猜也猜得回来,可咱们有真数,凭什么让它猜。AT_EXECFN 说的是被执行文件的路径,值就是 argv 第零支指的那句串的落点,咱们一串都不用多写。UID 的四味,用户与组各两问的身份号,咱们再加一味 AT_SECURE,管的是身份与安全态,咱们的机器只认一个 root,五味的值清一色的零。第三档是有守卫的散席,AT_HWCAP 的那一类,musl 拿的时候要看 bitmask,缺了不问。咱们摆前两档,散席就不摆了。

咱们把整桌菜连着号摆开,号在 loader.hpp 的常量区里单点写着,一格管的是一味,值的那一半要等摆桌的时候才落:

| 号 | 名字 | 档 | 值从哪来 | 缺了会怎样 |
|---|---|---|---|---|
| 3 | AT_PHDR | 硬需 | 盖住 e_phoff 的 LOAD 段推算的头表落点 | ld.so 拿零解引用,当场死 |
| 4 | AT_PHENT | 硬需 | 头表一条的字节数 | 头表没法走,崩 |
| 5 | AT_PHNUM | 硬需 | 头表的总条数 | 头表没法走,崩 |
| 6 | AT_PAGESZ | 硬需 | 机器页大小 4096 | 页大小落零,按页取整全歪,崩在下游 |
| 9 | AT_ENTRY | 硬需 | 主程序的入口 | 最后一跳没有落点,崩 |
| 7 | AT_BASE | 营养 | 解释器基址 256MiB | 有兜底,拿 AT_PHDR 的页对齐猜回 |
| 11 | AT_UID | 营养 | 用户身份号,恒零 | 缺了能活 |
| 12 | AT_EUID | 营养 | 有效用户号,恒零 | 缺了能活 |
| 13 | AT_GID | 营养 | 组号,恒零 | 缺了能活 |
| 14 | AT_EGID | 营养 | 有效组号,恒零 | 缺了能活 |
| 23 | AT_SECURE | 营养 | 安全态,恒零 | 缺了能活 |
| 25 | AT_RANDOM | 营养 | 十六字节随机载荷的落点 | musl 走确定性回退,金丝雀可预测 |
| 31 | AT_EXECFN | 营养 | argv 第零支那句串的落点 | 缺了能活 |
| 0 | AT_NULL | 收尾 | 恒零 | 桌没有边,必摆 |
| — | AT_HWCAP 一类 | 散席 | musl 按 bitmask 问过才用 | 缺了不问,咱们不摆 |

实现上情报的源头收成两个半场。装载的半场,EntryFacts 结构的前四格:程序入口、头表落点、头表几何,装载器装图的时候顺手填。世界的半场:解释器基址、页大小、随机源,exec 的路自己填。随机源接的是 getrandom 那一路真随机的线:探 CPUID,有 RDRAND 的话就连取两发八字节,拼成的是十六字节,两发都落了空,整条 AT_RANDOM 就省了,musl 退回它的确定性回退,咱们不装。argv 与 envp 也收进了一个 EntryRuns 小包,递给画像的时候还有情报作伴。参数也一并收进了包里,就是装载那一篇交代的那道参数上限的家法咬出来的:两次都咬在超了四个词上,合进包里就成了一趟。摆桌的写码就住在画像构件里,一连串的 put_pair 排下来,每一对的号与值都看得见:

```cpp
bool put_facts(PicturePlane& plane, const EntryFacts& facts, unsigned long first_run_landed,
               unsigned long random_landed) {
    if (!put_pair(plane, kAuxPhdr, static_cast<unsigned long>(facts.program_headers)) ||
        !put_pair(plane, kAuxPhent, static_cast<unsigned long>(facts.header_entry_bytes)) ||
        !put_pair(plane, kAuxPhnum, static_cast<unsigned long>(facts.header_count)) ||
        !put_pair(plane, kAuxPageSize, static_cast<unsigned long>(facts.page_bytes)) ||
        !put_pair(plane, kAuxBase, static_cast<unsigned long>(facts.interpreter_base)) ||
        !put_pair(plane, kAuxEntry, static_cast<unsigned long>(facts.program_entry)) ||
        !put_pair(plane, kAuxUid, 0) || !put_pair(plane, kAuxEuid, 0) ||
        !put_pair(plane, kAuxGid, 0) || !put_pair(plane, kAuxEgid, 0) ||
        !put_pair(plane, kAuxSecure, 0)) {
        return false;
    }
    if (random_landed != 0 && !put_pair(plane, kAuxRandom, random_landed)) {
        return false;
    }
    if (first_run_landed != 0 && !put_pair(plane, kAuxExecfn, first_run_landed)) {
        return false;
    }
    return true;
}
```

十一味是固定上桌的,每对的第一格是号、第二格是值,UID 四味与 AT_SECURE 的值直接写死零——单 root 的机器,给的就是全部的实话。收尾的两味则是条件的:random_landed 是那十六字节随机数落地的地址,探到了才上桌,first_run_landed 是 argv 第零支串的落点,有了 argv 才有 AT_EXECFN。两味缺席也都是合法的形态,静态客人走的正是缺席的那一路。链条上任何一个 put_pair 答了假,作废的就是整张画像,余量的检查就藏在 put_pair 的门里,下一篇咱们专门算它。最后收桌的是一对 AT_NULL,WriteEntryPicture 的末尾两行写的正是它:一行写的是类型零,另一行写的是值零,一桌的边就收在这儿。

十四枚类型号咱们一个都没有放任自抄。每一枚都拿 static_assert 对着宿主系统 <elf.h> 里的 AT_ 常量一一对表:AT_PHDR 必须是 3,AT_BASE 的号必须是 7,错了一个,编译期就爆了。锁的本体住在宿主长凳的 test_elf 里,每一行锁的都是一枚号,连 PT_INTERP 在内的段表号也一并锁了,十五行的原文就是下面这些:

```cpp
static_assert(cinux::loader::kProgramInterp == PT_INTERP);
static_assert(cinux::loader::kAuxNull == AT_NULL);
static_assert(cinux::loader::kAuxPhdr == AT_PHDR);
static_assert(cinux::loader::kAuxPhent == AT_PHENT);
static_assert(cinux::loader::kAuxPhnum == AT_PHNUM);
static_assert(cinux::loader::kAuxPageSize == AT_PAGESZ);
static_assert(cinux::loader::kAuxBase == AT_BASE);
static_assert(cinux::loader::kAuxEntry == AT_ENTRY);
static_assert(cinux::loader::kAuxUid == AT_UID);
static_assert(cinux::loader::kAuxEuid == AT_EUID);
static_assert(cinux::loader::kAuxGid == AT_GID);
static_assert(cinux::loader::kAuxEgid == AT_EGID);
static_assert(cinux::loader::kAuxSecure == AT_SECURE);
static_assert(cinux::loader::kAuxRandom == AT_RANDOM);
static_assert(cinux::loader::kAuxExecfn == AT_EXECFN);
```

等号左边是咱们自家的常量,右边是宿主系统头里的宏,两边各说各的源,对不上的话编译期就爆。这些号和系统调用的号是同一种东西,都是 ABI 的门牌,自家抄错自家的表,测试永远发现不了,得让别人的表来对。这一道锁眼下倒是看着顺手,到了实弹那一篇,它会变成一句庆幸的话。

画像本身的代码也搬了一次家。WriteEntryPicture 从装载器的大文件里分出来,自成一件纯内存算术的构件:进的是一页字节、一个栈顶、两组串、一份情报,出的是画好的页,不碰页表、不碰分配、不碰内核的任何设施。这样的身子宿主世界链得起,画像的案子从此全在宿主长凳上直测,分家的章法在这一卷又落了一子。写画像时顺带交代:静态客人走的也是同一张画像,装载的半场照填,世界的半场基址给零,随机源没有就省了,一条 AT_RANDOM 和 AT_EXECFN 的缺席都是合法形态,静态的实弹一发不能少,这是咱们给自己定的零回归门槛。开门迎客那一卷挂过的画像机内案,这里一并交代下落:画像成了纯内存构件,案子全数搬进了宿主长凳直测,机内的装载案改口断言 EntryFacts,旧挂的案子销掉。

宿主长凳上添的新案,咱们按名点齐。共享座良构的一案,换座两拒的一案,头表落点两形的一案,解释器路径直拷的一案,auxv 全向量走查的一案,静态缺省的一案。这批案子的手稿咱们如实记四笔错,全是测试自己的错,不是实现的。argc 数错了一处,单参数的案却断言了两支。落点差了一字,/bin/prog 连 NUL 的长度是十字节,串区的偏移该是二十六,咱们写了二十五。起点用错了一处,带 envp 的画像 auxv 从第四十字起,咱们拿去量了 envp 空的版本。对数漏了一词,EXECFN 在场的时候,数出来是十二对而不是十一对。四笔都摆在这儿了,给往后写测试的咱们留一面镜子:实现一遍过的时候别高兴太早,回头去把测试查了。
