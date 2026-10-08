---
title: "04 · 客人的世界"
description: "页表项的 user 位收进词汇,走查长出 MapUserPage 这张脸,新起的中间表把 user 位一路传下去。AddressSpace 开出用户版,高半区的镜像把帧缓冲的门一并带进新世界。EFER 的 SCE 位解锁 SYSCALL 与 SYSRET,STAR 从 GDT 常量拼出。跳板五条指令,把当年一下午换来的栈对齐收进一处。"
chapter: 13
order: 4
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - userland
---

# 客人的世界

上一节收尾时座位、锅、工具、落脚点四件都齐了,该给客人造世界了。客人得有自己的页表、自己的栈,咱们把这件事想透。内核的高半区,客人是看得见的:内存当家那一卷立 AddressSpace 时就把家法定下,每个世界的高半边从根上镜像,内核不管谁当家都完整地在。低半区就两样了,从地址零往上整整 128TiB 一片纯白,客人的代码、客人的栈,都得咱们从零画。

动笔前得过权限这一关:页表项的第二位就是 user 位。咱们把它收进词汇,起了 kUser 这个名字:一位写明“这一级允许 ring3 通过”。它最要紧的脾气是得一路都带:CPU 查表的每一级都拿当前特权级验一次权限,所以任何一级写着仅内核可用,ring3 的脚就迈不过去。当年第一遍在这里摔了跟头,叶表带足了 user 位,中间一级漏了:缺页错误码 0x05,咱们把它读作“页在,用户态却碰了只有内核能走的路”。四级页表:咱们一级都省不得。这一遍咱们把它根治在走查里:EnsureLeafTable 多收一个 leaf_flags 参数,路上新起的每一张中间表:门牌出生就带着这个位。伺候客人的脸长这样:

```cpp
template <TableWorld World>
bool MapUserPage(World& world, unsigned long root, unsigned long virtual_address,
                 unsigned long physical) {
    Entry* const kTable = EnsureLeafTable(world, root, virtual_address, cinux::arch::page::kUser);
    if (kTable == nullptr) {
        return false;
    }
    kTable[SlotIndex(virtual_address, WalkLevel::kPageTable)] = cinux::arch::page::MakeTableEntry(
        physical, cinux::arch::page::kWritable | cinux::arch::page::kUser);
    return true;
}
```

走查从此有了两张脸,MapPage 伺候内核的页,MapUserPage 伺候客人的页。分两张脸不为别的,咱们让消费面从此不必接触 flags 的词汇,画谁的页就叫谁的名字。您跟一次 map_user 就能看见这个位怎么传:走查从客人的根出发,头一级 PML4 里这项还是空的,咱们就新起一张表,指向它的表项写上 kWritable 加 kUser:第二级、第三级一样办理,到了叶子,表项还是同样的两位。四级都验过了,ring3 的查表才一路放行。AddressSpace 跟着添了 map_user。画图的便利还是老样子:走查拿根当入参,机器脚下的 CR3 稳稳不动,咱们坐在内核的桌前把客人世界里的门一张一张挂好再谈进场。

这儿有一处当年摔得最重的地方,咱们得特意盘一遍:帧缓冲的门怎么跟过去。当年第一遍咱们把帧缓冲按物理地址原样映在低半区,用户世界只抄了高半区,到了换根那一拍,往屏幕去的路没了。偏偏缺页处理程序自己也要往屏幕写,再缺页就一层又一层地重入,把串口输出都搅碎了。这一遍咱们翻出地图一查,这病连复发的土壤都没有:内存当家那一卷早把帧缓冲搬进了高半区的设备窗,住 PML4 的 259 号槽。镜像抄的是 256 到 511 整个高半边,设备窗自然也抄了进去。客人的世界里,内核的每一扇门原样都在,一扇多不了,一扇也少不了。

世界有了,咱们还差门。门开在 MSR 里:上一节的读写件正好派上用场。启动表里添了新的一步:名字就叫 fast system calls。咱们眼下用到的两格:头一格把 EFER 的第 0 位 SCE 点上,SCE 正是 SYSCALL 与 SYSRET 的解锁位,不开它的话,两条指令本身就都成了非法指令,咱们哪个方向都出不去。另一格写的是 STAR,这个 MSR 存的是两边各自的选择子基值:高 16 位给 SYSRET,用座位扩容时定下的 0x23,RPL 3 早在基值里就编好了。SYSCALL 进内核用的是中间 16 位:内核代码段 0x08。两个数都不是手写的字面值,从 GDT 头文件的常量里拼出来,拼完再让编译期断言复核一遍:在机器通电之前它们就都对上了。同一排暗格里还有咱们留到柜台那一章才用的两格,LSTAR 存的是 SYSCALL 进门的入口地址,SFMASK 存的是进门要清的标志位。

头一版在这里摔的这一跤不冤。跳板写好了,咱们按下运行,sysretq 当场就吃了 #UD(非法指令异常),转储里 cs=8:其实 CPU 压根没出门,还坐在内核的代码段上。咱们回头去翻启动面板:efer 一栏是 500。这个数摆在那儿不是一天两天了,数数它的位:第 8 位和第 10 位亮着,是长模式的开关,偏偏第 0 位是 0。boot 开长模式的时候只管长模式:SCE 从来没人点过。证据早印在眼前:咱们天天看它就是没读它。补上了这一步,面板上报的是 501:一字之差,门就通了。

跳板本身短:咱们整个抄下来。

```asm
JumpToRing3:
    movq %rdi, %rcx
    movq $0x202, %r11
    movq %rsi, %rsp
    subq $8, %rsp
    sysretq
```

五条指令条条都有它的讲究,您一条条对着看。rcx 填的是入口,SYSRET 跳向的地址就是它。r11 填的是 0x202,落地的标志寄存器从它来:第 9 位正是 IF,客人落地就带着开中断。rsp 给的是栈顶。最不起眼的第四条,却是当年拿一整个下午换来的活知识:在咱们的机器上,C 程序的调用约定(SysV ABI)要求函数入口的栈指针除 16 余 8,因为正常进场是 call 压过返回地址。咱们的客人不是 call 进来的,是跳进来的,页顶除 16 余 0:齐整得恰好错了 8 个字节,编译器优化出的一条要求 16 字节对齐的传送指令:头一拍就崩。当年头一个疑的是浮点没开,开了还崩,最后才抠到了对齐上,现场补了一条减 8。这一遍咱们把这点知识收进跳板:调用方只管递页顶,余 8 的事门里自己办。前头的四条全是铺垫,末了一条 sysretq 这才起跳,这一跳本身就是第五条的讲究。

世界画好了,跳板立好了。门里还空空的,咱们还缺一个真的客人程序。
