---
title: 08 · 落地,三行字,和验收
description: "远跳落地,五个段寄存器换 64 位的衣服,栈顶回到原点;面板上多出第三行字,test_page 的五个哨兵,还有验收日可选的 GDB 取证:EFER=0x500、CS=0x18、CR0=0x80000011。"
chapter: 5
order: 8
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - testing
  - long-mode
---

# 落地,三行字,和验收

远跳落了地,`LmEntry` 把执行流接住了。它是 64 位世界的头一份产业,咱们把整个 `boot/lm/lm.cpp` 全量摆出来:

```cpp
#include "../gdt/gdt.hpp"
#include "early/boot_console.hpp"
#include "layout.hpp"

extern "C" [[gnu::section(".text.lm_entry")]] [[noreturn]] void LmEntry() {
    asm volatile(
        "movw %[data], %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %%ax, %%ss\n"
        "movl %[stack], %%esp\n"
        :
        : [data] "n"(cinux::boot::gdt::kSelectorData64), [stack] "n"(static_cast<unsigned int>(
                                                             cinux::boot::kPmStackTop))
        : "ax");
    cinux::boot::serial::PutString("[lm] 64-bit world alive\n");
    for (;;) {
        asm volatile("hlt");
    }
}
```

开场的那套动作,您眼熟得不能再眼熟了。远跳只照顾了 CS 一个,DS、ES、FS、GS、SS 五个还穿着 32 位的旧衣服,所以咱们拿 `0x20` 一个个刷干净——上一卷跨门槛那回干过同款,而这一回,是它的 64 位版本。栈也是老熟人了。咱们拿 `movl` 写的是 esp,而 64 位模式下写这颗寄存器、会自动把高 32 位清成零,rsp 从此就干净了。栈顶取的还是 `kPmStackTop` 的那个 0x90000,因为远跳把 32 位世界的栈帧整个弃下了,咱们顶回原点,boot 立下的栈地址,咱们一个字都没改。

接下来咱们报到。`PutString` 这边一句多余的 include 都没写,64 位的版本就到手了——它住在头文件的 inline 里,每个编译世界都各拿一份自家的实例化,这是咱们在上一卷就拿到手的红利,而如今开第四户世界,红利又领了一回。这行字的分量,咱们得跟当年对照着看:第一遍过桥的时候,对岸的报到,是拿裸机器码往 0xE9 塞的一个字母 `L`,能证明人到了,别的什么都说不出口。而这一遍,是整行整行的字。于是收工的面板,就是下面的样子:

```text
[stage2] leaving real mode
[pm] 32-bit world alive
[lm] 64-bit world alive
```

三行字的背后,咱们从头到尾守的就是一条对仗:头一行是 16 位世界的遗言,后两行是两个新世界各自的报到。您细看的话,会发现 32 位的世界只有一行,因为建表和切换都是不出声的活,它的第一行,同时也是它的最后一行。而第三行能亮起来,就等于整条链都通了:表填对了,门闩拨对了,序列走对了,远跳跳对了,64 位的译码和打印都活着。末尾的 `hlt` 还是长眠,上一卷的那句 `cli` 依旧生效着,门是不会响的。

咱们在 host 那头照老路数清点一遍:`ctest` 的清单添到了七件,而 boot 血统的出品,现在添到了第四件,就是带着五个哨兵的 `test_page`。咱们挑两个看:

```cpp
TEST("page: large pages encode the identity 0-8MB set") {
    ASSERT_TRUE(MakeLargePageEntry(0x0, kWritable).raw == 0x83);
    ASSERT_TRUE(MakeLargePageEntry(0x200000, kWritable).raw == 0x200083);
    ASSERT_TRUE(MakeLargePageEntry(0x400000, kWritable).raw == 0x400083);
    ASSERT_TRUE(MakeLargePageEntry(0x600000, kWritable).raw == 0x600083);
}

TEST("page: deposit and extract are inverse over the phys field") {
    Entry const kEntry = MakeLargePageEntry(0x600000, kWritable);
    ASSERT_TRUE(kEntry.has(kPresent));
    ASSERT_TRUE(kEntry.has(kWritable));
    ASSERT_TRUE(kEntry.has(kLarge));
    ASSERT_TRUE(kEntry.extract(kLargePagePhys) == (0x600000 >> 21));
}
```

头一个哨兵把恒等映射的四颗大页项逐颗对数,它核的四个值从 `0x83` 排到 `0x600083`,走表时咱们踩过的那颗 PD[2]、基址 `0x400000`,编出来正是其中的 `0x400083`,四颗全部都对上了。末一个哨兵验的是互逆:deposit 放进去的帧号,extract 会原样地读回来,三面旗咱们各问一遍。剩下的三个,一个管的是宽度定在 8 字节,一个管的是指针项裁掉脏低位,还有一个管的是大页地址按 2MB 对齐收拢。`test_gdt` 那头也添了长模式的哨兵,验的是 L=1、D=0,还有 `0xAF` 与 `0x8F` 的两个 flags 字节,选择子一路算到了 `0x18` 和 `0x20`,整表核对到了 40 字节。七件测试全部都通过了,QEMU 的三行逐行吻合、构建零告警——本站的验收,到这儿就算平了。

要是您验收的时候想再较真一步,咱们就照上一卷的体例,请 GDB 来给咱们取证。您给 `run` 的 QEMU 命令手工添上 `-s -S` 再启动,咱们在另一头把 GDB 打开、连上 `target remote :1234`,喂给它的,还是带符号的 stage2 ELF——`EnterLongMode` 的符号就住在它里面,而对岸的符号住在另一条链的文件里,这回是帮不上忙的。头一处咱们把断点下在 `EnterLongMode`,进去之前咱们把三张表看一眼:`x/8gx 0x1000` 的头一项该是 `0x2003`,`x/8gx 0x2000` 的头一项该是 `0x3003`,而咱们在 `x/8gx 0x3000` 里该看到 `0x83、0x200083、0x400083、0x600083` 四颗大页。您一眼就看得出桥面铺得平不平。而第二处咱们把断点下到 `*0x9300`,按的是地址、用不着符号,咱们放它过完远跳,在 `info registers` 里咱们看三样:EFER 的读数是 `0x500`,低处的 `0x100`,是咱们拨的 LME,而高处的 `0x400`,是 CPU 自己点亮的 LMA。CS 的值是 `0x18`,属性栏里亮着 CS64 的标记,这就是 64 位译码在跑的直接证据。而 CR0 的读数是 `0x80000011`,bit 0 的 PE 和 bit 31 的 PG 都亮着,当中 bit 4 的那一颗,是 Intel 留下的老痕迹,咱们从头到尾都没碰过它,开机它就在了。咱们顺手还能看见 RSP 顶在 0x90000 上,咱们说的顶回原点,在这儿是有数为证的。

咱们把尺寸也报一下。上一卷收官的镜像收在 4624 字节,本站新添的描述符、填表和切换代码一共 299 字节,把节的尾巴推到了 0x913b。可 blob 的住址定死在 0x9300,从节的尾巴到那里,镜像里垫了 453 字节的空,再加上 68 字节的 blob 本体,stage2.bin 的实测正好是 5444 字节。12 个扇区装的是 6144 字节,余量还有 700 字节的富余,而构建这边一个告警都没有。

收工了。本站攒下的东西里,最贴身的有两样:链尾那 68 个字节,和面板上新出的第三行字——前者装着对岸要跑的全部代码,后者是它活着的第一句证据。别的家当,断言和哨兵都替咱们看着,咱们就不逐件点数了。

咱们在考古箱里翻一件当年的旧事:第一遍的 MBR、保护模式和长模式,是同一个晚上连着赶完的,三个里程碑挤在了同一天。而那个 `efer=0x1000` 的定案,当晚的笔记写了两百多行。桥通了,就该有人过桥了。而下一站,咱们把一个真正的内核从磁盘装进来,把机器的家底交接给它、再送它去高处安家。

> 本站的外部依据,咱们一并归拢:Intel SDM Vol.3A 的 §9.8,IA-32e 模式的初始化,参考的序列就印在其中。长模式的出身文档是 AMD 的架构程序员手册。社区的视角,咱们看 [OSDev 的 Long Mode 页](https://wiki.osdev.org/Long_Mode)和 [Paging 页](https://wiki.osdev.org/Paging)。当年的调试现场,咱们在调试档案里另存了一份细目:[进长模式的顺序与标志位陷阱](/debug-notes/003-long-mode-entry.md)。
