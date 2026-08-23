---
title: 04 · 验证 + 没做的 + 小结
---

# 验证 + 没做的 + 小结

## 验证

这一章的验证不靠 QEMU 截图,而是靠两层互锁的证据:

**第一层,core 真的能脱离内核编。** 进 [`libs/gui/`](../../../libs/gui/) 目录(并回的 Cinux-GUI 库),跑它自己的 standalone ctest:`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`。绿就证明 core 在没有任何内核参与的情况下能编能跑,host-neutral 不是嘴上说的。host 单测想更严,push 前自验开 ASAN:`-DCMAKE_CXX_FLAGS="-fsanitize=address" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"`(本地默认 ctest 不开 ASAN 会漏判)。

**第二层,host 进程能起来 + 屏幕真有像素。** 跑构建脚本 [`tools/musl/build-cinux-gui-host.sh`](../../../tools/musl/build-cinux-gui-host.sh) 出静态 musl ELF,塞进 initramfs,内核起来后看串口应该有这两行(都来自源码 kprintf):

```
[INIT] ===== Milestone 035: GUI Desktop (b3b userspace) =====
[INIT] userspace GUI host launched (pid=...)
```

host 进程冒烟自检那个「跑 N 圈读中心像素非零」的逻辑([`main.cpp:578`](../../../user/cinux_gui_host/main.cpp#L578))是非零返回 5,跑通了就是 0。QEMU 下能看见桌面 + Hello 窗口 + Shell/Calculator 图标,点 Shell 弹出终端窗口、能输命令——这一连串就是上面「整条数据路径」的可见版本。具体动手步骤、坑、断点该看哪里,见配套 lab。

## 这章没做的

像 051 那样,把没做完的事说清楚,免得读者把骨架当成成品。

**脏矩形优化实际没生效(但原因不是「core 没修」,是「host 的全屏 flush 没拆」)。** 上面讲 `host_render_frame` 时说过:host 现在每帧都报一个全屏脏矩形,把 `pump()` 的 idle 跳过和 Region 去重实际短路掉了。这件事的来龙去脉再说一遍,因为它容易让读者误会:**当年** core 的 `WindowManager::remove_window` 关窗不标脏,host 只能每帧全屏 flush 兜底;**现在** core 那个 bug 早已修掉(`remove_window` 在 [`window_manager.cpp:58`](../../../libs/gui/core/widget/window_manager.cpp#L58) 已经 `invalidate(stale)`,`add_window`/`add_icon`/光标移动/Z-order 变化也都各自 invalidate),所以 host 的全屏 flush 在原理上已经不必要。**没做的是把 host 改回报真脏区**——把 `host_render_frame` 里那段「强制 `frame->count = 1` 全屏」拆掉、改成把 `desktop.render` 算出的 `dirty` Region 原样喂回 `frame->rects`,然后实跑验证关窗路径真不留残影。这是个范围明确的收尾活,本章没做。「空闲不空转」「只推变化像素」这两个设计目标因此目前没真正落地,但卡在 host 这一侧,不在 core。

**`/dev/event0` 还是 dual-write,老队列没拆。** 鼠标 ISR 和键盘 listener 现在每产生一个事件就同时塞进老 `Mouse::event_queue()` 和 `/dev/event0`。老队列只有 `test_mouse_event` 这类内核测试还在用,生产路径已经走 `/dev/event0` 了。把老队列彻底拆掉、ISR 只 push `/dev/event0`,是后续清理的事——目前 dual-write 是为了迁移期内两条路都能跑、测试不破。

**软件光栅化(`swraster`)是 core 的一等公民,但 host 没全用它。** core 里 `swraster.cpp` 是纯整数的软件光栅化器(Q8.8 定点,不引浮点),控件树的 `render` 路径确实走它。但 host 进程里有些绘制(比如 `host_flush` 那一层的 memcpy)还是手写循环,没全归到 swraster。这不算 bug,只是「光栅化统一收口」没收完。

**host 进程的 stdout 重定向是个 hack。** host main 开头有一段 `open("/dev/console", O_WRONLY); dup2(cfd, 1); dup2(cfd, 2);`（[`main.cpp:477`](../../../user/cinux_gui_host/main.cpp#L477))——因为 fork+execve 出来的 host 继承的 fd 表不一定把 stdout 接到串口控制台,host 想打日志(`host_log` 用 `printf`)就得自己重定向。注释自己也写了「redirect stdout/stderr to /dev/console so host_log reaches the serial log」。这是个实用主义的补丁,不是架构问题,但读者看到别以为这是「正经的 stdio 接线」。

**Calculator 图标是个 stub。** 桌面上的 Shell 图标点击会 spawn `/bin/sh`(走一套 `open("/dev/ptmx")` + `ioctl(TIOCGPTN)` + `open("/dev/pts/N")` + `TIOCSCTTY` + `fork` + `execve` + `TerminalWidget` + PTY drain 的完整路径,在 [`shell_activate`](../../../user/cinux_gui_host/main.cpp#L387) 里——这套 `/dev/ptmx` + `TIOCGPTN` + `/dev/pts/N` + `TIOCSCTTY` 正是 066 立的 PTY ABI,host 这里只是它的一个用户),但 Calculator 图标的 `calc_activate` 是个空函数（[`main.cpp:467`](../../../user/cinux_gui_host/main.cpp#L467))——点它什么都不会发生。注释里就一行 `// stub,calculator 还没接`。这是个留给后续的占位。

**host 没有 CPU 让步,编译时会卡。** `host_poll_event` 那段注释提了:host 用 `poll(ev_fd, 0)` 非阻塞,所以它在 `for(;;) core->pump()` 里**忙等**——除了 `poll` 没拿到事件时那一小段,其余时间 CPU 是满的。更糟的是:当 shell 里跑 `gcc` 这种重活,因为 NVMe 驱动的同步 poll 不能 yield,host 进程连同整个桌面都会卡住。这不是本章的设计缺陷,而是 v1.0.0 一个已知问题,正确修法是异步 IO(v1.1+)。本章把 host 立起来,但没解决这个让步问题——读者点 Shell 跑 gcc 看见桌面冻住别以为是自己配错了。

## 小结

这一章干了一件「搬家」:把 GUI 从内核里整个拔出来,搬成三层——**host-neutral core**(几何 + 事件泵 + 光栅化,不 include 任何内核头)、**Host ABI 表**(core 和宿主之间唯一的硬接缝,一张函数指针表,谁填表 core 就跑在谁上面)、**userspace host 进程**(普通的 ring3 ELF,`open`/`mmap`/`read`/`poll` 全是 syscall)。内核这边只剩 `gui_init.cpp`(ISR 里 dual-write 事件到 `/dev/event0`)和 `desktop_launch.cpp`(fork+execve `/cinux_gui_host`)两个文件,旧的 `host_cinux.cpp` 已删。

三件值得记住的硬规矩:**flush 显示模型**(core 拥 staging 缓冲,host 只被邀请来画 + 报脏帧);**Region 永不欠覆盖**(容量溢出坍缩成包围盒,多推几个像素是性能损失,漏推一个变了的像素是视觉 bug);**file gate 切文件不切源码**(`launch_userspace()` 两份实现,CMake 选编一份,调用处零 `#ifdef`)。还有一条已经认下的债:host 的全屏 flush workaround 是补一个**早已修掉的** core bug 留下的,把它拆回报真脏区是个收尾活——本章搭的是骨架,不是成品。

下一章 055 接 xHCI USB——那一步落地后,鼠标输入才有真 USB 可走(现在只有 PS/2);087 讲 `/dev/fb0` + `/dev/event0` 这两个设备 fd 的设备模型细节,本章把它们当黑盒用;066 讲 PTY,host 那套 `open("/dev/ptmx")` + `TIOCGPTN` + `/dev/pts/N` 的 shell 启动路径就建立在它上面。
