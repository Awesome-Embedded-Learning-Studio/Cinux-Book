---
title: 03 · 目标 / 链接脚本 / ELF→裸镜像
---

# 03 · 目标 / 链接脚本 / ELF→裸镜像:把代码"摆"到它该在的地址

> 一句话:讲清楚 Cinux 怎么用 OBJECT 库在多个目标间共享源码、用 `file(WRITE)` 现场生成链接脚本、用 objcopy 把 ELF 抽成裸镜像、用 `embed_binary.sh` 把任意二进制塞进内核。读完你能无障碍看懂正文 [001 · 实模式引导](../../book/01-boot/001-boot-real-mode.md) 里"MBR 链接在 `0x7C00`、Stage2 链接在 `0x8000`、再 objcopy 成 `.bin`"那套构建逻辑。

## 为什么前置卷要专门讲这个

正文 001 开篇就甩出一个反直觉的事实:我们写的第一段代码,**必须**落在内存的 `0x7C00`,而且**最终交给磁盘的不是 ELF,而是一个没有任何文件头的裸二进制(`.bin`)**。这跟我们平时写 Linux 程序完全相反——平时 gcc 帮我们把代码链接到某个"默认"地址,产物是一个带文件头的 ELF,操作系统加载器会按文件头里的程序头(Program Headers)把它摆好。

裸机没有操作系统,没有人帮我们"摆地址",也没有人帮我们"解析 ELF 文件头"。所以我们必须自己干两件平时 gcc 偷偷替我们做的事:

1. **告诉链接器每一段代码该摆在哪个地址**(链接脚本);
2. **把链接好的 ELF 抽成纯字节流**(objcopy),因为 BIOS 只认"原封不动的一坨字节"。

而在这之前还有个现实问题:Cinux 的 boot 阶段,MBR 和 Stage2 共享同一份 `common/serial.S`;内核阶段,生产内核和测试内核共享同一堆驱动源码。我们得有一套机制,让**同一批源文件只编译一次,却能被多个目标各取所需地链进去**。这就是 OBJECT 库干的事。

这一章就是围绕这三件事:OBJECT 库共享源、链接脚本定地址、objcopy 抽裸镜像(外加一个"把任意二进制嵌进 ELF"的进阶技巧)。

> 外部依据:GNU ld 手册的 "Linker Scripts" 一章定义了 `SECTIONS`/`AT()`/`PROVIDE`/`KEEP`/`ASSERT` 的语义;GNU objcopy 手册定义了 `-O binary`/`--rename-section`/`--redefine-sym`;OSDev 的 "Embedding binary data" 页总结了把二进制嵌进内核对象文件的社区做法。

## 一、OBJECT 库:一份源码,喂给多个目标

先看 boot 阶段最典型的场景。`boot/CMakeLists.txt` 里有一段:

```cmake
# Compile common/serial.S as an object file for inclusion
add_library(boot_common OBJECT
    common/serial.S
    common/boot.S
)
```

`add_library(名字 OBJECT ...)` 创建的是一个 **OBJECT 库**。它和静态库(`STATIC`)、动态库(`SHARED`)本质不同:**OBJECT 库不产 `.a`/`.so`,它只负责"把这几个源文件编译成 `.o`,摆在那儿"**。谁来用,谁就把这些 `.o` 直接链进自己——不经 `ar` 打包,也不经 `ld` 二次链接成库。

为什么要这样?因为 `common/serial.S` 里的 `print_string`、`common/boot.S` 里的工具函数,**MBR 想用、Stage2 也想用**。如果把它们写成普通静态库,再用 `ar` 打包,反而绕;OBJECT 库就是为"共享一份编译产物"量身做的。用法是一条生成器表达式:

```cmake
add_executable(stage2
    stage2.S
    $<TARGET_OBJECTS:boot_common>       # 把 boot_common 的 .o 全链进来
    $<TARGET_OBJECTS:boot_longmode>     # 再链一份 long mode 的
)
```

`$<TARGET_OBJECTS:boot_common>` 在生成阶段展开成 `boot_common` 编译出来的那几个 `.o` 文件的路径,等价于把它们直接写进 `add_executable` 的源列表。注意它和 `target_link_libraries(stage2 PRIVATE boot_common)` 是**两回事**:

- `$<TARGET_OBJECTS:...>` —— 真的把对象文件**链进**当前目标(代码进可执行文件);
- `target_link_libraries(... boot_common)` —— 对 OBJECT 库而言,主要是**继承编译选项、include 目录等 PUBLIC 属性**,对象文件本身**不会**因此自动进来。

所以你会看到 [boot/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt) 里 Stage2 用 `$<TARGET_OBJECTS:` 拉代码;而到了内核侧,`mini/CMakeLists.txt` 把这两步**都做了**,缺一不可:

```cmake
# 共享对象库,源文件只在一处维护
add_library(mini_kernel_common OBJECT
    arch/x86_64/boot.S
    arch/x86_64/gdt.cpp
    # ... 一长串共享源 ...
)

# 拉对象文件(代码进 mini_kernel)
target_sources(mini_kernel PRIVATE $<TARGET_OBJECTS:mini_kernel_common>)
# 继承 PUBLIC 属性(编译选项、include 目录)
target_link_libraries(mini_kernel PRIVATE mini_kernel_common)
```

big kernel 那边同样如此:[kernel/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/CMakeLists.txt) 用 `add_library(big_kernel_common OBJECT)` 开一个空 OBJECT 库,让各子目录(`arch/`、`drivers/`、`mm/` …)用 `target_sources()` 往里填源文件,然后生产内核 `big_kernel` 和测试内核 `big_kernel_test` 各自 `$<TARGET_OBJECTS:big_kernel_common>` 取用。**生产内核和测试内核共享同一份源码、同一套编译选项,只是入口和测试用例不同**——这正是 OBJECT 库的价值。

> 一个容易踩的点:MBR 故意**不**链 `boot_common`。原因正文 001 讲透了——MBR 只有 512 字节预算,链进 `serial.S`/`boot.S` 那些函数很容易撑爆,而 BIOS 只加载第 0 扇区那 512 字节,多出来的根本进不了内存。所以 MBR 只链 `mbr.S`,要打印就用自己的极简 `print_string_mbr`。详见正文 001 第 4 节。

## 二、链接脚本:把地址钉死,并用 `file(WRITE)` 现场生成

链接器 `ld` 默认有一套布局规则(把 `.text` 放某处、`.data` 放某处……)。但裸机里默认规则几乎全是错的:它会把代码摆在 `0x400000` 之类,而 BIOS 要求 MBR 在 `0x7C00`。所以我们要**自己写链接脚本**,逐段指定地址。

Cinux 没有把链接脚本写成 `.ld` 文件提交进仓库(内核那个 `kernel/linker.ld` 是例外,因为它是手写的复杂脚本),boot 阶段的脚本是用 CMake 的 `file(WRITE)` **在构建目录里现场生成**的。看 `boot/CMakeLists.txt`:

```cmake
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/mbr.ld
"
OUTPUT_FORMAT(\"elf32-i386\")
ENTRY(_start)
SECTIONS
{
    . = 0x7C00;
    .text : {
        *(.text)
        *(.rodata)
    }
    .data : { *(.data) }
    .bss  : { *(.bss) }
    /DISCARD/ : { *(.comment*) *(.note*) }
}
")
```

`file(WRITE 路径 "内容")` 在 configure 阶段把字符串原样写到文件里。这段脚本干了四件最关键的事:

- **`OUTPUT_FORMAT("elf32-i386")`** —— 输出 32 位 i386 ELF。注意 boot 的汇编里既有 16 位实模式代码(MBR、Stage2 早期),也有 32/64 位代码(Stage2 进保护/长模式的那段,见 `boot_longmode` 库),但**对象文件**统一按 32 位 ELF 链接(`-Wa,--32`),16/32/64 位的切换由汇编器里的 `.code16`/`.code32`/`.code64` 指令在代码内部完成。链接器的 ELF 位宽和代码的实际运行位宽是两回事。
- **`ENTRY(_start)`** —— 告诉 ELF 文件头"入口符号是 `_start`"。对裸机没啥直接用(BIOS 才不管你的 ELF 头),但留着便于 objcopy 定位、便于调试。
- **`. = 0x7C00;`** —— **这是整段脚本的核心**。`.` 是"位置计数器",代表"当前摆到哪个地址了"。把 `. = 0x7C00` 意味着从这一行起,后续所有段的虚拟地址(VMA)都从 `0x7C00` 开始往上排。于是 MBR 里 `msg_booting` 这类标号链接后得到的地址就在 `0x7C00` 附近,和 BIOS 把 MBR 实际加载到内存的物理位置吻合。这就是正文 001 反复强调的"标号算出来的偏移要和访问它的段对得上"的物质基础。
- **`/DISCARD/`** —— 把 `.comment`、`.note` 这些编译器塞进来的元数据段丢掉,不进镜像。裸机一个字节都不能浪费,这些对运行没用。

Stage2 的脚本([boot/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt) 第 103–137 行)多两样东西,值得单独讲:

```cmake
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/stage2.ld
"OUTPUT_FORMAT(\"elf32-i386\")
ENTRY(_start)
SECTIONS
{
    . = 0x8000;
    .text : { *(.text) }
    .gdt ALIGN(8) : { *(.gdt) }     # GDT 必须 8 字节对齐
    ...
    . = 0x10000;
    ASSERT(. <= 0x10000, \"stage2 too large! exceeds 64KB\")
    /DISCARD/ : { *(.comment*) *(.note*) }
}
")
```

两个新东西:

- **`.gdt ALIGN(8)`** —— GDT(全局描述符表,正文 002 讲)是 CPU 硬件要求按 8 字节对齐的结构,这里用 `ALIGN(8)` 保证它落在 8 的倍数地址上。这属于"硬件契约",不是软件偏好。
- **`ASSERT(. <= 0x10000, \"stage2 too large! exceeds 64KB\")`** —— 一个**编译期断言**。`.` 此刻的位置如果超过了 `0x10000`(**64KB**),链接**直接失败**并打印这条信息。这是裸机开发的"防呆":Stage2 在磁盘上的扇区预算是有限的(正文 001 磁盘布局里给到扇区 1..15,约 7.5KB;这里 ASSERT 卡的是更宽松的 64KB 上限),写超了就在构建阶段炸出来,而不是跑到机器上莫名其妙死机。比"MBR 超 512 字节"那种阴间 bug 友好得多。

> 为什么 MBR 的 `.text` 里把 `.rodata` 也并进来,而 Stage2 单独分了 `.rodata`?因为 MBR 要极致紧凑,能合就合;Stage2 没那么挤,分清楚更利于后面 objcopy 和调试。这些是工程取舍,没有对错。

### 进阶:VMA 与 LMA,以及 `AT()`

到内核侧,[kernel/linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/linker.ld) 脚本复杂得多,引入了一个 boot 阶段用不到、但理解内核加载必须懂的概念:**VMA 和 LMA 的分离**。看脚本顶部和 `.text` 那段:

```ld
KERNEL_VMA   = 0xFFFFFFFF80000000;   /* higher-half virtual base */
KERNEL_LMA   = 0x1000000;            /* physical load address (16 MB) */

SECTIONS
{
    . = KERNEL_VMA + KERNEL_LMA;

    .text : AT(ADDR(.text) - KERNEL_VMA) ALIGN(4096) {
        *(.text.start)         /* _start MUST be first */
        *(.text .text.*)
        *(.rodata .rodata.*)
    }
    ...
}
```

- **VMA(Virtual Memory Address,虚拟地址)** —— 链接器给段、给符号分配的"逻辑地址",也就是代码里取一个标号、`objdump` 看到的那个地址。
- **LMA(Load Memory Address,加载地址)** —— 这一段**实际应该被加载到物理内存哪个位置**。

平时这俩相等(VMA 在哪,LMA 就在哪),所以多数人没区分过。但在"高半内核(higher-half kernel)"里,它们必须分开:内核代码希望运行时它的虚拟地址落在 `0xFFFFFFFF80000000` 之上(高半区,这样用户进程的低地址空间不被内核占),可它被 mini 内核从磁盘加载进物理内存时,只能放在 `0x1000000`(16MB)这种普通物理地址。`. = KERNEL_VMA + KERNEL_LMA` 让标号的 VMA 从高半区起步;`AT(ADDR(.text) - KERNEL_VMA)` 这个 `AT()` 就是专门指定 LMA 的——它告诉链接器"这一段的**加载地址**等于它的虚拟地址减去 `KERNEL_VMA`",也就是落回物理 `0x1000000`。

> 外部依据:GNU ld 手册 "Optional Sections Attributes" 一节明确,`AT(addr)` 设置段的 LMA;段描述里冒号后的 `AT()` 不写时 LMA 默认等于 VMA。"3.1 Basic Linker Script Concepts" 一节定义了 VMA(`.`/`ADDR`)与 LMA(`LOADADDR`)的区别。

怎么验证链接真的按你想的走了?正文 001 没展开,这里补一个最小验证法——用 `objdump -h` 看每个段的 VMA 和 LMA:

```bash
objdump -h build/boot/mbr
# 期待 .text 的 VMA 列是 0x7C00 附近
```

`objdump -h` 输出里 `VMA` 和 `LMA` 两列对得上你的脚本,就说明地址钉对了;`objdump -r` 看重定位表(链接前 `.o` 里有 `.rel.text`,链接成可执行文件后应该消失),能判断链接是否"有效"。这套工具是核对链接脚本最直接的手段。

## 三、objcopy POST_BUILD:ELF → 裸二进制

链接出来的是 ELF——带文件头、段头表、程序头的"有格式"文件。可 BIOS、可 mini 内核的 ELF loader 之外的大部分裸机加载路径,要的是**没有任何头的纯字节流**。把它俩之间这一步打通的,是 `objcopy -O binary`:

```cmake
# MBR binary
add_custom_command(
    TARGET mbr
    POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mbr> $<TARGET_FILE_DIR:mbr>/mbr.bin
    COMMENT "Converting MBR to raw binary: mbr.bin"
    VERBATIM
)
```

几个点拆开讲:

- **`add_custom_command(TARGET mbr POST_BUILD ...)`** —— 给 `mbr` 这个目标挂一个"构建完成后"的命令。`mbr` 链接成 ELF 之后,自动跑这条 objcopy。
- **`-O binary`** —— 输出格式 `binary`,即"原样字节流,剥掉一切文件头"。objcopy 会按段的 LMA 顺序,把所有 `ALLOC`+`LOAD` 的段内容拼成一坨,从最低 LMA 开始铺。这就是为什么前面链接脚本里 `. = 0x7C00` 那么 crucial:objcopy 不会真的在输出文件开头填 31744 个零字节去"凑"到 `0x7C00`,它只写**段内容本身**,起始偏移对齐到最低 LMA。换句话说,`.bin` 文件第 0 字节 = 链接脚本里最低 LMA 段(这里是 `.text` @ `0x7C00`)的第 0 字节。
- **`$<TARGET_FILE:mbr>` / `$<TARGET_FILE_DIR:mbr>`** —— 生成器表达式,展开成 `mbr` 的产物路径和它所在目录。`mbr.bin` 就落在 `mbr` 旁边。
- **`VERBATIM`** —— 让 CMake **原样**传递命令参数,不对空格/特殊字符做二次转义。这是写 `add_custom_command` 时的好习惯,能避免"参数里有路径含空格,被 CMake 拆成两半"这类隐蔽 bug。mini 内核那段 POST_BUILD 也带 `VERBATIM`:

```cmake
add_custom_command(TARGET mini_kernel
    POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mini_kernel> $<TARGET_FILE_DIR:mini_kernel>/mini_kernel.bin
    COMMENT "Converting mini kernel to flat binary: mini_kernel.bin"
    VERBATIM
)
```

跑完这一步,`build/boot/mbr.bin` 就是那个 512 字节、末尾 `0xAA55` 的 MBR;`build/kernel/mini/mini_kernel.bin` 就是 mini 内核的裸镜像。后面 `scripts/build_image.sh` 再把这些 `.bin` 按扇区拼成 `cinux.img`。

> 注意:big kernel **不走** objcopy 抽裸镜像这条路。它的产物是标准 ELF,由 mini 内核里的 ELF loader 解析 PT_LOAD 段、按程序头里的 `p_paddr`/`p_vaddr` 摆好再跳进去。所以 [kernel/linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/linker.ld) 顶部注释特意写明"Because we are a proper ELF binary (not a flat blob), p_paddr / p_vaddr … determine the load addresses"。这是裸机里两种并存的加载模型:**早期阶段(MBR/Stage2/mini)用裸镜像 + objcopy;能跑复杂代码后(big kernel)用真 ELF + 自己的 loader**。

## 四、反向操作:把任意二进制嵌进 ELF——`embed_binary.sh`

最后讲一个更骚的:把 initrd(初始内存盘,一个 `.tar` 文件)当作数据塞进内核 ELF 里,这样内核一启动就能拿到这个 tar,不用额外读盘。这套手法在 OS 社区叫 "embedding binary data"。

`objcopy` 有个特殊输入格式 `-I binary`,能把**任意文件**当成一个全是数据的"对象文件":

```bash
objcopy -I binary -O elf64-x86-64 -B i386:x86-64 input.tar output.o
```

它生成的 `output.o` 里,整个 tar 的内容被放进一个 `.data` 段,并自动产生三个符号——指向数据起点、终点、长度。**问题来了**:这三个符号的名字是 objcopy 根据**输入文件的绝对路径**推出来的(把 `/`、`.` 等非字母数字字符换成 `_`)。比如输入 `/home/you/build/kernel/data/initrd.tar`,符号可能叫 `_binary_home_you_build_kernel_data_initrd_tar_start` 之类。这名字又长又脆,换台机器、换个构建目录就变,代码里没法稳定引用。

Cinux 用 [scripts/embed_binary.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/embed_binary.sh) 解决这个问题,核心是两步:

**第一步:生成对象文件,顺便把段名改掉。**

```bash
"${OBJCOPY}" \
    -I binary -O elf64-x86-64 -B i386:x86-64 \
    --rename-section .data="${SECTION}",CONTENTS,ALLOC,LOAD,READONLY,DATA \
    "${INPUT}" "${OUTPUT}"
```

`--rename-section .data=.initrd,...` 把默认的 `.data` 段改名成调用者指定的段名(这里传进来的是 `.initrd`)。这样它在链接脚本里能被单独收纳——你看 [kernel/linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/linker.ld) 专门有一段 `.initrd : AT(...) { *(.initrd) }`,就是接住这个改名后的段。后面那串 `CONTENTS,ALLOC,LOAD,READONLY,DATA` 是段属性,告诉链接器"这里有内容、要分配空间、要加载进内存、只读、属于数据类"。

**第二步:把自动生成的丑陋符号名,重定义成稳定前缀。**

```bash
SYM_START=$(nm "${OUTPUT}" | grep '_start$' | awk '{print $3}')
SYM_END=$(nm "${OUTPUT}" | grep '_end$' | awk '{print $3}')
SYM_SIZE=$(nm "${OUTPUT}" | grep '_size$' | awk '{print $3}')

"${OBJCOPY}" \
    --redefine-sym "${SYM_START}=${SYM_PREFIX}_start" \
    --redefine-sym "${SYM_END}=${SYM_PREFIX}_end" \
    --redefine-sym "${SYM_SIZE}=${SYM_PREFIX}_size" \
    "${OUTPUT}"
```

先用 `nm` 把对象文件里以 `_start`/`_end`/`_size` 结尾的符号名抓出来(就是 objcopy 自动起的那三个长名字),再用 `--redefine-sym` 把它们改成调用者要的稳定前缀。Cinux 调用时传的前缀是 `_binary_initrd`,所以最终内核代码里能稳稳地引用 `_binary_initrd_start`、`_binary_initrd_end`、`_binary_initrd_size` 三个符号——不管在哪台机器、哪个构建目录编译都一样。这套符号名也在 [kernel/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/CMakeLists.txt) 顶部的注释里写明了。

### 用 CMake 把这条脚本接进构建图

光有脚本不够,得让 CMake 知道"生成 `initrd.o` 这件事依赖 `initrd.tar` 和脚本本身,且要在链接 big kernel 之前完成"。这正是 `add_custom_command` + `add_custom_target` + `add_dependencies` 的标准组合拳:

```cmake
set(INITRD_TAR ${CMAKE_CURRENT_SOURCE_DIR}/data/initrd.tar)
set(INITRD_OBJ  ${CMAKE_BINARY_DIR}/kernel/initrd.o)
set(EMBED_SCRIPT ${CMAKE_SOURCE_DIR}/scripts/embed_binary.sh)

# ① 声明"怎么生成 initrd.o":跑脚本,依赖 tar 和脚本
add_custom_command(
    OUTPUT  ${INITRD_OBJ}
    COMMAND bash ${EMBED_SCRIPT}
        ${INITRD_TAR} ${INITRD_OBJ} .initrd _binary_initrd
    DEPENDS ${INITRD_TAR} ${EMBED_SCRIPT}
    COMMENT "Converting initrd.tar -> initrd.o (embedded ramdisk)"
)

# ② 把这个 OUTPUT 包成一个有名目标,方便别处挂依赖
add_custom_target(initrd_obj DEPENDS ${INITRD_OBJ})

# ③ 链接 big kernel 前,把 initrd.o 当源文件塞进去,并显式声明依赖
target_sources(big_kernel PRIVATE
    ${CMAKE_BINARY_DIR}/user/user_binary.o
    ${INITRD_OBJ}
)
add_dependencies(big_kernel user_binary_obj initrd_obj)
```

三步各自的角色:

- **`add_custom_command(OUTPUT ...)`** —— 定义"产物 ← 命令"的规则,并声明 `DEPENDS`(输入变了才重跑)。这是增量构建的关键:tar 没动,就不重新生成 `.o`。
- **`add_custom_target(initrd_obj DEPENDS ${INITRD_OBJ})`** —— 造一个**有名字、永远会被视为"需要构建"**的目标。`add_custom_command` 的 OUTPUT 只在"有别人依赖它"时才会触发;包成 custom target 后,这个产物就有了一个可以被 `add_dependencies` 引用的把手。Cinux 对 `initrd_obj`、`user_binary_obj` 都是这么干的。
- **`add_dependencies(big_kernel ... initrd_obj)`** —— **显式**告诉 CMake "big_kernel 依赖 initrd_obj 这个目标",保证链接 big kernel 时,`initrd.o` 一定已经生成。没有这行,`target_sources` 里虽然写了 `${INITRD_OBJ}`,但 CMake 不一定能推出正确的先后顺序,可能出现"链接时 `.o` 还没生成"的竞态。这是自定义产物接进正常构建图最容易漏的一环。

## 调试现场

这套构建链路踩过的坑,挑最典型的两个。

**症状一**——改了链接脚本里的 `. = 0x7C00`,重新 `make`,可代码行为没变。 根因八成是**链接脚本没被重新生成或没被重新读**。`file(WRITE)` 写的是 `${CMAKE_CURRENT_BINARY_DIR}/mbr.ld`,如果 CMake 的 configure 阶段没重跑,这个文件就不会更新。手动 `cmake build/` 重新 configure 一次,或者干脆 `rm -rf build` 重来。判断方法:直接 `cat build/boot/mbr.ld`,确认里面的 `0x7C00` 是不是你刚改的值。

**症状二**——objcopy 抽出来的 `.bin` 文件大得离谱,或者开头有一大片 `0x00`。 这通常是链接脚本里最低 LMA 段的地址写错了,或者某段被标成了 `ALLOC` 但落在极高地址,导致 objcopy 为了"铺满"中间的空洞而填零。核对 `objdump -h` 里每个段的 LMA,确保你想进镜像的段 LMA 连续且从低地址起步;不想进镜像的段(比如 `.bss`、guard 区)要标 `(NOLOAD)`——`kernel/linker.ld` 里的 `.boot_guard (NOLOAD)`、`.stack (NOLOAD)` 就是这么处理的,它们不占镜像文件字节。

还有一个隐蔽的:把 `add_dependencies` 漏了,本地偶尔构建成功、CI 上却随机失败("链接时找不到 `initrd.o`")。这就是上面说的构建图竞态——`add_dependencies` 不是可选的糖,是正确性的保证。

## 详见正文跳转

- **正文 [001 · 实模式引导](../../book/01-boot/001-boot-real-mode.md)**:MBR 为什么必须链接在 `0x7C00`、Stage2 为什么链接在 `0x8000`(以及链接地址和运行时段寄存器怎么配合、才能让标号和访问对得上)、MBR 那 512 字节死线为什么逼得它不链 `boot_common`。这些是本章"链接脚本定地址"在真实代码里的全部动机,正文讲透了。
- **正文 [002 · 进入保护模式](../../book/01-boot/002-boot-gdt-protected.md)**(后续章节):Stage2 链接脚本里那条 `.gdt ALIGN(8)` 到底在给谁铺路——GDT 的 8 字节对齐是 CPU 硬件要求,正文 002 会展开。

---

### 参考

- GNU ld 手册 — "Linker Scripts" 一章:`SECTIONS`/`.` 位置计数器/`AT()`(LMA)/`ALIGN`/`ASSERT`/`PROVIDE`/`KEEP`/`/DISCARD/` 的语义;`OUTPUT_FORMAT`/`ENTRY` 的作用。在线版:https://sourceware.org/binutils/docs/ld/Scripts.html。
- GNU objcopy 手册 — `-O binary`(剥文件头抽裸字节流)、`-I binary`(把任意文件当对象)、`--rename-section`、`--redefine-sym` 的用法与各段属性标志。在线版:https://sourceware.org/binutils/docs/binutils/objcopy.html。
- OSDev — [Embedding binary data](https://wiki.osdev.org/Embedding_binary_data):把二进制嵌进 ELF 对象文件、自动生成的符号名问题与社区惯例。
- 本仓库源码:[boot/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt)(OBJECT 库 / `file(WRITE)` 生成 mbr.ld、stage2.ld / objcopy POST_BUILD)、[kernel/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/CMakeLists.txt)(big_kernel_common OBJECT 库 / initrd embed 依赖图)、[kernel/mini/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/CMakeLists.txt)(mini_kernel_common 共享 + flat binary POST_BUILD)、[kernel/linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/linker.ld)(VMA/LMA/`AT()`/`PROVIDE`/`KEEP`/`(NOLOAD)`)、[scripts/embed_binary.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/embed_binary.sh)(`--rename-section` + `--redefine-sym` 两步)。

> 参考 URL 的有效性会在全局审查阶段用 open-websearch(bing)统一核活,与本系列其它章节一致。
