---
title: 03 · 调试现场与收尾
---

# 调试现场与收尾

## 调试现场

这一章没有现成的调试笔记,我们从源码层面提炼两个真实陷阱——一个是 composite 顺序的几何后果,一个是必须诚实写明的"功能存在但没接线"的中间态。

### 图标画错图层:位置差一行,效果差一个世界

`draw_desktop_icons` 那一行调用的位置,是这一章最容易调错的地方。前面讲过它必须夹在 `clear` 和 blit 窗口之间,这里展开讲讲"画错位置会看到什么",方便你将来自己搭类似分层时对照排查。

**画在 `clear` 之前。** 你会看到桌面完全是空的,和 030 一模一样——因为 clear 用 `DESKTOP_COLOR` 把整块 back buffer 涂了一遍,你刚才画的图标像素被原封不动覆盖。这种 bug 最迷惑:代码明明调了 `draw_desktop_icons`,单步进去也确实在写像素,可屏幕上就是没有图标。排查时别盯着 draw 函数本身看,去 composite 里数调用顺序——"先 clear 再画"这个顺序是硬约束,反了就等于白画。

**画在 blit 窗口之后、`draw_cursor` 之前。** 你会看到图标浮在所有窗口上面。拖一个窗口到 Shell 图标上方,图标不会被动窗口盖住,反而粘在窗口前面,像一张贴纸。视觉上极其违和,而且更糟的是命中逻辑会跟着错乱:`handle_mouse` 里窗口是优先命中的,可视觉上图标在窗口前面,用户点"看到的图标",实际点中的却是底下的窗口——视觉和命中对不上,体验崩坏。所以 composite 的分层顺序和 handle_mouse 的命中优先级必须一致:**视觉上谁在前,命中时就先查谁**。这两个方向凑在一起,才是"所见即所点"。

`test_desktop_composite_icons_only`(只有图标、没窗口时合成不崩)、`test_desktop_composite_icons_and_windows`(图标和窗口同时存在)、`test_desktop_composite_icons_behind_windows`(窗口盖住图标)这三个测试,就是从不同角度固化这套分层顺序的。改 composite 顺序时,这三个测试会第一时间红给你看。

### 诚实点:点 Shell 会开终端,点 Calculator 什么都不会发生

这一章最需要诚实的地方,是"能点击"和"点击有结果"未必是一回事——Shell 有结果,Calculator 没有。

点 Shell 图标会发生什么?`handle_mouse` 把 `OpenShell` 存进槽,`gui_tick_callback` 在下一个滴答 `consume_pending_icon_action()` 把它取出来,看到是 `OpenShell` 就调 `create_shell_terminal()`——一个新的 `Terminal` 被 new 出来、绑上管道、`add_window` 进 WM,屏幕上真的弹出一个跑 shell 的终端窗口,串口也打印 `[GUI] Shell terminal created and connected.`。所以 Shell 是"能点击、点击有结果"的那一个。

Calculator 则是另一回事。`OpenCalculator` 这个动作在 033 里没有任何消费者:点 Calculator 图标,`handle_mouse` 把 `OpenCalculator` 存进槽,tick 回调取出来一看"这不是 OpenShell",直接忽略——动作被**静默吞掉**。所以 Calculator 图标是"能看见、能点中、但点了毫无反应"的摆设,而且这个摆设状态会持续相当长一段时间。

为什么要在这一章就注册一个"暂时没用"的 Calculator 图标?因为它让桌面看起来像个真桌面(不止一个图标),也因为它把"函数存在不等于功能接线"这件事摆到了台面上。点击链路、`consume_pending_icon_action`、`create_shell_terminal` 这些零件对 Shell 已经接通了,唯独 `OpenCalculator` 这一支缺一个消费者。这种"占位状态"在系统开发里太常见了——接口先定好、数据先流起来,真正的消费者后面再接。诚实地把它写出来,比假装"两个图标都能开窗"有价值得多。

所以这一章的验证标准是:我们验证"图标被画出来了""点击被识别并存进了槽""点 Shell 真的弹出终端""合成分层正确",Calculator 那一支则明确是"点了无反应"的已知占位。

## 验证

033 的验证分三层:纯逻辑用 host 单测、机内集成用 QEMU kernel 测试、视觉效果用 `run` 肉眼看。

**第一层:host 单元测试。** 图标注册、命中检测、`consume` 语义、点击设 action、空白点击不设 action、窗口压住图标时点不到图标——这些纯逻辑在 host 上 `-O2` 编、`CINUX_HOST_TEST` 门控跑。和 030 一样是"镜像"测法,ctest 名叫 `desktop`:

```bash
ctest --test-dir build -R "desktop" --output-on-failure
```

它覆盖了 `add_desktop_icon` 的计数与上限(`"desktop: add_desktop_icon increments icon count"`、`"... returns false at MAX_ICONS"`)、`hit_test_icon` 的命中/未命中/边界/重叠优先(`"desktop: hit_test_icon returns icon on hit"`、`"... nullptr on miss"`、`"... boundary edge is miss"`、`"... later icon takes priority on overlap"`)、`consume_pending_icon_action` 的取出-清零(`"desktop: consume_pending_icon_action returns None when empty"`、`"... resets to None"`)、点击设 action 与空白点击不设(`"desktop: icon click sets pending_icon_action"`、`"desktop: desktop blank click does not set action"`)、以及窗口压住图标时点不到图标(`"desktop: window on top of icon prevents icon click"`)。跑全 host 套件也行:

```bash
cmake --build build --target test_host
```

**第二层:QEMU kernel 测试。** 真正跑内核代码、走真 `Canvas` 的机内测,入口是 `run_desktop_tests()`,`TEST_SECTION("Desktop Tests (033_gui_desktop)")`:

```bash
cmake --build build --target run-big-kernel-test
```

它跑的是真 WindowManager + 真 off-screen Canvas + 真 framebuffer,逐个 `RUN_TEST`:`test_desktop_init_and_add_icon`(init 后能加图标)、`test_desktop_icon_capacity_limit`(加到 16 个再加返回 false)、`test_desktop_hit_test_boundaries`(左上角在内、`x+w-1, y+h-1` 在内、各方向外 1 像素在外)、`test_desktop_hit_test_z_priority`(后注册的图标重叠优先)、`test_desktop_click_sets_and_consumes_action`(点一次得 `OpenShell`、再点得 `None`)、`test_desktop_composite_icons_behind_windows`(窗口盖住图标,`(5,25)` 是窗口内容色)、`test_desktop_full_scenario`(端到端)、`test_desktop_init_resets_icons`(init 重置图标状态)等。这是把 host 镜像验证过的逻辑,放到真内核环境里再过一遍。

**第三层:视觉效果。** 想亲眼看到两个图标:

```bash
cmake --build build --target run
```

预期串口(此处只摘与桌面图标相关的前后文,管道接线相关的 `[GUI] Shell pipes stored...` 与 `[INIT] Terminal-shell pipes connected...` 两行实际打在 milestone 之前,留到 033b 讲):

```text
[GUI] ===== Milestone 033: GUI Desktop =====
[MOUSE] Mouse enabled (ACK received).
[MOUSE] PS/2 mouse driver initialised.
[GUI] Desktop icons registered: Shell, Calculator.
[GUI] GUI tick callback registered on PIT.
```

看到 `Desktop icons registered: Shell, Calculator.` 就说明两个图标都注册成功了。开机进 GUI 后,暗青桌面上 `x=40` 这一列,`y=40` 处是 Shell 图标、`y=120` 处是 Calculator 图标,各有(大致居中的)白色标签。把鼠标移到 Shell 图标上点一下——屏幕上会**弹出一个 Cinux Terminal 窗口**,串口也多一行 `[GUI] Shell terminal created and connected.`:点击被识别、意图被存进槽、tick 回调消费它并 `create_shell_terminal`,终端就这么开出来了。但 Calculator 图标点下去依然什么都不会发生——它的动作还没有消费者。

## 下一站

到 033,桌面终于不再是光秃秃的背景色了:它有了图标,鼠标点上去能被识别,点 Shell 图标真的会通过 `consume_pending_icon_action` → `create_shell_terminal` 弹出一个跑 shell 的终端窗口,Calculator 图标则还没有消费者。

`create_shell_terminal`、`set_shell_pipes`、`is_terminal` 这套 gui_init 侧重构是怎么搭起来的——终端对象怎么 new、管道怎么绑、`is_terminal` 的虚函数怎么让 tick 回调认出窗口是终端——是 [下一章 007](../007/) 的主题。再往后,我们要离开 GUI 桌面、回到进程:给内核加上 `fork` / `execve`,让一个用户进程能生出另一个(见 [001](../10-multitasking/001/))。

## 参考

- C++ `enum class`(支撑 `IconAction` 的类型安全枚举与 `pending_icon_action_` 初值):https://en.cppreference.com/w/cpp/language/enum
- C++ 指定初始化(designated initializers,支撑 `gui_start()` 里 `DesktopIcon{.x=40, .y=40, ...}` 这种按成员名构造的写法):https://en.cppreference.com/w/cpp/language/aggregate_initialization
- 030 章确立的合成分层顺序(`clear` → blit 窗口 → `draw_cursor` → `flip`),033 在 `clear` 与 blit 之间插入 `draw_desktop_icons`:见 [002 · 窗口管理器](../002/)
- 032 章的位图原语(`Canvas::draw_bitmap` 透明像素跳过)与 `DesktopIcon::contains` 左闭右开命中框:见 [005 · 位图图标](../005/)
