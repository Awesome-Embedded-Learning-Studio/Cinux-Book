# 必跑验收

BusyBox、GCC/G++、完整内核自编译和原生 CMake 构建均已有实弹脚本。这次补齐的是统一调度、构建类型与 CPU 维度、CI 门禁及失败证据，复用这些验证。

代码最终验收运行四组构建，并在普通与 UBSan+lockdep 两套镜像上运行现有生态实弹：

```sh
CINUX_CHECK_JOBS=4 scripts/checks/acceptance.sh
```

每段内部遇错停止，六段全部运行后汇总失败；不得用关闭检查、忽略退出码或跳过失败用例来报“验收通过”。QEMU 必须有可访问的 `/dev/kvm`，不回退 TCG。沙箱或容器若阻止 KVM/sanitizer 运行，应在允许的宿主环境重跑；未运行成功必须明确记录阻塞项。KVM 用例串行运行，host 用例可并行。不同验收命令也应串行执行，避免争用镜像和键盘实弹的监控 socket。

设置 `CINUX_CHECK_LOG_DIR=build/acceptance-logs` 可在成功时也保留六段完整日志和状态文件；未设置时，失败日志留在临时目录，成功后清理。每格显式指定 Release，避免沿用旧 CMake cache 中的构建类型或检查开关。

| 构建目录 | 检查 | 覆盖范围 |
|---|---|---|
| build | Release 常规编译及 host/KVM 测试 | 全部 host、单核与双核机内测试；刷新 boot_image |
| build-asan | `CINUX_HOST_ASAN=ON`、`CINUX_LOCKDEP=ON` | host 用例及其链接的 base/kernel 逻辑；ASan、UBSan、泄漏检查，UB 不恢复执行 |
| build-tsan | `CINUX_HOST_TSAN=ON`、`CINUX_LOCKDEP=ON` | 同一 host 测试树；Tick 的两生产线程与并发读者用例；遇到数据竞争失败 |
| build-checks | `CINUX_UBSAN=ON`、`CINUX_LOCKDEP=ON` | 普通 host 测试和全部 KVM 机内测试；内核 UB 触发 trap，异常导致测试失败 |
| build / build-checks | ecosystem | glibc/PID1/14 applet、tmpfs/shm/AF_UNIX/poll/RELRO、as/ld/GCC/G++/PIE/exec/sendfile、ping/信号/TTY/PTY/DNS/SMP、TCP、GUI |

完整矩阵使用同一单格入口，可本地串行复现 CI：

```sh
CINUX_CHECK_JOBS=4 bash scripts/checks/matrix.sh
bash scripts/checks/run-cell.sh kernel Debug ubsan build-debug-ubsan
bash scripts/checks/run-cell.sh ecosystem Release checks build-checks
```

`matrix.sh` 默认在 `build-matrix/` 创建隔离构建目录，日志和状态位于 `build-matrix/logs/`；通过 `CINUX_MATRIX_DIR` 改目录。`CINUX_COMPILER_LAUNCHER=ccache` 可启用编译缓存。selfhost/native 导出构建命令时会清空 launcher，保证客体使用真实编译器命令。

| CI 任务 | 构建矩阵 | 执行范围 |
|---|---|---|
| host-tests | Release × ASan+UBSan+LSan / TSan | 全部 host 套件，两格均开启 lockdep |
| kernel-tests | Debug / Release × none / UBSan / lockdep | 六格，每格全部 host、单核/双核机内测试，并启动生产内核验证 PID1、BusyBox 与生态基础件 |
| ecosystem | Release × none / UBSan+lockdep | 复用上表生态脚本；组合检查格额外跑全部 host 与机内测试 |
| selfhost（独立 workflow） | Release × none / UBSan+lockdep | 从源码编译完整内核、提取并核对哈希、新内核启动及 GCC hello |
| native（独立 workflow） | Release × none / UBSan+lockdep | Buildroot 空开发盘、原生 CMake 全量/增量/改源重建、新产物启动、GUI、命令行及 GUI 各三轮持久化/fsck |

前三项与测试基础设施回归在 push/PR/手动触发。完整 selfhost/native 任务每周六北京时间 03:17（UTC 周五 19:17）运行，也可手动触发；本地完整矩阵不默认重复这四项长测试。单独运行：

```sh
bash scripts/checks/run-cell.sh selfhost Release none build-ci-selfhost
bash scripts/checks/run-cell.sh native Release checks build-ci-native
```

native 使用独立的新构建目录，发现已有开发盘就拒绝运行，避免覆盖日常开发成果。生态、网络与 GUI 冒烟使用 `ext2.img` 夹具及 QEMU snapshot，不依赖日常 `native-build.ext2`；完整自编译和持久化使用各自的专用磁盘。

Debug 内核使用 `-O0`，Release 的 host 与内核统一使用 `-O2`，不再由固定 `-O2` 覆盖 Debug，也不沿用 CMake 的 host Release `-O3` 默认值。当前注册 82 个 host CTest 套件、22 个机内测试镜像：19 个镜像分别跑 CPU1/CPU2，`block`、`smp`、`smp_pipe` 的屏障明确要求双核，只注册 CPU2，共 41 个机内 CTest。CPU2 参数不代表每个测试都启动 AP；跨核参与由这三套测试及用户态 SMP 压测各自断言。

本机 GCC 16 的未优化构建会启用 libstdc++ 内部断言。freestanding 内核通过外部 ABI 符号将 `__glibcxx_assert_fail` 接到现有 panic，保留条件、文件、行号与函数；不链接宿主运行库，也不关闭检查。基础设施回归通过真实 `std::atomic` 违约验证该桥接。调度测试的 Shared 所有权放回任务，避免 Debug 引入内核不具备的全局析构注册。Release 首次全量检查另发现堆测试在构造拒绝输入时越过数组边界，已改用合法、但仍在已初始化堆范围外的指针。

SMP 测试用一个有序记录数组保证“引导分配失败注入→AP 自检→跨核调度”的顺序，不依赖编译器排列顶层变量；ICMP 测试检查本次操作的计数增量，不假设其他用例尚未运行。测试基础设施本身的回归入口为 `python3 -m unittest discover -s test/tools -p 'test_ci_checks.py' -v`，模拟 QEMU 的用例只验证调度与失败处理，真实硬件覆盖以 KVM 串口记录为准。

`test/ctest-baseline.json` 锁定已注册的套件及 CPU 腿；每格配置后检查，允许新增，删除/改名须显式更新基线。它不统计套件内部的断言数。机内 runner 同时检查退出码和至少一例通过、零失败的串口总结，拒绝空跑或失败数截断造成的假绿。CTest 的 `RESOURCE_LOCK` 保证并行调用 CTest 时机内测试仍串行。各 CI 格保留 CMake 配置、测试目录清单、CTest/串口日志及相应 GUI 截图。

CI 固定 Ubuntu 24.04 和 GCC/G++ 14，并递归检出 GUI 子模块。KVM 任务先检查设备和 API，缺席即失败；GitHub 对嵌套虚拟化[不提供正式支持保证](https://docs.github.com/en/actions/concepts/runners/github-hosted-runners)，因此不能将某次 runner 的能力当作永久保证。native 使用固定 SHA-256 的 e2fsprogs 1.47.2，启用 libarchive 并实测 tar 装盘；Ubuntu 24.04 自带的 [mke2fs 1.47.0 仅支持目录输入](https://manpages.ubuntu.com/manpages/noble/man8/mke2fs.8.html)，不满足现有 Buildroot 账号元数据装盘路径。

ASan 与 TSan 不能放在同一次构建，配置时同时开启会失败。Host sanitizer 选项不传给 boot、user 或 freestanding kernel；内核 UBSan 使用编译器 trap，无需链接宿主 sanitizer runtime。普通 shell 镜像与内核检查镜像均生成；涉及用户态交互的改动，还必须分别实弹验证对应镜像，自动模块测试不能代替该步骤。

lockdep 沿用 legacy 的锁顺序图思路，SpinLedger 记录 per-CPU 持锁状态。它检测历史 AB/BA、传递环、重复获取、错误释放和容量耗尽；睡眠时持有自旋锁的检查继续保留。通用图、位掩码、定长栈、稳定槽表和引用计数在 base。开关由 CMake 生成 constexpr 配置。

锁身份按地址记录。默认账本中的临时锁在存储回收前必须调用 `Spinlock::retire()`，或使用随测试世界一起销毁的独立 SpinLedger；RamFs 析构已接入 retire。常驻内核锁保持平凡析构，以满足常量初始化单例的约束。图最多记录 64 把锁、16 层同时持有，达到上限会失败，不能静默丢弃边。

当前 lockdep 覆盖自旋锁获取顺序，不代表完整 Mutex 持有顺序验证或内核数据竞争检测。Host TSan 也不检测真实 IRQ/汇编上下文切换。这些边界必须与结果一起说明；后续真实并发逻辑应新增相应并发用例。

站 34 的完整内核自编译实弹使用独立磁盘，先组装源码卷，再启动 KVM：

```sh
cmake -S . -B build
cmake --build build --parallel 4 --target kernel_selfhost_volume
python3 scripts/test_station34_kernel_selfhost.py
```

宿主导出当前 CMake 配置的编译、链接命令，准备 GCC/G++、binutils、Python 运行环境、源码、生成配置和引导器。源码卷不携带内核对象或已链接内核。Cinux 从空输出目录编译全部内核源文件，在机内生成资源对象和 kallsyms、完成两轮链接、核验镜像头并封装启动盘，经 `fsync` 落盘。验收脚本关闭 QEMU 后提取产物，核对机内 SHA-256，再用这个产物启动，并重新编译、运行 hello。

产物是 `build/boot/cinux-selfhost.img` 和 `build/kernel/cinux_kernel_selfhost`；编译及重启日志分别为 `build/station34-kernel-selfhost.serial.log`、`build/station34-kernel-selfhost-reboot.serial.log`。源码卷为 256 MiB，guest 内存为 512 MiB。该步骤编译完整内核；引导器和用户态工具链作为自举输入保留。`build-checks` 可用同样的目标和 `--image build-checks/boot/cinux.img` 验证内核检查配置。全部构建树与 QEMU 仍须串行运行。

第一批性能优化将根文件系统块缓存从固定 32 KiB 改为按初始化时空闲 RAM 的 1/32 预算、最大 64 MiB，缓存页按需分配，使用哈希索引与 CLOCK 淘汰。2026-10-26 在相同 KVM、2 vCPU、512 MiB、串行编译及原有 `-g -O2` 参数下，前后匹配的 138 个编译单元累计耗时为 834.685s / 281.288s，约快 2.97 倍；完整 188 次编译、两轮链接及镜像封装共 369.811s，产物启动与 GCC 编译运行通过。旧基线在后续目录容量限制处失败，因此只比较双方完成的相同单元，不作完整构建耗时对比。ext2 目录按需增长、动态描述符表及 `RLIMIT_NOFILE` 同时保证完整链接能够完成；统一文件页缓存、目录项缓存及并行构建留待后续测量。

站 35 的 GUI 实弹使用同一张 ext2 卷，但输入通道必须摘掉 USB 键鼠对（QEMU 的 sendkey 优先喂 usb-kbd，桌面的事件节点走 PS/2）：

```sh
cmake -S . -B build
cmake --build build --parallel 4 --target boot_image ext2_img
python3 scripts/test_station35_gui.py
```

脚本自起 QEMU（KVM、串口兼监控 Unix socket），断言桌面铺满 framebuffer、软件光标移动且旧位置恢复、含 `0xFA/0xFE` 的小幅位移不破坏包对齐、双击图标开终端窗口、sendkey 打字经 PTY 到 busybox ash 回显（EVIOCGRAB 成立、串口零漏字）、按住标题栏相对位移拖动窗口。截图与监控 socket 落在 build/station35/，`--keep` 保留 PPM 证据。内核检查镜像另跑 `python3 scripts/test_station35_gui.py --image build-checks/boot/cinux.img`。

`cmake --build build --target run_gui` 同样摘掉 USB HID 对，并开启 GTK 的 `zoom-to-fit`；在 WSLg 中拖动窗口边缘，整张桌面会随显示区域缩放。客体仍使用启动时的 VESA 分辨率，不会因宿主窗口变化而切换显示模式。PS/2 鼠标需先点击画面捕获；按 `Ctrl+Alt+G` 释放，Vesper 自己绘制光标。最终 GUI 参数同时写入各构建目录的 `qemu-gui-args.txt`，方便核对与复现。

桌面源码由公开仓库的 Git 子模块放在 `third_party/vesper`，版本由 gitlink 固定；初始化使用 `git submodule update --init --recursive`。源码和头文件是产物依赖，更新后会重新构建静态桌面并刷新 ext2；每个 Cinux 构建目录使用自己的 `third_party/vesper/` 输出目录。

日常 `run`、`run_debug` 和 `run_gui` 统一挂载 Buildroot 开发盘 `boot/native-build.ext2`，含 `/home/cinux/Cinux`。首次准备按下面的 `buildroot_rootfs` 命令执行；之后启动会复用盘内工作。`qemu-run-args.txt` 记录日常运行参数，`qemu-gui-args.txt` 记录 GUI 参数；测试仍使用 `qemu-device-args.txt` 中的生态夹具盘，避免改写开发环境。

`run_gui` 传递 GUI 初始环境，由 BusyBox init 自动以 `cinux` 用户启动桌面，串口终端继续可用；`run`、`run_debug` 保持命令行。开发盘上的 `inittab` 和启动服务共用，无需切换或改写 rootfs。GUI 编译与补全脚本先等待自动桌面出现，再执行测试命令。

完整项目的原生 CMake 构建及 Buildroot 配置由 [user/rootfs/buildroot](../../user/rootfs/buildroot/README.md) 管理：

```sh
cmake -B build -S .
cmake --build build --parallel 4 --target buildroot_rootfs
python3 scripts/test_native_build.py
python3 scripts/test_native_gui.py
```

该路径使用 Buildroot 产出的基础 rootfs，并通过 `BR2_ROOTFS_OVERLAY` 合入源码和原生开发工具。从空目录运行真实 CMake/Ninja，覆盖引导器、用户程序、完整内核和启动镜像，再核对产物哈希、重新启动并编译运行 hello。开发卷为 1536 MiB，guest 为 2 vCPU、1024 MiB RAM；日志 `build/native-build.serial.log`、`build/native-build-reboot.serial.log`，GUI 证据 `build/native-gui/`。KVM 与四矩阵验收串行。显式重建 rootfs 会覆盖开发卷；日常使用 `run` / `run_gui` 保留卷中工作。配置、覆盖片段、工具来源及重建边界详见上述目录说明。
