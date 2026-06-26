---
title: 04 · QEMU、磁盘镜像与主机测试
---

# 04 · QEMU、磁盘镜像与主机测试:让机器真跑起来

> 工具链前三章把"怎么编译、怎么链接"讲透了,这一章补上最后一环:**怎么把内核摆进一张可引导磁盘,塞进 QEMU 跑起来,再让它自己报告成败**。

## 这一章干什么

正文 `001` 末尾会让我们敲一条命令:

```bash
make run
```

然后盯着 QEMU 弹出的窗口看黑屏——那是"上电成功、切进图形模式"的标志。可这条 `make run` 背后发生了什么?BIOS 凭什么认识我们编译出来的 `mbr.bin`?内核跑完又怎么"自己告诉主机测试通过"?

这一章不碰内核逻辑,只讲"外环":三个文件撑起整个运行/测试面——

- `scripts/build_image.sh`:把 MBR、Stage2、内核拼成一张磁盘镜像,并校验那个 BIOS 认的 `0xAA55` 魔数;
- `cmake/qemu.cmake`:定义 `run` / `run-debug` / `run-kernel-test` 这几个目标,处理 KVM、CI 环境差异,以及内核怎么通过 `isa-debug-exit` 设备"按值退出";
- `test/CMakeLists.txt`:主机侧单元测试怎么用 CTest 登记、`add_cinux_test` 到底替你做了哪几步。

读完你应该能在正文里无障碍地跑 `make run` / `make run-debug` / `make test_host`,并看懂每一步在做什么。

## 1. 磁盘镜像:把三个二进制拼成"BIOS 认得的一张盘"

### 1.1 BIOS 要的是"扇区 0 末尾带 0xAA55 的一张盘"

BIOS 自检完,只认一件事:去启动盘的**第 0 扇区**读 512 字节,看最后两字节是不是 `0x55 0xAA`(小端即 `0xAA55`)。是,就把这 512 字节原样读进 `0x7C00` 跳过去;不是,直接报"No bootable device"。

这个魔数不是我们写脚本时塞进去的,而是 MBR 源码自己写在末尾的。[boot/mbr.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S) 结尾是这样:

```asm
// Pad to 510 bytes, then MBR signature
.org 510
.word 0xAA55
```

`.org 510` 把当前位置强行对齐到偏移 510,`.word 0xAA55` 再填进两字节——恰好落在 512 字节扇区的第 510、511 字节。注意 `boot/CMakeLists.txt` 里 MBR 的链接脚本是 `. = 0x7C00`,所以这 510 字节的偏移是相对 `0x7C00` 算的,链接后再用 `objcopy -O binary` 抽成裸二进制 `mbr.bin`,它正好 512 字节,末尾就是 `55 aa`。

### 1.2 build_image.sh:用 dd 拼扇区

BIOS 只认扇区,我们手上却是一堆独立编出来的 `.bin`。`build_image.sh` 的活就是把它们按约定的 LBA(线性扇区号)摆好。它的磁盘布局(脚本 68-74 行):

```text
扇区 0       MBR(512B,末尾 0xAA55)
扇区 1..15   Stage2(最多 15 扇区 = 7680B)
扇区 16+     Mini 内核(LBA 16,匹配 boot.S 里的常量)
扇区 848+    Big 内核(可选,有就铺,没有跳过)
```

注意一个**容易踩**的点:这些 LBA 不是随便定的。`STAGE2_LBA=1`、`MINI_KERNEL_LBA=16`、`BIG_KERNEL_LBA=848` 必须和 MBR 读盘代码里写的扇区号**一一对应**——MBR 用 `INT 0x13 AH=0x42` 读盘时填的 DAP 里 `lba.low32` 是几,镜像里对应内容就得摆在第几扇区(详见正文 001 第 3 节)。镜像和读盘代码对不上,读进来就是一坨垃圾。

拼装用 `dd`,核心几步(脚本 174-191 行,关键部分):

```bash
# 先建一张 1MB 的空白盘
dd if=/dev/zero of="$OUTPUT_IMAGE" bs=1M count=$IMAGE_SIZE_MB status=none

# 扇区 0:写 MBR,1 个扇区,不截断
dd if="$MBR_BIN" of="$OUTPUT_IMAGE" bs=512 count=1 conv=notrunc status=none

# 扇区 1 起:写 Stage2,用 seek 跳过 MBR 占的扇区
dd if="$STAGE2_BIN" of="$OUTPUT_IMAGE" bs=512 seek=$STAGE2_LBA conv=notrunc status=none

# 扇区 16 起:写 Mini 内核
dd if="$MINI_BIN"  of="$OUTPUT_IMAGE" bs=512 seek=$MINI_KERNEL_LBA conv=notrunc status=none

# 扇区 848 起:写 Big 内核(可选)
dd if="$BIG_KERNEL_BIN" of="$OUTPUT_IMAGE" bs=512 seek=$BIG_KERNEL_LBA conv=notrunc status=none
```

`conv=notrunc` 很关键——不加的话 `dd` 会把镜像截断到这次写入的长度,后面摆好的内容全没了。`bs=512 seek=N` 的意思是"块大小 512 字节,从第 N 块开始写",恰好对上扇区号。

脚本还顺手做了**大小校验**:Stage2 超过 15 扇区(132-137 行)、Mini 内核超过 416KB(140-156 行)都会报错退出。416KB 这个上限不是拍脑袋——是实模式栈(`0x9000` 起)、保护模式栈(`0x90000`)之间那块可用加载区 `0x20000~0x88000` 的大小,注释里写得清清楚楚。内核塞超了会撞栈。

### 1.3 0xAA55 校验:最后一道闸

拼完,脚本自己验一遍魔数(200-206 行):

```bash
SIGNATURE=$(dd if="$OUTPUT_IMAGE" bs=1 skip=510 count=2 status=none | xxd -p)
if [ "$SIGNATURE" = "55aa" ]; then
    log_success "MBR signature valid: 0xAA55"
else
    log_warn "MBR signature invalid: $SIGNATURE (expected 55aa)"
fi
```

`bs=1 skip=510 count=2` 是按**字节**精确抓镜像第 510、511 字节,`xxd -p` 转成十六进制字符串跟 `55aa` 比。如果 MBR 代码超过了 510 字节,`.org 510` 那个对齐就失效了,魔数会被挤掉,这里立刻能抓住。这是镜像拼装最便宜也最值钱的一道自检。

## 2. QEMU 目标:run / run-debug / run-kernel-test

### 2.1 CMake 怎么调起 QEMU

`cmake/qemu.cmake` 用 `add_custom_target` 把 QEMU 命令包成 `make` 目标。最朴素的 `run` 目标(115-126 行):

```cmake
add_custom_target(run
    COMMAND ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_DEVELOP_FLAG}
        -drive file=${CINUX_IMAGE_PATH},format=raw,index=0,media=disk
        -device ahci,id=ahci
        -drive file=${AHCI_TEST_IMAGE},format=raw,if=none,id=ahci-disk
        -device ide-hd,drive=ahci-disk,bus=ahci.0
        ...
    DEPENDS image ${AHCI_TEST_IMAGE} ${EXT2_IMAGE}
    COMMENT "Starting QEMU (serial: stdio)"
    VERBATIM
)
```

几件事:

- **`DEPENDS image`**:`make run` 前会先把 `image` 目标(它依赖 `build_image.sh` 那条 custom command)跑一遍,保证镜像是最新的。
- **`-drive file=...,format=raw,index=0,media=disk`**:把我们拼好的 `cinux.img` 当成第 0 块硬盘,`format=raw` 表示原样字节、不带任何容器格式,`index=0` 是第一块盘(BIOS 从这块启动)。
- **`-serial stdio`**:把内核的串口输出导到当前终端,所以你能在命令行看到内核打的日志(正文 001 的打印走 VGA 窗口、不走串口,这点正文会强调)。

`run-debug`(128-134 行)在 `run` 基础上多了 `QEMU_DEBUG_FLAGS = -s -S`:

```cmake
set(QEMU_DEBUG_FLAGS
    -s      # GDB stub 监听 :1234
    -S      # 启动即停,等 GDB 连上来再放行
)
```

`-s` 是 `-gdb tcp::1234` 的简写,`-S` 让 QEMU 上电后第一件事就是 `hlt` 等着。这样你可以在另一个终端 `gdb` 连 `:1234`(`make run-gdb` 帮你把这步也做了)。`make run-debug` 起来窗口一片黑、机器不动,不是坏了,是在等 GDB。

### 2.2 KVM 和 CI:同一个目标,两条路径

这台机器真跑起来要快,CI 上要能过——这俩诉求是冲突的。`qemu.cmake` 用两个 `if` 把它分开(9-19 行):

```cmake
# 跑得快:本机有 KVM 才开
if(EXISTS "/dev/kvm")
    set(QEMU_ACCEL -accel kvm -cpu max)
endif()

# CI 无图形界面:降内存、改用 VNC
if(DEFINED ENV{CI})
    set(QEMU_MEMORY "1G")
    set(QEMU_DISPLAY -vnc :0)
else()
    set(QEMU_MEMORY "8G")
endif()
```

两条逻辑很直白:

- **KVM**:只在本机 `/dev/kvm` 存在时才加 `-accel kvm -cpu max`。CI runner(尤其容器化环境)通常没有 `/dev/kvm`,加了会直接报错退出。所以这里用文件存在性探测,而不是写死开。
- **CI 变量**:CI 环境里没有显示器,GTK 窗口起不来;内存也给不起 8G。于是 `if(DEFINED ENV{CI})` 命中时改成 1G 内存 + `-vnc :0`(不弹窗口,改走 VNC),本机交互才给 8G。

`QEMU_COMMON_FLAGS`(21-30 行)把 `-m`、`-serial stdio`、`-no-reboot`、`-debugcon`(0xE9 调试口,正文会用它打调试日志)和上面这两段拼接起来。所有 `run*` 目标都吃同一份 `QEMU_COMMON_FLAGS`,这就是 KVM/CI 差异被自动处理的根源。

> 外部依据:QEMU 文档对 [isa-debug-exit](https://www.qemu.org/docs/master/system/devices/isa.html) 设备及 `-accel kvm`、`-no-reboot` 的语义有官方说明;OSDev 的 [Testing](https://wiki.osdev.org/Testing) 页总结了 OS 开发中用 QEMU 跑测试的常见做法。

### 2.3 run-kernel-test:让内核"自己报告"成败

`run` 目标有个根本问题:**内核跑完不会自己退出**——它要么 `hlt` 死循环,要么在交互式 shell 里等你。CI 没法判断"这次到底过没过"。解法是 QEMU 的 `isa-debug-exit` 设备。

`qemu.cmake` 把它放进测试专用的 `QEMU_TEST_EXTRA_FLAGS`(56-63 行):

```cmake
set(QEMU_TEST_EXTRA_FLAGS
    -device isa-debug-exit,iobase=0xf4,iosize=0x04
    ...
)
```

这个设备挂在 I/O 端口 `0xf4`。内核往 `0xf4` 写一个字节,QEMU 立刻退出,**退出码按固定公式算**。`qemu.cmake` 顶部那段注释(65-72 行)是这章最该记住的一段:

```text
QEMU's isa-debug-exit device encodes: exit_code = (value << 1) | 1
  Kernel writes 0 → QEMU exits 1 → test SUCCESS
  Kernel writes 1 → QEMU exits 3 → test FAILURE
```

也就是:**内核写 `0` 表示成功、写 `1` 表示失败,QEMU 的退出码却是 `1` 和 `3`**。`(value << 1) | 1` 这个编码是 QEMU 故意的——它是一条单向编码,保证 QEMU 退出码恒为非零:即使内核写 `0`(成功),QEMU 也退出 `1`,绝不会和 shell 里"正常退出 = 0"撞车。左移一位再或 1,就是为了让 `0` 这个成功值永远映射不到退出码 `0`。

但 `make` 的世界有个坑:**进程退出码非 0 就算失败**。内核明明写 `0`(成功),QEMU 却退出 `1`,直接 `make` 就报红了。所以 `run-kernel-test`(263-271 行)不直接调 QEMU,而是先过一个 bash 包装脚本:

```cmake
add_custom_target(run-kernel-test
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/qemu_test_wrapper.sh
        ${QEMU_EXECUTABLE} ${QEMU_COMMON_FLAGS} ${QEMU_TEST_EXTRA_FLAGS}
        -drive file=${CINUX_TEST_IMAGE_PATH},format=raw,index=0,media=disk
    DEPENDS test-image ${AHCI_TEST_IMAGE} regenerate-ext2-image
    ...
)
```

这个 `qemu_test_wrapper.sh` 就是来翻译退出码的(全文很短):

```bash
"$@"
rc=$?

if [ "$rc" -eq 1 ]; then
    # 内核写 0(成功)→ QEMU 退出 1
    exit 0
elif [ "$rc" -eq 3 ]; then
    # 内核写 1(失败)→ QEMU 退出 3
    exit 1
else
    echo "QEMU unexpected exit code: $rc"
    exit "$rc"
fi
```

`"$@"` 把传进来的 QEMU 命令原样跑一遍,拿到真实退出码 `rc`,再做映射:`1 → exit 0`(成功)、`3 → exit 1`(失败)、其它原样透传(通常是 QEMU 自己崩了,不该被当成"测试失败")。这层翻译是让"内核写 0/1"的语义能干净地流到 `make` / CI 的关键。

`run-big-kernel-test`、`run-stress-test` 都是同一套机制,只是换不同的测试镜像(详见正文 004 的内核测试章节,那里会展开"内核在什么时机往 `0xf4` 写值")。

## 3. 主机测试:enable_testing / add_test / add_cinux_test

### 3.1 CTest 是什么、enable_testing 干嘛

CMake 自带一个叫 **CTest** 的测试运行器,它干的活很轻量:**跑一堆命令,看每个命令的退出码,0 是通过、非 0 是失败,再汇总报告**。要用它,得在某个 `CMakeLists.txt` 里先开闸:

```cmake
enable_testing()
```

[test/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/CMakeLists.txt) 第 18 行就是这一句。它把当前目录及子目录纳入 CTest 管理,之后 `add_test(...)` 登记的测试才会被 `ctest` 命令看到。开闸之后,我们就能用 `ctest`、或者 CMake 生成的 `${CMAKE_CTEST_COMMAND}` 来批量跑测试——`test_host` 目标(190-195 行)就是这么做的:

```cmake
add_custom_target(test_host
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS ${ALL_HOST_TESTS}
    ...
)
```

`--output-on-failure` 让失败的测试把 stdout/stderr 打出来,这是排错时最有用的一个开关。

### 3.2 add_cinux_test:一个薄封装

Cinux 把"登记一个主机单元测试"的标准动作收进一个 CMake 函数 `add_cinux_test`(25-31 行),避免每加一个测试就抄一遍四件套:

```cmake
function(add_cinux_test name)
    add_executable(test_${name} unit/test_${name}.cpp ${ARGN})
    target_compile_definitions(test_${name} PRIVATE CINUX_HOST_TEST)
    target_include_directories(test_${name} PRIVATE ${TEST_INCLUDE_DIRS})
    add_test(NAME ${name} COMMAND test_${name})
    set_tests_properties(${name} PROPERTIES LABELS "${name}")
endfunction()
```

它替你做了四件事,值得逐条看懂(因为正文里你会不断看到 `add_cinux_test(xxx)`):

1. **`add_executable(test_${name} unit/test_${name}.cpp ...)`**:约定测试源码就叫 `test/unit/test_<name>.cpp`,可执行文件叫 `test_<name>`。`${ARGN}` 是"函数名之后多出来的参数",用来塞额外源文件(集成测试用)。
2. **`target_compile_definitions(... CINUX_HOST_TEST)`**:给这个测试定义宏 `CINUX_HOST_TEST`。内核里很多代码会 `#ifdef CINUX_HOST_TEST` 切换实现——比如本该用内联汇编读 CR3 的地方,在主机测试里换成纯 C 模拟。没这个宏,内核代码根本编不进普通主机程序。
3. **`target_include_directories(... ${TEST_INCLUDE_DIRS})`**:把测试框架、kernel 源码目录加进头文件搜索路径,这样测试文件能 `#include` 内核头。
4. **`add_test(NAME ${name} COMMAND test_${name})`**:这是 CTest 真正认的那一行——告诉 CTest"有个测试叫 `${name}`,跑它的命令是执行可执行文件 `test_${name}`"。

登记一个测试,就一行:

```cmake
add_cinux_test(smoke)        # → 编译 test/unit/test_smoke.cpp → 跑 test_smoke
add_cinux_test(gdt_idt)
```

`add_cinux_integration_test`(34-44 行)是它的姐妹函数,差别只在源文件不强制走 `unit/test_<name>.cpp` 约定、可以自己列一串内核源码(比如 `${CMAKE_SOURCE_DIR}/kernel/fs/inode.cpp`),用于"要把内核某几个 `.cpp` 真正链进来跑"的集成测试。

### 3.3 怎么跑

主机测试**和 QEMU 完全无关**——它是在你的 Linux 主机上直接编译、直接跑的纯 C++ 程序。日常就两条命令:

```bash
make test_host       # 跑全部主机测试,失败的打印输出
ctest -R smoke       # 只跑名字匹配 smoke 的测试
ctest --verbose      # 详细模式,每个测试的命令和输出都打
```

任务卡要求这里**只讲外部用法**:`add_cinux_test` 框架内部怎么 mock、怎么和内核源码耦合,属于测试框架自己的设计,本前置卷不展开。你只要知道"写个 `test/unit/test_xxx.cpp`,加一行 `add_cinux_test(xxx)`,`make test_host` 就能跑"这一条闭环就够了。

`test_all`(203-208 行)则是把主机测试和内核测试(`run-kernel-test` 那类)串起来,由一个 shell 脚本编排——主机测试全过了,再起 QEMU 跑内核侧的 isa-debug-exit 测试。这是 CI 上一键跑全套的入口。

## 4. 三者怎么串成一条链

把这一章的三块拼起来,完整的"编译→摆盘→运行→判定"链路是这样:

```text
mbr.S / stage2.S / kernel
   │  (boot/、kernel/ 的 CMakeLists 编译 + objcopy)
   ▼
mbr.bin / stage2.bin / mini_kernel.bin / big_kernel
   │  (cmake/qemu.cmake 里的 image 目标 → scripts/build_image.sh)
   │     dd 拼扇区 + 校验 0xAA55
   ▼
cinux.img  ────────────────► make run / make run-debug
   │  (测试镜像 cinux_test.img)        ↑ QEMU + KVM/CI 自适应
   ▼
make run-kernel-test ──► qemu_test_wrapper.sh
   │     内核写 0xf4 → QEMU 退出 (value<<1)|1
   │     包装脚本把 1↔0、3↔1 翻译给 make
   ▼
CI 绿/红

主机侧另开一条:unit/test_*.cpp → add_cinux_test → make test_host (ctest)
```

记住一句话:**镜像拼装靠 `dd` + `0xAA55`,运行靠 `make run*`,内核自报成败靠 `isa-debug-exit` 的 `(value<<1)|1` + bash 翻译,主机测试靠 CTest**。这条链就是 Cinux 从"编出来的字节"走到"测试通过/失败"的全部中介。

## 下一站

工具链卷到这里就齐了:你能编译、能链接、能拼出 BIOS 认的镜像、能塞进 QEMU 跑、能让内核自报成败、能在主机上跑单元测试。下一卷我们正式进实模式——正文 [001 · 实模式引导](../../book/01-boot/001-boot-real-mode.md) 里,你会亲手敲下 `make run`,看着 BIOS 把这章拼好的镜像第 0 扇区读进 `0x7C00`,点亮黑屏。到那时,这一章的 `build_image.sh` 和 `run` 目标,就是"机器真正动起来"的那只手。

---

### 参考

- QEMU 文档 — [isa-debug-exit 设备](https://www.qemu.org/docs/master/system/devices/isa.html)与 `value<<1|1` 退出码编码、`-accel kvm` / `-no-reboot` / `-serial stdio` 语义。
- CMake 文档 — [enable_testing()](https://cmake.org/cmake/help/latest/command/enable_testing.html)、[add_test()](https://cmake.org/cmake/help/latest/command/add_test.html)、[ctest 命令行](https://cmake.org/cmake/help/latest/manual/ctest.1.html)。
- OSDev — [Testing](https://wiki.osdev.org/Testing)(OS 开发中用 QEMU `isa-debug-exit` 做退出判定的社区总结)、[MBR (x86)](https://wiki.osdev.org/MBR_(x86))(`0xAA55` 引导签名)。
- 本仓库源码:[qemu.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/cmake/qemu.cmake)、[build_image.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/build_image.sh)、[qemu_test_wrapper.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/qemu_test_wrapper.sh)、[test/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/CMakeLists.txt)、[mbr.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S)、[boot/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt)。
