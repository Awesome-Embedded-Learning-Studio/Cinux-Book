---
title: 07 · 六十八个字节,自己一条链
description: "64 位的目标件进不了 32 位的链,只能自己成链再剥成 68 个字节,0x9300 这个地址是实测里试出来的,先被 rodata 的尾巴拦了一回,又被看不见的 bss 拦了一回,去掉 KEEP 的对照实验绿着错。"
chapter: 5
order: 7
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - cmake
  - linker
  - long-mode
---

# 六十八个字节,自己一条链

远跳的目标为什么是个数?因为对岸的代码,跟咱们连同一条链接都进不去。咱们给 `lm.cpp` 按 `-m64` 一编,出来的是 ELF64 容器的目标件,而 stage2 链的产出是 32 位的 ELF,链接器见了 64 位的件就直接拒收,报的原话就是 incompatible。它跟选项的搭配没有半点关系,毛病出在容器对容器的不相认——第四户编译世界连借住的地方都没有,只能自己立一条链了。配套的编译旗也早就备好了:64 位世界在共用的那套 freestanding 旗之外,多戴了两样。咱们头一样戴的是 `-mno-red-zone`,中断随时会来的世界里,函数栈帧底下那 128 字节的偷懒空间,是用不得的。另一样戴的是 `-mcmodel=small`,按低地址的小代码模型编,跟咱们定在低区的链址正相配。

咱们把走法从头过一遍。这三步的头一步是独立成链:

```cmake
add_executable(lm_blob lm/lm.cpp)
add_cinux_boot_binary(lm_blob 64 lm/lm.ld)
```

咱们给 `lm_blob` 这个目标配了它自己的链接脚本 `boot/lm/lm.ld`,链完了是一枚小小的 ELF64:

```
ENTRY(LmEntry)
SECTIONS
{
    . = 0x9300;
    .text   : { *(.text.lm_entry) *(.text*) }
    .rodata : { *(.rodata*) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.eh_frame) }
}
ASSERT(ADDR(.text) == 0x9300, "lm blob must sit at the pinned entry VMA")
```

`. = 0x9300` 把整条链锚在了 0x9300,而 `*(.text.lm_entry)` 排在最前头。对岸的入口函数 `LmEntry`,头上顶着的是 `[[gnu::section(".text.lm_entry")]]`,靠的就是这一句收集令,稳稳地落在第一节的第一字节上。而这个翻译单元里还住着打印函数的 64 位实例化,咱们要是不定住它,链接器哪天兴起把次序重排了,咱们的远跳按数跳过去,就是一脚踏空了。脚本尾巴的 ASSERT,又替咱们多闸了一道。而到了第二步,咱们用 `objcopy -O binary` 把这枚 ELF 剥成裸字节,剥出来的是 68 个字节的 `lm.bin`。而第三步,咱们把裸字节包回一件“32 位的目标件”,再从外面拼进 stage2 的链:

```cmake
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lm_blob.tmp.o
           ${CMAKE_CURRENT_BINARY_DIR}/lm_blob.o
    COMMAND ${CMAKE_OBJCOPY} -I binary -O elf32-i386 -B i386
            --rename-section .data=.lm_blob
            ${CMAKE_CURRENT_BINARY_DIR}/lm.bin
            ${CMAKE_CURRENT_BINARY_DIR}/lm_blob.tmp.o
    COMMAND ${CMAKE_OBJCOPY} --set-section-flags .lm_blob=alloc,load,readonly,data
            ${CMAKE_CURRENT_BINARY_DIR}/lm_blob.tmp.o
            ${CMAKE_CURRENT_BINARY_DIR}/lm_blob.o
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/lm.bin)
```

咱们把裸字节包成目标件,`-B i386` 让它进得了 elf32 的门。而 blob 本身的二次链接早已完毕、零重定位,所以当一包只读数据来搬,是没有副作用的。可为什么改名和上旗要分成两步?因为并成一步的写法,是在实测里试出来的。咱们要是让 `--rename-section` 连 flags 一起带上,咱们手上这版的 binutils,会把节的内容整段洗成零——尺寸还在、字节却全成了零,比把节整个弄丢的后果还要阴险,因为丢了节起码还有构建报错可指望。而裸改名的内容是完好的,可改名后的节上又挂着 W 位,跟代码并段会引来 RWX 的告警:新版 binutils 见了又可读又可写又可执行的段,会冲咱们唠叨上一句,而咱们给链接加上 `-Wl,--no-warn-rwx-segments`,就把告警安顿下去了。引导镜像本来就是单段平铺、三种权限合一的老布局。告警拦下的,就是咱们明知为之的老布局。所以头一步裸改名,第二步咱们单独用 `--set-section-flags` 把 W 摘掉,实测下来是不洗内容的,两边就都顾到了。

节名上咱们也改过一回:头一版的名字叫 `.rodata.lm_blob`,被 stage2 脚本里既有的 `*(.rodata*)` 通配一并收了进去,地址就白定了。而改名叫 `.lm_blob` 之后,谁也抢不走了。

然后是 stage2 这头的收纳。`boot/stage2.ld` 里新添的行,位置是咱们拿两次事故换来的:

```
    .lm_blob 0x9300 : { KEEP(*(.lm_blob)) }
    .bss    : { *(.bss*) *(COMMON) }
```

咱们从 KEEP 说起,它保的是这包字节的命。stage2 的链接开着 `--gc-sections`,没人引用的节会被垃圾回收裁掉。而 blob 的“引用”是什么?是一句远跳里的立即数。链接器看不见这样的引用,所以在它的眼里,这 68 个字节就是一包没人用的死数据。咱们拿一个最小复现做对照:哑函数里只含一条 `hlt`、外加链上的 blob。有 KEEP 的时候,4.7KB 的镜像里 blob 是在场的。而把 KEEP 删掉再链,构建回给咱们的,是一路的绿灯和零告警,镜像就只剩 6 个字节了,剩下的恰好就是哑函数自己。blob 就这么蒸发了,而构建毫无表示。而这样的错,咱们指望不上告警、也指望不上尺寸闸,因为它只拦长得太大的,拦不住莫名变小的情况。咱们判断这包字节能不能留下,靠的就是 KEEP 这个词。

脚本那头咱们只排了位置,真正把 blob 挂进链里的,是 `boot/CMakeLists.txt` 里的两行接线。头一行的接线,把包好的目标件放进 stage2 的源清单:

```cmake
        ${CMAKE_CURRENT_BINARY_DIR}/lm_blob.o
```

stage2 的源清单里从此多了一件不是源码的件。另一行的接线是依赖声明,免得咱们哪回拿旧剥的 blob 来链:

```cmake
add_dependencies(stage2 lm_blob_obj)
```

咱们要是漏了这两行,构建回给咱们的照样是绿灯,可远跳按数跳到的,就是一片没装东西的空洞——这正是本节前面讲过的绿着的错。

再说地址。咱们动手前的预判是 0x9000:按老记录里 stage2.bin 的 4608 字节一算,镜像的尾巴恰好收在 0x9000,咱们看着简直像天作之合。而实测给了咱们两个耳光。第一记耳光打在预判的数字上:上一卷收官时,镜像其实是 4624 字节,尾巴已经探过 0x9000 一点了。而本站再添的代码,也就是扩出来的两项描述符、填表和切换的新代码,一共是 299 字节的增量,把 rodata 的尾巴推到了 0x913b。把地址定在 0x9000,正好压在它的地界上,ld 的 overlap 护栏当场就把咱们拦下了。而第二记更刁:这回报重叠的是 `.bss`,一位尺寸闸永远看不见的居民。bss 是 NOBITS 的节,1.8KB 的身量不占镜像文件里一个字节,尺寸闸对它是全盲的,可它的 VMA 区间,却实打实地罩住了定址的点。咱们没有再挪地址,而是动了次序:把 `.lm_blob` 排到 `.bss` 的前头,bss 自己就飘到 blob 的后面去了,就天然没有了冲突。定值 0x9300 两侧各留出几百字节的呼吸,12 扇区的硬顶 0x9600 一动都没动。这两记耳光教给咱们的很具体:镜像的尺寸要重新量,旧记录里的数字不能直接搬。地界的事也一样,咱们把它写进断言,让构建替咱们把关。咱们在 layout 里给这个数配了四条断言:

```cpp
inline constexpr unsigned long kLmEntryVma = 0x9300;

static_assert(kLmEntryVma % 16 == 0);
static_assert(kLmEntryVma >= static_cast<unsigned long>(kStage2Spot.offset));
static_assert(kLmEntryVma < static_cast<unsigned long>(kStage2Spot.offset) +
                                (static_cast<unsigned long>(kStage2Spot.sectors) * 512U));
static_assert(kLmEntryVma > kPageTables.top);
```

咱们看四条断言各管什么:头一条管的是 16 字节对齐,那是代码入口的整洁底线。中间两条把 blob 关进了 MBR 一把读进来的那 12 个扇区里。投递靠的也是这个:blob 嵌在 stage2 镜像的内部,MBR 原本的那次读盘,自动就把它捎进来了,一次新的 BIOS 服务都不用,何况这个时候的 BIOS,早就叫不应了。末一条管的是页表的地界,不许压到 `kPageTables` 的头上。这个数在三处各写了一遍:lm.ld 和 stage2.ld 的两份脚本,而 layout 里也有它的一份。因为 ld 脚本读不了 C++ 的常量,而这样的文档性重复,咱们从 0x7E00 那年起就是这么过的。

> 咱们在构建系统里顺手做了一次收敛:boot 里的第三条链接咒语,也就是 mbr、stage2 加上本节的 lm_blob,落地的时候咱们就把三处重复的样板,收进了 `cmake/boot/boot_target.cmake` 的 `add_cinux_boot_binary`。它给目标穿上对应世界的编译配置、按各自的脚本链接、再剥出 `${target}.bin`。而函数里藏着一个真知识点:编译的世界和链接的驱动是两码事,16 位与 32 位的目标件同为 ELF32 的容器,所以链接一律按 `-m32` 驱动,而只有 64 位走 `-m64`。而收敛完之后,三枚镜像连一个字节的改动都没有,测试也全部通过了,boot 的 CMakeLists 薄了将近一半。而 blob 的两步打包,咱们有意没把它收进去,因为那是只有 blob 打包才用的操作,这样的路标留在明处比藏进函数里更好。

对岸世界的全部家当,就装在这 68 个字节里了。门开了,咱们下一节过去看看。
