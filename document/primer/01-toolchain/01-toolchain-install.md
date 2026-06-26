---
title: 01 · 工具链安装与验证
---

# 01 · 工具链安装与验证:把 MBR 到内核的整条流水线先架起来

> 这一章不写一行操作系统代码。我们只做一件事:装好 GCC/CMake/QEMU/clangd 这套工具,配好 Cinux 的 CMake 骨架,让 `cmake -B build` 能干净地配置成功、编译 flag 正确注入。有了这个底子,正文 001 才有地方落脚。

## 这一章我们在解决什么

写 OS 教程最容易劝退人的不是某一段汇编有多难,而是开篇那句"先把环境搭起来"。网上随便一搜,十有八九让你**先去编一个交叉编译器**(`x86_64-elf-gcc`),丢给你一堆链接和命令,敲完也不知道自己干了什么。等后面链接找不到符号、QEMU 一启动就黑屏的时候,你根本分不清是代码错了还是工具链配错了。

Cinux 的选择是**不交叉编译**:我们的开发机本身就是 x86_64,目标机(QEMU 里的虚拟机)也是 x86_64,host 和 target 指令集完全一致,没必要为了一个"理论更纯"的交叉工具链多绕一圈。我们要做的,只是用一组 flag 把系统 GCC "驯化"成能在裸机上干活的内核编译器,再用 CMake 把"汇编 → 链接 → objcopy → 拼磁盘镜像 → 起 QEMU"这条流水线串起来。

读完这章,你会得到一个能跑通的构建骨架:**`cmake -B build` 配置成功、终端打印构建摘要、`make image` 能拼出 `cinux.img`**。但此时镜像里还没有能点亮屏幕的代码——那是正文 001 的活。这里只是把铁轨铺好,火车还没造。

> 外部依据:CMake 官方手册 `cmake-toolchains(7)` 描述了 toolchain file 的标准用法和 `_INIT` 变量语义;GCC 手册的 "Options for Code Generation" 一节逐条解释了 `-ffreestanding`、`-mno-red-zone`、`-mcmodel=kernel` 的含义;OSDev 的 [GCC Cross-Compiler](https://wiki.osdev.org/GCC_Cross-Compiler) 页给出了"为什么需要/不需要交叉编译器"的经典论述。

## 要装哪些东西

实验环境是一台 x86_64 Linux 机器(Ubuntu 22.04 或更新发行版)。工具全部来自系统包管理器,不需要 Docker,不需要自己编 GCC。下面这份清单是照着 Cinux 实际依赖列的——构建系统会真的去 `find_program(qemu-system-x86_64)`,真的去用 `as`/`ld`/`objcopy`,缺哪个就在哪一步炸。

```bash
# Ubuntu / Debian
sudo apt update
sudo apt install -y \
    build-essential        # gcc g++ make(基础编译工具)
    binutils               # as ld objcopy(汇编/链接/二进制转换)
    cmake                  # 构建系统
    qemu-system-x86        # qemu-system-x86_64(模拟器)
    gcc-multilib g++-multilib   # 32 位支持,见下面"别省 multilib"
    clangd                 # (可选)代码补全/跳转
    xxd                    # build_image.sh 校验 MBR 魔数用
```

各工具的版本要求,以下表为准(对照仓库里 `cmake/check`):

| 工具 | 最低版本 | 出处 |
|------|----------|------|
| GCC / G++ | 11+ | toolchain 用到 `-mcmodel=kernel` 等成熟特性,11 足够;Cinux 当前实测跑在 GCC 16 上 |
| CMake | 3.20(`cmake_minimum_required`)| 顶层 `CMakeLists.txt:1` |
| QEMU | 8.0+ | 模拟 x86_64 启动;Cinux 当前实测跑在 QEMU 11 上 |
| clangd | 任意稳定版 | 配合 `.clangd` 配置做补全 |

这里有个**容易看走眼**的点:[`scripts/check_toolchain.sh`](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/check_toolchain.sh) 里把 CMake 的最低版本写成了 `4.1`(`check_cmake_version` 函数,`min_version="4.1"`),而顶层 `CMakeLists.txt:1` 的 `cmake_minimum_required(VERSION 3.20)` 才是构建系统真正能跑的下限。脚本里的 `4.1` 是个偏保守的"推荐值"——若你机器上的 CMake 是 3.21,构建照样能过,但 `check_toolchain.sh` 会拦你。这是两道不同的闸:构建看 `cmake_minimum_required`,自检脚本看 `check_cmake_version`。

### 别省 multilib:为什么 64 位项目还要 32 位库

这是 Cinux 真正踩过、且**报错信息完全不指向真正原因**的坑。

Cinux 最终的内核是 64 位的,但**引导阶段(MBR / Stage2)是 16 位实模式代码,却被当成 32 位 ELF 链接**。看 [`boot/CMakeLists.txt`](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt):

```cmake
target_compile_options(mbr PRIVATE
    -Wa,--32                    # 当成 32 位汇编(允许内嵌 .code16)
)

target_link_options(mbr PRIVATE
    -Wl,-m,elf_i386             # 链接成 32 位 ELF
    -T ${CMAKE_CURRENT_BINARY_DIR}/mbr.ld
    -nostdlib
    -no-pie
)
```

`-Wa,--32` 让汇编器产出 32 位目标文件(源码里再用 `.code16` 切到 16 位指令);`-Wl,-m,elf_i386` 让链接器按 32 位 i386 格式链接;链接脚本 `mbr.ld` 开头也是 `OUTPUT_FORMAT("elf32-i386")`。这意味着构建 MBR/Stage2 时,工具链需要 32 位的运行时组件在位——32 位的 crt 启动文件、以及 32 位的 libgcc 辅助例程(比如 32 位代码里编译器可能生成的 64 位整数运算 `__divdi3` 之类)。64 位系统默认只带 64 位这一套,缺了 32 位组件,链接 32 位引导代码时就会因找不到这些 32 位符号而失败。

解决办法就是装上 `gcc-multilib` / `g++-multilib`——它把 32 位的 crt、libgcc 补齐。一行 `apt install` 能省你半天对着链接错误发呆的时间。

## 为什么不交叉编译

OSDev 的 [Bare Bones](https://wiki.osdev.org/Bare_Bones) 教程强烈建议你构建 `x86_64-elf-gcc` 交叉编译器,理由是系统 GCC 可能隐式链接宿主库、假设宿主 ABI、生成裸机上跑不了的代码。这些担忧理论上成立,但对 Cinux 不成立,原因就一条:

> **host == target == x86_64。我们的开发机和目标机指令集完全相同,不存在"为别的架构生成代码"的跨平台问题。**

OSDev 之所以推荐交叉编译,核心场景是"你在 x86 Mac/PC 上给 ARM 或 RISC-V 写内核"——那时 host 和 target 的指令集、字节序、ABI 都不一样,系统 GCC 会偷偷链进宿主的 glibc,必须用交叉编译器隔离。而我们写 x86_64 内核、在 x86_64 上编,指令集层面零差异。只要把"别链标准库""别假设有 OS"这些约束通过 flag 明确告诉系统 GCC,它产出的代码在裸机上就是可用的。

这组"驯化"flag 集中在 [`cmake/toolchain-x86_64.cmake`](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/toolchain-x86_64.cmake),下一节逐条讲。**所以前置卷里我们不会出现任何 `x86_64-elf-gcc` 的字样,全文用系统 `gcc`/`g++`/`as`/`ld`。**

## 驯化 GCC:freestanding 与那串 flag

写普通应用程序时,编译器默认帮你链接 glibc、假设 OS 提供文件系统/内存分配/线程调度、甚至插入栈保护 canary 和异常处理表。这些在写内核时全是累赘——内核本身就是"提供一切服务"的那个东西,它运行在没有 OS 支撑的裸机上,连 `printf`、`malloc` 都没有。

编译理论把这两种环境分得很清:**有 OS 支撑的叫 hosted(托管),没有的叫 freestanding(独立)**。内核属于后者,必须用一串 flag 告诉编译器"别自作聪明"。这串 flag 就是 Cinux toolchain file 的全部核心:

```cmake
# cmake/toolchain-x86_64.cmake(节选)
set(CMAKE_C_FLAGS_INIT "
    -ffreestanding          # 不假设 hosted 环境,只提供 freestanding 头(<stdint.h> 等)
    -fno-stack-protector    # 关栈 canary,canary 的 __stack_chk_fail 需要标准库
    -mno-red-zone           # 关 x86_64 SysV ABI 的 128 字节红区
    -mcmodel=kernel         # 代码运行在高半区(high half,0xFFFFFFFF80000000)
    -Wall -Wextra
")

set(CMAKE_CXX_FLAGS_INIT ${CMAKE_C_FLAGS_INIT} "
    -fno-exceptions         # 异常展开需要 .eh_frame + __cxa_begin_catch,标准库依赖
    -fno-rtti               # 关 RTTI(dynamic_cast / typeid 需要类型信息表)
    -std=c++17              # Cinux 实际用的 C++ 标准
")

set(CMAKE_EXE_LINKER_FLAGS_INIT "
    -nostdlib               # 不链标准启动文件和库
    -static                 # 纯静态,不依赖动态链接器
")
```

每条 flag 都有具体的技术理由,挑两条最容易翻车的讲:

**`-mno-red-zone`** —— x86_64 System V ABI 给用户态程序的优化:函数可以在不调整 `RSP` 的情况下,直接使用栈顶下方 128 字节(红区)作为临时空间。但内核里**中断可以在任意时刻打断执行**(包括红区里的代码),中断处理程序压栈会覆盖掉红区里还没用完的数据,导致数据损坏和极难复现的随机崩溃。内核必须关掉它。

**`-mcmodel=kernel`** —— 告诉编译器代码会运行在地址空间顶端(高半核),这影响它生成绝对地址引用的方式。内核不像用户程序那样待在低地址,它的代码和数据普遍在 `0xFFFFFFFF80000000` 附近,代码模型不对,生成的地址引用就全错。

> 这串 flag 是 freestanding 内核编译的**最小公约数**,前置卷只讲到这一层。约束很硬:C/C++ 只用 freestanding 子集,**不碰 STL 容器/异常/RTTI/智能指针/虚函数多态**;CMake 只到 OS 手搓程度,**不碰 install/CPack/find_package/FetchContent**。这些超出本卷范围,需要时再查手册。

关于这串 flag 的"为什么",以及 `-ffreestanding` 到底给你留了哪些头文件、为什么内核里不能用 `printf`,详见正文各章实际用到的位置——前置卷点到为止。

## CMake 三层骨架:toolchain / 顶层 / QEMU

Cinux 的构建系统是三层结构,每个文件职责单一:

```text
cmake/toolchain-x86_64.cmake   <- "怎么编译":注入 flag、声明目标平台
            │
            ▼
CMakeLists.txt (顶层)          <- "编译什么":项目声明 + 子目录编排
   ├─ add_subdirectory(boot)    <- MBR/Stage2 → mbr.bin / stage2.bin
   ├─ add_subdirectory(user)
   ├─ add_subdirectory(kernel)
   └─ include(cmake/qemu.cmake) <- "怎么运行":make run / make run-debug
```

### 顶层 CMakeLists:项目声明与 flag 编排

[`CMakeLists.txt`](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/CMakeLists.txt) 顶层做的事很朴素:

```cmake
cmake_minimum_required(VERSION 3.20)

project(cinux
    VERSION 0.1.0
    LANGUAGES C CXX ASM)          # 三种语言:C / C++ / 汇编(asm)

option(CINUX_GUI "Enable GUI mode" ON)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_SOURCE_DIR}/cmake/toolchain-x86_64.cmake"
        CACHE FILEPATH "Toolchain file")   # 不指定就用项目自带的
endif()

add_compile_options(-Wall -Wextra)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)      # 导出 compile_commands.json 给 clangd
```

两个值得注意的点。第一,`LANGUAGES C CXX ASM` 把汇编也声明成一等语言,这样 `add_executable(mbr mbr.S)` 才能直接把 `.S` 当源文件编译(GAS,AT&T 语法)。第二,`CMAKE_EXPORT_COMPILE_COMMANDS ON` 会生成 `compile_commands.json`,这是 clangd 工作的命脉——下面"配 clangd"会用到。

### Toolchain file:为什么用 `_INIT` 后缀

[`cmake/toolchain-x86_64.cmake`](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/toolchain-x86_64.cmake) 里设置 flag 时,**必须用 `CMAKE_C_FLAGS_INIT` 而不是直接设 `CMAKE_C_FLAGS`**:

```cmake
set(CMAKE_SYSTEM_NAME Generic)         # Generic = 没有目标 OS,别按 Linux 配
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_FLAGS_INIT " -ffreestanding ... ")   # ← _INIT 后缀
set(CMAKE_FIND_ROOT_PATH "")                      # 禁用库/头文件自动查找
```

这是 CMake 官方推荐的 toolchain file 写法:`_INIT` 变量**只在第一次 configure 时被读取**,之后用户通过命令行追加的自定义 flag(比如 `-DCMAKE_C_FLAGS=...`)会和 toolchain 的 flag 叠加,而不是被覆盖。

`CMAKE_SYSTEM_NAME` 写成 `Generic` 是另一个关键点。它告诉 CMake"目标没有操作系统",从而禁用一切假设 OS 存在的行为。**别手滑写成 `Linux`**——笔者亲历过一次:写成 `Linux` 后编译全过、链接也过,但内核一跑就 triple fault,排查两小时才发现是这一个词的问题。CMake 会按"这是个 Linux 程序"来推断链接行为,产出的内核根本不能在裸机上跑。

> 外部依据:CMake 手册 `cmake-toolchains(7)` 的 "Cross Compiling" 一节明确了 `CMAKE_SYSTEM_NAME` 取值如何影响 build,以及 `_INIT` 变量相对普通 cache 变量的注入时机。

### QEMU 集成:`make run` 是怎么来的

[`cmake/qemu.cmake`](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/qemu.cmake) 用 `find_program` 找 `qemu-system-x86_64`,然后注册了一堆 `add_custom_target`。前置卷只需关心最核心的两个:

```cmake
find_program(QEMU_EXECUTABLE qemu-system-x86_64)
# ...
set(QEMU_COMMON_FLAGS
    -m ${QEMU_MEMORY}        # 内存(本机 8G,CI 里 1G)
    -serial stdio            # 串口重定向到终端——正文多数打印走这里
    -no-reboot               # triple fault 后别重启,方便看现场
    -debugcon file:debug.log # Bochs debug console,I/O port 0xE9
    ${QEMU_ACCEL}            # 有 /dev/kvm 就 -accel kvm -cpu max
    ${QEMU_DISPLAY}
)

add_custom_target(run
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ...
        -drive file=${CINUX_IMAGE_PATH},format=raw,index=0,media=disk
    DEPENDS image
    ...)
```

`make run` 依赖 `image` 这个 target,而 `image` 又由 [`scripts/build_image.sh`](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/build_image.sh) 用 `dd` 把 `mbr.bin`(扇区 0)、`stage2.bin`(扇区 1+)、内核拼成 `cinux.img`。也就是说,**`make run` 一条命令会触发"汇编 → 链接 → objcopy → dd 拼镜像 → 起 QEMU"整条链**。前置卷目标就是把这条链跑通,产物是黑屏(还没代码画屏)——黑屏恰恰说明链路通了。

## 配 clangd:让编辑器能跳转

项目根目录有 `.clangd` 配置:

```yaml
# .clangd
Diagnostics:
  UnusedIncludes: Strict
  MissingIncludes: Strict
InlayHints:
  Enabled: Yes
  ParameterNames: Yes
  DeducedTypes: Yes
Index:
  Background: Build
```

clangd 工作的前提是项目根目录下有 `compile_commands.json`——它正是顶层 `CMakeLists` 里 `CMAKE_EXPORT_COMPILE_COMMANDS ON` 生成的,落在 `build/compile_commands.json`。所以**配 clangd 的唯一动作就是先成功跑一次 `cmake -B build`**,让这个文件被生成出来。装上 clangd 的 VS Code 扩展(或对应编辑器的 LSP 客户端),它就能做跳转、补全、实时诊断。

> 前置卷不展开 clangd 的进阶用法。`-Wa,--divide`(toolchain file 里 `CMAKE_ASM_FLAGS_INIT`)这类汇编专属 flag 的细节,需要时查 GAS 手册。

## 验证:两个闸都要过

环境搭得对不对,有两道闸。两道都过,这章才算完。

**第一道:`check_toolchain.sh` 自检。** 这个脚本逐个 `command -v` 检查 `gcc`/`g++`/`as`/`ld`/`objcopy`/`qemu-system-x86_64`/`cmake`,并对 CMake 单独做 `>= 4.1` 的版本比较(注意:这里的 4.1 比构建实际需要的 3.20 保守,见前文):

```bash
./scripts/check_toolchain.sh
# 预期:每个工具后跟 [OK],最后一行
# [SUCCESS] [OK] All required tools are installed!
```

缺哪个工具,脚本会打印对应的 `Install:` 提示后 `exit 1`,不会继续往下查。

**第二道:`cmake -B build` 配置成功,且 flag 注入正确。** 在仓库根目录:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -S .
```

配置成功时,终端末尾会打印一段构建摘要(Cinux 顶层 `CMakeLists` 里 `message(STATUS ...)` 故意打出来的):

```text
-- === Cinux Build Configuration ===
--   Project:     cinux v0.1.0
--   Build type:  Release
--   C Compiler:  /usr/sbin/cc
--   CXX Compiler:/usr/sbin/cc
--   ASM Compiler:/usr/sbin/cc
--   Toolchain:   .../cmake/toolchain-x86_64.cmake
--   GUI mode:    ON
-- ===================================
-- Configuring done
-- Generating done
```

看到 `Toolchain:` 指向项目里的 `cmake/toolchain-x86_64.cmake`、`Build type:` 是你传的类型,配置这关就过了。

**怎么确认 flag 真的注入了?** 别只看摘要(摘要不显示 flag)。直接验证产物:

```bash
cmake --build build -j$(nproc) -- VERBOSE=1 2>&1 | grep -- '-mno-red-zone' | head -1
# 或看 build/compile_commands.json:
grep -o '\-ffreestanding -fno-stack-protector -mno-red-zone -mcmodel=kernel' build/compile_commands.json | head -1
```

能从实际编译命令 / `compile_commands.json` 里 grep 到 `-ffreestanding`、`-mno-red-zone`、`-mcmodel=kernel` 这串,就证明 toolchain file 的 flag 确实喂到了编译器嘴里。这是本章的硬验收点:**不是"CMake 配置没报错",而是"flag 真的出现在编译命令里"**。

## 调试现场

**症状一**——构建 MBR/Stage2 时链接器报找不到 32 位组件(形如 32 位 crt 或 libgcc 辅助例程缺失)。 99% 是没装 `gcc-multilib` / `g++-multilib`。引导阶段按 32 位 i386 处理(`-Wa,--32` + `-Wl,-m,elf_i386`),64 位系统默认不带 32 位运行时库。`apt install gcc-multilib g++-multilib` 即解。报错信息完全不指向真正原因,笔者在这卡过半天。

**症状二**——改了 toolchain file 但 `cmake -B build` 没变化。 CMake 缓存了上一次的 configure 结果,toolchain file 只在**首次 configure**被读。删掉整个 build 目录再 configure:

```bash
rm -rf build && cmake -B build -DCMAKE_BUILD_TYPE=Release -S .
```

**症状三**——CMake 摘要里 `Toolchain:` 是空的,或 flag 没注入。 多半是你 `cmake -B build` 时手滑带了 `-DCMAKE_TOOLCHAIN_FILE=` 指向了别处,或者 build 目录是从别的项目复用的。清 build 目录重来。顶层 `CMakeLists` 的逻辑是"`NOT CMAKE_TOOLCHAIN_FILE` 时才自动套项目自带 toolchain",一旦缓存里有值就不会覆盖。

## 下一站

到这里,工具装齐了,CMake 三层骨架立住了,`cmake -B build` 能干净配置、flag 也确认注入。现在你可以:

```bash
cmake --build build -j$(nproc)   # 编出 mbr.bin / stage2.bin
cmake --build build --target image   # 拼出 cinux.img
cmake --build build --target run     # make run 起 QEMU
```

`make run` 会弹出一个 QEMU 窗口——**现在是黑屏**,因为镜像里还没有任何"往屏幕上写东西"的代码,MBR/Stage2 的源码本卷还没讲。但这恰恰说明整条"编译 → 链接 → objcopy → dd → QEMU"流水线已经通了。

下一站去正文 [001 · 实模式引导](../../book/01-boot/001-boot-real-mode.md):我们要让 BIOS 把第一段汇编从磁盘读进 `0x7C00`,在 512 字节里读盘、配屏,把这块黑屏**点亮**。前置卷到此为止,从 001 开始,我们写的每一行代码都会真实地在虚拟机里跑起来。

---

### 参考

- CMake 官方手册 — [cmake-toolchains(7)](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html)(toolchain file 变量、`_INIT` 注入时机、`CMAKE_SYSTEM_NAME` 取值)。
- GCC 手册 — "Options for Code Generation"(`-ffreestanding`/`-mno-red-zone`/`-mcmodel=kernel`/`-fno-exceptions`/`-fno-rtti`)、"Options for Linking"(`-nostdlib`/`-static`)。
- OSDev — [GCC Cross-Compiler](https://wiki.osdev.org/GCC_Cross-Compiler)(为什么需要/不需要交叉编译器、`-ffreestanding` 语义)、[Bare Bones](https://wiki.osdev.org/Bare_Bones)(freestanding 编译 flag 入门)。
- CMake `cmake_minimum_required(VERSION 3.20)` 见本仓库 [CMakeLists.txt:1](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/CMakeLists.txt);CMake `>= 4.1` 自检门槛见 [scripts/check_toolchain.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/check_toolchain.sh)。
- 本仓库源码:[toolchain-x86_64.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/toolchain-x86_64.cmake)、[qemu.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/qemu.cmake)、[boot/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt)、[build_image.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/build_image.sh)、[.clangd](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/.clangd)。

> 参考 URL 的有效性会在全局审查阶段用 open-websearch(bing)统一核活,与本系列其它章节一致。
