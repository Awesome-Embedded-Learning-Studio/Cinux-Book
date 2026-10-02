---
title: 08 · 内核出生,和验收
description: "KernelEntry 的四件事:立栈、存 rdi、自清 BSS、五桩极早异常处理器;kernel 主函数的三行字、不带清单的婴儿内核、host 十件测试和 QEMU 面板逐行验收,外加验收日可选的 GDB 取证。"
chapter: 6
order: 8
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - testing
  - kernel
---

# 内核出生,和验收

跳落了地。接住执行流的,是内核的头一行代码 KernelEntry——住在 `kernel/arch/x86_64/entry.cpp`。它出生头一口气要办的四件事:立栈、存 rdi、清院子、挂异常桩,办完了才轮到 Main。咱们一件一件看。

咱们头一件看立栈,代码就这么几行:

```cpp
extern "C" void KernelEntry() {
    asm volatile(
        "movabsq $0x90000, %%rsp\n"
        "xorq %%rbp, %%rbp\n"
        :
        :
        : "memory");
```

又是熟悉的 0x90000,咱们跟它打了三站交道。boot 在 32 位世界立栈用的就是它。上一卷 64 位落地,顶回原点的也是它。如今内核自己又立了一遍。那一跳把上一个世界的栈帧整个弃下了,栈顶从原点重新起算——这一回是内核亲手立的——打这儿起,栈就姓内核了。rbp 也顺手清了零,栈回溯翻开新的一页。

第二件办的是存 rdi。就这么一行,平平淡淡的,当年偏偏崩得莫名其妙,咱们从现场说起。当年的第一遍,内核进场后要清自己的 BSS,清零用的指令是 rep stosb——它干活的时候,目的地指针用的正是 rdi。可 rdi 里此刻装的,是 boot 递过来的单子地址。院子清完了,单子地址跟着一起被搅没了:后头的代码拿着一个被清零指令洗过的指针,读出去的单子全是垃圾。偏偏祸不单行,当年 BSS 边界的符号还跟别的数据落在同一个地址上,清零的起点自己就不干净。两桩祸叠在了一起、崩得莫名其妙,查明白的时候笔者哭笑不得。这一遍的形态就一行:进门把 rdi 存进栈上的局部、再动手清院子。rep stosb 毁指针的那一祸,到这儿就解了。至于边界符号同址的那一桩,不归顺序管——归链接脚本的界碑和对齐管,下一件咱们就见到界碑。

第三件办的是清院子。鞋盒上 mem_size 比 file_size 多出来的那截,讲鞋盒的时候咱们交代过:盘上一个字节没为它花,boot 也一个字节不替它清——内核自家的院子自己扫。链接脚本在 .bss 的两头立了界碑——g_kernel_bss_start 和 g_kernel_bss_end,memory_zero 从界碑到界碑、一个字节一个字节地抹。当年闯祸的 rep stosb,这一遍换成了一个老老实实的 C++ 循环。咱们这版内核,院子统共占了 512 个字节。

第四件办的是挂异常桩。出生的清单里为什么有这个?cli 只关可屏蔽中断,而异常另有走法,走的是 IDT:表里没有桩,头一个 #PF 的下场就是三重故障、机器复位——连一句遗言都没有。上一卷咱们在三重故障的射程里过日子,那会儿倒是可以说反正对岸没有住户。如今内核当了家,出事的早上总得留句话。挂了五个桩,压最常出事的五种:

```cpp
asm(".section .text.exc,\"ax\"\n"
    ".global ExcDe\n"
    "ExcDe:\n  pushq $0\n  pushq $0\n  jmp ExceptionCommon\n"
    ".global ExcUd\n"
    "ExcUd:\n  pushq $0\n  pushq $6\n  jmp ExceptionCommon\n"
    ".global ExcDf\n"
    "ExcDf:\n  pushq $0\n  pushq $8\n  jmp ExceptionCommon\n"
    ".global ExcGp\n"
    "ExcGp:\n  pushq $13\n  jmp ExceptionCommon\n"
    ".global ExcPf\n"
    "ExcPf:\n  pushq $14\n  jmp ExceptionCommon\n"
    ".text\n");
```

五个桩的号,咱们一个个认:#DE 除零是 0 号、#UD 无效指令 6 号、#DF 双重故障 8 号、#GP 一般保护 13 号、#PF 缺页 14 号,五个全认齐了。CPU 对带错误码的异常会自己压一个码,#GP、#PF 归的就是这一类。不带码的三种,桩自己垫了一个零把栈面找齐,再压上自家的向量号、跳进公共入口。公共入口把栈上压好的两笔捞出来交给 ReportException,面板上打出了一行 `[kern] exc #N @ rip=...`,然后就 Halt 了。

表呢?IDT 的 32 项住 .bss,拿 C++ 一项一项填:入口地址拆成三截装进门,选择子用的是 0x18,属性字节 0x8E——在场的、ring0 的中断门。顺序咱们前面其实已经埋好了:清院子的活在前,填表的活在后,表自己也是院子里的住户,顺序反了,扫帚会把刚挂上的门牌一并扫掉。五桩挂好了,lidt 把表的地址装进 CPU——从这一刻起,出事有回音了。

这一串都利索了才轮到 Main。咱们把 `kernel/kernel.cpp` 的当家函数整个搬出来——统共三行字:

```cpp
void Main(const cinux::boot::BootInfo& info) {
    Println("[kern] 64-bit C++ world alive");
    Println("[kern] bootinfo: %u e820 entries, fb %u*%u*%u", info.e820_count,
            info.framebuffer.width, info.framebuffer.height, info.framebuffer.bpp);
    Println("[kern] kernel at %X size %X entry %X", info.kernel_paddr, info.kernel_mem_size,
            info.kernel_entry);
    cinux::console::Halt();
}
```

第四行字亮了。后头的两行,一行报的是家底:8 条图谱、画布 1024 乘 768 乘 32,这是单子真送到的回执。另一行报的是身世:住 200000、地界 C40、门牌 2001B4,跟鞋盒上盖的章一对、一个数都不差。三行打完了,Halt 落地,沉沉地长眠。您翻回本卷开头贴的那段面板尾巴,到这儿一行一行全有了着落。

这个内核小得可怜、拢共 3136 个字节的地界,咱们盘一盘它的家当,它带了几样?答案有点意外,它几乎什么都没带:没有内存管理、没有自己的盘驱动、没有第二张描述符表。打字的 Println 是 include 来的,格式化是 base 的老引擎,连 strlen 和 64 位的除法,都是从 boot 的 early 家里链接来的同一份。第五户世界落地头一天消费的就是同一个 base——对照当年,第一遍过桥报到的那一回,笔者只能拿裸机器码往 0xE9 塞一个字母。这一回内核张口就是整行整行的字。当年的小内核被迫长成半个操作系统,靠的是一份长清单,这一遍咱们一样都没带。不是省事——装载排在了告别之前,带的事根本没轮到它。

该验收了。host 这头的清单数到了十件——十件全过。本站添的是三件。test_bits 把 base 位操作的老帮手们挨个验了一遍:半字节切片的 LowNibble 一家、按字宽算的单比特掩码 MaskBit,还有 deposit 和 extract 的一去一回,全是您在前两卷见过的名字。test_layout 盯着 layout 的法条站岗:stage2 得蹲在 MBR 的身后,页表和栈各守各的地界,blob 的住址得落在 stage2 的扇区预算里。test_image 把九判的判据当纯函数喂数据,头和单子的布局连字段落在第几字节都量过,合法的鞋盒九判全过,逐类的拒绝挨个当面演过,最便宜的问话排在头里。QEMU 这头的面板尾巴,咱们再认一遍,还是本卷开头的那段,不一样的是,如今每一行的来历咱们都亲手办过:

```text
[stage2] kernel image ok
[stage2] kernel ferried
[stage2] leaving real mode
[pm] 32-bit world alive
[lm] 64-bit world alive
[kern] 64-bit C++ world alive
[kern] bootinfo: 8 e820 entries, fb 1024*768*32
[kern] kernel at 200000 size C40 entry 2001B4
```

数字咱们也核一遍:盘上的内核 2624 个字节,鞋盒认的是 2613,摆渡走的是六扇区一窗。内存里占了 3136,多出的 512 是自己扫的院子。门开了两张。stage2 长到了 12903 个字节、26 个扇区,28 扇区的预算还剩两个。胖是胖了些,进出是平的。

您要是验收日想较真到底,照上一卷的体例请 GDB 来取证:把 run 的 QEMU 命令抄出来,手工添了 `-s -S` 再启动。另一头的 GDB 连 `target remote :1234`,头一句 `set architecture i386:x86-64`——连上的时候,CPU 还停在 16 位 reset 的门口。断点挑的是 `hbreak *0xFFFFFFFF802001B4`:软断点要往内存写字节,而高半那一页此刻还没人映射,硬件断点只认地址、不讲这些难处。continue 放机器跑到断点命中的时候,`info registers` 里 rip 正落在高半的门牌上,rdi 里躺着单子的地址。咱们拿 `x/2gx $rdi` 看一眼,头 8 个字节是 0x0000000100114514——魔数挨着的正是版本号,单子的前门,咱们亲眼见到了。带符号的内核 ELF 里就住着 KernelEntry 的名字,按名字下断点也是一样的。

收工了。装载链在咱们身后合上:从 MBR 的第一声读盘,到末了那记 jmp,boot 把执行流亲手递了出去。面板上的四行字、四个世界,末一行是内核自己打的。它眼下会打三行字了,会扫自己的院子,出事的早上喊一声 rip。往后的日子轮到它自己当家,下一卷从内存的家底起头,其余的活往后各卷里一件一件来,咱们接着看它长大。
