---
title: 02 · 工具链第一课
---

# 02 · 工具链第一课:系统 GCC 凭什么能编内核

> 咱们看的源码对齐 commit `9bf0b05`(2026-09-13)。上一章咱们 grep 到了 `-ffreestanding`、`-mno-red-zone`、`-mcmodel=kernel` 这串 flag,一个都没解释;这一章把它们逐条讲清,顺带回答一个更根本的问题:OSDev 教程几乎都让咱们先编一个 `x86_64-elf-gcc` 交叉编译器,Cinux 为什么敢直接用系统 `gcc`?

## 交叉编译的本质:host 与 target

先看那套"标准建议"在防什么。OSDev 的 [Bare Bones](https://wiki.osdev.org/Bare_Bones) 教程让咱们自建交叉编译器,理由很实:系统 GCC 会隐式链接宿主的 glibc、假设宿主 ABI,给裸机生成的代码根本跑不起来。这个担忧在它的典型场景里成立——在 x86 Mac/PC 上给 ARM、RISC-V 写内核,开发机(host)和目标机(target)指令集、字节序、ABI 全不一样,系统编译器处处拿宿主的习惯当家,必须用一个交叉编译器把它隔离掉。

Cinux 的处境不同:开发机是 x86_64,QEMU 里的目标机也是 x86_64,host 与 target 指令集完全一致。跨架构的问题在这里不存在,剩下的只是"别让编译器假设有操作系统、别让它链宿主的库"——这些约束咱们可以用一串 flag 跟 GCC 讲明白。所以 Cinux 全程用系统 `gcc`/`g++`/`as`/`ld`,教程里不会出现 `x86_64-elf-gcc` 的字样。

## hosted 与 freestanding:内核是没有操作系统的程序

那串 flag 的总纲,是编译理论里的一对词。编译器默认把咱们写的代码当**hosted(托管)**程序:底下有操作系统撑着,链 glibc,`printf`/`malloc` 随便用,栈保护、异常展开这些机制都有运行时兜底。而内核恰恰是**提供这些服务的那一层**,它底下什么都没有——裸机、没有 libc、没有线程库,连"打印"都得自己写。

所以咱们的内核代码要用 **freestanding(独立)**模式编译:编译器收起一切"底下有 OS"的假设,只提供 `<stdint.h>`、`<stddef.h>` 这类纯类型头。`-ffreestanding` 就是切换到这个模式的开关,后面每条 flag 都是在替内核挡掉一个 hosted 世界才有的假设。

## 这串 flag 住在哪:toolchain file 与三个目标

上一章咱们看到,这些 flag 出现在 `compile_commands.json` 里,但它们并不写在 toolchain file 里。先看 `cmake/toolchain-x86_64.cmake` 的真身,它现在只管"环境层"的事:

```cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_ASM_FLAGS_INIT "-Wa,--divide")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib -static")

set(CMAKE_FIND_ROOT_PATH "")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
```

`Generic` 是整份文件里最值钱的一个词,咱们先记住它:它告诉 CMake "目标没有操作系统",于是 CMake 关掉一切按 Linux 程序推断的行为。编译 flag 则按目标分了家,各住各的 `CMakeLists.txt`。大内核这一份(`kernel/CMakeLists.txt`):

```cmake
set(BIG_KERNEL_COMPILE_OPTIONS
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-pie
    -fstack-protector-strong -mstack-protector-guard=global  # F9 batch 6: stack canary
    -mcmodel=kernel
    # Cancel the global _FORTIFY_SOURCE=2 (root CMakeLists CMAKE_CXX_FLAGS_RELEASE)
    # for the freestanding kernel: gcc fortify rewrites memcpy/memset/memmove to
    # __memcpy_chk/__memset_chk/__memmove_chk, which the kernel does not provide
    # (no glibc) -> link failure. Host unit tests keep _FORTIFY_SOURCE (they link
    # glibc and benefit from the overflow checks); kernel objects opt out here.
    -U_FORTIFY_SOURCE
    -mno-red-zone
    -Wall
    -Wextra
```

咱们逐条拆,先挑两个"为什么"最深的。

**`-mno-red-zone`:中断随时会来。** x86_64 给用户态程序的 ABI 有个优化:咱们的函数可以在不动 `RSP` 的情况下,把栈顶下方 128 字节当临时空间用,这块叫红区。用户态没事,内核不行——中断是 CPU 在**任意时刻**打断当前代码的机制(它怎么打断、打断后发生什么,是正文 03 卷的主线),中断处理程序一压栈,就把红区里还没用完的数据盖掉了,留下极难复现的随机崩溃。所以内核代码必须把这 128 字节的"借用"关掉。

**`-mcmodel=kernel`:内核住在地址空间顶端。** 编译器生成地址引用,得先假设咱们的代码和数据大概落在哪个范围。大内核的代码数据普遍在 `0xFFFFFFFF80000000` 一带的高半区(为什么住那儿,正文 05 卷讲地址空间时给完整故事),`mcmodel=kernel` 就是告诉 GCC 按这个住址生成寻址;假设错了,链接出来的绝对地址引用全错。

咱们再看**栈保护**,这一条最能体现 flag 的 why 值得逐条讲。老教程的口径是"内核必须 `-fno-stack-protector`,因为 canary 需要 glibc",Cinux 现在的大内核反而开着 `-fstack-protector-strong`,配 `-mstack-protector-guard=global`:canary 换成内核自己提供的一个全局变量,不依赖任何运行时,防护照吃。紧挨着它的 `-U_FORTIFY_SOURCE` 是同一思路的反向操作:Release 构建全局开了 `_FORTIFY_SOURCE=2`,GCC 会把 `memcpy` 改写成 `__memcpy_chk` 一类的强化版,而内核没有这些符号,直接链接失败,所以内核目标要显式"退订" fortify,主机单测则保留(它们链 glibc,强化检查真实受益)。

`-mcmodel` 一家在仓库里正好三兄弟,同一个问题(代码住哪)给了三种答案,咱们对照着看:

| 目标 | 取值 | 住址假设 |
|---|---|---|
| mini kernel(`kernel/mini/`) | `large` | 不假设:所有地址引用都用完整 64 位形式,loader 被加载到哪儿都能跑 |
| big kernel(`kernel/`) | `kernel` | 高半区 `0xFFFFFFFF80000000` 一带 |
| 用户程序(`user/`) | `small` | 低 2GB 用户区 |

咱们看 mini kernel 为什么走 `large`:引导阶段位置不定所迫,它先被 MBR 读进 `0x20000` 一带的加载区,后来又搬家,干脆不赌任何地址范围。用户程序留在 `small`,是标准用户态住址,寻址最省。

## multilib 的真相:64 位项目里的 32 位环节

上一章咱们装了 `gcc-multilib`,现在给它完整解释。看 `boot/CMakeLists.txt` 里 MBR 的编译链接配置:

```cmake
# Assemble 16-bit code as 32-bit objects, link as 32-bit ELF
target_compile_options(mbr PRIVATE
    -Wa,--32                    # Assemble as 32-bit (allows 16-bit code)
)

target_link_options(mbr PRIVATE
    -Wl,-m,elf_i386
    -T ${CMAKE_CURRENT_BINARY_DIR}/mbr.ld
    -nostdlib
    -no-pie
)
```

咱们看引导代码:源文件里是 `.code16` 的 16 位指令,却被汇编成 **32 位目标文件**(`-Wa,--32`),再按 **i386 的 32 位 ELF** 链接(`-Wl,-m,elf_i386`;链接脚本 `mbr.ld` 是构建时现场生成的,开头 `OUTPUT_FORMAT("elf32-i386")`)。32 位链接就需要 32 位的 crt 启动文件和 32 位 libgcc 辅助例程,比如 32 位代码里 GCC 可能生成的 64 位整数运算帮助函数 `__divdi3` 一类。64 位系统默认只带 64 位那套,缺了 32 位组件,引导段链接时就会因找不到这些符号而失败,而报错信息完全不指向"少装了个包"。`gcc-multilib`/`g++-multilib` 补的正是这 32 位的一半。

## toolchain file 里剩下的两个机关

回头补 `Generic` 之外的细节。**`_INIT` 后缀**:`CMAKE_EXE_LINKER_FLAGS_INIT` 这类带 `_INIT` 的变量只在首次 configure 时被读一次,之后咱们在命令行追加的自定义 flag 与它叠加而非互相覆盖,这是 CMake 官方推荐的 toolchain 写法。顺带解释了上一章的调试现场:toolchain file 只在首次 configure 生效,改了它没反应,就得删 build 目录。

**`CMAKE_FIND_ROOT_PATH_MODE_*`** 把咱们项目的库与头文件查找锁在根路径规则内(`PROGRAM` 永不跨界、`LIBRARY`/`INCLUDE` 只认根路径),防止它悄悄摸到宿主系统的 `/usr/lib` 里去。配合 `Generic`,CMake 对"这是个裸机项目"就有了完整的认知。

最后记一个笔者亲历的老坑:`CMAKE_SYSTEM_NAME` 手滑写成 `Linux` 会怎样?编译全过、链接也过,内核一跑就 triple fault,笔者当年排查了两个小时才发现是这一个词的问题——CMake 按"这是个 Linux 程序"推断链接行为,产出的内核根本不能在裸机上跑。`Generic` 与 `Linux` 一词之差,差在整套假设。

## 怎么确认自己没配错

咱们有三道自检,从硬到软。配置期版本闸最硬:GCC 低于 11,configure 直接失败,报错文案在顶层 `CMakeLists.txt` 里写着:

```text
Cinux requires GCC >= 11 (C++17 freestanding + strict warnings + UBSAN);
found GCC <你的版本>.
```

其次是上一章跑过的 `check_toolchain.sh` 与构建摘要的 `Toolchain:` 行。最实在的一道还是查 `compile_commands.json`:咱们上次 `grep` 的是 `-mno-red-zone`,这回再确认 `-ffreestanding` 与 `-mcmodel=kernel` 也在编译命令里,三个 flag 各就各位,这套"驯化"就算落实了。

## 下一站

咱们把 flag 的为什么凑齐了:freestanding 关假设、no-red-zone 防中断踩栈、mcmodel 定住址、multilib 补 32 位一半。下一章往上走一层:这些编出来的目标文件,怎么被链接脚本"摆"到 `0x7C00` 和高半区,又怎么从 ELF 变成 BIOS 认的裸二进制,见[03 · 目标 / 链接脚本 / 裸镜像](03-targets-linker-objcopy.md)。

---

### 参考

- GCC 手册:"Options for Code Generation"(`-ffreestanding`/`-mno-red-zone`/`-mcmodel=*`/`-fstack-protector*`)、"Options for Linking"(`-nostdlib`/`-static`);红区定义见 x86-64 psABI。
- CMake 手册:[cmake-toolchains(7)](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html)(`CMAKE_SYSTEM_NAME` 取值、`_INIT` 变量注入时机、`FIND_ROOT_PATH_MODE_*`)。
- OSDev:[GCC Cross-Compiler](https://wiki.osdev.org/GCC_Cross-Compiler)(为什么需要/不需要交叉编译器)、[Bare Bones](https://wiki.osdev.org/Bare_Bones)。
- 本仓库源码(对齐 `9bf0b05`):[cmake/toolchain-x86_64.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/9bf0b05/cmake/toolchain-x86_64.cmake)、[kernel/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/9bf0b05/kernel/CMakeLists.txt)、[kernel/mini/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/9bf0b05/kernel/mini/CMakeLists.txt)、[user/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/9bf0b05/user/CMakeLists.txt)、[boot/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/9bf0b05/boot/CMakeLists.txt)、[顶层 CMakeLists.txt 的 GCC 版本闸](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/9bf0b05/CMakeLists.txt)。
