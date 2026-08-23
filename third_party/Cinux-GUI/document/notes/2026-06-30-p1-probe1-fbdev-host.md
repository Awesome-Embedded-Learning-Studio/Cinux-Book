# 2026-06-30 · P1 Probe-1 · Linux fbdev+evdev host（第二个真 host）

> P0 standalone 跑通后，P1 用真 Linux fbdev + evdev 当第二个真 host，证明 Host ABI 解耦缝干净——同一个 core（pump/staging/swraster/region）只换表填充，就从 offscreen/replay 换成真屏+真鼠标。**代码 + 单测 + 真 QEMU 冒烟全部 PASS（2026-07-01）。**

## 背景

P0 的 offscreen/replay host 是「假 host」（无真屏/真鼠标，事件 scripted）。P1 要「真 host」：mmap `/dev/fb0` 当屏、读 `/dev/input/event*` 当鼠标，跑在 QEMU Linux 上，真鼠标动→光标跟。这是「证解耦缝」的临门一脚。

## 目标

1. fbdev host（fbdev mmap + evdev + Host 表），复用 b2/b3 场景 + 拖拽状态机。
2. evdev `input_event` 流 → `PointerPayload` 的转换可单测（不依赖真设备）。
3. 真 QEMU 冒烟 PASS（自构建 kernel + initramfs，见下）。

## 设计

### `host/fbdev_device.{hpp,cpp}` — fbdev 后端

`open("/dev/fb0")` → `ioctl(FBIOGET_VSCREENINFO/FIXEDSCREENINFO)` 拿 xres/yres/line_length → `mmap`。`blit_rect(x,y,w,h,src,src_stride)` 把 core staging 的脏 rect memcpy 进 framebuffer（按 fb stride 定位行，不是 staging stride）。32bpp XRGB8888 路径；其他布局 `ready()==false`。

### `host/evdev_device.{hpp,cpp}` — evdev 后端 + 纯累加器

**`EvdevAccumulator`（纯函数，单测核心）**：evdev 一次 `read` 给的是 `input_event` 记录流，一个完整 pointer frame = 两次 `EV_SYN` 之间的变化集。`feed(type,code,value)` 累积 ABS_X/ABS_Y/EV_KEY（BTN_LEFT/RIGHT/MIDDLE），`finish()` 在 SYN 时产出一帧（位置 + buttons + kind：button 边沿→down/up，否则 move）并复位。touched 标志区分 idle SYN。

**`EvdevDevice`**：`open` 抢占设备（`EVIOCGRAB`，避免 console 也收）+ `O_NONBLOCK`；`read_frame()` 非阻塞 drain，喂 accumulator，遇 SYN 返回一帧。

### `host/linux_fbdev_main.cpp` — host main

组装 Host 表：`poll_event`=evdev→Event，`dispatch_event`=拖拽状态机（同 b3），`render_frame`=场景+真脏区（同 b3），`flush`=`fb.blit_rect`。主循环 `for(;;) core.pump(); usleep(16000)`（~60fps 节流；idle 帧 flush 0）。`timeout 40` 外部杀。argv：`argv[1]=fb` 路径（默认 `/dev/fb0`），`argv[2]=event` 路径（默认 `/dev/input/event0`）。

### `test/test_evdev.cpp` — accumulator 单测

喂合成 `input_event` 三元组，验：idle SYN / ABS move / BTN 边沿 down/up / 按住移动（无新边沿）/ 同值 idle / 多键位。**不碰 fd**，纯逻辑。

## 决策

- **EvdevAccumulator 纯函数**：evdev→pointer 转换是 P1 的复杂点；抽成无 fd 的状态机，单测喂 fake 流，不依赖 `/dev/input/event*`。真设备只影响 `EvdevDevice::read_frame`（把 fd 字节喂给 accumulator）。
- **SYN_DROPPED 重置**：内核丢事件时，P1 简化重置 accumulator（位置归零）。后续可 `EVIOCGABS` 查当前绝对轴值恢复——P1 冒烟够用。
- **`usleep(16000)` 节流**：P1 简单（轻忙等，idle 不写屏）。真 `poll(fd)` 阻塞等输入留后续优化。
- **组织：`test/` vs `host/`**：纯单测（不填 Host 表）进 `test/`；host 适配器 + harness main（填 Host 表，含 fake/offscreen/replay/linux-fbdev）留 `host/`。已写进 CODING-TASTE §2。

## 陷阱

- **evdev 不是一 read 一事件**：一帧鼠标 = 多个 `input_event`（X/Y/button 各一条）+ `EV_SYN`。必须 accumulator 同步。
- **`poll_event` 必须非阻塞**（`O_NONBLOCK`）：pump 的 `while(poll_event)` 会 drain 到空，阻塞 read 会卡死整个 GUI。
- **EVIOCGRAB**：抢占设备避免 console TTY 也收事件（否则鼠标同时移动光标 + 切 console）。
- **fbdev stride ≠ width×4**：按 `finfo.line_length` 定位行（VESA fb 可能行尾有 padding）。
- **三方案全废，改自构建**（最大教训，见下）：Alpine ISO + 9p / + HTTP / + VNC 手动驱都不通（9p tag 死活不匹配、SLIRP wget 卡死、VNC 手动驱不符合 CI 化目标）。**自构建 x86_64 kernel + busybox initramfs**（PenguinLab 源码复用）一气呵成——initramfs 自带一切（busybox + 静态 fbdev-host + `/init`），零 guest→host transport。

## 验证

- **4 ctest 全绿**：cinux-gui-smoke + offscreen-dump + replay-dump + **evdev-test**（accumulator 7 断言）。
- **ASAN 干净**：evdev-test 直接 run exit 0。
- **fbdev-host 静态编译**（936K，`glibc-static`，`-static -fno-exceptions -fno-rtti`）。
- **真 QEMU 冒烟 PASS**（2026-07-01，见下）。

## QEMU 冒烟（scripts/，自构建 kernel + initramfs）

**方案：自构建 kernel + busybox initramfs。** 三大 transport（9p / HTTP / VNC 手动驱）全废后，改用 PenguinLab 源码（`~/PenguinLab/third_party/{linux,busybox}`，Linux 6.19.9 + busybox 1.37）自构建一个最小 x86_64 环境：initramfs 自带一切，kernel `rdinit=/init` 直接跑 `/init`，`/init` 自己发现设备 + 跑 fbdev-host。**零 guest→host transport = 零 9p/HTTP/网络依赖 = 不可能卡。**

### 脚本链（一次构建，缓存）

| 脚本 | 产物 | 备注 |
|---|---|---|
| `build_kernel_x86_64.sh` | `build/smoke/linux-build/.../bzImage`（15M） | `x86_64_defconfig` + 强开 FB/FB_VESA/FRAMEBUFFER_CONSOLE/VT/USB/USB_XHCI(_PCI)/HID/USB_HID/INPUT_EVDEV/**DRM/DRM_KMS_HELPER/DRM_FBDEV_EMULATION/DRM_BOCHS** + olddefconfig。**关键**：`-vga std` 在此内核走 bochs-drm 而非 vesafb，必须开 DRM_BOCHS + DRM_FBDEV_EMULATION 才有 `/dev/fb0`（`fb0: bochs-drmdrmfb`）。 |
| `build_busybox_x86_64.sh` | `build/smoke/busybox-root/`（2.5M 静态） | defconfig + `CONFIG_STATIC=y` + 去 `CONFIG_TC`（1.37 tc.c 用了 kernel 6.19 已删的 CBQ uapi，编不过）+ `EXTRA_CFLAGS=-Wno-error`。 |
| `build_fbdev_host_static.sh` | `build/smoke/fbdev-host-static`（936K） | `cmake -DCMAKE_EXE_LINKER_FLAGS=-static`，glibc-static 路径。 |
| `build_initramfs.sh` | `build/smoke/rootfs.cpio.gz`（1.7M） | busybox-root + fbdev-host-static + `/init` + 静态设备节点（console/fb0/null/ttyS0）→ cpio.gz newc。 |
| `smoke_p1.sh` | `build/smoke/run/serial.log` + `shot.ppm` | QEMU `-kernel bzImage -initrd rootfs.cpio.gz -append "console=ttyS0 rdinit=/init" -vga std -device qemu-xhci -device usb-tablet -display vnc=127.0.0.1:99 -serial file -monitor unix`，poll serial 上的 `GUEST_*` marker，自动 gates。 |

### `/init`（kernel 跑，输出→ttyS0→serial.log→host gates）

```sh
set -x; echo GUEST_INIT_OK
mount -t proc none /proc; mount -t sysfs none /sys
mount -t devtmpfs none /dev; mount -t tmpfs none /tmp
[ -c /dev/fb0 ] && echo "GUEST_FB0_OK name=$(cat /sys/class/graphics/fb0/name)"
# tablet 发现：/proc/bus/input/devices grep（/sys/class/input glob 在此 guest 是空的）
EVT=""; i=0
while [ "$i" -lt 10 ] && [ -z "$EVT" ]; do
  i=$((i+1))
  EVT=$(grep -A6 "USB Tablet" /proc/bus/input/devices | grep -o "event[0-9]*" | head -1)
  [ -z "$EVT" ] && sleep 1   # tablet ~3.5s 才注册，retry
done
echo "GUEST_TABLET_EVENT=${EVT:-none}"
[ -n "$EVT" ] && [ -c "/dev/input/$EVT" ] && echo GUEST_TABLET_OK
echo GUEST_RUN_START
timeout 40 /usr/bin/fbdev-host /dev/fb0 "/dev/input/$EVT"
echo "GUEST_RUN_RC=$?"; echo GUEST_RUN_DONE
```

### 真跑结果（2026-07-01，PASS）

```
GUEST_INIT_OK
GUEST_FB0_OK name=bochs-drmdrmfb          # gate1 ✓ — DRM bochs + fbdev emulation → /dev/fb0
GUEST_TABLET_EVENT=event3                  # tablet 发现 ✓（retry 第 1 次命中，/proc grep）
GUEST_TABLET_OK                            # gate2 ✓
fbdev-host: fb 1280x800 stride 5120        # gate3 ✓ — mmap 成功，stride=1280×4（无 padding）
fbdev-host: running -- move/drag the mouse # 主循环起来
GUEST_RUN_RC=143                           # 外部 timeout SIGTERM（预期；40s 跑满）
[smoke] PASS -- boot + devices + fbdev-host loop, no crash.
```

**像素证据**（`shot.ppm` 1280×800 Python 采样，证 fbdev-host 真画了场景）：
- `(0,0)` 背景 = `(24,24,42)` = `kBg 0x0018182A` ✓
- `(640,328)` 标题栏 = `(48,96,160)` = `kTitleBar 0x003060A0` ✓
- sweep y=400：`x=560/720` 窗口面 `(200,200,200)` = `kWinFace 0x00C8C8C8` ✓，`x=640` 白 `(255,255,255)` = 光标落在窗口中心 ✓

### 踩过的坑（记录价值）

- **`-vga std` ≠ vesafb**：此 defconfig 内核不开 `CONFIG_FB_VESA` 的 VBE 切换，`video=vesafb:` 不会出 `/dev/fb0`。开 **DRM_BOCHS + DRM_FBDEV_EMULATION** → bochs-drm 探测 → `/dev/fb0 (bochs-drmdrmfb)`。
- **tablet 发现用 `/proc/bus/input/devices`，别用 `/sys/class/input` glob**：此 guest 下 `/sys/class/input/event*/name` glob 全程不展开（sysfs input class 空），10 次 retry 全字面。改 `/proc/bus/input/devices | grep -A6 "USB Tablet" | grep -o event[0-9]*` 一次命中 `event3`。
- **busybox 1.37 vs kernel 6.19**：`CONFIG_TC` 默认开，tc.c 用了 6.19 删掉的 CBQ uapi，编不过 → sed 关掉。
- **三方案全废（教训）**：9p（driver 绑了但 tag 死活不匹配）、HTTP（SLIRP guest→host wget 卡死）、VNC 手动驱（不符合 CI 化）——都堵在 guest→host transport。自构建 initramfs 把 transport 整个消掉，最干净。

## 结论

**P1 Probe-1 成立**：同一个 core（pump/staging/swraster/region/Host ABI 表），从 offscreen/replay host 换到真 Linux fbdev + evdev host，**只换 host 表填充，core 一行没动**。真 fbdev mmap + 真 evdev usb-tablet，fbdev-host 在 QEMU 上跑满 40s 不崩、画对场景。Host ABI 解耦缝在第二个真 host 上干净——这是 P1 的全部主张。

## 后续

- **P2 渲染收敛**：swraster 正式接管，compositor dirty-region 双缓冲差分（b3/P1 的「整屏重画 staging」优化成真差分）。
- **真 `poll(fd)` 阻塞等输入**：替掉 P1 的 `usleep(16000)` 忙等节流（省 CPU）。
- **CinuxOS 零改动**（P1 standalone；CinuxOS 自己的 host_cinux 是另一个真 host，本仓不动它）。
