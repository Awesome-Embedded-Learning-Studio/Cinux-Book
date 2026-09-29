---
title: 05 · cmake --build build -j，启动！
description: "链接脚本把 512 字节红线写成构建期断言:0x7C00 的链接地址、末尾 55 AA 的签名、把 bin 顶到 536 字节的 PIE,还有 rm 掉产物之后要能自愈的依赖图。"
chapter: 1
order: 5
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - cmake
  - linker-script
---

# cmake --build build -j，启动！

代码齐了,可代码离"能启动的磁盘"还差一整条流水线:`.cpp` 要编成目标文件、目标文件要链接、链接出来的可执行文件还要剥掉一切包装,变成 512 字节整的裸字节,最后写进一块磁盘镜像的头部。本节咱们把流水线架起来、顺便在构建系统里砌几道墙。boot 这个地界上,事故大多不出在您写错逻辑的地方,而出在"东西悄悄超标了""地址悄悄对不上了"这类没人看着的角落——墙,就是砌给这些角落的。

## 链接地址:代码以为自己住在哪

头一道防线、砌给一个当年让笔者死得很惨的认知,**链接地址和运行地址、是两码事**。

咱们写的 C++ 里有绝对地址吗?

有,而且藏得很深。桩里 `movb %dl, g_boot_drive` 要访问全局变量、编出来的指令里写的是它的地址。`call MbrMain` 跳的是函数的地址。这些地址是**链接器**算的——它把所有目标文件摊开,决定每个符号住在哪、再把地址填进指令。链接器怎么决定?看它的地图。咱们要是撒手不管,它按平常程序的排法,把代码从默认地址排起。可咱们的代码实际运行在 `0x7C00` 啊!链接器以为住在东、实际住在西。于是每一条带绝对地址的指令全指错地方。症状是运行时莫名其妙地乱跳、乱写,崩在离病根很远的地方、查无可查。第一遍的时候,笔者在它身上耗掉过整整一个晚上,最后发现就是两个数字对不上——您体会一下当时的心情。

所以咱们的地图必须明说:代码从 `0x7C00` 起。地图就是链接脚本 `boot/mbr.ld`,咱们全文过一遍:

```ld
ENTRY(mbr_start)
SECTIONS
{
    # Put it here, we assume we start here
    . = 0x7C00;
    .text   : { *(.text.boot) *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
    _mbr_end = .;
    ASSERT(_mbr_end <= 0x7C00 + 510, "MBR exceeds 510 bytes")
    . = 0x7C00 + 510;
    .sig    : { SHORT(0xAA55) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
```

咱们逐行看——`ENTRY(mbr_start)`:入口是桩上那个标号,这是给链接器的提示。`. = 0x7C00`,全脚本的定盘星——那个孤零零的 `.` 是**位置计数器**,链接器用它记"接下来这一段从哪个地址排起"。咱们把它一把拨到 `0x7C00`、后面所有段就都按它落位。这个地址有个学名,叫 VMA(virtual memory address、虚拟内存地址),符号们呢——"以为自己住在哪"。跟它做伴的、还有一个 LMA(load memory address、加载内存地址),说的是"装载工具把这一段实际搬到哪"。本站咱们用不到两家分家——MBR 由 BIOS 从镜像头直接读进 `0x7C00`,住在哪、以为住哪、是同一个数——往后等到内核搬去高地址住的那几站、这对概念会真刀真枪派上用场,名字咱们在这儿就记下了。

`.text` 行里有个小机关:`*(.text.boot)` 排在 `*(.text*)` 前面、`.text.boot` 是桩自己声明的专属段,咱们把最前面的位置留给它:BIOS 从偏移 0 开始执行,入口桩必须一字不差落在扇区开头——这一行的次序,就是"谁打头"的白纸黑字。往后 `.rodata` 只读数据、`.data` 带初值的数据,`.bss` 没初值的数据、各就各位——上一节说 DAP 的初值躺进 `.data` 被镜像带进场,说的就是它。

## 512 字节红线,砌进构建里

接下来是整份脚本里笔者最喜欢的一行:

```ld
    ASSERT(_mbr_end <= 0x7C00 + 510, "MBR exceeds 510 bytes")
```

咱们看它管的是什么。`_mbr_end` 是咱们自己立的符号、紧跟在 `.bss` 后面、指向"代码和数据到此为止":拿它跟 `0x7C00 + 510` 一比,就是在问:全部家当、超没超过 510 字节?为什么是 510,不是 512?因为末尾两个字节有主了——`0x1FE` 和 `0x1FF` 两个位置,按约定必须躺着头尾相反的两个字节 `55 AA`、BIOS 就认它当签名。您看脚本再往下两行:`. = 0x7C00 + 510` 把位置计数器直接推到签名位、`SHORT(0xAA55)` 写下一个两字节值。这里有个小端序的把戏:值 `0xAA55` 按 x86 的小端习惯写进文件、低字节在前,磁盘上躺出来的顺序正好是 `55 AA`。BIOS 检查的就是两个字节、不多不少。

ASSERT 之后再这么一垫、bin 拿 objcopy 剥了出来,永远是整 512 字节、签名永远在位了。可这行 ASSERT 真正的价值、不在"垫齐",在它断了后路:**代码加到超线、链接当场报错**。您今天手一滑,往 MBR 里多塞了两个字符串,构建信息里直接甩您一句 `MBR exceeds 510 bytes`,改在哪、为什么、一清二楚。咱们拿现在这版 mbr.cpp 量过:家当拢共 333 字节、510 的预算用了不到三分之二、余量可观,可超标的通道,从今天起就是关死的。

![MBR 扇区的 512 字节布局](assets/mbr-sector-layout.drawio)

为什么对"超标"这么狠?因为它背后的死法太阴了。您想,MBR 超过了 512 字节,超出去的部分、根本不会被读进内存——BIOS 只搬一个扇区。可代码不知道啊、它照样往那个方向 `call`,一头扎进一块**从没加载过的内存**。跑没加载的代码、行为没有任何承诺:可能当场崩、可能跑出鬼来。而且您在源代码层面完全看不出问题——代码逻辑对得很,就是整体超重了。当年第一遍没这道闸、同样的死法笔者原汁原味吃过。现在它被一行 ld 脚本挡在了链接期:运行时的事故,提前到构建时的一句报错,怎么算都划算。这其实和上一站把警告当错误是同一路数,能交给机器把关的事、就不押在人眼上。`-Werror` 在编译期替咱们看代码,ASSERT 在链接期替咱们看体积。

## 536 字节的神秘小幽灵:PIE

墙砌好了,咱们看一道更隐蔽的裂缝——它不出在 ld 脚本里、出在"谁在调链接器"。

咱们把时钟拨回做最小实验的那天:手里还是实验的那套源文件和 mbr.ld,咱们只是还没给链接选项添上 `-no-pie`。咱们把它链出来、剥成 bin、一量——536 字节!512 的红线呢?平白多出了 24 字节、可 `ASSERT` 一声没吭,家当明明在 510 以内、断言没撒谎:那这 24 字节是从哪冒出来的?大伙跟咱们一起查:拿 objdump 把链接出来的 mbr 可执行文件按段列一遍,答案就躺在清单的最后一行,多出来的是一个叫 `.rel.dyn` 的段,24 字节、不多不少的、正是 536 减 512。而它被安置的地址呢、是 `0x7E00`:签名之后。

这就有意思了、`_mbr_end` 数到 `.bss` 就停,`ASSERT` 只盯着 `0x7C00` 到 `0x7C00+510` 的辖区,签名之后的镜像,压根不在它管的地界里——它不算失职、是够不着。真正的漏洞在 objcopy 那一侧:咱们以为它剥裸字节会"剥到签名位就收手",其实它从低到高,一路剥到最后一个有分配属性的段末尾。`.rel.dyn` 住在 `0x7E00` 往后、签名之后又多剥出了 24 字节。512、就这么变成了 536。

那 `.rel.dyn` 是谁塞进来的?凶手叫 PIE。多数 Linux 发行版的 GCC,默认给用户程序开的**位置无关可执行**(Position Independent Executable),代码不假定自己住在固定地址、运行时可以整体搬家。这对现代系统是好事、可它需要在镜像里带一张"搬家要用的表",`.rel.dyn` 就是咱们抓到的东西。咱们的 ld 脚本没给它登记位置、链接器就自作主张,把它安置在了所有正规段之后。

更气人的是、这毛病有隐身术。咱们做最小实验的时候、链接是拿裸 `ld` 手工敲的、裸 ld 不开 PIE,出来的 bin 干干净净的 512 字节、实验一路绿灯。进了 CMake,情形变了:CMake 拿 gcc 驱动去链接,发行版的 gcc 默认带 PIE、毛病这才现形、同一份代码、同一份链接脚本,换个人调用、结果不同了。工具链默认值里藏的裂缝,您踩过一次,一辈子忘不了。

咱们再做一层对照,拿现在这版家当 333 字节的 mbr.cpp:把 `-no-pie` 删掉再链接,这回轮不到 objcopy 出场了。报错的换成了链接器、一步抢在前头,喊了一声段重叠:PIE 带来的 `.dynamic` 撞进了 `.sig` 的地界,当场就罢工了。您看,家当更大的代码,这毛病换了个凶相。偏偏是最小实验量级的小不点,它才静悄悄漏出一个 536 给咱们看。

修法一行:链接选项里明写 `-no-pie`、咱们就是要在 `0x7C00` 住到死,位置无关的好意,咱们心领、用不上。顺带说破一层:这道裂缝也提醒了咱们,`ASSERT` 的辖区只到 510 字节红线,签名之后的世界它不管。堵这道缝的路子不止一条——咱们在 `/DISCARD/` 里把 `.rel*`、`.dyn*` 一家子挨个收编,也能收回一个干干净净的 512。咱们选 `-no-pie`,图的是从源头就不让这些孤儿段出生,省得咱们追着名单一个个清点。咱们再补一条 `-Wl,--build-id=none`:gcc 驱动默认还会往输出里塞一节 build-id 备注,咱们脚本里 `/DISCARD/` 已经把它丢弃了、可丢弃要惹一声链接警告,与其在下游捡垃圾、不如在源头就不生产了。

## 产物要被构建系统认领

链接和 objcopy 都站住了,咱们还剩最后一环:把这些动作组织成 CMake 目标。这里有一个笔者亲历、当场改掉的写法陷阱,值得原样讲给您。

objcopy 出 `mbr.bin` 这个动作、最顺手的挂法是 `POST_BUILD`:在 mbr 可执行文件构建完了之后,把 bin 剥出来的命令也追加一条。写出来能跑、绿了、皆大欢喜。可它有个看不见的窟窿:make 的眼里、只有那个可执行文件的**新旧**——可执行文件没变,它就认为万事太平了,谁还记得 bin 在不在?您哪天手滑 `rm` 掉了 `mbr.bin`、再跑构建,make 一看 mbr 挺新的,直接宣布无事发生——bin 没了就是没了。您想让 bin 重生、得去 touch 一下源文件,骗 make 说"我变了"、那是拿症状当药方。

正解、是让 `mbr.bin` 自己成为依赖图里的一等公民。CMake 给咱们备好了路子:`add_custom_command` 的 OUTPUT 形态。咱们声明"命令的产出是 mbr.bin、它依赖 mbr 可执行文件",咱们再立一个目标认领产出:

```cmake
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/mbr.bin
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:mbr>
            ${CMAKE_CURRENT_BINARY_DIR}/mbr.bin
    DEPENDS mbr
)

add_custom_target(mbr_bin
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/mbr.bin
)
```

这么一来,make 直接跟踪 `mbr.bin` 这个文件本身:文件不在了、不管 mbr 新不新,生成命令都得重跑——咱们实测过最狠的情形:把 `mbr.bin`、`stage2.bin`、磁盘镜像三个产物全删了、一条 `run` 命令下去,全链自愈、该重生的重生、该拼的拼、一点不含糊。这中间的分别,往后再添产物时用得上:bin 一类的二级产物靠 OUTPUT 记户口。boot_image 那样的拼装目标不在此列——它每次全量重拼,丢了也不怕。POST_BUILD 是庆典上的仪式,OUTPUT 才是依赖图上的户口。

## 拼一块能启动的盘

bin 到手、咱们最后拼盘。`boot_image` 目标干的活、三刀下去:

```cmake
add_custom_target(boot_image
    COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_CURRENT_BINARY_DIR}/cinux.img
    COMMAND dd if=/dev/zero of=${CMAKE_CURRENT_BINARY_DIR}/cinux.img bs=1M count=1 status=none
    COMMAND dd if=${CMAKE_CURRENT_BINARY_DIR}/mbr.bin
            of=${CMAKE_CURRENT_BINARY_DIR}/cinux.img bs=512 count=1 conv=notrunc status=none
    COMMAND dd if=${CMAKE_CURRENT_BINARY_DIR}/stage2.bin
            of=${CMAKE_CURRENT_BINARY_DIR}/cinux.img bs=512 seek=1 conv=notrunc status=none
    DEPENDS mbr_bin stage2_size_check
)
```

头一刀、拿一个 1MB 的全零文件打底,咱们管它叫"硬盘"。第二刀、把 mbr.bin 的 512 字节原样盖进第 0 扇区。咱们第三刀最有意思、`seek=1`、把 stage2.bin 从**第 1 扇区**开始写——扇区号从 0 数,第 1 扇区、就是紧贴 MBR 的下一格。您拿它跟上一节对一对:DAP 里 `lba = 1`,读的就是这里。`ljmp` 的目标 `0x7E00`,正好是它被读进内存后的落脚点。镜像侧、参数侧、跳转侧、三处说的都是同一件事、而它们共同的源头、是 `layout.hpp` 里那几个常量,单一来源四个字落到了实处、就是这个样子。

这里同时跨了构建期、磁盘布局和运行时内存，光在脑子里对三套地址容易串线。动画把同一份 stage2 从源码一路送到 `0x7E00`，最后才交给 `ljmp`:

<Anim id="boot-image-pipeline" />

## 最后一道闸:stage2 不许超重

MBR 有 512 字节的死线,stage2 也有——咱们给它分的预算、是 4 个扇区、2048 字节。眼下它只有五十几个字节、可它会长大的:往后几站它要接管探内存、配显示、切模式的活,个子蹿起来是迟早的事。超重了会怎样?`boot_image` 的 `dd` 可不管多长、一律往后写、stage2 一旦越过 2048 字节,多出来的部分会悄悄写进镜像上更远的地方,而 MBR 还是只读 4 个扇区:stage2 被拦腰截断,跑起来又是一桩没有现场的悬案。

所以笔者把闸提前立上了——`cmake/boot/stage2_size_check.cmake`、全文六行:

```cmake
file(SIZE "${SIZE_FILE}" bytes)
math(EXPR sectors "(${bytes} + 511) / 512")
if(sectors GREATER MAX_SECTORS)
    message(FATAL_ERROR
        "stage2.bin is ${bytes} bytes (${sectors} sectors), exceeds kStage2Sectors=${MAX_SECTORS} (${MAX_SECTORS} * 512 bytes); raise kStage2Sectors in boot/layout.hpp and the MBR DAP follows")
endif()
```

量 bin 的字节数、折成扇区数、跟预算一比、超了就报错——有两处讲究。一处是报错文案:它不光说"超了",还把修法写了出来——去 `layout.hpp` 抬高 `kStage2Sectors`、DAP 会自动跟上。把修复指引写进报错、是给三个月后抓耳挠腮的您自己留的路标。另一处更妙、`MAX_SECTORS` 这个预算数、是 CMake 在配置期从 `layout.hpp` 里**用正则抓出来的**——`boot/CMakeLists.txt` 读入头文件文本,匹配 `kStage2Sectors = ` 后面的数字。为什么不直接在 CMake 里另写一个 4?因为一旦写了两份,总有一天您改了头文件,忘了改脚本、闸门的标尺就跟代码里的实情对不上了。抓过来的,永远是一家的话。

## 旗子的家

还剩一件事收尾——咱们这一路添的旗子不少:`-m16` 是新面孔,`-ffreestanding`、`-fno-exceptions` 这一类选项是上一站的老朋友,往后 32 位、64 位的引导件还要各自成套,旗子不能散落在各个 `CMakeLists.txt` 里抄来抄去、武器库里咱们把警告旗封装成 `cinux_warnings` 一个可链接目标、谁链谁带上、这里原样沿用那个手法、给三种模式各立一个家:

```cmake
foreach(mode 16 32 64)
    add_library(cinux_boot${mode} INTERFACE)
    target_compile_options(cinux_boot${mode} INTERFACE
        -m${mode}
        -Os
        -ffreestanding
        -fno-pie
        -fno-pic
        ...
    )
endforeach()
```

`cinux_boot16`,就是本站所有引导源文件链的那套配置:编 16 位代码、按体积优化,独立环境全家桶——往后 32 位的引导件链 `cinux_boot32`、64 位的引导件链 `cinux_boot64`,都是从同一个模具里出来的。哪些旗子各管什么,前置卷的[工具链课](../../primer/01-toolchain/)逐面讲过,咱们不重开课——全套里老朋友居多,新面孔除了 `-m16`,还有 `-fno-pic`、`-mgeneral-regs-only` 几位——各守各的摊,咱们走到用得上的站点再说,不在这儿逐面细数了。

墙砌到这里、构建链条上超标的、丢产物的,都有机器拦着了,还差最后一步:把 QEMU 拉起来、让机器真的跟咱们说句话。
