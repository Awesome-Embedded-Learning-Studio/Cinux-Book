---
title: "07 · 头一个照本敲门的客人"
description: "实弹首弹 RELRO protection failed,炸出两桩连破的案。案一:号位表上 munmap 与 mprotect 对调多年的座,musl 的 ld.so 用裸号十敲 mprotect,敲进了咱们的 munmap,自家用符号常量的测试一路通过,遮了这么多年,探针的零打印半刻定案,修完拿宿主 syscall 表 static_assert 四连锁号。案二:mprotect 借了撤销区段的整吞检查,RELRO 的子区段必拒,换成逐页覆盖走查的新门。两案都在,缺一跑不通。"
chapter: 25
order: 7
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - abi
  - debugging
---

# 头一个照本敲门的客人

地基铺完,客人也铸好了,盘烤好了,开了机,咱们在教学 shell 里敲 run /bin/hello-dyn。炸了。报出来的是一句 RELRO protection failed。咱们把这句报错翻译一下:RELRO,而行话叫重定位后转只读,ld.so 把自家的重定位做完之后,要把那块只用一次的可写区真正锁成只读,这是它收尾的一道保险。注意这句报错是谁说的:是 ld.so 自己说的。它已经进了内存,已经跑起来了,已经做完了大部分的活,死在了收尾的保险上。离成功一层窗户纸的事,可窗户纸也得一层一层地捅。

第一层的窗户纸,咱们在号位表上捅破了一桩陈年旧案。懒到家那一卷铺号位的时候,咱们把 munmap 挂在十号、mprotect 挂在十一号,x86_64 的 ABI 正好倒过来:mprotect 十号,而 munmap 十一号。懒到家那一卷的正文里白纸黑字写着一句话:往后讲 ABI 的那一卷,会把 munmap 与 mprotect 的座次换回来。今天轮到的就是那一卷。可为什么这么多年的测试一路通过?因为咱们自家所有测试用的都是符号常量:测试敲 kMprotect,分发表认的也是 kMprotect,常量与表是同源的,号对不对得上真的 ABI,内部永远是自洽的。迎客的路上,musl 的 ld.so 是头一个照本敲门的:它的启动代码里,RAW __syscall 直接写的是裸号,SYS_mprotect 的值就是 10,10 敲进咱们的表,落的是 munmap 的门。改 mprotect 的请求进了拆映射的门,它的收尾保险自然做什么都失败。

这一案的诊断,咱们想单独说一说,因为它真的值钱。咱们没有抱着源码一行行推,咱们在 mprotect 的检查进门的第一行,临时打了一行探针,把请求的地址、长度、权限三个参数原样打上串口,开机复跑了一遍,看的是串口有没有动静。答了零打印。就这一句零打印:请求根本没有进到 mprotect 的检查里。门牌号错了,检查里的代码读一百遍也读不出这个错。验完了探针撤干净,一针也不留了。这一课咱们总结成一句话:想知道请求进没进某处检查,当然别猜,咱们在检查里点一盏灯,看灯亮了没有。零打印的分量,比读代码去猜快了一半。

修法的一行:两个号对调回来。可光修这一案不过瘾,咱们把同一家的四个号一起管住:kMmap 是 9,kMprotect 的号是 10,kMunmap 的号是 11,kBrk 的号是 12,四条 static_assert 全部对着宿主系统的 <sys/syscall.h>,号表再想自说自话的话,编译期就爆了。四行锁的原文住在宿主的 test_syscall_face 里:

```cpp
static_assert(static_cast<int>(cinux::syscall::SyscallNr::kMmap) == SYS_mmap);
static_assert(static_cast<int>(cinux::syscall::SyscallNr::kMprotect) == SYS_mprotect);
static_assert(static_cast<int>(cinux::syscall::SyscallNr::kMunmap) == SYS_munmap);
static_assert(static_cast<int>(cinux::syscall::SyscallNr::kBrk) == SYS_brk);
```

等号左边是自家的枚举,右边是宿主系统头里的宏,和 auxv 那一篇锁 elf.h 用的则是同一路数,现在咱们两处都齐了。回头看 auxv 那一篇早早落了锁,当时咱们只当顺手,今天这一案才让它变成了一句庆幸:同一个病根,人家早一步上了锁。开门迎客那一卷 stat 的一百四十四字节,假文件与真终端那一卷的四纸判决,加上今天的这一桩,病根其实是同一条,绿了的测试没造出真客人的形态。自家的常量,要用别人的表来锁。

第二层的窗户纸。号修好了再跑,报的还是那句 RELRO protection failed。这回进门了,灯亮了,死在了门里的章法上。咱们 mprotect 的实现,借的是懒到家那一卷为撤销区段写的一道检查,请求必须完整吞下一个登记的区段:不然拒。这一道检查对撤销是对的,撤销要的就是整段拿走。可 mprotect 做的不是撤销,RELRO 改的是段内一小块子区段,请求骑在区段的腰上,整吞检查当然拒,答的是 EINVAL。懒到家那一卷的 mprotect,只有整区的测试喂过,子区段的保护是一片真空白,咱们当时的正文还写下了整段改词、不许劈半的说法,把一个待补的语义当成了成型的章法记了下来,这里咱们如实更正。

修法是咱们给 protect 换上它自己的检查,行话叫逐页覆盖走查:从请求的起点出发,一步一步地往终点走,每一步脚底下都必须踩在已登记的地界上,踩空了一处,直接拒了。走查的循环长这样:

```cpp
bool VmaStore::span_known(unsigned long start, unsigned long end) const {
    unsigned long spot = start;
    while (spot < end) {
        unsigned long reach = spot;
        if (heap_.start <= spot && spot < heap_.end) {
            reach = heap_.end;
        }
        for (const VmaRegion* scan = regions_.head(); scan != nullptr;
             scan                  = regions_.after(*scan)) {
            if (scan->start <= spot && spot < scan->end) {
                reach = cinux::base::math::Max(reach, scan->end);
            }
        }
        if (reach <= spot) {
            return false;
        }
        spot = reach;
    }
    return true;
}
```

spot 是那双脚:每一步问的都是堆的地界认不认它,接着要扫的就是登记册,谁家的地界盖住了它,reach 就抬到谁家的末尾。而谁都不认的话,reach 就原地不动了,踩空落的就是 reach <= spot 那一下,当场答的也是假。spot = reach 则是一步跨过整片已登记的地界,再接着问的才是下一寸。整吞检查的旧问法,问的是请求装不装得进某个区段,新问法问的则是路过的每一寸是不是都有主,咱们换的就是这一问。

检查过了,咱们再分两种落笔,登记册这一半的落笔就是 protect 里那个循环:

```cpp
    for (VmaRegion* scan = regions_.head(); scan != nullptr; scan = regions_.after(*scan)) {
        if (inside(scan->start, scan->end, start, end) ||
            inside(start, end, scan->start, scan->end)) {
            scan->readable   = terms.readable;
            scan->writable   = terms.writable;
            scan->executable = terms.executable;
        }
    }
```

循环里的判据,就是咱们分两种落笔的分界:区段整段落在请求里,或者请求整段落进了同一个区段,任一方整含了另一方,登记册的读、写、执行三问就直接改词。而请求骑墙的时候,两边各自伸出了头,if 的两个条件就都不成立了,区段登记的词原样不动,页表那一半在系统调用的路里单独逐页地照改。未登记的地界照旧拒,mprotect 不发明地皮。旧的测试原案是 protect 拒劈半,随着语义翻转改写成接受一段穿过区段的请求,机内的孪生旧案也被机内测试头一轮抓出来同步改写,宿主与机内各改各的,这是同一语义两处落笔必须两处跟上的又一课。

两案都在手里了,缺了哪一件都跑不通,咱们把这句话掂了掂分量:第一案修完还跑不通,卡的就是第二案,第二案不修的话,第一案就白修了。破完案的当次实弹,敲的还是那句 run /bin/hello-dyn,串口里出来了问候,正是 Hello from dynamic musl on Cinux! 的原文,退出码报了零,同一台机上再跑静态的 /bin/hello,结果还是老样子:一发没炸。

咱们把这趟全链在心里过一遍,每一环都对得上号:扫出 PT_INTERP 的路径,822KB 的解释器整读进内核,双重间接的读路真走了,解释器座位落在 256MiB 的家,ld.so 自家的重定位,RELRO 的 mprotect 穿过新门,依赖表里的 libc.so 自识别,所以最后一跳跳进 AT_ENTRY,而 printf 经 libc.so,结果 write 进内核,末了 exit 收工。顺带一笔:musl 报错里 %m 打出来的 errno 是陈旧的,裸 __syscall 是不设 errno 的,那一晚咱们差点被自己的报错骗去查别的门,各位往后读 ld.so 的报错,留这么一手心眼。
