---
title: 01 · 导引:点亮什么与为什么
---

# 导引:点亮什么与为什么

::: tip tag-bound(本章源码见 tag 033_gui_desktop / e96fff2)
本章描述的是 **整体外置前的内核内 GUI** —— 那时 `desktop_icon.hpp` / `window_manager.hpp` / `window_manager.cpp` / `gui_init.cpp` 都还在 `kernel/gui/` 下,`DesktopIcon` 是带 `IconAction` 枚举的 POD,Window Manager 用"意图槽 `pending_icon_action_` + tick 消费"的两段式点击模型。

主线在后来的 visor 解耦把 GUI 整体外置到用户态 `libs/gui/` 重写为 Widget 版:`DesktopIcon` 变成 `cinux::gui::DesktopIcon` Widget(`set_bitmap` / `set_label` / `set_on_activate`),点击改用 down+up press capture + `on_activate` 回调,不再有"槽 + 消费"语义;`draw_desktop_icons` / `composite` / `flip` 改成 `WindowManager::paint_to_list(PaintList&)` 三层(PaintList)由 Compositor flush。

读这一章请按 **tag `033_gui_desktop`** 的快照读源码,而不是当前主线 `main`。下面所有源码链接(`kernel/gui/...`)指向的是该 tag 的历史路径。
:::

> 到 032 为止,我们已经攒齐了画一个图标的全部零件:`Canvas::draw_bitmap` 会把一块 32×32 的像素数组原样贴到画布上,透明像素自动跳过;`desktop_icon.hpp` 里有了 `DesktopIcon` 这个结构体,把"位置、位图、标签、动作"打成一个包,还自带 `contains(mx, my)` 这个左闭右开的命中框。可问题在于——这些零件全躺在仓库里,桌面压根没用它们。开机进 GUI,你看到的还是 030 那张光秃秃的暗青桌面,一个终端窗口漂在背景上,仅此而已。这一章,我们让 Window Manager 长出一个"桌面层":开机时往桌上摆两个图标,合成时把它们画到屏幕上,鼠标点上去能命中,并把"你点了什么"记下来。读完这一章你会看到:桌面有了图标,点 Shell 图标真的会弹出一个跑 shell 的终端窗口,而点 Calculator 图标则什么都不会发生——它的动作还没有消费者。

## 这一章我们要点亮什么

一句话:让开机后的桌面出现两个图标(Shell、Calculator),鼠标点中图标时,Window Manager 能识别"是哪个图标被点了"并把这个意图存进一个槽里。

完整链路是这样:

```text
gui_start() 开机注册:
    add_desktop_icon(Shell @ 40,40)      → icons_[] 数组
    add_desktop_icon(Calculator @ 40,120) → icons_[] 数组

每个 PIT 滴答 composite():
    clear(桌面色)
      → draw_desktop_icons(把图标位图 + 标签画到屏幕)   ← 新增的一层
      → 从底到顶 blit 各窗口
      → draw_cursor → flip()

鼠标 MouseDown:
    handle_mouse → hit_test() 找窗口
        命中窗口 → 走原有的 raise/拖拽/关闭
        没命中窗口 → hit_test_icon() 找图标
            命中图标 → pending_icon_action_ = 图标自己的 action
            没命中   → 只清焦点(和 030 一样点空白)
```

注意这条链路的终点:`pending_icon_action_ = ...`。这一章里,`gui_tick_callback` 已经在每个滴答里调 `consume_pending_icon_action` 把槽取走:取到 `OpenShell` 就调 `create_shell_terminal` 真的弹出终端,取到别的(比如 `OpenCalculator`)就忽略。所以这一章的可见回报是:你会看到图标、点击被识别,并且点 Shell 真的能弹出终端。唯一开不出东西的是 Calculator——`OpenCalculator` 还没有任何消费者,取出来后直接被忽略。

## 为什么现在需要它

回顾 030 给我们留下的 Window Manager。它的 `composite()` 只有三步:把屏幕 `clear(DESKTOP_COLOR)` 成暗青色、从底到顶把每个可见窗口 `blit_to` 上去、最后画鼠标光标。这套循环已经能让窗口拖得动、关得掉。但桌面这个概念,在 030 里是**缺席**的——`clear` 之后、`blit` 之前那一大片暗青区域,Window Manager 对它一无所知,它只是"没被窗口盖住的背景色"。

体现在输入侧更明显。030 的 `handle_mouse` 处理 `MouseDown` 时,先 `hit_test()` 从顶往下找窗口;如果没命中任何窗口(`hit == nullptr`),它做的事只有一件——清焦点:

```cpp
if (hit == nullptr) {
    // 点到桌面:清焦点
    break;
}
```

也就是说,你在桌面空白处点一下,Window Manager 的反应是"哦,没点到窗口,那我把当前焦点摘了"。桌面在它眼里和"一块什么都没有的地方"完全等价。没有"桌面上摆着东西、点东西能触发动作"这层概念。

032 恰好把原材料备齐了:`DesktopIcon` 结构体有了,`contains` 命中框有了,`icons::data::k_shell_icon` / `k_calc_icon` 这两组 32×32 像素数据也有了。但它们都还是"独立存在的零件",没有任何人去注册它们、绘制它们、点击它们。033 要做的,就是在 Window Manager 里给这些零件安一个家:一个存图标的数组、一套注册/命中/取走意图的接口、合成循环里多画一层、输入路径上多一条"没点中窗口就去找图标"的分支。

所以这一章的位置很清楚:032 给了原语,030 给了 WM 骨架,033 把两者焊起来,让桌面从"一块背景色"变成"一个能摆东西、能被点击的层"。

## 设计图

整个 033 的桌面层长这样,关键是**合成顺序**和**命中优先级**这两件事:

```text
composite() 一帧的分层(从下往上画):

  ┌──────────────────────────────────────────────┐
  │  clear(DESKTOP_COLOR)          暗青底色       │  最底
  ├──────────────────────────────────────────────┤
  │  draw_desktop_icons()          图标层         │  ← 新增
  │     [Shell]                                   │     位图 + 居中白字标签
  │     [Calculator]                              │
  ├──────────────────────────────────────────────┤
  │  blit 各可见窗口(从底到顶)    窗口层         │     窗口会盖住图标
  ├──────────────────────────────────────────────┤
  │  draw_cursor()                 鼠标光标       │  最顶
  └──────────────────────────────────────────────┘
                    flip()
```

图标画在 `clear` 之后、`blit` 窗口之前。这个顺序的含义是:**图标是桌面背景的一部分,窗口浮在它上面**。把一个窗口拖到图标上方,图标会被窗口盖住——这正是真实桌面的行为(你不会指望图标穿透窗口显示)。等到下一章终端窗口弹出来,它会直接盖在 Shell 图标上面,而不是和图标挤在一起。

命中检测的优先级,则和合成顺序相反——从用户视角的"最上面"开始往下找:

```text
handle_mouse(MouseDown):
    1. hit_test() 找窗口          ← 从顶往下,窗口永远优先
       命中窗口 → raise/拖拽/关闭,完全不碰图标
       没命中   ↓
    2. hit_test_icon() 找图标     ← 逆序遍历,后注册的优先
       命中图标 → pending_icon_action_ = icon.action(并清焦点)
       没命中   ↓
    3. 纯桌面空白 → 只清焦点(030 的老行为)
```

窗口永远比图标优先——如果某处同时被窗口和图标覆盖,点下去命中的是窗口。这和合成时"窗口画在图标之上"是一致的:你看到的是窗口,自然点到的也是窗口。只有当鼠标落在一个"没有窗口、但有图标"的位置,图标才有机会被命中。

这套优先级里还藏着一个细节:`hit_test_icon` 是**逆序遍历**图标的,后注册的图标在重叠区优先。这和窗口的 `hit_test` 从顶往下找是同一个道理——重叠时,谁"在视觉上更靠前"谁先被命中。

