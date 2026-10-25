# glibc 用户系统

在有可写 `/dev/kvm` 的 x86-64 Linux 开发机上：

```sh
bash user/rootfs/busybox/build.sh
```

这一条命令下载并校验 BusyBox 1.38.0、按仓内配置编译动态 BusyBox、构建内核和用户程序、生成 ext2 系统盘，再启动双核 KVM。依赖 GCC/G++ 14 或更新版本、glibc 开发环境、CMake、make、QEMU、e2fsprogs、Python 3、curl、tar 和 bzip2。编译工具链及复制进盘的 glibc 必须来自同一套宿主运行时。

整条链已并入 CMake 依赖图，也可以直接用构建系统入口：`cmake --build build --target run`（交互启动）、`run_gui`（同链，窗口方式启动）、`run_debug`（冻结复位加 gdb stub），三者都会先备齐镜像和系统盘再开机。`user/rootfs/busybox/build.sh` 是同一入口的薄壳，只负责挑选出口。

```sh
bash user/rootfs/busybox/build.sh build
bash user/rootfs/busybox/build.sh verify
```

`build` 只组装镜像。`verify` 还会启动 KVM，验证用户态 PID 1、14 个 applet、glibc hello、fstab/tmpfs，以及共享内存跨 fork、本地 socket 和 poll。串口证据留在构建目录的 `station33-ecosystem.serial.log`。

| 环境变量 | 默认值 | 用途 |
|---|---|---|
| `CINUX_BUILD_DIR` | 仓库下的 `build` | CMake 和镜像输出目录 |
| `CINUX_ROOTFS_CACHE` | `.cache/rootfs` | 源码归档及解包缓存 |
| `CINUX_ROOTFS_JOBS` | `4` | BusyBox 和 CMake 的构建并发数 |
| `CINUX_BUSYBOX_SOURCE` | 校验后解包的源码目录 | 使用指定的本地源码，跳过下载 |
| `CINUX_GLIBC_LDSO` | `/lib64/ld-linux-x86-64.so.2` | 宿主解释器来源 |
| `CINUX_GLIBC_LIBC` | `gcc -print-file-name=libc.so.6` | 宿主 libc 来源 |

BusyBox 缓存按源码内容、配置、构建脚本、编译器及 libc 校验值失效。上游归档的 SHA-256 固定在 `build.sh` 中；网络失败、校验失败、库缺失和 KVM 缺席都直接报错。

系统盘只装 glibc 动态运行时：解释器位于 `/lib64`，libc、libm、libresolv 位于 `/lib`，程序显式使用 `/lib` 搜索路径。`/sbin/init` 和 `/bin/sh` 是同一个 BusyBox 的硬链接，inittab 的 console 项打开控制终端。默认盘为 64 MiB；musl 造盘模式仅保留作旧站回归入口，不参与本入口。

NVMe 是块设备的第二个实现，系统根盘继续使用 AHCI。默认 QEMU 另挂构建目录内生成的 NVMe scratch 盘；仅识别为 `cinux-nvme` 的测试控制器会执行写读回与队列绕回检查，其他控制器不会被启动自检写入。传输按单个 DMA 页分批，支持 512 至 4096 字节扇区；要求固件已分配 BAR、控制器支持 4 KiB 页，完成记录的命令号、队列号、phase 和错误状态都必须匹配。

本地 socket 提供 stream/datagram、socketpair、bind/listen/connect/accept、半关闭和 poll。名字登记表随 socket 寿命管理；当前不创建文件系统 socket inode，不支持 SCM_RIGHTS 和凭据传递。共享内存提供 shmget/shmat/shmdt，以及 shmctl 的 IPC_STAT/IPC_RMID；最多 16 个具名段，每段最多 1 MiB，IPC_RMID 后已有映射继续存活。

race 检查包含真实本地 socket 路径的双线程有序传输回归，随 host TSan 运行；内核锁顺序继续由 UBSan+lockdep 镜像覆盖。它们覆盖各自执行过的路径，不代表完整的内核数据竞争检测器。
