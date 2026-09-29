---
title: 08 · 起居序列,一口气
description: "一条命令跑完整个起居序列,七条内存图谱陪着机器逐条认一遍,stage2 从五十来个字节长到三千多字节。"
chapter: 2
order: 8
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - stage2
  - e820
---

# 起居序列,一口气

收工的验证还是老面孔,咱们闭着眼睛都能敲:

```bash
cmake -B build && cmake --build build --target run
```

这回终端里等着咱们的,是这么一串:

```text
Ready to call bios
Jump to Stage 2
[stage2] stack ok
A20 ok
E820: 7 entries
  #00 base=0000000000000000 len=000000000009FC00 type=1
  #01 base=000000000009FC00 len=0000000000000400 type=2
  #02 base=00000000000F0000 len=0000000000010000 type=2
  #03 base=0000000000100000 len=0000000007EE0000 type=1
  #04 base=0000000007FE0000 len=0000000000020000 type=2
  #05 base=00000000FFFC0000 len=0000000000040000 type=2
  #06 base=000000FD00000000 len=0000000300000000 type=2
[stage2] alive
```

终端这串字的次序,就是 stage2 的晨间清单。咱们把这趟晨间连成了一遍动画:MBR 把棒交出来,stage2 把自己的栈立起来、把 A20 的门打开、把内存的家底问到手,末了才把那一声 alive 说出口。您看动画的时候,留意每一步兑现的是哪一样:

<Anim id="stage2-morning-routine" />

## 这串输出从哪来

一行行认它们是待会儿的活,眼下咱们把打印它们的代码请出来。头一段是负责吐图谱的 `dump_memory_map`:

```cpp
void dump_memory_map(cinux::boot::MemoryMap const& map) {
    static constexpr unsigned short kFormatBufferSize = 72;
    char                            buf[kFormatBufferSize];
    format::Arg const               kHeader[] = {format::NumU(map.count)};
    format::FormatToBuf(buf, sizeof(buf), "E820: %u entries\n", format::ArgsView::of(kHeader));
    serial::PutString(buf);
    for (unsigned int i = 0; i < map.count; ++i) {
        MemoryMapEntry const& entry   = map.entries[i];
        // %016llX 零填充定宽:十六列 base 一眼对上,当年 GDB 里对地址的读法
        format::Arg const     kArgs[] = {format::NumU(i), format::NumU(entry.base),
                                         format::NumU(entry.length), format::NumU(entry.type)};
        format::FormatToBuf(buf, sizeof(buf), "  #%02X base=%016llX len=%016llX type=%X\n",
                            format::ArgsView::of(kArgs));
        serial::PutString(buf);
    }
}
```

再看把它们串起来的 `Stage2Main`,咱们接着对。

```cpp
extern "C" [[noreturn]] void Stage2Main() {
    using namespace cinux::boot;
    serial::PutString("[stage2] stack ok\n");

    if (!bios::EnableA20AddressLine()) {
        fail("A20");
    }
    serial::PutString("A20 ok\n");

    if (bios::CollectMemoryMap(&g_memory_map) != 0) {
        fail("E820");
    }

    dump_memory_map(g_memory_map);

    serial::PutString("[stage2] alive\n");
    for (;;) {
        asm volatile("hlt");
    }
}
```

stage2 说的每一行字,您都能在这两段里找到它的出处。`E820: %u entries` 那行是 `dump_memory_map` 开门打的招呼。后面七行摘要是那个 for 循环一条一条吐出来的——格式串 `"  #%02X base=%016llX len=%016llX type=%X\n"` 里,两个 `%016llX` 的零填充撑出十六列,base 才对得那么齐。前头讲 16 位世界里打 64 位数的两件麻烦时,咱们给引擎添的那个类型擦除入口 `FormatToBuf`,眼前就是它在 boot 侧的头一批消费现场:参数包成 `Arg` 数组递进去,咱们连调用约定都不用赌。`Stage2Main` 里的失败通道您已经见过:哪一步黄了,`fail` 就把 `A20 FAILED` 或 `E820 FAILED` 那样的字样打到 debugcon,然后睡死等咱们提着终端赶到。

## 从头一行认起

头两行是 MBR 说的,上一站咱们逐字认过:一句报的是马上请 BIOS 读盘,一句道别的是世界交给 stage2。第三行换了嗓门,`[stage2] stack ok` 是 `Stage2Main` 在新栈上开口说的头一句话——这句话本身,就是搬家的验收。咱们能走到这儿,说明 cli、重铺段、换 SS:SP、call 这一串是一气呵成的,新栈上的头一个 C++ 函数,活着开了口。`A20 ok`,门开了。`E820: 7 entries`,咱们问回来的家底一共七条。最后的 `[stage2] alive`,还是上一站那句回声的腔调。

## 七条图谱,陪着机器认一遍

重头戏是中间的那七行,咱们一条一条来。您会发现每一条都是标准值——SeaBIOS 在默认 128MB 内存的 QEMU 上,交出来的就是这么一张图,几乎一个字节都不带含糊的。

`#00`:从 `0x00000000` 起、长 `0x9FC00` 的这一段、type 1——低内存 639KB,咱们能用。这个数眼熟吧,`0x9FC00` 正是 EBDA 的底。不过您留意,BIOS 口中的可用是整段从地址 0 开始算的,头 `0x500` 字节里住着中断向量表和 BDA——图上咱们早就标了禁区。BIOS 的“可用”是法律意义上的空地,里面有没有什么户口,得看咱们自己的地图。

咱们接着认。`#01` 是 EBDA 的地盘:从 `0x9FC00` 起、长 `0x400`,type 2——BIOS 自己的 1KB 记事本,咱们碰不得。`#02` 是 1MB 门槛前的最后 64KB,BIOS ROM 的影子:从 `0xF0000` 起、长 `0x10000`,type 2——咱们同样绕着走。

`#03`:从 `0x100000` 起、长 `0x7EE0000` 的这一段、type 1——主存,从 1MB 伸到了 `0x7FE0000`,约 127MB 的个头,平时咱们问机器内存多大,问的就是它。您拿它跟 A20 那段对上:主存恰好从 1MB 起——门要是没开,头一兆就折回了 0,后头每个奇数兆都串到低一兆的地方,整个主存等于被搓乱了。

咱们接着看 `#04`,它紧跟在主存的尾巴上,又标了 `0x20000` 的保留:从 `0x7FE0000` 到 `0x8000000`,正好是 128MB 的整数边界。BIOS 在 RAM 顶上给自己留了块自留地。

`#05` 是 `0xFFFC0000` 起、长 `0x40000`:4GB 天花板下的 256KB BIOS ROM,跟咱们在 `#02` 见过的是一个来路。

`#06` 值得咱们多看两眼:`0xFD00000000` 起、长 `0x300000000`,type 2——它的 base 站在 4GB 之上,从约 1012GB 伸到了整整 1TB,是 QEMU 在高空中给 PCI 留的大洞,32 位的地址根本装不下它。咱们在前头说过,E820 是唯一看得见 4GB 之上的 BIOS 内存服务。现在您看到活证据了。它也顺手解释了 base 和 length 为什么非得是 64 位字段:图谱里的住户,真的住到天上去了。

七条图谱还有个赏心悦目的性质,咱们顺手验一遍:`#00` 的 base 加 len 是 `0x9FC00`,正是 `#01` 的 base。`#01` 的 `0x9FC00` 加 `0x400` 是 `0xA0000`。`#03` 的 `0x100000` 加 `0x7EE0000` 是 `0x7FE0000`,正是 `#04` 的 base。可链子并不是真的一环扣一环,它是有洞的:`#01` 收在 `0xA0000`,`#02` 却从 `0xF0000` 才起步——中间的 `0xA0000` 到 `0xEFFFF` 是老显存的地盘,图谱里没它的条目。`#04` 收在 128MB 整的 `0x8000000`,`#05` 却一步跨到了 `0xFFFC0000`。所以图上没列出来的地址,咱们一概当不能用对待——跟前头 type 名单外的值一律归保留,是同一个保守的方向。不过咱们心里要有数:约定并不保证条目有序、不保证不重叠,眼下的整齐是 SeaBIOS 恰好排出来的,不是咱们能依赖的合同。真到消费图谱的那天,还得按保守的翻译来。咱们再看 type 那一列,打的是 1 和 2 这样的裸值,探测层忠于 BIOS 的原话,翻译的活留给消费层,咱们收工时打出来的,也还是这样的裸值。[OSDev 的 E820 页](https://wiki.osdev.org/Detecting_Memory_(x86))上摆着别的模拟器的典型图谱,您拿去跟咱们手里的图谱对照着看,标准值就对上了号。

![七条图谱的物理地址全景](assets/e820-memory-bands.drawio)

## 个子蹿起来了

最后咱们给 stage2 量量个子。上一站收工的时候,它拢共五十来个字节的小身板加一句回声。本站干完了,`stage2.bin` 实测 3376 字节——起居室里住进了格式引擎、BIOS 服务层、运行时支撑件,`.bss` 里还有 772 字节的存档等着开张。

个子是跳着长的。探测件刚拼好的那会儿,镜像九百多个字节、4 个扇区、2048 字节的预算绰绰有余。格式引擎一搬进来的当天,镜像就量出了 2672 字节,超线——构建那道闸当场把咱们拦下,报错信息还附了修法:去 `layout.hpp` 抬 `.sectors`,并告诉您 MBR 的 DAP 会自动跟上。咱们把它抬到 8,预算变成了 4096 字节。DAP 那边是一个字都不用改的,它读的是 `kStage2Spot` 的字段。闸门那边:上一站讲过的正则——匹配 `kStage2Sectors = ` 抓预算——也随着收拢迁到了 `.sectors = ` 上,报错文案指名的还是 `kStage2Spot`。咱们只改了一处,DAP 和闸门都跟着走——画地图的时候收拢成的一个值,好处当天就见了效。还有一茬要交代:闸门的预算是 configure 期从 `layout.hpp` 里抓的,改了 `.sectors` 的值,闸门得重新 configure 一遍才看得见新的值。这层洞就是抬 `.sectors` 那天现的形:依赖没登记,构建一声不响地吃着旧值。仓库里咱们用 `CMAKE_CONFIGURE_DEPENDS` 把这层依赖登记上了:`layout.hpp` 一动,CMake 自己就把 configure 重跑了。您要是自己手搭构建,这茬您得自己记着。

接线的变化,一路跟着敲的您马上会用到:base 的源件头一回进了 boot 的构建。`stage2` 目标的源清单从单件变成了五件。include 路径添了两条:`base/include` 供头文件,`base/src` 供引擎自用的内部头。编译期多了 `-ffunction-sections`,把每个函数分进自己的节,链接期的 `--gc-sections` 这才有得裁——这俩开关是一对的,少了前一个,后一个就没了可裁的,没被调用的代码照样进镜像:

```cmake
add_executable(stage2
    stage2.cpp
    bios.cpp
    early/libc.cpp
    ${CMAKE_SOURCE_DIR}/base/src/format/entry.cpp
    ${CMAKE_SOURCE_DIR}/base/src/format/radix.cpp)
```
尺寸回到眼前:咱们手里 3376 对 4096,余量宽出了七百多字节,下一站的 VESA 那摊子活,是住得下的。

## 收工

本站进门的时候,咱们欠着三样:借来的栈、锁着的线、不知道的内存。出门都兑了:栈立在图上自己的宅基地,A20 的门开了,七条图谱在 `.bss` 的存档里躺得好好的——`stage2.bin` 3376 字节,住着 4096 的预算。

下一站的活,卷首已经替咱们预告了:配屏。画布的尺寸、framebuffer 的落脚处,这些参数眼下都还握在 BIOS 的手里。往前走的下一程,咱们趁它还没下班,把该问的一样一样问到手。
