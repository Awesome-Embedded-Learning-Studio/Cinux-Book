---
title: 01 · 装机与首胜
---

# 01 · 装机与首胜:三十分钟,从 apt 到 QEMU 黑屏

> 咱们看的源码对齐 commit `9bf0b05`(2026-09-13)。本章只干三件事:装齐工具、过两道自检闸、拿到第一个看得见的战果。每条命令为什么长这样、每个 flag 在防什么,是[下一章](02-compiler-first-lesson.md)的主菜;咱们先把机器跑起来。

## 劝退人的不是汇编

汇编看着吓人,可真正劝退人的多半是教程第一句"先把环境搭起来":丢过来一堆链接和命令,敲完不知道自己干了什么,等后面链接报错、QEMU 一片黑,咱们根本分不清是代码错了还是工具链配错了。这一章反着来:每装一样东西都配一道验证,最后给一个能亲眼看见的结果。目标很朴素——`cmake -B build` 干净配置、构建摘要打印出来、`make run` 弹出 QEMU 窗口。窗口是黑的,但那块黑屏就是今天的战果:整条"汇编、链接、拼镜像、起虚拟机"的流水线通了,只差往里填代码。

## 装什么

实验环境是一台 x86_64 Linux 机器(Ubuntu 22.04 或更新;WSL2 也行,笔者就在 WSL2 里跑)。工具全部来自系统包管理器,不用 Docker,不用自己编 GCC:

```bash
sudo apt update
sudo apt install -y \
    build-essential             # gcc g++ make
    binutils                    # as ld objcopy
    cmake                       # 构建系统
    qemu-system-x86             # qemu-system-x86_64
    gcc-multilib g++-multilib   # 32 位支持:引导代码按 32 位链接,真相在 02 章
    clangd                      # 可选:编辑器跳转/补全
    xxd                         # build_image.sh 校验引导魔数用
```

版本下限咱们以这张表为准,每条的出处都标在旁边:

| 工具 | 下限 | 出处 |
|---|---|---|
| GCC | 11 | 顶层 `CMakeLists.txt` 配置期版本闸:低于 11 直接拒绝配置 |
| CMake | 3.20 | `cmake_minimum_required(VERSION 3.20)` |
| QEMU | 8.0+ | 模拟 x86_64 启动;无硬闸,太老会在设备参数上报错 |

Cinux 当前实测跑在 GCC 16、QEMU 11 上,笔者这台机器刚过的是 `cmake 4.4`。另有一个容易看走眼的数:`scripts/check_toolchain.sh` 里 CMake 的门槛写的是 `4.1`,比构建真正需要的 3.20 保守。咱们机器上若是 3.21,构建能过,但自检脚本会拦——两道闸宽严不同,自检那道是推荐值。

## 第一道闸:自检脚本

```bash
./scripts/check_toolchain.sh
```

它逐个 `command -v` 查 `gcc`/`g++`/`as`/`ld`/`objcopy`/`qemu-system-x86_64`,再单独比对 CMake 版本。笔者这台机器的真跑输出:

```text
[INFO] Checking required tools...
[INFO] [OK] gcc found
[INFO] [OK] g++ found
[INFO] [OK] as found
[INFO] [OK] ld found
[INFO] [OK] objcopy found
[INFO] [OK] qemu-system-x86_64 found
[INFO] [OK] cmake 4.4 (>= 4.1)
[SUCCESS] [OK] All required tools are installed!
```

(原文带 ANSI 颜色;缺哪个工具,它会打印对应的 Install 提示再退出,不给咱们往下查的机会。)

## 第二道闸:干净配置一次

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -S .
```

配置成功时终端会打印一段构建摘要,是顶层 `CMakeLists.txt` 故意打给咱们看的。真跑输出节选:

```text
-- === Cinux Build Configuration ===
--   Project:      cinux v1.0.0
--   Build type:   Release
--   C Compiler:   /usr/sbin/cc
--   CXX Compiler: /usr/sbin/c++
--   ASM Compiler: /usr/sbin/cc
--   Toolchain:    /home/charliechen/Cinux/cmake/toolchain-x86_64.cmake
--   -- Feature switches (add one in cmake/options.cmake) --
--   CINUX_GUI = ON
...
-- Configuring done
-- Generating done
```

咱们盯两行:`Toolchain:` 得指向仓库里的 `cmake/toolchain-x86_64.cmake`,`Build type:` 得是刚传的 Release。这两行对了,配置这关就过。

咱们接着验最后一件事:裸机编译 flag 有没有真的进编译命令。摘要不显示 flag,那就直接查产物:

```bash
grep -c '"-mno-red-zone"' build/compile_commands.json
```

笔者这台机器数出来 359 处,flag 确实喂到了编译器嘴里,不是只在配置文件里写了写。这串 flag 每条在防什么,下一章逐个讲。

## 首胜:make run

```bash
cmake --build build -j$(nproc)          # 编出 mbr.bin / stage2.bin / 内核
cmake --build build --target image      # 拼出 build/cinux.img
cmake --build build --target run        # 起 QEMU,弹窗
```

窗口是黑的,一块纯黑。**这块黑屏就是胜利**:镜像里此刻还没有任何"往屏幕上写字"的代码,但 BIOS 已经认了咱们拼的盘、把它读进内存、跳进了咱们的代码——流水线每一环都通了,只差填肉。代码怎么被"摆"进镜像、盘怎么拼、QEMU 怎么起,分别是[03](03-targets-linker-objcopy.md)、[04](04-qemu-image-test.md)两章的主场;写下第一行能点屏的汇编,是正文 001 的事。

## 配 clangd:五分钟

仓库根目录有现成的 `.clangd`(开严格诊断、参数名与推导类型提示),咱们唯一要做的动作就是上面那步 `cmake -B build`——它生成的 `build/compile_commands.json` 是 clangd 的命脉。装上编辑器的 clangd 扩展(VS Code 装 clangd 插件),重载窗口,跳转和补全就有了。

## 装不上怎么办

三个最常见的症状,咱们按命中概率排。

咱们头一个撞的大概是构建引导段时链接器报缺 32 位组件(crt、libgcc 一类),报错完全不指向"少装了包"这个真因。九成九是没装 `gcc-multilib`/`g++-multilib`,`apt` 补上即解;为什么 64 位项目会需要 32 位库,02 章给完整解释。

另一个坑看时机:咱们改了 toolchain file,`cmake -B build` 却毫无变化。CMake 缓存了首次 configure 的结果,toolchain file 只在那一次被读,删掉整个 build 目录重来:

```bash
rm -rf build && cmake -B build -DCMAKE_BUILD_TYPE=Release -S .
```

还有一种咱们也躲不开:摘要里 `Toolchain:` 是空的。多半是 configure 时手滑带了自己的 `-DCMAKE_TOOLCHAIN_FILE=`,或 build 目录从别的项目复用而来——顶层的自动回退只在缓存里没有值时生效。同样清 build 目录解决。

## 下一站

工具齐了,黑屏为证,流水线通了。下一章回答咱们刚才 grep 到的那串 flag:系统 GCC 凭什么能编出一个没有操作系统的程序?

---

### 参考

- 版本下限出处(对齐 `9bf0b05`):[CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/9bf0b05/CMakeLists.txt)(GCC 11 配置期版本闸、`cmake_minimum_required 3.20`)、[scripts/check_toolchain.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/9bf0b05/scripts/check_toolchain.sh)(CMake 4.1 自检门槛)。
- 安装清单对照:CI 实际安装的包见 [.github/workflows/ci.yml](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/9bf0b05/.github/workflows/ci.yml)(qemu-system-x86、e2fsprogs、bc、xxd 等)。
- clangd 与 `compile_commands.json`:见 [clangd 官方文档](https://clangd.llvm.org/design/compile-commands)。
