---
title: Lab 033 · 把图标摆上桌面、接上点击
---

# Lab 033 · 把图标摆上桌面、接上点击

> 这个实验对应主书 [033 · 桌面图标层](../../book/09-gui/033-gui-desktop.md)。我们不贴完整答案代码——你要自己在 Window Manager 里长出"桌面图标"这一层,并把"点图标"这件事接上。这里只给目标、约束、验证手段和排错方向。点图标真正弹出终端窗口是 [Lab 033b](lab-033b-gui-lazy-terminal.md) 的事;这个 lab 做到"图标画出来、点击能命中、命中后把意图记进一个槽"就收工。

## 实验目标

到 032 为止,桌面还只是暗青背景上漂着几个窗口,光秃秃的。这个 lab 要让 Window Manager 长出**桌面图标层**:开机在桌面上画出 Shell、Calculator 两个图标(带居中白字标签),鼠标点中图标时,把"这个图标被点了"这件事**记进一个 pending 槽**,等待下一帧的 tick 回调去消费。

要点亮五件事:WM 私有里多出一组图标状态、注册/命中/消费图标的三件套函数、composite 合成顺序里插一层图标绘制、handle_mouse 在"没点中窗口"时改成去点图标、以及 gui_start 在桌面上摆两个图标。

一个必须先说清的诚实边界:**这个 lab 收尾时,点 Shell 图标也开不出终端窗口**。我们只把"被点了"存进槽,真正读槽去开窗的逻辑在 033b。点 Calculator 图标则更彻底——整个 033 时间线里都没有人消费它的动作,点了**什么都不发生**,这是预期的占位状态,别假装它弹出过计算器。

## 前置条件

- 跑通 030:`WindowManager` 的 `composite()`(clear→blit 窗口→draw_cursor→flip)、`handle_mouse()`(命中窗口走 raise/拖拽、点空白只清焦点)、`Canvas` 合成。
- 跑通 032:`icons::data::k_shell_icon` / `k_calc_icon` 的位图数据、`icons::ICON_SIZE`、`Canvas::draw_bitmap` 的透明像素跳过、以及 `DesktopIcon` 结构体和它的 `contains(mx, my)` 左闭右开命中。
- 030/031 已有的 `Window::contains` / `WindowManager::hit_test` 命中检测、`focused_` 焦点管理、PIT 滴答里的 `gui_tick_callback` 骨架。

## 任务分解

按依赖顺序,分五块做。都在 Window Manager 和 gui_init 这两个模块里。

### 任务 1:WM 私有里多出图标状态

改 `kernel/gui/window_manager.hpp` 的 `WindowManager`。

- 加一个容量常量,叫 `MAX_ICONS`,值给 16(和已有的 `MAX_WINDOWS=64` 同款风格,独立计数)。再定两个颜色常量:图标标签色 `ICON_LABEL_COLOR`(白)、桌面底色 `DESKTOP_COLOR`(沿用 030 的暗青)。
- 私有成员加三样:一个 `DesktopIcon` 数组(长度 `MAX_ICONS`,元素默认初始化)、一个 `icon_count_` 计数(初值 0)、一个 `pending_icon_action_`(初值 `IconAction::None`)。
- 想清楚为什么 pending 要放在 WM 里、而不是放在图标结构体自己身上:动作是"一次点击的瞬时结果",不是图标的固有属性。图标只贡献自己的 `action` 值,WM 统一收口"最近一次点击想干什么",tick 回调再来取。这样图标层和"消费动作"那一层就解耦了——也正是 033 能停在这里、把消费留给 033b 的原因。
- 别忘了让 `init()` 把 `icon_count_` 和 `pending_icon_action_` 重置,这是开机和测试复用 WM 实例时的命门。

### 任务 2:注册 / 命中 / 消费三件套

在 `window_manager.hpp` 声明、`window_manager.cpp` 实现。这三个函数是图标层的全部对外接口。

- **`add_desktop_icon(const DesktopIcon&) -> bool`**:数组满了(`icon_count_ >= MAX_ICONS`)就返回 false,否则拷进 `icons_[icon_count_]`、计数自增、返回 true。满返回 false 不是"出错",而是给调用方一个"注册席满了别再塞"的信号。
- **`hit_test_icon(int32_t mx, int32_t my) const -> const DesktopIcon*`**:**逆序遍历**(从 `icon_count_` 往 0 扫)。命中返回那个图标的指针,全 miss 返回 nullptr。为什么逆序?当两个图标重叠时,后注册的画在上面、视觉上盖住先注册的,点击命中也该归上层——遍历顺序要和"谁盖谁"对齐。每个图标的命中判定用现成的 `DesktopIcon::contains(mx, my)`,别自己重写区间判断(那个 `contains` 是左闭右开:`mx>=x && mx<x+width`)。
- **`consume_pending_icon_action() -> IconAction`**:取出 `pending_icon_action_` 的当前值,**然后立刻把它清回 `None`**,再把取到的值返回。注意是"取出即清零"——一次点击产生的动作只能被消费一次,消费完槽就空了。连续两次 consume,第二次必须拿到 `None`。

### 任务 3:composite 里插一层 draw_desktop_icons

改 `window_manager.cpp` 的 `composite()`。这是最容易画错顺序的一步。

- **严格顺序**:先 `screen->clear(DESKTOP_COLOR)`,然后**立刻** `draw_desktop_icons(*screen_)`,再从底到顶 blit 各可见窗口,最后 `draw_cursor`、`flip`。图标层夹在 clear 和 blit 之间。
- 想清楚这层为什么必须在窗口之下:窗口会移动、会盖住桌面区域。如果图标画在窗口之上,桌面图标就会从窗口背后"穿"出来盖住窗口内容,违背"窗口是前景"的直觉。先 clear 铺底色、画图标、再用窗口 blit 覆盖,图标就老老实实待在最底层。
- **`draw_desktop_icons(Canvas&)`**:逐个图标 `draw_bitmap(x, y, w, h, bitmap)`(透明像素由 draw_bitmap 自己跳过,不用你管);算 label 文字宽度、用 `font_->width()` 居中,label 的横坐标 = `icon.x + (icon.width - label_len * glyph_w) / 2`,纵坐标 = `icon.y + icon.height + 2`(图标下方留两像素);用 `ICON_LABEL_COLOR` 白字、`*font_` 画。开头加 `if (font_ == nullptr) return;` 守卫——和 031 的 render_to_canvas 同款命门,字体没注入就别画。

### 任务 4:handle_mouse 接上图标命中

改 `window_manager.cpp` 的 `handle_mouse` 的 MouseDown 分支。这一步把"点击"接进 pending 槽。

- MouseDown 时,**先**用现有的 `hit_test()` 找窗口。这是命中优先级的第一档:窗口永远盖在图标之上。
- **若 `hit_test()` 返回 nullptr(没点中任何窗口)**,才轮到 `hit_test_icon(mx, my)`:
  - 命中图标 → 把那个图标的 `action` 存进 `pending_icon_action_`,**并**清焦点(把当前 `focused_` 的 `set_focused(false)`、`focused_` 置空)。点桌面对应"离开所有窗口",焦点本就该收走。
  - 没命中(纯桌面空白)→ 只清焦点,不设 action。这就是 030 原本的行为,保持不变。
- 若 `hit_test()` 命中了窗口,走原来的 raise/拖拽/关闭逻辑,**图标完全不参与**。别在窗口命中的分支里碰图标状态。
- 一个实现细节提醒:单击(MouseDown)就触发,不是双击。如果你照着某处注释以为要双击,以代码为准——MouseDown 一次就够。

### 任务 5:gui_start 在桌面上摆两个图标

改 `kernel/gui/gui_init.cpp` 的 `gui_start()`。

- 返回类型从 032 的 `Terminal*` 改成 **`void`**。开机不再创建终端窗口——那是 033b 点 Shell 时才做的事。这个 lab 里 gui_start 只负责把桌面摆出来。
- 打里程碑串口:`[GUI] ===== Milestone 033: GUI Desktop =====`。
- 构造两个 `DesktopIcon` 并 `add_desktop_icon` 进 WM:
  - Shell 图标:坐标 `(x=40, y=40)`,位图用 `icons::data::k_shell_icon.data()`,label `"Shell"`,宽高都是 `icons::ICON_SIZE`,`action = IconAction::OpenShell`。
  - Calculator 图标:坐标 `(x=40, y=120)`,位图用 `icons::data::k_calc_icon.data()`,label `"Calculator"`,同样尺寸,`action = IconAction::OpenCalculator`。
- 注册完打印 `[GUI] Desktop icons registered: Shell, Calculator.`,然后照常挂上 PIT 的 `gui_tick_callback`,打印 `[GUI] GUI tick callback registered on PIT.`。
- 注意 gui_tick_callback 这个版本里**还没有**消费 pending action 开窗的逻辑——那是 033b。这个 lab 的 tick 保持 032 的样子就行,别提前写 `if (action == OpenShell) create_shell_terminal()`。

## 接口约束

- `MAX_ICONS = 16`;`ICON_LABEL_COLOR` 白色,`DESKTOP_COLOR` 沿用 030 暗青。
- `add_desktop_icon` 满返 false、否则计数自增返 true;`hit_test_icon` 逆序遍历、命中返 `const DesktopIcon*`、miss 返 nullptr;`consume_pending_icon_action` 取出即清零、返回 `IconAction`。
- `composite()` 顺序**不可变**:`clear` → `draw_desktop_icons` → blit 窗口 → `draw_cursor` → `flip`。
- `draw_desktop_icons`:label 横向居中于图标、纵向在图标下方 2px;`font_==nullptr` 直接 return。
- `handle_mouse` MouseDown 命中优先级:窗口 `hit_test` 在先,空时才 `hit_test_icon`;命中图标设 pending 并清焦点,命中窗口不碰图标状态。
- 图标坐标固定:Shell `(40,40)`、Calculator `(40,120)`;位图取 `k_shell_icon.data()` / `k_calc_icon.data()`,`action` 分别是 `OpenShell` / `OpenCalculator`。
- `gui_start()` 返回 `void`,本 lab 的 tick 回调**不消费** pending action。

## 验证步骤

**第一步:host 单元测试**(`test/unit/test_desktop.cpp` 用 mock 重画了一版 WM 镜像,不碰硬件,ctest 名 `desktop`):

```bash
ctest --test-dir build -R "desktop" --output-on-failure
```

预期:这一组覆盖 `add_desktop_icon` 计数递增、`add multiple desktop icons`、`add_desktop_icon returns false at MAX_ICONS`(满返 false)、`init resets icon state`、`hit_test_icon returns icon on hit` / `nullptr on miss` / `with no icons`、`hit_test_icon later icon takes priority on overlap`(逆序优先)、`hit_test_icon boundary edge is miss`(82 越界 / 81 内,左闭右开边界)、`hit_test_icon works at screen origin`、`consume_pending_icon_action returns None when empty` / `resets to None` / `multiple consumes without click all return None`、`icon click sets pending_icon_action`、`different icons set different actions`、`icon click with None action sets None`、`blank click does not set action`、`clicking empty area near icons does not set action`、`window on top of icon prevents icon click`、`icon click preserves window focus`(host 镜像命中图标不清焦点,这是 mock 实现的细节差异,见故障一节)、`composite renders icon bitmap`(像素落在图标色 / 桌面色)。全过即可。

**第二步:QEMU kernel 测试**(真 WM、真 Canvas,入口 `run_desktop_tests()`):

```bash
cmake --build build --target run-big-kernel-test
```

预期:`TEST_SECTION("Desktop Tests (033_gui_desktop)")` 下这一批通过:`test_desktop_init_and_add_icon`、`test_desktop_icon_capacity_limit`、`test_desktop_add_multiple_icons`、`test_desktop_hit_test_empty`、`test_desktop_hit_test_boundaries`、`test_desktop_hit_test_z_priority`、`test_desktop_consume_no_pending`、`test_desktop_click_sets_and_consumes_action`(MouseDown 命中→consume 得 OpenShell→再 consume 得 None)、`test_desktop_click_no_icon_no_action`、`test_desktop_click_window_no_icon_action`、`test_desktop_composite_icons_only`、`test_desktop_composite_icons_and_windows`、`test_desktop_composite_empty`、`test_desktop_composite_icons_behind_windows`、`test_desktop_full_scenario`、`test_desktop_init_resets_icons`、`test_desktop_hit_test_zero_size_icon`。退出码约定:全过写 `exit_code=0`,QEMU 退 1,脚本判 `[ $QEMU_EXIT -eq 1 ]`。

**第三步:视觉效果**:

```bash
cmake --build build --target run
```

预期串口依次出现:

```text
[GUI] ===== Milestone 033: GUI Desktop =====
[GUI] Desktop icons registered: Shell, Calculator.
[GUI] GUI tick callback registered on PIT.
```

开机进 GUI:暗青桌面左侧 `x=40` 处有 Shell 图标(`y=40`)和 Calculator 图标(`y=120`),各带居中白字标签。鼠标移过图标位置、点中 Shell,**桌面上的现象就是"焦点从窗口收走"**——但**没有窗口弹出**。点 Calculator 同样只是收焦点、什么都不会发生。这是这个 lab 诚实的收尾状态:图标到位、点击被记进槽、槽还无人消费。

## 常见故障

- **图标画不出来 / 一闪就被擦掉**:`composite()` 顺序错了。如果你把 `draw_desktop_icons` 放在了 blit 窗口**之后**,窗口内容会把图标盖住(图标只在窗口没覆盖的桌面区域可见,还说得过去);更糟的是放到了 `draw_cursor` 之后或 `flip` 之后,图标要么被光标压、要么根本没上屏。回到 clear → draw_desktop_icons → blit 窗口 → draw_cursor → flip 这条线核对。
- **label 偏到一边**:横向居中算错了。正确做法是 `(icon.width - label_len * glyph_w) / 2` 再加到 `icon.x` 上。如果你忘了乘 `glyph_w`、或者拿 `icon.x` 当成屏幕中心去减,文字就会贴左或贴右。纵向也别错:label 在图标**下方**(`icon.y + icon.height + 2`),不是叠在图标上。
- **点击图标没反应 / 边界点不中**:`hit_test_icon` 的区间和 `DesktopIcon::contains` 对不齐。`contains` 是左闭右开(`mx >= x && mx < x+width`),所以 `x+width-1` 那一列在内、`x+width` 那一列在外。如果你自己手写了 `mx <= x+width`(闭区间),边界上会比设计多收一列、和测试 `test_desktop_hit_test_boundaries` / host 的 `boundary edge is miss` 对不上。统一用现成的 `contains`,别另造。
- **重叠区域点错了图标**:`hit_test_icon` 没逆序。正序遍历会让先注册的(被盖住的)图标抢到点击,和"画在上面、点也归上面"的视觉直觉相反。从 `icon_count_` 往 0 扫。
- **consume 一次点击被消费两次**:`consume_pending_icon_action` 漏了"取出后清零"。记住口诀:取出即清零。`test_desktop_click_sets_and_consumes_action` 和 host 的 `multiple consumes without click all return None` 就是查这个。
- **点窗口区域反而触发了图标动作**:你在 `handle_mouse` 里把图标命中检测写在了窗口 `hit_test` **之前**,或者命中窗口的分支里没 return 就 fall-through 进了图标逻辑。窗口必须先 `hit_test`、命中就直接走窗口路径,只有"没点中任何窗口"时才查图标。
- **host 和 kernel 对"点图标是否清焦点"结果不一样**:这不是你的 bug。kernel 版 WM 命中图标时清焦点(点桌面=离开所有窗口);host 那份 mock 重写的镜像里命中图标**保留**焦点(对应 `"icon click preserves window focus"`)。两份实现是独立维护的,以 kernel 源码为准。看到 host 那条测试名别慌,也别为了"统一"去改 kernel 行为。
- **点 Calculator 真的什么都不发生,是不是我没写对**:这就是预期。整个 033 时间线里 `gui_tick_callback` 只有处理 `OpenShell` 的打算(而那也是 033b 才接上),没有任何分支处理 `OpenCalculator`。点 Calculator 命中图标 → 存进 pending 槽 → tick 取出来 → 没有匹配分支 → 动作被静默吞掉。函数存在(图标注册了、action 存了)不等于被接线消费,这正是"函数存在 ≠ 接线"的活样本,别把它当故障去修。`IconAction` 用 `enum class` 就是想让这种"枚举值定义了但没人消费"的状态在类型层面清楚可见([cppreference: enum class](https://en.cppreference.com/w/cpp/language/enum));`gui_start()` 里 `DesktopIcon{.x=40, .y=40, ...}` 这种按成员名构造,是 C++ 的指定初始化([cppreference: aggregate initialization](https://en.cppreference.com/w/cpp/language/aggregate_initialization))。

## 通过标准

- [ ] host `ctest -R "desktop"` 全绿,`test_host` 整体不回归。
- [ ] `run-big-kernel-test` 里 `run_desktop_tests()` 全部通过,包括容量、命中边界、逆序优先、consume 取出-清零、composite 图标在窗口之下。
- [ ] `WindowManager` 私有里有 `icons_[MAX_ICONS]`、`icon_count_`、`pending_icon_action_`,`init()` 重置后两者;`MAX_ICONS=16`。
- [ ] `add_desktop_icon` / `hit_test_icon`(逆序)/ `consume_pending_icon_action`(取出即清零)三件套签名与语义正确。
- [ ] `composite()` 顺序为 clear → draw_desktop_icons → blit 窗口 → draw_cursor → flip;`draw_desktop_icons` 居中 label、`font_==nullptr` 守卫。
- [ ] `handle_mouse` MouseDown 命中优先级为窗口在先、空时才查图标;命中图标设 pending 并清焦点,命中窗口不碰图标。
- [ ] `gui_start()` 返回 `void`,注册 Shell `(40,40)` + Calculator `(40,120)` 两图标,串口打出里程碑与注册行。
- [ ] 在代码或报告里**诚实标注**两条边界:① 点 Shell 此刻**开不出窗**(pending 槽只记意图,消费它属 033b);② 点 Calculator **无任何反应**(整个 033 没有消费 `OpenCalculator` 的分支,函数存在 ≠ 接线)。不把未接线的东西写成已工作。
