---
title: 02 · 一扇窗,一个来回
description: "实模式 64KB 的牢笼、0x10000 起的暂存窗、保护模式闪进闪出的一窗一往返;两个编译单元各守母语,GDT 加到六项,归程那条带前缀的 ret;还有 Linux、GRUB 和老自举年代的三种玩法对照。"
chapter: 6
order: 2
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - loader
  - real-mode
  - protected-mode
---

# 一扇窗,一个来回

装载要翻的墙,是实模式 64KB 的牢笼。咱们在前面实模式那些卷里就摸过它的边:16 位世界的 C++ 是按 `-m16` 编的,指针再怎么算,解引用走的是 DS——而 DS 是一枚实模式的段、限长 64KB,线性地址一过 0x10000,一次访问就异常了。上一卷那 68 个字节倒是不用愁这个,当年它们的家在 0x9300,牢笼的圈内,搭 MBR 读盘的顺风车就到了。可内核的家定在 2MB,C++ 的手够不着。BIOS 的手其实也够不着:经典读盘要一个段:偏移的收货地址、段值左移四位再拼偏移,胳膊抡圆了,顶多摸到 1MB 上头的一丁点。所以两头都够不着,咱们就得想辙。

咱们想出来的辙,是开这么一扇窗。layout 里立了两个新数:窗基址 0x10000、窗大小 64KB,也就是从 0x10000 到 0x20000 的这一片——低内存地图上,它向来是空的。BIOS 把盘上的数据读进这扇窗,收货地址写 0x1000:0x0000,拼出来的正好是线性 0x10000。窗里的数据,C++ 侧一个指头都不去碰:它住在牢笼的外头,16 位的世界看得见、摸不着。真正动它的,一头是往里写的 BIOS,另一头是往外搬的 32 位世界。

往外搬的这段,咱们叫它摆渡。您把 `boot/load/ferry.cpp` 里的那两小段 asm 摆开看,门道就全在桌面上了:

```cpp
asm(".section .text.ferry,\"ax\"\n"
    ".global PmVisit\n"
    ".global FerryExit16\n"
    "PmVisit:\n"
    "   cli\n"
    "   movl %cr0, %eax\n"
    "   orb $1, %al\n"
    "   movl %eax, %cr0\n"
    "   ljmp $0x08, $FerryEntry32\n"
    "FerryExit16:\n"
    "   movl %cr0, %eax\n"
    "   andb $0xfe, %al\n"
    "   movl %eax, %cr0\n"
    "   ljmp $0x0000, $1f\n"
    "1: xorw %ax, %ax\n"
    "   movw %ax, %ds\n"
    "   movw %ax, %es\n"
    "   movw %ax, %ss\n"
    "   .byte 0x66, 0xc3\n"
    ".text\n");
```

去程的几条,全是咱们在保护模式那一卷里混熟的老熟人。`cli`,CR0 置 PE、带 `0x08` 的远跳、闪进保护模式。归程的路倒着走一遍:清 PE,远跳回实模式的 CS=0,再把三个数据段寄存器的值刷回零。一窗一窗地搬,每一窗就是这么一个来回。而归程远跳用的选择子是 `0x28`,它把 GDT 又加宽了一项:表从上一卷的五项长到六项,第六项是一枚 D=0 的 16 位代码段。远跳落进它的门内,CPU 的默认操作宽度就换回 16 位,后面清 PE、回实模式的道才走得顺。loader 里的 `lgdt` 在装载动手之前,就把这六项的表提前装上——摆渡的来回从第一趟起就有表可查。

对岸的活住在 `boot/load/ferry32.cpp`,咱们原样摆出来——它短得可爱:

```cpp
void CopyFlat(unsigned dst, unsigned src, unsigned len) {
    auto*       destination = cinux::base::PtrAt<unsigned>(dst);
    auto const* source      = cinux::base::PtrAt<unsigned>(src);
    for (unsigned i = 0; i < len / 4; ++i) {
        destination[i] = source[i];
    }
}

extern "C" [[gnu::naked]] void FerryEntry32() {
    asm volatile(
        "movw $0x10, %ax\n"
        "movw %ax, %ds\n"
        "movw %ax, %es\n"
        "movw %ax, %ss\n"
        "subl $12, %esp\n"
        "movl g_dst, %eax\n"
        "movl %eax, (%esp)\n"
        "movl g_src, %eax\n"
        "movl %eax, 4(%esp)\n"
        "movl g_len, %eax\n"
        "movl %eax, 8(%esp)\n"
        "call CopyFlat\n"
        "addl $12, %esp\n"
        "ljmp $0x28, $FerryExit16\n");
}
```

您把两份文件摆在一起看,一边一句多余的话都没有,因为它们各守各的母语:`ferry.cpp` 生而为 16 位,跟 stage2 同住 `-m16` 的世界。`ferry32.cpp` 是全程 32 位的,住在保护模式那一卷立的那个 OBJECT 库里,平坦段的基址全零、限长 4GB,2MB 的家随便走。两家的往来只有两条远跳,去程用的 `0x08`,归程用的 `0x28`,而符号靠 `extern` 在链接期见面。真正搬东西的 `CopyFlat`,是正经的 C++:三个整数参数由入口的 asm 手工压进栈,`call` 过去、一趟 `for` 循环按四字节一铲地往外送。参数不走寄存器而走栈,是因为这几条 asm 是 naked 的,咱们不跟编译器抢寄存器的调度权,栈上的进出咱们自己摆得平。

栈上的进出,咱们单独对一遍,这是最容易糊涂的地方。16 位世界里 GCC 生成的调用,压栈的返回地址其实是 4 个字节。`-m16` 的代码按 32 位的脾气生成、再让汇编器补前缀,反汇编里看得明明白白的,`push %ebp` 的机器码是 `66 55`,带着 0x66 的操作数宽度前缀。所以 `RunFerry` 把三个参数存进全局之后,干的是一次尾跳,直接跳进了 `PmVisit`,把回家的路留给栈上那 4 个字节。而 `FerryExit16` 回到实模式之后,收尾用的是一条手写的 `.byte 0x66, 0xc3`——带同样前缀的 `ret` 一口气弹 4 个字节。您要是让它弹 2 个字节,SP 从此就歪了,咱们的栈当场乱套,而且歪得悄无声息。整趟的摆渡,32 位那头加的是 12、减的也是 12、进出相抵,栈像没动过一样——这就是那个来回的全部开销。

一窗一往返的手艺,咱们可不是发明人,业界里玩装载的几家各有各的玩法,咱们对一遍就心里有底了。现代的 Linux 自己不玩花活:装载整个外包给 GRUB,自家残存的实模式代码老老实实按 64KB 的段过日子,setup 代码开头的注释,大意是人家打了个招呼:按 64KB 段干活。GRUB 和 syslinux 才是 unreal mode 的从业者:把段限长偷偷放宽到 4GB,而人还赖在实模式里用 BIOS,这套戏法的细节,咱们就不在主线里摊开了。数到更老的年头,bootsect 时代的 Linux 自举,就是保护模式闪进闪出、一趟搬 64KB——咱们的摆渡,正是这一脉的整洁变体:依赖面只剩经典读盘和自家的 CPU,一来一回都发生在跟 BIOS 的告别之前,谁的一分钱都不欠。

末了还有一条法条,是这一遍动手定稿之前实测换来的:BIOS 说成功,不等于数据真送到了。咱们从 MBR 读盘起,用的就是 16 字节的盘地址包,它还有个 24 字节的加宽版,多出来的字段是一个 64 位的平地址——按规范,把收货地址写成它,BIOS 就能直接把数据送到 1MB 以上的高处,一趟就免掉全部的来回。咱们当时真试过。回执一路报的都是成功,目的地一个字节都没动,咱们预埋的毒记号原封不动躺在那儿。翻 SeaBIOS 的源码才死心:版本号报得冠冕堂皇,平址的那套它压根没实现。所以装载完成之后,必须验的是内容,回执的事咱们留到讲流水线的时候去落实——头一窗的魔数回读,就是给它配的动作。

窗开了,摆渡的船也备下了,可还有一件事没交代:船该往哪儿开?内核住多大的房子、从哪个门进,这些数 boot 凭什么知道?咱们下一节去看鞋盒,鞋码就写在盒上了。
