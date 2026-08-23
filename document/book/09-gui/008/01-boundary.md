---
title: 01 · 问题与边界:被焊死的桌面 + host-neutral core
---

# 问题与边界:被焊死的桌面 + host-neutral core

> 029 到 033 在内核里搭起了一整套桌面:双缓冲画布、窗口管理器、位图图标、终端。但那套 GUI 当时是**焊死在内核里**的——刷新挂在时钟中断回调里,合成完直接写帧缓冲,任何一笔改动都得把整个内核跑起来才能看见。这一章把它整个搬出来:GUI 的核心几何/事件/光栅化逻辑抽成一个**不 include 任何内核头**的独立库,内核这边连一个 host 适配单元都不剩;真正驱动桌面的是一个**普通的 ring3 进程** `/cinux_gui_host`,它打开 `/dev/fb0` 画像素、读 `/dev/event0` 拿输入,合成和事件泵全在用户态跑。内核只剩两个文件:一个在 ISR 里把鼠标键盘事件推进 `/dev/event0`,一个 fork+execve 把 host 进程拉起来。
>
> 一条诚实的边界先说在前头:这一章讲的是**架构骨架**——host-neutral core 怎么立、Host 表怎么填、内核薄接缝留到多薄。host 进程的渲染路径里目前有一条**遗留的防御性 workaround**:`host_render_frame` 每帧都报一个全屏脏矩形,把脏矩形优化实际短路掉。这条 workaround 当年是用来补一个 core bug 的,但**那个 core bug 早已修掉**(下面会讲到为什么 host 这边的全屏 flush 留着没拆),拆它是个小活但本章没做。

## 这章咱们要点亮什么

1. **被焊死的桌面长什么样**:刷新在时钟中断里、合成直写帧缓冲、GUI 代码和内核搅在一起——以及为什么这样既不可靠也不能动。
2. **Host ABI 表**:host-neutral core 和宿主之间**唯一**的硬接缝,一张函数指针表。谁填表,core 就跑在谁上面。
3. **host-neutral core**:不 include 任何内核头、不带 vtable/RTTI/异常,只用 `<stdint.h>`/`<stddef.h>`。同一份代码,host 填上表就能在内核测试里跑、在 Linux fbdev 上跑、在 SDL 窗口里跑、在 Cinux 用户态进程里跑。
4. **userspace host 进程**:`/cinux_gui_host` 是一个普通的 ring3 ELF,`open`/`mmap`/`read`/`poll`/`fork`/`execve` 全是 syscall,跟 `/bin/sh` 没本质区别。它填 Host 表、循环调 `core->pump()`。
5. **内核薄接缝**:整个 `kernel/gui/` 只剩 `gui_init.cpp`(ISR 里 dual-write 事件到 `/dev/event0`)和 `desktop_launch.cpp`(fork 出 host 进程),旧的内核 host 适配器已删。
6. **Region 永不欠覆盖**:脏矩形集合容量溢出时坍缩成包围盒——多推几个没变的像素是性能损失,漏推一个变了的像素是视觉 bug。

## 被焊死的桌面:刷新挂在时钟中断里

先看要修的「病」。在 029–033 那套 GUI 里,屏幕刷新是这么驱动的:向 PIT 注册一个回调,每次 IRQ0 时钟中断就在 **ISR 上下文里**排空鼠标键盘事件、合成一帧、把整帧 `flip()` 到帧缓冲。这是「内核/中断把刷新推给 GUI」的模型。

这个模型有两个病根,而且一个比一个要命。

第一个,**在中断里跑渲染本身就是雷区**。中断上下文里任何意外的阻塞——合成时撞上一个还没就绪的状态、某条路径多绕了一下——都会把整条 IRQ 路径钉死,表现就是卡屏或黑屏。中断是「要尽快交还」的上下文,在里面干一整帧的活本来就违和。

第二个更隐蔽。把刷新押在 PIT tick 的到达上,等于押在一个**在另一种中断路由配置下可能根本不反复触发**的硬件特性上——当内核切到 APIC 路由后,legacy PIT 那条 IRQ0 的投递行为会和 8259 PIC 下不一样。也就是说,「每次时钟中断刷新一帧」这个假设本身就很脆。

所以要做的事,不是给旧路径打补丁,而是换一个模型。但换模型之前得先解决一个前置问题:GUI 代码现在和内核搅在一起,无论把它放进哪个线程都还是内核态那一坨,放进哪个中断都还是 ISR 上下文。于是第一步是**把 GUI 从内核里整个拔出来**——拔到一个连内核头都不 include 的库里,拔到连「跑在哪条特权级上」都不自知。

## 立一道硬边界:host-neutral core

拔出来的办法,是给 GUI 立一道**硬边界**:边界这边是「host-neutral core」——只懂 GUI 的事(矩形代数、事件泵、光栅化、控件树),不认识 framebuffer、不认识 IRQ、不认识进程结构、不认识 syscall;边界那边是「宿主」——今天是 Cinux 的 userspace host 进程,明天可能是 SDL 模拟器,后天可能是个 offscreen 测试驱动。两边之间**只通过一张表**说话。

这张边界在文件层就看得见。core 全部住在 [`libs/gui/core/`](../../../libs/gui/core/gui_core.hpp),它的核心会话类 `GuiCore` 的源文件 [`gui_core.cpp`](../../../libs/gui/core/gui_core.cpp) 顶部写得很直白:

> Host-neutral: ZERO host includes. Owns the staging buffer; render_frame paints into it; region algebra collects the dirty rects; flush pushes each to the host. See gui_core.hpp for the flush-display-model contract.
>
> ([`gui_core.cpp:5`](../../../libs/gui/core/gui_core.cpp#L5))

注意这句 "ZERO host includes" 不是修辞。看 [`core/`](../../../libs/gui/core/) 下任何 `.cpp` 的 `#include` 段,出现的全是 `<stdint.h>` + 同目录的兄弟头(`host.hpp`/`region.hpp`/`event.hpp`)——**没有一个内核头,也没有一个 Linux 头**。`core/` 因此能用普通的 g++/clang++ 直接编,跑 ctest,完全不需要把内核起来。

怎么证明它真的 host-neutral?这个库里带了好几个**零目标平台**的 host 程序,每个都是一份独立的表填充:[`host/widgets_host_main.cpp`](../../../libs/gui/host/widgets_host_main.cpp) 把控件树渲一帧到 malloc 的缓冲里、dump 成 PPM;[`host/fake_host_main.cpp`](../../../libs/gui/host/fake_host_main.cpp) 手填一张假表(`fake_poll_event` 永远返回 false、`fake_flush` 记调用次数),照样调 `pump()`;[`host/sdl_host_main.cpp`](../../../libs/gui/host/sdl_host_main.cpp) 在一个 SDL 窗口里跑同样的控件树;[`host/linux_fbdev_main.cpp`](../../../libs/gui/host/linux_fbdev_main.cpp) 直接 mmap `/dev/fb0` + 读 `/dev/input/event*`。**同一份 core 驱动 SDL 窗口、Linux framebuffer、offscreen dump、Cinux 用户态进程,只靠换一张表的填充**——这就是 host-neutral 的可证伪证据。

