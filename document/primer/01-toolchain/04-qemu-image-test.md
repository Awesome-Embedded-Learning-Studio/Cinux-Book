---
title: 04 · QEMU、磁盘镜像与主机测试
---

# 04 · QEMU、磁盘镜像与主机测试:让机器真跑起来

> 咱们看的源码对齐 commit `9bf0b05`(2026-09-13)。文中行号、脚本与 CMake 的行为都以这个版本为准;仓库往前走了,您 `git checkout 9bf0b05` 再对照就能复现,或把下面 GitHub 链接里的 `main` 换成这串短哈希。

## 内核写 0,QEMU 的退出码却是 1

正文 `001` 里咱们敲下 `make run`,QEMU 弹窗,黑屏点亮。可这只是"人眼看着像跑起来了",CI 没有眼睛,它凭什么判定这一版内核跑对了?Cinux 的答案是让内核自己按值退出:测试跑完往 I/O 端口 `0xf4` 写一个字节,QEMU 收到就退出,退出码按固定公式从这个字节算出来。怪就怪在公式:内核写 `0` 表示"全部通过",QEMU 的进程退出码偏偏是 `1`;内核缺页崩了,退出码会是 `33`,咱们不看日志也能反推出死因。这套机关,连同拼磁盘镜像、起 QEMU、跑主机单测,合起来就是工具链卷的收尾:从"编出来的字节"到"测试通过与失败"的整条路。咱们按数据流走:拼盘,起机器,最后看退出码怎么翻译。

## 拼一张 BIOS 认的盘

### BIOS 只认一个魔数

BIOS 自检完只做一件事:去启动盘第 0 扇区读 512 字节,看最后两个字节是不是 `55 aa`。是,就把这 512 字节读进物理地址 `0x7C00` 并跳过去执行;不是,报 "No bootable device" 完事。那咱们编译出的 `mbr.bin` 末尾凭什么有这对字节?它不在拼盘脚本里,写在 `boot/mbr.S` 的末尾:

```asm
// Pad to 510 bytes, then MBR signature
.org 510
.word 0xAA55
```

`.org 510` 把当前位置对齐到偏移 510,`.word 0xAA55` 再补两个字节,合计恰好一个扇区。MBR 的链接脚本把基址固定在 `0x7C00`,经 `objcopy -O binary` 抽成裸二进制,512 字节不多不少。空口无凭,咱们在仓库里真跑一遍(镜像由 `make image` 产出):

```bash
$ xxd -s 510 -l 2 build/cinux.img
000001fe: 55aa                                     U.
$ stat -c %s build/cinux.img
2097152
```

咱们看到的正是 `55aa`,整张镜像 2MB。这对魔数要是丢了(比如 MBR 代码膨胀过了 510 字节,`.org` 对齐失效),后面拼盘脚本的自检会当场抓住它。

### dd 拼扇区,布局由读盘代码说了算

BIOS 只认扇区,咱们手上却是一堆各自编出来的 `.bin`。`scripts/build_image.sh` 的活就是把它们按约定的 LBA(线性扇区号)摆进一张空白盘:扇区 0 放 MBR,1 到 15 放 Stage2,16 起放 mini 内核,big 内核有就铺在 848 之后;底板默认 1MB,内容装不下就自动加大。这些编号看着任意,其实**一个都动不得:MBR 用 `INT 0x13` 扩展读盘时,DAP 里填的扇区号是几,镜像里对应的内容就得摆在第几扇区**。布局和读盘代码对不上号,读进来的就是一坨垃圾,而且往往不报错,只是后面莫名跳飞。

拼装的活全交给 `dd`,咱们要看的主段在 `build_image.sh` 的 174 行起:

```bash
dd if=/dev/zero of="$OUTPUT_IMAGE" bs=1M count=$IMAGE_SIZE_MB status=none

# Step 2: Write MBR to sector 0
dd if="$MBR_BIN" of="$OUTPUT_IMAGE" bs=512 count=1 conv=notrunc status=none

# Step 3: Write Stage2 starting at sector 1
dd if="$STAGE2_BIN" of="$OUTPUT_IMAGE" bs=512 seek=$STAGE2_LBA conv=notrunc status=none

# Step 4: Write mini kernel starting at sector 16
dd if="$MINI_BIN"  of="$OUTPUT_IMAGE" bs=512 seek=$MINI_KERNEL_LBA conv=notrunc status=none
```

`bs=512 seek=N` 从第 N 块开始写,块大小恰好对上扇区号,读盘代码里的 LBA 常量可以直接当 seek 用。**`conv=notrunc` 您千万别省**:不加它,`dd` 会把镜像截断到本次写入的长度,前面摆好的内容全部丢掉,而且一声不吭。脚本收尾还会按字节抓第 510、511 位比对 `55aa`,并校验两档尺寸:Stage2 超过 15 扇区、mini 内核超过 416KB 都直接报错退出。416KB 有来历——脚本注释里算得明白:实模式加载区只有 `0x20000` 到 `0x88000` 这一段,再往上就撞栈了,`0x88000 - 0x20000 = 0x68000`,正好 416KB。内核塞超了,启动阶段必撞内存,这道闸把它拦在镜像期。

## 起 QEMU:run 与 run-debug

`cmake/qemu.cmake` 把所有 `run*` 目标收进一个辅助函数 `cinex_qemu_run_target`,用声明式的开关(SMP、DEV_NET、DEV_XHCI……)拼出不同配置。咱们日常敲的 `make run`,背后就是一行 `cinex_qemu_run_target(run SMP DEV_NET DEV_XHCI DEV_VIRTIO_BLK DEV_VIRTIO_NET ...)`:先依赖 `image` 目标拼出最新镜像,再带着 `-serial stdio` 起机器。串口接到当前终端,内核打的日志咱们在命令行里直接看得到;公共旗子里还有一个 `-debugcon`(I/O 口 `0xE9`),把调试输出写进 `debug.log`,正文排错时它会再出场。

`make run-debug` 在同一函数上开 `DEBUG` 档,多两面旗子:

```cmake
set(QEMU_DEBUG_FLAGS
    -s      # GDB stub on :1234
    -S      # Stop at startup (for debugging)
)
```

`-s` 是 `-gdb tcp::1234` 的简写;`-S` 让 QEMU 上电后连第一条指令都不执行,停在那里等人。所以您第一次跑 `run-debug`,窗口全黑、机器纹丝不动——它是在等 GDB,`make run-gdb` 会把连接这步也替咱们做好。

加速这一档有个真实的坑,值得咱们记一笔。现在的代码默认走 TCG 纯软件模拟,要用 KVM 得 `-DCINUX_USE_KVM=ON` 显式打开,而且还得 `/dev/kvm` 真存在。为什么默认不开?`qemu.cmake` 顶部注释记着事故:宿主机 `/dev/kvm` 的属组一度漂移成 `kmem`,普通用户(只在 `kvm` 组里)直接 permission denied,KVM 整个不可用,于是 2026-07-06 起改成显式开关。另外无论哪个后端都跟一句 `-cpu max`:qemu64 这个默认 CPU 不广告 SMAP/SMEP,真内核里的 `stac/clac` 会直接 `#UD`。环境靠不住的时候,把"要不要 KVM"从猜测变成开关,是这类工具脚本该有的诚实。

CI 那头,`qemu.cmake` 用 `if(DEFINED ENV{CI})` 分岔:CI 环境没有显示器、内存也给不起,于是 1G 内存加 `-vnc :0` 不弹窗;本机交互才给 8G。所有 `run*` 目标吃同一份公共旗子,KVM 与 CI 的差异就这样被自动消化,咱们敲的命令一个字不用改。

## 按值退出:isa-debug-exit 的编码与翻译

现在咱们回到开头的怪事。测试镜像会多挂一个设备(挂进 `QEMU_TEST_EXTRA_FLAGS` 的动作在 `qemu.cmake` 的 213 行):

```cmake
-device isa-debug-exit,iobase=0xf4,iosize=0x04
```

内核往 `0xf4` 写一个字节,QEMU 立刻退出,退出码怎么算,`qemu.cmake` 的 238 行起有一段注释,咱们原样抄来:

```text
# QEMU's isa-debug-exit device encodes: exit_code = (value << 1) | 1
#   Kernel writes 0 → QEMU exits 1   → test SUCCESS
#   Kernel writes 1 → QEMU exits 3   → test FAILURE (unit test failed)
#   Panic writes a cause-coded value → QEMU exits (value<<1)|1, FAST (no
#   cli;hlt → timeout stall): exception panic value = vector+2 (#DF(8)→21,
#   #PF(14)→33, #GP(13)→31), generic kpanic value = 64 → exit 129.
```

咱们把这个公式拆开看:为什么故意左移一位再或 1?为了让退出码永远不为 `0`。QEMU 自己崩溃、被信号杀死,退出码本来就非零;现在"内核说成功"也映射到 `1`,shell 里"0 才是正常退出"的约定就永远不会被误触发。代价是 `make` 看到退出码 `1` 直接报红——明明内核写的是"成功"。翻译这一步交给 `scripts/qemu_test_wrapper.sh`,`run-kernel-test` 目标不直接调 QEMU,先过它:

```bash
"$@"
rc=$?

if [ "$rc" -eq 1 ]; then
    # Kernel wrote 0 (success) → QEMU exit 1
    exit 0
elif [ "$rc" -eq 3 ]; then
    # Kernel wrote 1 (failure) → QEMU exit 3
    exit 1
elif [ "$rc" -eq 129 ]; then
    echo "KERNEL PANIC: kpanic/assertion (QEMU exit $rc)"
    exit "$rc"
else
    # Exception-fault panic (isa-debug-exit) or a raw QEMU crash. Decode the
    # vector when it fits the value=vector+2 scheme; otherwise report raw.
    vec=$(( (rc - 1) / 2 - 2 ))
    if [ "$rc" -ge 5 ] && [ "$rc" -le 67 ] && [ "$vec" -ge 0 ] && [ "$vec" -le 31 ]; then
        echo "KERNEL PANIC: exception vector $vec (QEMU exit $rc)"
    else
        echo "QEMU unexpected exit code: $rc"
    fi
    exit "$rc"
fi
```

成功与失败翻译回 make 的世界;退出码 `129` 是内核主动 `kpanic`,脚本把死因打成一行再原样传码;落在 5 到 67 之间的码,按 `(rc-1)/2-2` 反解出异常向量,咱们拿 `33` 一算就是 `#PF(14)` 把内核打死的。CI 不用翻日志,看退出码就知道该去哪儿查。这套"快死加报死因"的设计还有个动机写在注释里:内核 panic 后若只是 `cli;hlt` 死循环,QEMU 根本不会退出,CI 会傻等到超时;让它按值退出,失败传得又快又带原因。

## 主机单测:CTest 与 add_cinux_test

内核之外,咱们还有一条纯主机的测试线。CMake 自带 CTest,干的活很朴素:跑一批命令,按退出码判定,汇总报告。开闸只要在 `test/CMakeLists.txt` 里一句 `enable_testing()`,之后 `add_test` 登记的测试才能被 `ctest` 看到。Cinux 把登记动作收进一个函数(`test/CMakeLists.txt` 的 60 行起),以后每加一个测试,一行 `add_cinux_test(xxx)` 完事:

```cmake
function(add_cinux_test name)
    add_executable(test_${name} unit/test_${name}.cpp ${ARGN})
    target_compile_definitions(test_${name} PRIVATE CINUX_HOST_TEST)
    target_include_directories(test_${name} PRIVATE ${TEST_INCLUDE_DIRS})
    add_test(NAME ${name} COMMAND test_${name})
    set_tests_properties(${name} PROPERTIES LABELS "${name}")
    list(APPEND ALL_HOST_TESTS test_${name})
    set(ALL_HOST_TESTS ${ALL_HOST_TESTS} PARENT_SCOPE)
endfunction()
```

函数体五行各司其职,咱们按效果记:约定测试源码叫 `unit/test_<名字>.cpp`、可执行文件叫 `test_<名字>`,`${ARGN}` 接住额外源文件;给编译定义挂上 `CINUX_HOST_TEST`,内核代码里那些 `#ifdef CINUX_HOST_TEST` 分支(比如把读 CR3 的内联汇编换成纯 C 模拟)才会打开,内核头才能编进普通主机程序;`add_test` 那一行才是 CTest 真正认的登记;末尾两行把新测试自动汇进 `ALL_HOST_TESTS` 清单。这两行自动登记有段血泪史,注释里写着:以前靠手工维护清单,后加的测试会静默漏跑,网络栈、extable、pty、tty 都漏过,后来才改成函数里自动收编。

日常两条命令:

```bash
make test_host     # 全部主机单测,失败时打印输出
ctest -R smoke     # 只跑名字匹配 smoke 的测试
```

`make test_host` 调的就是 CTest,带着 `--output-on-failure`,失败用例的输出直接糊在咱们脸上,排错最顺手的一个开关。`make test_all` 再往外扩一圈:先跑主机单测,全过了再起 QEMU 跑内核侧的按值退出测试,由一个生成的 shell 脚本编排,CI 上一键全套就是它。

## 串起来:从字节到判定

```text
mbr.S / stage2.S / kernel
   │  (编译 + objcopy)
   ▼
mbr.bin / stage2.bin / mini_kernel.bin / big_kernel
   │  (build_image.sh:dd 拼扇区 + 55aa 自检 + 尺寸闸)
   ▼
cinux.img ──── make run / run-debug(QEMU,KVM/CI 自适应)
   │
   ▼
make run-kernel-test ─► qemu_test_wrapper.sh
   │     内核写 0xf4,退出码 (value<<1)|1
   │     1=成功、3=失败、129/向量=panic 死因
   ▼
CI 判定

主机侧另开一条:unit/test_*.cpp → add_cinux_test → make test_host (ctest)
```

整条链咱们现在都摸过了:拼盘靠 `dd` 加魔数自检,跑机器靠 `run*` 目标,内核自报成败靠 `isa-debug-exit` 的编码加一层 bash 翻译,主机单测靠 CTest。从"编出来的字节"到"测试通过与失败",中间没有一个环节靠人眼。

## 下一站

工具链卷到这里就齐了:咱们能编译、能拼出 BIOS 认的镜像、能塞进 QEMU 跑、能让内核自报成败、能在主机上跑单测。下一卷正式进实模式,正文 [001 · 实模式引导](../../book/01-boot/001/) 里咱们亲手敲下 `make run`,看 BIOS 把这章拼好的第 0 扇区读进 `0x7C00`,点亮黑屏。到那时,这一章的 `build_image.sh` 和 `run` 目标,就是"机器真正动起来"的那只手。

---

### 参考

- QEMU 文档:[isa-debug-exit 设备](https://www.qemu.org/docs/master/system/devices/isa.html)与 `(value<<1)|1` 退出码编码、`-accel kvm` / `-no-reboot` / `-serial stdio` 语义。
- CMake 文档:[enable_testing()](https://cmake.org/cmake/help/latest/command/enable_testing.html)、[add_test()](https://cmake.org/cmake/help/latest/command/add_test.html)、[ctest 命令行](https://cmake.org/cmake/help/latest/manual/ctest.1.html)。
- OSDev:[Testing](https://wiki.osdev.org/Testing)(QEMU 按值退出的社区总结)、[MBR (x86)](https://wiki.osdev.org/MBR_(x86))(`0xAA55` 引导签名)。
- 本仓库源码(对齐 `9bf0b05`):[qemu.cmake](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/9bf0b05/cmake/qemu.cmake)、[build_image.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/9bf0b05/scripts/build_image.sh)、[qemu_test_wrapper.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/9bf0b05/scripts/qemu_test_wrapper.sh)、[test/CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/9bf0b05/test/CMakeLists.txt)、[mbr.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/9bf0b05/boot/mbr.S)。
