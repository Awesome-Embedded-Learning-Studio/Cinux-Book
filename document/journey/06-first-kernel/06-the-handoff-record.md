---
title: 06 · 家底的交接
description: "ABI 前门头三样、整份誊抄的内存图谱、带六个位域的画布参数、内核自己的四个数;交接单住在 kernel/boot 的一份头文件里,两只低址信箱负责跨链递话。"
chapter: 6
order: 6
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - handoff
---

# 家底的交接

该办交接了。上一卷咱们过桥,对岸只有 68 个字节的代码,什么都用不着带。这一回对岸是个要当家的内核,接手的是一整台机器,总得知道自家的家底。boot 要交清三样:内存的图谱、画布的参数、内核自己的身份。这样的单子,其实当年第一遍也交过,这一遍重誊的时候,咱们按内核真正需要什么,把字段重新数了一遍。单子的家在 `kernel/boot/boot_info.hpp`,咱们把主体摆出来:

```cpp
struct [[gnu::packed]] BootInfo {
    uint32_t        magic;
    uint32_t        version;
    uint32_t        struct_size;
    uint32_t        e820_count;
    E820Entry       e820[kBootInfoE820Max];
    FramebufferInfo framebuffer;
    uint64_t        kernel_paddr;
    uint64_t        kernel_file_size;
    uint64_t        kernel_mem_size;
    uint64_t        kernel_entry;
};
```

单子的前门,咱们来认头三样:magic、version、struct_size。写单子的和读单子的,是两个世界各自编译的两拨代码:哪天 boot 这头改了字段、挪了顺序,过时的内核再来读新单子,就会整个读串位了。所以进门对暗号:魔数对不上,单子就不可信了。struct_size 把单子的长度写在脸上。version 管字段变义——单子将来换了朝代,旧内核体面地拒读,免得读出满纸的糊涂。魔数的值是 0x00114514,笔者挑的,您要是看着眼熟,就算认对门牌了。

E820 的整份誊抄,占了单子往后的一大片。图谱当年是 BIOS 一条条报上来的,咱们存了档,上一节验鞋盒用的也是它。现在咱们把整张图谱原样誊进单子,条数记进了 e820_count。数组的容量给到 128 条,比问图那一头 32 条的档宽裕得多,余量是给图谱变长的那天留的。誊完了这一段,内核从此不欠 BIOS 一分钱:它不用知道 E820 是什么调门,更不用再进 BIOS 的门——内存的事,boot 替它问完了一辈子。

`FramebufferInfo` 咱们摆出来看,画布的地契,五个老面孔都见过了,真正的新东西是六个位域:

```cpp
struct [[gnu::packed]] FramebufferInfo {
    uint64_t physical;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t red_size, red_shift, green_size, green_shift, blue_size, blue_shift;
};
```

red、green、blue 各自带了一对 size 和 shift。为什么非要六个数?32bpp 只说一个像素 32 个比特,没说红绿蓝各占哪几个——排列是模式自己的事,想当然不得,这口气笔者在配屏那一卷挨过双重的打。所以趁 BIOS 还应着,把三对掩码的宽度和位置从 ModeInfoBlock 里摘下来,写进单子:内核将来点亮它的头一个像素,红的在哪儿、绿的在哪儿,单子上都写了,不用再回 VBE 查一遍了。配屏那一卷存的参数,到这儿头一回派上了正经用场。

尾巴上的四个数,内核自己的身份:住址、两个尺寸、门牌,从鞋盒上一字不差地转誊。您可能要问:boot 不是刚读过一遍?可读单子的是内核——单子递到了它手里,它把单子里的身世原样报了出来,打印用的数一个不落全来自单子。真做核对的是咱们:拿面板上的 kernel at 那一行,去对鞋盒上盖的章。对上了,说明单子真送到了。

单子的家安在 `kernel/boot/`,方向是咱们特意挑的:单子是内核的脸,boot 是誊写的人,脸是正主的,誊写的照着抄。所以 boot_info.hpp 住在内核的树里,而 boot 反过来 include 它。同一个屋檐下还住着 console.hpp:debugcon 的 0xE9、一次一个字节的 PutChar、还有那个体面的 Halt,boot 和内核 include 的是同一份头。头文件 inline 的老红利,每个编译世界各拿一份自家的实例化。本站出生的第五户世界,落地第一天就能整行整行打字,靠的就是这个:同一个 base、同一份头,它消费的全是现成的家当。

单子誊好了,咱们怎么把它递过去?誊好的单子是 stage2 院子里的一个全局,住在低内存 boot 自家的地界。而读它的内核,世界观在高半的地界,两条链的符号互相看不见——68 个字节那回就定下的老章程:握手的凭据,是 layout 里写下的数。所以递话走两只邮箱,layout 里立着两个地址:0x4F00 放门牌,0x4F08 放单子的地址。stage2 这头就写了这么两句:

```cpp
StoreWord(kHandoffMailboxEntry, static_cast<unsigned long>(kHeader.entry));
StoreWord(kHandoffMailboxInfo, reinterpret_cast<unsigned long>(g_kernel_boot_info));
```

写的人和读的人隔着一个世界,一写一读——差的正好是那一跳。StoreWord 和 LoadWord 都标了 volatile:写必须落在那一跳之前,读必须发生在落地之后。咱们隔世递话,凭的就是这两下不许被优化掉。

单子进了邮箱,内核的家当齐了。可它的世界观在高处:代码里取址全按 0xFFFFFFFF80200000 那一套,CPU 走路偏偏要走表,而高处还没有路。咱们下一节去开两扇门。
