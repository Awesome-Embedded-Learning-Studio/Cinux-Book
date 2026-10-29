# Cinux 开发 rootfs

此目录管理 Buildroot 的固定版本、配置覆盖、Cinux overlay 和 CMake 装配入口。Buildroot 源码及所有产物均在构建目录；不在源码树中展开第三方工程。

```sh
git submodule update --init --recursive
cmake -B build -S .
cmake --build build --target run
```

首次构建联网下载 `source.json` 固定的 Buildroot 归档并校验 SHA-256，再由 Buildroot 下载并校验 Bootlin x86-64 glibc 工具链、BusyBox 等包。下载缓存为 `.cache/rootfs/`。默认不构建 Linux 或引导器，启动的内核仍是 Cinux。

宿主的 `mkfs.ext2` 需要支持 libarchive tar 输入。装盘直接读取 Buildroot 的 `rootfs.tar`，保留数值 UID/GID、权限位与符号链接；镜像生成完毕后才替换目标文件。

| 路径 | 内容 |
|---|---|
| `configs/cinux_defconfig` | x86-64、外部 Bootlin 工具链、BusyBox init、tar 根镜像；关闭 merged-/usr 与 Linux 启动脚本 |
| `configs/users.txt` | 开发用户 `cinux`，UID/GID 1000，HOME 为 `/home/cinux` |
| `overlay/` | 版本管理的 Cinux 启动和登录环境配置 |
| `prepare.py` | 生成源码与本机原生开发工具 overlay，不生成基础 rootfs |
| `build.py` | 校验下载、合并配置片段、调用 Buildroot、将其 rootfs.tar 封为 Cinux 可读 ext2 |
| `build/buildroot/source-<version>/` | 解包后的固定版本源码 |
| `build/buildroot/output/` | Buildroot 独立输出；最终配置为 `.config` |
| `build/buildroot/overlay/` | 生成的开发 overlay，作为 `BR2_ROOTFS_OVERLAY` 合入 |
| `build/buildroot/cinux.config` | 解析占位路径、应用覆盖后的输入配置 |
| `build/boot/native-build.ext2` | 1536 MiB 开发卷，1 KiB ext2，无扩展特性 |

只生成配置或 overlay：

```sh
cmake --build build --target buildroot_configure
cmake --build build --target native_build_overlay
```

显式构建 `buildroot_rootfs` 会重建开发磁盘，覆盖其中已有源码修改与构建结果，需要刷新盘内内容时直接跑它。日常用 `run` 或 `run_gui` 复用磁盘，磁盘缺失时由启动目标自动构建一次，不覆盖已有工作。

启动入口统一为 `run`、`run_debug` 和 `run_gui`，默认都挂载这张含 `/home/cinux/Cinux` 的开发盘，内存为 1024 MiB。启动目标构建内核、引导镜像与缺失的辅助磁盘，开发盘缺失时同样自动首次构建，之后持续复用。QEMU 配置集中在 `cmake/qemu/`；需要换盘或调整内存时设置 `CINUX_QEMU_ROOTFS` 和 `CINUX_QEMU_MEMORY`，不增加启动目标。`boot/ext2.img` 保留为生态与内核测试夹具。

结束使用时，在 Cinux 终端运行：

```sh
poweroff
```

先保存源码并结束编译，随后 BusyBox init 会结束用户进程、同步文件系统，再让 QEMU 正常退出。`sync` 可以在工作过程中主动等待缓存和设备刷新；关机系统调用也会再次刷新，刷新失败时保留运行状态。直接关闭 QEMU 窗口或终止宿主进程相当于断电，尚未落盘的内容可能丢失，当前无日志的 ext2 也可能需要离线修复。

运行中的 rename/unlink 使用缓存页副本准备修改，全部目标页就绪后一起发布；准备失败保留原命名空间。打开的 inode 在描述符和映射存活期间不会被复用，最后关闭只记账，由后续 `sync` 回收已删除对象。这不提供断电事务：无日志 ext2 的恢复边界仍如上所述。

目录 fd 的 openat/fstatat 以原后端和节点为起点；不支持的节点相对解析会明确拒绝，不会按旧路径重定向到新目录。当前没有完整的跨挂载目录 walker，RamFs 的父级遍历及部分符号链接解析尚不支持。

持久化回归使用开发盘副本，覆盖编译、`sync`、同步后继续写入、`poweroff`、离线 fsck 与多轮冷启动内容核验。先关闭正在使用源盘的虚拟机，再在宿主运行：

```sh
python3 scripts/test_persistence.py
python3 scripts/test_persistence.py --image build-checks/boot/cinux.img --volume build/boot/native-build.ext2 --gui
```

两条命令串行执行；测试拒绝带文件系统错误的初始副本，保留日志和磁盘现场，不会自动修复或覆盖源盘。

## 配置覆盖

用替代 defconfig 或按顺序叠加片段，后一个片段对同一键的设置优先：

```sh
cmake -B build -S . \
  -DCINUX_BUILDROOT_DEFCONFIG=/path/to/cinux_defconfig \
  '-DCINUX_BUILDROOT_FRAGMENTS=/path/to/base.fragment;/path/to/local.fragment' \
  -DCINUX_BUILDROOT_JOBS=4
```

默认配置中的 `@CINUX_BUSYBOX_CONFIG@`、`@CINUX_NATIVE_OVERLAY@` 和 `@CINUX_USERS_TABLE@` 解析为当前源码、构建目录与用户表的绝对路径。片段可以覆盖包选项、BusyBox 配置、`BR2_ROOTFS_USERS_TABLES` 和 `BR2_ROOTFS_OVERLAY`，包括以空格分隔的多个 overlay 路径；替换 overlay 时应保留开发层，才能使用原生工具。

Buildroot 的工具链、libc 或包选择变化通常需要干净重建。入口发现输入配置变化会明确失败；先保存磁盘里的工作，再运行 `cmake --build build --target buildroot_clean` 清除 Buildroot 输出，然后重新构建。下载缓存保留。建议不同配置直接使用不同 CMake 构建目录；不要手改生成的 `.config` 当作持久配置。

## 原生开发环境

开发盘的 `ping` 使用宿主 iputils 程序及其运行库，普通用户通过 ICMP datagram socket 收发，可直接执行 `ping -c 3 baidu.com`。默认网络为 QEMU 的 `10.0.2.0/24`，公网经 `10.0.2.2` 转发；`/etc/resolv.conf` 使用 `10.0.2.3`，并通过 `options use-vc` 让系统解析器走 DNS over TCP。当前未实现 ICMP 错误队列和 IPv4 选项回传，iputils 对这些可选功能可能输出警告；回显回复、TTL、接收时间戳和丢包统计正常工作。

Linux 宿主默认构建一个 QEMU 辅助共享库，由仓库启动器通过 `LD_PRELOAD` 加载到 QEMU。它在 `slirp_input` 返回后调用 `qemu_notify_event`，让主循环立即重新收集新建的宿主 socket，避免外网 ICMP 回包等待约一秒后批量到达。`run`、`run_debug`、`run_gui` 和测试脚本共用该启动器；不需要手工设置环境变量，也不修改系统 QEMU。此补偿依赖动态链接的 libslirp 和 QEMU 导出的通知符号；对已修复或不兼容的 QEMU，可配置 `-DCINUX_QEMU_SLIRP_WAKEUP=OFF`，或单次使用 `CINUX_QEMU_SLIRP_WAKEUP=0 cmake --build build --target run_gui`。Cinux 机内默认关闭该宿主组件。

串口默认通过 `su - cinux` 进入开发用户，HOME 与初始工作目录都是 `/home/cinux`。`run_gui` 由 init 自动以同一用户启动 Vesper，PTY 终端继承相同身份与环境。项目工作区在 `~/Cinux`，系统程序仍放 `/usr/bin` 等目录。

当前账户身份可用于单人开发，但内核尚未完整实现文件权限隔离：路径访问、文件创建属主、umask、chmod 鉴权及 stat 属主信息仍有缺口。镜像里的 UID/GID 元数据得到保留，不代表普通用户已受到完整 Unix 权限约束；多用户隔离需另行补齐和验收。

Buildroot 提供基础用户环境；目标机的 GCC/G++、binutils、CMake/Ninja/Make、Python、头文件和运行库由 `prepare.py` 打包本机 x86-64 glibc 开发环境。它复用已有 `toolchain_staging`，不把 Buildroot 的交叉编译器误当成机内原生 GCC。现阶段开发工具并非 Buildroot 交叉构建的可移植发行包；换宿主架构或 libc 需要另配工具链。

源码仅复制项目输入，位于 `/home/cinux/Cinux`，不带宿主 build、CMakeCache 或对象。登录环境选用 Ninja，编译池 2、链接池 1，配合 1024 MiB RAM；机内执行：

```sh
cd /home/cinux/Cinux
cmake -B build -S .
cmake --build build -j
```

默认构建项目的引导器、用户程序、完整内核和启动盘；宿主测试、QEMU 和外部工具链的构建留在宿主侧。

想先用几秒钟体验机内编译，可运行随源码带入的 [mini-primes 示例](../../examples/mini-primes/README.md)：

```sh
cp -r ~/Cinux/user/examples/mini-primes ~/mini-primes
cd ~/mini-primes
cmake -B build -S .
cmake --build build -j
./build/mini-primes 30
```

它将两个 C++17 源文件编译、链接为素数列表程序，输出 2..30 中的 10 个素数。示例说明还包含修改源码后增量编译，以及切换 Unix Makefiles 的操作。开发 overlay 将已有的 Cinux 平台文件安装到 CMake 的 Modules/Platform，让独立项目也能识别当前系统。

```sh
python3 scripts/test_native_build.py
```

该验证从空目录配置、并行构建，确认不改源码时不编译、改源码后重编译；将机内启动盘 fsync，提取为 `build/boot/cinux-native.img`，比对机内 SHA-256，再启动产物运行 GCC 与 hello。日志及计时为 `build/native-build*.log`、`build/native-build.metrics.json`。`--reuse` 验证已有构建目录，仍会在客体源码末尾追加空行来确认增量重编译；默认要求空目录。验证会将 CMake 无法读取 CPU/内存信息的诊断视为失败。

`/proc/cpuinfo` 按在线逻辑 CPU 逐项输出 CPUID 标识、品牌、修订和已识别的特征，支持跨读取边界访问。`/proc/meminfo` 使用 PMM 的实际页数与架构页大小，提供 kB 字段及旧式字节表；后者兼容根据内核版本号猜格式的 [CMake 系统信息解析器](https://github.com/Kitware/CMake/blob/master/Source/kwsys/SystemInformation.cxx)，保留 Cinux 自身的版本号。`MemAvailable` 保守取空闲内存，尚未单独计账的 Buffers/Cached 及未实现的 swap 项返回 0，不将这些项目加进可用内存。

GUI：运行 `cmake --build build --target run_gui` 即自动进入桌面，双击终端图标后执行同样三行命令。串口终端同时保留；`run` 和 `run_debug` 进入命令行。三种入口共享开发盘，切换模式不改写盘内配置。

GUI 入口通过 [QEMU fw_cfg](https://www.qemu.org/docs/master/specs/fw_cfg.html) 的 `opt/cinux/init-env` 传递 `CINUX_SESSION=gui`。内核只负责把有界的初始环境项交给 `/sbin/init`；`inittab` 调用 `/bin/sh /etc/init.d/desktop`，由用户态选择是否启动桌面。该环境项缺省时服务直接退出，串口登录继续运行。

`python3 scripts/test_native_gui.py` 在任何串口命令之前等待桌面自动启动，再用 PS/2 输入与 PTY 登录 shell 验证 GUI 配置和源码增量重编译。`python3 scripts/test_native_build.py` 同时检查命令行模式没有误启动桌面。所有 KVM 测试须串行，避免共享测试磁盘。

终端持有焦点时，Tab 送入 PTY 供 shell 补全，其他控件保留 Tab 焦点切换。`python3 scripts/test_gui_tab.py --vesper build/glibc/vesper-desk` 使用开发卷副本验证 `/home/cinux/C<Tab>`、连续目录与文件名补全，以及双 Tab 列候选后继续输入；日志和截图在 `build/gui-tab/`，不会覆盖已有开发卷。

普通 `all` 在 Linux 宿主和 Cinux 中都生成项目程序、内核和启动镜像，不存在按操作系统切换的 `CINUX_NATIVE_BUILD`。Linux 宿主还构建上述 QEMU 唤醒库；host 测试、机内测试、rootfs 打包与 QEMU 运行均由显式目标调用。QEMU 设备与启动参数在 `cmake/qemu/`，rootfs 配方在 `cmake/rootfs.cmake`，测试内核清单在 `cmake/test/kernel_catalog.cmake`。Vesper 使用公开仓库的 `third_party/vesper` Git 子模块，版本由 Cinux 的 gitlink 固定，Visor 版本由 Vesper 固定；无需旁边另有开发仓或额外文件哈希清单。

2026-10-27 实测：宿主和客体使用同一张构建图，客体从空目录完成 284 步构建，增量与源码修改后的重编译通过；产物哈希比对、启动标记、GCC hello 通过。GUI 中通过 PS/2 输入和 PTY 登录 shell 执行两条命令，并验证源码长度变化和对象时间更新。普通及 UBSan/lockdep 镜像的 GUI 回归、检查版开发探针通过。证据见 `build/native-build-test.log`（全量构建；首次重启受宿主 KVM 权限变更阻断）、`build/native-build-resume-test.log`（恢复后全流程 PASS）、`build/native-gui/`、`build/native-acceptance/`。

HOME 布局实测：UID/GID 1000 下完成迁移后的 284 步编译、增量、哈希与产物重启；新打包镜像登录和 CMake 配置通过。GUI 用户身份、补全和内核增量编译通过；`run`、`run_debug`、`run_gui` 实际启动均通过。四组验收全绿。证据在 `build/home-migration/`；权限审计同时记录 root 私有文件仍可读等缺口，未宣称完整用户隔离。
