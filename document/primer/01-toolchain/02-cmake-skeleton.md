---
title: 02 · CMake 裸机骨架
---

# 02 · CMake 裸机骨架:让构建系统相信"没有操作系统"

> 这一章我们把 Cinux 的 CMake 骨架拆给你看:一个 toolchain file 怎么骗过 CMake 让它不再去找 libc、顶层 `project` 为什么要把 ASM 也列进去、裸机标志怎么靠 PUBLIC/PRIVATE 沿着 target 继承下去。读完你应该能对着仓库里的四个文件自己解释每一行。

## 这一章要解决什么

普通的 CMake 教程默认你是在写**应用程序**——目标平台是 Linux 或 Windows,有 libc、有动态链接器、`add_executable` 出来直接能跑。Cinux 不是。我们要编译的东西最终被 `objcopy` 抽成裸二进制塞进磁盘扇区,在 QEMU 里被 BIOS 当一段数据读进来执行。这套流程里**根本没有操作系统参与构建**:没有 `libc.so`、没有 `crt0.o`、没有 `main` 的标准入口、链接器脚本是我们自己手写的。

CMake 不知道这些。它配置时会去 introspect 宿主机:探测 `cc` 能不能编译一个会 `printf` 的小程序、链接器默认输出什么格式、目标平台有哪些库。如果我们什么都不交代,它就会拿宿主 Linux 的假设往上套——结果就是 `try_compile` 探测一堆根本没有的东西、链接器拼命去找 `libc`、最后生成的 ELF 里混进一堆宿主运行时的垃圾。所以我们干的第一件事,是写一个 **toolchain file**,把目标环境告诉 CMake:你是在给一个"没有操作系统的东西"编译。

这一章只讲骨架的三层:

```text
cmake/toolchain-x86_64.cmake   ← 第①层:toolchain,声明目标平台 + 预置裸机标志
            │
CMakeLists.txt (顶层)            ← 第②层:project/option/add_subdirectory
            │
   ┌────────┴────────┐
 boot/CMakeLists.txt  kernel/CMakeLists.txt   ← 第③层:各 target 怎么用这些标志
```

具体每个 target 怎么拼 MBR、怎么塞 Stage2、怎么把 ELF 抽成裸二进制,那是正文 [001 · 实模式引导](../../book/01-boot/001-boot-real-mode.md)的事——这里只把"为什么 CMake 这么写"讲清楚,够你读懂就行。

> 外部依据:CMake 官方手册 [cmake-toolchains(7)](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html) 描述了 toolchain file 的加载时机与 `CMAKE_SYSTEM_NAME` 的语义;[target_compile_options](https://cmake.org/cmake/help/latest/command/target_compile_options.html) 描述了 INTERFACE/PUBLIC/PRIVATE 三档作用域。下面凡是涉及这些外部事实的地方,我们用 blockquote 再点一次权威出处。

---

## 第①层:toolchain file —— 把"没有 OS"写进 CMake 的脑子里

[cmake/toolchain-x86_64.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/toolchain-x86_64.cmake) 整个文件不到 30 行,但每一行都在回答一个问题。我们逐段看。

### `CMAKE_SYSTEM_NAME Generic`:为什么这一个词就关掉了标准库

```cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
```

`CMAKE_SYSTEM_NAME` 是 CMake 对**目标平台**的标识符。它的取值是 CMake 写死的一张表——`Linux`、`Windows`、`Darwin`、`FreeBSD`……每一个已知取值背后,CMake 都挂了一套"针对这个平台的默认行为":去哪找库、链接器怎么调、要不要假定 POSIX。这张表里有一个值专为"什么平台假设都不要做"准备的,就是 `Generic`。

> 外部依据:CMake 官方 [CMAKE_SYSTEM_NAME](https://cmake.org/cmake/help/latest/variable/CMAKE_SYSTEM_NAME.html) 的"System Names Known to CMake"清单里,`Generic` 的描述是 "Some platforms, e.g. bare metal embedded devices"(部分平台,如裸机嵌入式设备)。`Generic-ELF`(3.23 起)是它的 ELF 专用变体。

它的效果不是一行命令式的"关闭标准库",而是一个**推理的结果**:

- 设了 `CMAKE_SYSTEM_NAME` 且它和宿主不一样(宿主是 `Linux`),CMake 就判定这次是**交叉编译**,把 `CMAKE_CROSSCOMPILING` 置真,`try_compile` 这类探测会改变行为。
- `Generic` 这一项背后**没有任何平台模块**(platform module)。CMake 没有"Generic 平台该去哪找 libc"的知识,于是它**不会**去找宿主的 `/usr/lib/libc.so`、不会假定 `pthread`、不会塞 `-lc`。这就从根上断掉了标准库被悄悄链进来的可能。

这就是"为什么 `Generic` 关标准库"——不是它主动关,而是它拒绝假设任何东西,而"假设的东西"里恰恰包含标准库。对我们来说这正合适:内核就是要 `-nostdlib`,什么都不要 CMake 替我们决定。

`CMAKE_SYSTEM_PROCESSOR x86_64` 只是记录目标架构,本身不触发什么动作,但某些内置的编译器探测会读它,留着无害。

### `CMAKE_<LANG>_FLAGS_INIT`:在第一次配置就钉死裸机标志

```cmake
set(CMAKE_C_FLAGS_INIT "
    -ffreestanding
    -fno-stack-protector
    -mno-red-zone
    -mcmodel=kernel
    -Wall
    -Wextra
")

set(CMAKE_CXX_FLAGS_INIT ${CMAKE_C_FLAGS_INIT} "
    -fno-exceptions
    -fno-rtti
    -std=c++17
")

set(CMAKE_ASM_FLAGS_INIT "-Wa,--divide")
```

注意带 `_INIT` 后缀。这是 CMake toolchain 的一个**约定变量**:它的值会在**第一次配置时**被当作 `CMAKE_<LANG>_FLAGS` 的初值。和直接 `set(CMAKE_C_FLAGS ...)` 的区别在于时机——`_INIT` 版本在 `project()` 之前、编译器还没被正式启用时就生效,所以 `try_compile` 这些早期探测就已经带着这些标志跑了。

这里每个标志都有"裸机内核"的理由,我们逐条说 why:

- `-ffreestanding`:告诉 GCC "我处于 freestanding 环境",于是它**不假定**标准库存在,不把 `main` 当特殊入口,也不会因为 `memcpy`/`memset` 这些内建被替换而去找库实现。内核里能用的就是 `<stdint.h>`、`<stddef.h>` 这类纯头文件,STL 容器、异常、RTTI 一概不碰。
- `-fno-stack-protector`:关掉栈金丝雀(`__stack_chk_fail`)。那个 canary 函数在 libc 里,内核没有 libc;留着它链接期就报 `undefined reference`。
- `-mno-red-zone`:x86-64 的 System V ABI 留了一块"红区"(栈顶下方 128 字节,叶子函数可不调整栈指针就用),但**中断/异常处理进内核时硬件会无脑往这块区域压栈**,把红区里的数据踩烂。内核代码必须关掉它。
- `-mcmodel=kernel`:告诉编译器"地址都在高地址(负偏移那一段)",生成的代码用 `kernel` 代码模型寻址,内核地址空间(典型 `-2GB` 那段)才能编出来。这是 64 位内核的标配。
- `-Wall -Wextra`:多报警告,内核这种地方,警告越早暴露越好。

C++ 在 C 的基础上追加两条:

- `-fno-exceptions -fno-rtti`:异常需要 unwind 表和 libstdc++ 运行时,RTTI 需要 vtable 之外的类型信息表,内核统统没有,直接禁掉。这也是本前置卷"只讲 freestanding 子集"的来源——**STL 容器、异常、RTTI、智能指针、虚函数多态这些,内核里要么不能用、要么得自己造轮子,本章不展开**。
- `-std=c++17`:钉死语言版本。

ASM 的 `-Wa,--divide` 是个有意思的小坑:`gas` 在 x86 AT&T 语法下默认把 `/` 当行注释的起始,`--divide` 强制让 `/` 在表达式里仍作除号。我们当前汇编还没大量用到除法表达式,但保留这个标志是为今后算地址/偏移时不必回头踩坑——属于防御性兜底。

### `_LINKER_FLAGS_INIT`:`-nostdlib` 在这里落

```cmake
set(CMAKE_EXE_LINKER_FLAGS_INIT "
    -nostdlib
    -static
")
```

`-nostdlib` 让链接器**不**自动链标准启动文件和标准库(`crt0`/`crt1` 等启动文件、`libc`,乃至 `libgcc.a`),`-static` 防止它去搞动态链接。配合前面 `Generic` 关掉的平台假设,链接期就干净了:谁进 ELF,完全由我们手写的链接脚本(正文 001 里那段 `mbr.ld`/`stage2.ld`)决定。

### `CMAKE_FIND_ROOT_PATH_MODE_*`:别去宿主翻东西

```cmake
set(CMAKE_FIND_ROOT_PATH "")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
```

交叉编译时,CMake 的 `find_*` 命令默认会在"目标平台根"和"宿主根"都找。这几行是说:目标根为空(`""`),库和头文件只在(空的)目标根里找(等于找不到就别瞎链),而**程序**(`find_program`,比如构建期要跑的工具)反过来只在宿主找(`NEVER` 表示"别去目标根找")。Cinux 现在 toolchain 里其实没怎么用 `find_*`,但这是交叉编译 toolchain 的标准护栏,留着防止以后有人误写 `find_package` 把宿主库拉进来。

> 外部依据:[cmake-toolchains(7) — Cross Compiling](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html#cross-compiling) 用一段 Linux ARM 交叉编译的完整示例解释了 `CMAKE_FIND_ROOT_PATH_MODE_*` 这套"该在宿主找还是目标找"的分工。

---

## 第②层:顶层 `CMakeLists.txt` —— project、option、add_subdirectory

[顶层 CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/CMakeLists.txt) 把三个东西摆出来:工程名与语言、一个开关、子目录。

### `project(... LANGUAGES C CXX ASM)`:为什么 ASM 也在里面

```cmake
project(cinux
    VERSION 0.1.0
    LANGUAGES C CXX ASM)
```

`project()` 是 CMake 配置的"真正起点"——`enable_language`/`project` 触发时,CMake 才去启用语言、找编译器、填 `CMAKE_<LANG>_COMPILER`。`LANGUAGES C CXX` 好理解,关键是为什么要写 `ASM`。

因为 Cinux 大量代码是**汇编**:[mbr.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S)、[stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S) 这一整套引导代码,以及内核里和体系结构强相关的部分,都是 `.S`(注意大写 S,表示走预处理器的 GNU 汇编)。如果不把 `ASM` 列进 `LANGUAGES`:

- CMake 不知道怎么编译 `.S`,没有 `CMAKE_ASM_COMPILER`,构建这些源文件时找不到编译器规则。
- `target_compile_options` 那些面向"所有语言"的标志,ASM 目标根本拿不到。

> 外部依据:[cmake-toolchains(7) — Languages](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html#languages) 说明语言由 `project()` 启用,默认只开 `C` 和 `CXX`;要汇编就得显式写 `ASM`(或更细的 `ASM_NASM`/`ASM_MASM`/`ASM-ATT`,GNU as 用 `ASM`)。启用后 CMake 才会去找汇编器并填 `CMAKE_ASM_COMPILER`。

写进去之后,CMake 就知道 `.S` 该用 `gcc` 走预处理再交给 `gas`,toolchain 里那个 `CMAKE_ASM_FLAGS_INIT` 也就有地方接了。顶层最后那段 `message(STATUS "ASM Compiler:${CMAKE_ASM_COMPILER}")` 就是顺手把这个填好的编译器打印出来,确认 ASM 真的启用了。

### toolchain 的回退:`if(NOT CMAKE_TOOLCHAIN_FILE)`

```cmake
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_SOURCE_DIR}/cmake/toolchain-x86_64.cmake"
        CACHE FILEPATH "Toolchain file")
    message(STATUS "Using default toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
endif()
```

toolchain file 通常是在命令行 `-DCMAKE_TOOLCHAIN_FILE=...` 传的。Cinux 这里做了一个"默认值回退":你没传,我就替你填仓库里那个。这样 `cmake -B build -S .` 不带任何参数也能配出来。**但这里有个真实的坑**:打开顶层 [CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/CMakeLists.txt) 对一眼——`project()` 在第 3 行,这个回退 `if` 在第 9 行,**回退在 `project()` 之后**。CMake 的规矩是 `project()` 一执行就去加载 toolchain、探测编译器,严格说 toolchain 该在 `project()` 之前设好。Cinux 这个"先 `project()` 再补设回退值"的写法,靠回退值落进 cache、重新 configure 时才完整生效;所以最稳的姿势仍是命令行显式传:`cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64.cmake`,别完全依赖那个回退。

> 小坑:`CACHE FILEPATH` 那个 `CACHE` 不能省。不写 `CACHE`,这只是个普通变量,只在当前 CMakeLists 范围有效,而且**下次 configure 不会记住**;加了 `CACHE` 才会落进 CMakeCache,后续 `project()`/子目录都看得到。

### `option` + `add_subdirectory`:开关和模块化

```cmake
option(CINUX_GUI "Enable GUI mode" ON)

add_subdirectory(boot)
add_subdirectory(user)
add_subdirectory(kernel)
```

`option(CINUX_GUI "Enable GUI mode" ON)` 定义一个**布尔缓存变量**,默认 `ON`。它的意思是"这个开关可以从命令行覆盖"——`cmake -DCINUX_GUI=OFF ..` 就能关掉 GUI。后面 [kernel/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/CMakeLists.txt) 里真的用到了:

```cmake
if(CINUX_GUI)
    target_compile_definitions(big_kernel_common PUBLIC CINUX_GUI)
    add_subdirectory(gui)
endif()
```

GUI 开着,就给内核目标加一个 `CINUX_GUI` 宏定义、并把 `gui/` 子目录加进来。这就是 `option` 的典型用法:**用一个开关决定要不要编某块代码**。应用开发里 `option` 还常配 `install()`/`find_package`,但本前置卷只到"开关决定编不编"这一层。

`add_subdirectory` 把 `boot`、`user`、`kernel` 三个目录递归纳入构建——每个目录自己有一份 `CMakeLists.txt`,定义自己的 target。这种"顶层只管布局、子目录各自管 target"的结构,正是 CMake 管理多模块工程的惯用法(你的 CMake 跟做笔记里"MathLibs 子目录"也是这个套路)。

最后还有一行值得提:`set(CMAKE_EXPORT_COMPILE_COMMANDS ON)`——它让 CMake 额外生成一份 `compile_commands.json`,这是 `clangd`/IDE 自动补全的输入。内核项目源文件多、include 路径绕(后面你会看到 `target_include_directories` 指了好几层根),没有这个文件,编辑器补全基本是瞎猜。开它一行,受用全程。

---

## 第③层:裸机标志怎么塞 —— `-Wa,--32` 与 PUBLIC/PRIVATE 继承

到这里,toolchain 提供的是"所有 target 共有的底线"。但不同 target 还需要各自专属的标志——`boot` 和 `kernel` 的位数、链接方式完全不一样。这就是 `target_*` 系列命令登场的地方。

### 为什么需要 `-Wa,--32`:把 16 位代码编成 32 位对象

[boot/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt) 里反复出现这一行:

```cmake
target_compile_options(mbr PRIVATE
    -Wa,--32                    # Assemble as 32-bit (allows 16-bit code)
)
```

`-Wa,--32` 的意思是"把这个选项透传(`-Wa,`)给汇编器(`as`)的 `--32`":**让汇编器产出 32 位 ELF 对象**。这看着有点拧巴——MBR 明明是 16 位实模式代码,为什么要编成 32 位对象?

因为这里的 16 位是**逻辑上的**,不是对象文件格式上的。`mbr.S` 里有 `.code16` 这类汇编伪指令,告诉 `gas` "请按 16 位指令编码生成机器码";但**输出的对象文件本身**可以是 32 位的 ELF(`elf_i386`),两者不冲突。链接器(`-Wl,-m,elf_i386`)也按 32 位 ELF 来链。这样的好处是:16 位/32 位/64 位的代码片段(Stage2 里切换长模式那段就是 `.code16`/`.code32`/`.code64` 混着的)可以编进**同一种**对象格式,统一用一套链接流程处理,而不是为每种 CPU 模式各搞一套。

> 注意:这个细节和正文 001 强相关——`mbr` 用 `-Wl,-m,elf_i386` 链成 32 位 ELF、再用 `objcopy -O binary` 抽成裸二进制、最后写进磁盘扇区 0,整套拼装流程在 [001 · 实模式引导](../../book/01-boot/001-boot-real-mode.md)的"设计图/代码路线"里有完整的图。这里只解释 CMake 这几个标志**为什么这么写**,不重复流程。

`PRIVATE` 这里的作用是"这些标志只给 `mbr` 自己用"。同一个文件里的 `boot_common`、`boot_longmode`、`stage2` 各自带自己的 `-Wa,--32`(因为它们也是汇编目标),互不串味。如果把 `mbr` 改成 `PUBLIC`,这些标志会通过 `INTERFACE_COMPILE_OPTIONS` 暴露给任何链了 `mbr` 的目标——可 `mbr` 没人链,所以这里写 `PUBLIC`/`PRIVATE` 行为一样,但语义上 `PRIVATE` 更准确:这是 mbr 自己的汇编设定,不是给别人用的接口。

### PUBLIC 才会继承:看 `big_kernel_common`

要体会 PUBLIC/PRIVATE 的真正区别,得看 [kernel/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/CMakeLists.txt)。它定义了一个 `OBJECT` 库,把裸机标志**有意声明成 PUBLIC**:

```cmake
set(BIG_KERNEL_COMPILE_OPTIONS
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    ...
)

add_library(big_kernel_common OBJECT)

target_compile_options(big_kernel_common PUBLIC ${BIG_KERNEL_COMPILE_OPTIONS})

target_include_directories(big_kernel_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}
)
```

然后两个真正出可执行文件的目标用它:

```cmake
add_executable(big_kernel main.cpp)
target_sources(big_kernel PRIVATE $<TARGET_OBJECTS:big_kernel_common>)
target_link_libraries(big_kernel PRIVATE big_kernel_common)
```

这里的门道在于:**`big_kernel_common` 自己不产代码**(`OBJECT` 库,源由各子目录 `target_sources` 往里塞),真正要编出来的是 `big_kernel`。那 `big_kernel_common` 上那串 `PUBLIC` 标志怎么就到了 `big_kernel`?

靠 `target_link_libraries(big_kernel PRIVATE big_kernel_common)` 这条链接关系。CMake 的"使用需求(usage requirements)"机制规定:

- `PRIVATE` 标志只填进目标自己的 `COMPILE_OPTIONS`,**不外传**。
- `PUBLIC`/`INTERFACE` 标志额外填进 `INTERFACE_COMPILE_OPTIONS`,**会传给链了它的目标**。

所以 `big_kernel_common` 把 `-ffreestanding` 等声明成 `PUBLIC`,再被 `big_kernel` 链上,这些标志就**自动出现在 `big_kernel` 的编译命令里**。同理 `target_include_directories(... PUBLIC ...)` 让头文件搜索路径也一并继承过去。这正是为什么要用 `target_*` 而不是全局 `add_compile_options`/`include_directories`——标志跟着目标走、靠依赖关系传播,谁需要谁拿,不会污染无关目标。

> 外部依据:[target_compile_options](https://cmake.org/cmake/help/latest/command/target_compile_options.html) 原文:"PRIVATE and PUBLIC items will populate the COMPILE_OPTIONS property of `<target>`. PUBLIC and INTERFACE items will populate the INTERFACE_COMPILE_OPTIONS property of `<target>`." 这两句就是 PUBLIC/PRIVATE 继承语义的官方定义。`target_link_libraries` 把被链目标的 `INTERFACE_*` 属性累加到链入方,见 [cmake-buildsystem(7)](https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html)。

对比 `big_kernel` 自己的链接选项,它就老老实实写 `PRIVATE`,因为这些是它自己的链接设定,不该外传:

```cmake
target_link_options(big_kernel PRIVATE ${BIG_KERNEL_LINK_OPTIONS})
```

`BIG_KERNEL_LINK_OPTIONS` 里是 `-T linker.ld -nostdlib -static -no-pie`——自己手写的链接脚本、关标准库、禁止 PIE,清清楚楚。

---

## 三层怎么串起来的

把三层的关系画出来,你就明白为什么 Cinux 这么拆:

```text
toolchain-x86_64.cmake
  └─ CMAKE_SYSTEM_NAME=Generic  → CMake 不假设任何平台/库 → 关掉标准库假设
  └─ *_FLAGS_INIT              → 第一次配置就钉死 -ffreestanding/-nostdlib ...
        │ (project() 时生效)
顶层 CMakeLists.txt
  └─ project(... C CXX ASM)    → 启用三种语言,汇编才有编译器规则
  └─ option / add_subdirectory → 布局 + 开关
        │
boot/、kernel/CMakeLists.txt
  └─ target_compile_options PRIVATE  → 这个 target 独有的(-Wa,--32)
  └─ target_compile_options PUBLIC   → 沿依赖链继承给消费者(big_kernel_common)
```

一个标志最终落到哪条编译命令上,是这三层叠加的结果:toolchain 提供全工程底线、顶层 `add_compile_options`(`-Wall -Wextra`,全局)、子目录的 `target_*` 再加 target 专属或可继承的部分。这种分层让"裸机内核"这种约束极强、又分了"引导(16/32/64 混编)"和"内核(纯 64)"两套截然不同编译需求的项目,还能用同一份 CMake 表达清楚。

---

## 自检:别把应用开发的 CMake 习惯带进来

裸机 CMake 和应用 CMake 的分水岭,集中在几个点上,我们逐条对一遍:

- **不要找 `install()`/`CPack`/`find_package`/`FetchContent`**:内核不"安装"到 `/usr/local`,不打 deb/rpm,不依赖宿主的第三方库。这些是应用开发的工具,本前置卷一律不展开。`CMAKE_FIND_ROOT_PATH_MODE_*` 那几行护栏,正是为了万一有人手贱写了 `find_package`,也翻不到宿主东西。
- **入口不是 `main`**:`-ffreestanding` + 自写链接脚本的 `ENTRY(_start)`,决定了入口由我们自己定。正文 001 里 `mbr.ld` 的 `ENTRY(_start)` 就是这么来的。
- **C++ 只用 freestanding 子集**:STL 容器、异常、RTTI、智能指针、虚函数多态——要么依赖被我们用 `_INIT` 标志关掉的运行时,要么内核根本接不住。本前置卷讲 C++ 时只覆盖"够写内核的最小子集",不碰这些。
- **`OBJECT` 库是核心手段**:`boot_common`、`boot_longmode`、`big_kernel_common` 全是 `OBJECT` 库——它们不产最终文件,而是把一组对象文件(`.o`)攒起来,被多个可执行目标用 `$<TARGET_OBJECTS:...>` 引用。这是内核里复用汇编/公共代码的主要方式,和应用里动辄 `SHARED`/`STATIC` 库不同。

对着仓库核对一遍:`cmake/toolchain-x86_64.cmake`、顶层 `CMakeLists.txt`、`boot/CMakeLists.txt`、`kernel/CMakeLists.txt`——这几样就撑起了 Cinux 的整个构建骨架。正文 [001 · 实模式引导](../../book/01-boot/001-boot-real-mode.md)里 `boot/CMakeLists.txt` 怎么把 MBR 和 Stage2 拼成 `cinux.img`,是建在这副骨架之上的下一步,需要时再跳过去看。

---

### 参考

- CMake 官方手册 — [cmake-toolchains(7)](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html)(toolchain file 的加载时机、`CMAKE_SYSTEM_NAME`/`CMAKE_<LANG>_FLAGS_INIT`/`CMAKE_FIND_ROOT_PATH_MODE_*` 语义)、[CMAKE_SYSTEM_NAME](https://cmake.org/cmake/help/latest/variable/CMAKE_SYSTEM_NAME.html)(`Generic` = "bare metal embedded devices"、已知平台名清单)、[target_compile_options](https://cmake.org/cmake/help/latest/command/target_compile_options.html)(INTERFACE/PUBLIC/PRIVATE 作用域)、[cmake-buildsystem(7)](https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html)(usage requirements 沿依赖链传播)。
- GCC 手册 — `-ffreestanding`(freestanding 环境,不假定标准库)、`-mno-red-zone`/`-mcmodel=kernel`(x86-64 内核代码模型)、`-fno-exceptions -fno-rtti`(关闭异常/RTTI 运行时):https://gcc.gnu.org/onlinedocs/gcc/。
- 本仓库源码:[toolchain-x86_64.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/toolchain-x86_64.cmake)、[顶层 CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/CMakeLists.txt)、[boot/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt)、[kernel/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/CMakeLists.txt)。
- 详见正文跳转:[001 · 实模式引导](../../book/01-boot/001-boot-real-mode.md)(MBR/Stage2 的链接脚本、`objcopy` 抽裸二进制、拼装 `cinux.img` 的完整流程)。
