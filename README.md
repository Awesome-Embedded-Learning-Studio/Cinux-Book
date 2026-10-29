<div align="center">

# Cinux

### 从 512 字节的 MBR 起步，一路长到能把自己重新编译一遍

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)]()
[![CMake](https://img.shields.io/badge/CMake-supported-blue)]()
[![Docs](https://img.shields.io/badge/docs-VitePress-42b883)](https://awesome-embedded-learning-studio.github.io/Cinux-Book/)

[在线阅读](https://awesome-embedded-learning-studio.github.io/Cinux-Book/) · [从第一站开始](https://awesome-embedded-learning-studio.github.io/Cinux-Book/journey/01-mbr/) · [路线](#这本书怎么走)

</div>

---

**Cinux** 是一个从零手搓的 x86_64 操作系统，也是一部边写边讲的中文教程。

从实模式的第一行引导代码开始，保护模式、长模式、物理内存、进程与调度、系统调用、ext2、多核、USB、网卡、TCP/IP、图形桌面，一站一站自己长出来；最后在图形桌面里，用它自己的 GCC 把内核重新编译一遍。

代码和正文同步推进：每一站对应一个可以 checkout 的 tag，正文只讲清楚「为什么是这样」，剩下的交给你在 QEMU 里亲手敲。

> 当前状态：主线打到 `r35_gui_fin`（图形桌面与自编译），正文连载至网络协议栈与网卡驱动。

## 一眼看看

<table>
<tr>
<td align="center" width="33%"><img src="assets/README/boot.webp" alt="开机日志"><br><sub>开机：VESA 1024×768、PCI 枚举、AHCI / NVMe / xHCI 就位</sub></td>
<td align="center" width="33%"><img src="assets/README/gui.webp" alt="Vesper 桌面"><br><sub>Vesper 桌面：帧缓冲、窗口、图标与输入</sub></td>
<td align="center" width="33%"><img src="assets/README/cli.webp" alt="shell 与进程表"><br><sub><code>ls /</code> 与 <code>ps</code>：文件树和真实进程表</sub></td>
</tr>
<tr>
<td align="center"><img src="assets/README/filesystem.webp" alt="ext2 与挂载表"><br><sub>ext2 目录、属主、权限，<code>mount</code> 看挂载</sub></td>
<td align="center"><img src="assets/README/parallel.webp" alt="双核"><br><sub><code>/proc/cpuinfo</code>：两颗核都活着</sub></td>
<td align="center"><img src="assets/README/net.webp" alt="外网 ping"><br><sub><code>ping 1.1.1.1</code>：走 QEMU 用户态网络，真外网</sub></td>
</tr>
</table>

## 三十秒跑起来

```bash
git clone --recursive https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book.git
cd Cinux-Book
cmake -B build -S .
cmake --build build --target run_gui -j
```

QEMU 窗口里亮起来的就是 Cinux：开机日志滚完，直接进 Vesper 桌面。第一次构建会联网下载固定版本的 Buildroot，烤一张 1.5 GiB 的开发盘，视网速可能要等一会儿；之后全部走本地缓存复用。

只想看命令行就把 `run_gui` 换成 `run`，要带调试符号的内核用 `run_debug`。

<p align="center">
  <img src="assets/README/boot_cli.gif" width="720" alt="从 run 开始：开机日志滚完就是 shell">
</p>

## 进去以后能干什么

- 开个终端，`ping baidu.com`，回包是真的从外网绕回来的；
- `ps` 看进程表，`ls /` 看 ext2 目录，`mount` 看挂载关系；
- `~/Cinux` 放着整份源码，`cmake --build build -j` 就在系统里重新编译内核：

<p align="center">
  <img src="assets/README/compile_self.gif" width="512" alt="在 Cinux 里编译 Cinux">
</p>

## 这本书怎么走

全书沿着真实的开发史推进，主线从 `000_env_toolchain` 一直到 `r35_gui_fin`，共 31 个 tag：

```
引导 → 内核 → 内存 → 进程 → 用户态 → 存储 → 并发 → 多核 → 驱动 → 网络 → 桌面 → 自编译
```

跟着读，就一站一站 checkout：

```bash
git checkout r34_compile_self   # 例如：在 Cinux 里编译 Cinux 的那一站
```

正文按卷分册，配状态面板与实操截图，从[第一站](https://awesome-embedded-learning-studio.github.io/Cinux-Book/journey/01-mbr/)开始读；也可以直接 checkout 任意一个 tag，在源码里沉浸。

> 31 个 tag 不等于 31 个连续的站号：`r17`、`r18`、`r28`–`r30` 的代码已经在主线，tag 还在补。

## 装备你的机器

最小配置是 GCC ≥ 14 加 CMake ≥ 3.16，够跑宿主测试：

```bash
cmake --build build --target test_host -j$(nproc)
```

要跑完整系统（QEMU 引导加 Buildroot 开发盘），按发行版装一套。

Ubuntu / Debian：

```bash
sudo apt-get install -y gcc-14 g++-14 gcc-14-multilib g++-14-multilib \
  ccache cmake ninja-build make binutils libc6-dev python3 curl bzip2 xz-utils \
  wget perl patch \
  qemu-system-x86 e2fsprogs file bc bison flex rsync cpio unzip \
  libncurses-dev libssl-dev libarchive-dev pkg-config
```

Arch：

```bash
sudo pacman -S --needed base-devel wget unzip bc \
  cmake ninja qemu-system-x86 qemu-ui-gtk e2fsprogs file bison flex \
  rsync cpio ncurses libarchive openssl pkgconf
```

四个已知差异，撞上再回来看：

- 裸装 Ubuntu 常缺 `wget`、`perl`、`patch`，Buildroot 的宿主检查会拦，上面 apt 行已带上；
- `mkfs.ext2` 需要 libarchive 的 tar 输入能力：Ubuntu 24.04 自带的 1.47.0 只认目录输入，跑 `bash scripts/checks/setup-mke2fs.sh <安装前缀>` 自编 1.47.2 解决；Arch、Fedora 的发行版直接可用；
- Arch 把 QEMU 的 GTK 界面拆在 `qemu-ui-gtk` 分包里，缺它 `run_gui` 会报 `Display 'gtk' is not available`；
- WSL2 实测可用（WSLg 负责弹窗），首次下载大件时出口带宽小就挂代理或多连接下载。

## 仓库地图

| 路径 | 内容 |
| --- | --- |
| `document/` | 教程正文与站点配置（VitePress 分卷并发构建） |
| `document/journey/` | 主线各站章节 |
| `kernel/` | 内核本体：引导、内存、进程、文件系统、驱动、网络 |
| `user/` | 用户态程序、Vesper 桌面与 Buildroot 开发盘集成 |
| `cmake/qemu/` | QEMU 设备与启动参数配方 |
| `scripts/checks/` | 验收矩阵与 CI 编排脚本 |

## 测试与验收

宿主侧一条 `ctest` 就能跑完：

```bash
cmake --build build --target test_host -j$(nproc)
ctest --test-dir build/test --output-on-failure
```

更完整的验收矩阵覆盖宿主 sanitizer（ASan、TSan）、机内单核与双核、UBSan 与 lockdep 检查、生态冒烟和定时自编译实弹：

```bash
bash scripts/checks/acceptance.sh
```

细节见[测试检查说明](scripts/checks/README.md)。

## 相关仓库

| 相关仓库 | 定位 |
| --- | --- |
| [Cinux-Book](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book)（本仓） | x86_64 OS 中文教程，主线建设中 |
| [Cinux](https://github.com/Awesome-Embedded-Learning-Studio/Cinux) | 更前沿的 Cinux 开发线 |
| [Awesome-Embedded](https://github.com/Awesome-Embedded-Learning-Studio/Awesome-Embedded) | 组织总导航与项目索引 |
| [PenguinLab](https://github.com/Awesome-Embedded-Learning-Studio/PenguinLab) | Linux/Embedded Linux 内核到用户态实验 |

v1 的完整可跑内核（Bootloader 到 GUI 桌面）与旧版教程保留在 [`legacy/old_site`](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/tree/legacy/old_site) 分支，完整开发历程的历史 tag 也都还在。

## 为什么叫 Cinux

C/C++'s Linux，也是 CharlieChen's \*nix（逃）。

## 许可证

[MIT](LICENSE)
