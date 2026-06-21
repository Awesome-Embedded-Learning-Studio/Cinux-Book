---
title: 033 · 桌面图标层:让空白桌面长出可点击的东西
---

# 033 · 桌面图标层:让空白桌面长出可点击的东西

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

## 代码路线

### 桌面数据:三个新成员和两个常量

[window_manager.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/window_manager.hpp) 里,Window Manager 的私有成员多了三行,外加两个新常量:

```cpp
static constexpr uint32_t MAX_ICONS       = 16;
static constexpr uint32_t ICON_LABEL_COLOR = 0x00FFFFFF;  // 白色标签

// ...私有成员:
DesktopIcon icons_[MAX_ICONS] = {};                       // 注册的图标数组
uint32_t    icon_count_       = 0;                        // 已注册数量
IconAction  pending_icon_action_ = IconAction::None;      // 待消费的图标动作
```

`MAX_WINDOWS` 还是 64 不变,桌面图标单独有一个 16 的上限——朴素地用固定数组,和窗口那套是一个风格,够用、没有动态扩容的内存风险。

`icons_[16]` 是值数组,不是指针数组。这和窗口那边存 `Window*` 不一样,原因是 `DesktopIcon` 是个轻量的 POD(几个标量 + 一个 `const uint32_t*` 位图指针 + 一个 `const char*` 标签),拷起来很便宜,也没有 `Canvas` 那种不可拷贝的资源。所以直接按值存,注册时拷一份进数组,简单直接。

`pending_icon_action_` 初值是 `IconAction::None`,这个槽是这一章的核心产物。`IconAction` 是 032 就在 [desktop_icon.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/desktop_icon.hpp) 里定义好的枚举:`None`、`OpenShell`、`OpenCalculator`。一个图标被点中时,它自己的 `action` 字段(注册时填好)会被拷进这个槽,等下一章的 tick 回调来取。

顺带一提,`init()` 里也加了这两行的重置:

```cpp
icon_count_ = 0;
pending_icon_action_ = IconAction::None;
```

`init()` 是 WM 每次重新初始化时调的入口,把所有状态归零。新加的图标状态也必须在这里重置,否则一次测试跑完留下的 `icon_count_` 会污染下一次——这是固定数组状态机的常规卫生要求,`test_desktop_init_resets_icons` 这个测试就是专门盯它的。

### 三个 API:注册、命中、取走意图

围绕这三个成员,Window Manager 对外暴露三个新接口,职责切得很干净。

`add_desktop_icon` 负责注册,满了就拒绝:

```cpp
bool WindowManager::add_desktop_icon(const DesktopIcon& icon) {
    if (icon_count_ >= MAX_ICONS) {
        return false;
    }
    icons_[icon_count_] = icon;
    icon_count_++;
    return true;
}
```

数组没满就拷进去、计数加一;满了返回 false。没有去重、没有排序——后注册的就排在后面,顺序直接决定后面 `hit_test_icon` 的重叠优先级。

`hit_test_icon` 负责命中检测,逆序遍历:

```cpp
const DesktopIcon* WindowManager::hit_test_icon(int32_t mx, int32_t my) const {
    for (uint32_t i = icon_count_; i > 0; i--) {
        uint32_t idx = i - 1;
        if (icons_[idx].contains(mx, my)) {
            return &icons_[idx];
        }
    }
    return nullptr;
}
```

从 `icon_count_` 往 0 倒着找,第一个 `contains` 命中的就返回。`contains` 是 032 写好的左闭右开框:`mx >= x && mx < x+width && my >= y && my < y+height`——左上角在内、右下角恰好在外,这样相邻两个图标不会在边界上同时命中。逆序的意义前面说过:后注册(数组下标大)的图标,在重叠区优先被点中。这一章只注册两个不重叠的图标,暂时用不上这个优先级,但接口设计得和窗口 `hit_test` 一致,以后图标挤在一起也不会乱。

`consume_pending_icon_action` 负责把意图取走,并顺手清零:

```cpp
IconAction WindowManager::consume_pending_icon_action() {
    IconAction action = pending_icon_action_;
    pending_icon_action_ = IconAction::None;
    return action;
}
```

取出当前的 action,把槽清回 `None`,返回取到的值。关键在"取出并清零"这个语义:它是一次性的。调用方拿到 action 之后,槽就空了,下一次调用一定返回 `None`,除非中间又发生了一次图标点击。这个设计是为了避免一个动作被消费两次——你点一次 Shell,就应该只触发一次开窗,不能因为 tick 多跑了几轮就开出一串窗口。

这一章里,`consume_pending_icon_action` 不仅实现好了,而且已经被接进了 `gui_tick_callback`:tick 每个滴答来问一句"有没有人点了图标",取到 `OpenShell` 就调 `create_shell_terminal` 弹出终端,取到别的就忽略。`create_shell_terminal` 的具体实现(new Terminal、绑管道、add_window)留到 [033b](033b-gui-lazy-terminal.md) 展开,这一章先把它当作"点 Shell 会触发的那个动作"。`test_desktop_click_sets_and_consumes_action` 验证的就是这套语义:点一次图标,`consume` 得到 `OpenShell`,再 `consume` 一次得到 `None`。

### draw_desktop_icons:位图加居中标签

[window_manager.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/window_manager.cpp) 里的 `draw_desktop_icons`,把每个图标画成"一张位图 + 下面一行居中文字":

```cpp
void WindowManager::draw_desktop_icons(cinux::drivers::Canvas& screen) {
    if (font_ == nullptr) {
        return;
    }
    uint32_t glyph_w = font_->width();

    for (uint32_t i = 0; i < icon_count_; i++) {
        const DesktopIcon& icon = icons_[i];

        // 贴位图,透明像素由 draw_bitmap 自动跳过
        screen.draw_bitmap(icon.x, icon.y, icon.width, icon.height, icon.bitmap);

        // 算标签长度,居中画在位图正下方
        uint32_t label_len = /* 数到 '\0' */;
        if (label_len > 0) {
            uint32_t text_w  = label_len * glyph_w;
            uint32_t label_x = icon.x + (icon.width - text_w) / 2;
            uint32_t label_y = icon.y + icon.height + 2;
            screen.draw_text(label_x, label_y, icon.label, ICON_LABEL_COLOR, *font_);
        }
    }
}
```

位图这一步直接复用 032 的 `Canvas::draw_bitmap`——它拿到 `x, y, w, h` 和像素数组,逐像素拷,遇到透明色(0)就跳过,所以图标的圆角、镂空都自然成立,不会画出一个难看的方块底。

标签的居中算法值得说两句。我们假设每个字符宽度恒定(PSF 字体确实是这样,`glyph_w = font_->width()`),所以"标签总宽 = 字符数 × 字宽"。然后让标签相对图标**水平居中**:`label_x = icon.x + (icon.width - text_w) / 2`。图标宽 32,标签 "Shell" 五个字符,字宽 8,文本宽 40——比图标还宽,这时 `(32 - 40) / 2` 会算出一个负数,但因为是 `uint32_t`,它会回绕成一个巨大的值,标签就飞到屏幕外去了。所以这套居中只对"标签比图标窄"的情况成立;真要稳妥,得先判断 `text_w <= icon.width` 再居中,否则就左对齐。这一章注册的 Shell(5 字符 = 40px)正好踩在这个边界上,实际跑起来标签会偏——这是个已知的小毛刺,但不影响"图标能画出来、能被点中"这件主事,我们就不在这里纠缠。

垂直位置 `label_y = icon.y + icon.height + 2`,即标签紧贴位图下沿再留 2 像素空隙,文字用 `ICON_LABEL_COLOR`(纯白 `0x00FFFFFF`)画,在暗青桌面上足够醒目。`font_ == nullptr` 时整个函数直接返回——没字体就没法画标签,干脆一张图标都不画,避免画出一半。

### composite():为什么图标要插在 clear 和 blit 之间

合成循环只多了**一行**,但这一行的位置是这一章最关键的设计决定:

```cpp
void WindowManager::composite() {
    if (screen_ == nullptr) return;

    screen_->clear(DESKTOP_COLOR);        // 1. 抹成暗青桌面
    draw_desktop_icons(*screen_);         // 2. 画图标(新增)← 必须在这里
    for (uint32_t i = 0; i < count_; i++) // 3. 从底到顶 blit 各可见窗口
        if (windows_[i]->visible()) windows_[i]->blit_to(*screen_);
    draw_cursor(*screen_);                // 4. 画鼠标
    screen_->flip();                      // 5. 成帧
}
```

`draw_desktop_icons` 必须夹在 `clear` 和"blit 窗口"之间。位置错一点点,后果立刻可见:

- 如果画在 `clear` **之前**,clear 会把你刚画的图标整个抹成暗青色,桌面回到光秃秃,等于没画。
- 如果画在 blit 窗口**之后**,图标会盖在所有窗口上面——你拖一个窗口到图标上,图标反而浮在窗口前,像贴纸一样粘在最顶层,完全反直觉。

夹在中间才对:clear 先铺好干净的桌面底色,图标作为桌面的一部分画上去,然后窗口再叠在最上面。这样窗口盖住图标(视觉正确),鼠标光标画在所有东西之上(永远可见),顺序天然成立。`test_desktop_composite_icons_behind_windows` 就是来盯这条的:它在 `(0,0)` 摆一个图标,又在 `(0,0)` 建一个窗口,合成后断言 `(5,25)` 这个点(落在窗口内容区里)的颜色是 `Window::COLOR_CONTENT_BG`,不是图标的颜色——窗口确实盖住了图标。

这一行新增代码,也顺带回答了一个问题:为什么 030 的 composite 看起来"已经完整",却还是得改?因为 030 的桌面是空的,clear 之后直接 blit 窗口没问题;一旦桌面要摆东西,就必须在 clear 和 blit 之间留出一个"桌面层"的位置。033 做的就是把这个层插进去。

### handle_mouse():没点中窗口,就去找图标

输入侧的改动也集中在一处:`MouseDown` 分支里,原来 `hit == nullptr` 时只清焦点,现在多了一条图标命中的判断:

```cpp
case EventType::MouseDown: {
    if (!ev.mouse.left) break;

    Window* hit = hit_test(ev.mouse.x, ev.mouse.y);   // 先找窗口

    if (hit == nullptr) {
        // 没点中窗口 → 找桌面图标
        const DesktopIcon* icon_hit = hit_test_icon(ev.mouse.x, ev.mouse.y);
        if (icon_hit != nullptr) {
            pending_icon_action_ = icon_hit->action;  // 记下意图
            if (focused_ != nullptr) {                 // 并清焦点
                focused_->set_focused(false);
                focused_ = nullptr;
            }
        } else {
            // 纯桌面空白 → 只清焦点(030 的老行为)
            if (focused_ != nullptr) {
                focused_->set_focused(false);
                focused_ = nullptr;
            }
        }
        break;
    }

    // 命中窗口 → 走原有的 raise / 拖拽 / 关闭(图标完全不参与)
    ...
}
```

逻辑层次很清楚:窗口永远先查,命中窗口就走老路,图标完全不插手;只有"没点中任何窗口"时,才退而去找图标。命中图标就把它自带的 `action` 拷进 `pending_icon_action_`,同时清掉当前窗口焦点——点桌面图标意味着"你想离开当前窗口去启动别的东西",焦点自然该交出去。

注意这里触发的是**单击**(`MouseDown` 的瞬间),不是 [desktop_icon.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/desktop_icon.hpp) 注释里写的"double-click"。注释说"action fires on double-click",但代码实际只在左键按下时检查一次,根本没有双击计时逻辑。这种"注释和代码不一致"的情况,以代码为准——这也是读老代码时的一个习惯:注释描述的是设计意图,代码描述的是真实行为,两者打架时永远信代码。`test_desktop_click_sets_and_consumes_action` 用的就是一次 `MouseDown`,印证了实际是单击触发。

### gui_start():把两个图标摆上桌面

[gui_init.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/gui_init.cpp) 的 `gui_start()` 负责在开机时把图标注册进 WM——这一步归 033,因为不注册就什么图标都看不见:

```cpp
void gui_start() {
    cinux::lib::kprintf("[GUI] ===== Milestone 033: GUI Desktop =====\n");

    cinux::drivers::Mouse::init();
    if (g_screen != nullptr) {
        cinux::drivers::Mouse::set_screen_bounds(g_screen->width(), g_screen->height());
    }

    auto& wm = WindowManager::instance();

    DesktopIcon shell_icon{
        .x = 40, .y = 40,
        .bitmap = icons::data::k_shell_icon.data(),
        .label  = "Shell",
        .width  = icons::ICON_SIZE, .height = icons::ICON_SIZE,
        .action = IconAction::OpenShell,
    };
    wm.add_desktop_icon(shell_icon);

    DesktopIcon calc_icon{
        .x = 40, .y = 120,
        .bitmap = icons::data::k_calc_icon.data(),
        .label  = "Calculator",
        .width  = icons::ICON_SIZE, .height = icons::ICON_SIZE,
        .action = IconAction::OpenCalculator,
    };
    wm.add_desktop_icon(calc_icon);

    cinux::lib::kprintf("[GUI] Desktop icons registered: Shell, Calculator.\n");

    cinux::drivers::PIT::set_tick_callback(gui_tick_callback, nullptr);
    cinux::lib::kprintf("[GUI] GUI tick callback registered on PIT.\n");
}
```

两个图标都摆在 `x=40` 这一列:Shell 在 `y=40`,Calculator 在 `y=120`,竖向排开,中间隔 80 像素(图标本身 32 高,加上标签和留白)。位图取自 032 备好的 `icons::data::k_shell_icon` / `k_calc_icon`(各是 32×32 的像素数组),宽高都是 `icons::ICON_SIZE`(32)。两个图标的 `action` 分别填 `OpenShell` 和 `OpenCalculator`——这就是点击时会被拷进 `pending_icon_action_` 的那个值。

注册完打印一行 `Desktop icons registered: Shell, Calculator.`,然后把 `gui_tick_callback` 挂到 PIT 上。这个 tick 回调除了照常排空事件队列、调 `handle_mouse`、`composite` 之外,还会调 `consume_pending_icon_action` 取走点击意图:取到 `OpenShell` 就 `create_shell_terminal()` 真的弹出终端窗口,并打印 `[GUI] Shell terminal created and connected.`。所以点 Shell 图标,屏幕上会真的多出一个跑 shell 的终端,串口也多这一行;唯独 `OpenCalculator` 没有归宿——取出来不是 `OpenShell` 就直接忽略,Calculator 图标点了毫无反应。

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

`create_shell_terminal`、`set_shell_pipes`、`is_terminal` 这套 gui_init 侧重构是怎么搭起来的——终端对象怎么 new、管道怎么绑、`is_terminal` 的虚函数怎么让 tick 回调认出窗口是终端——是 [下一章 033b](033b-gui-lazy-terminal.md) 的主题。再往后,我们要离开 GUI 桌面、回到进程:给内核加上 `fork` / `execve`,让一个用户进程能生出另一个(见 [034](../10-multitasking/034-process-fork-exec.md))。

## 参考

- C++ `enum class`(支撑 `IconAction` 的类型安全枚举与 `pending_icon_action_` 初值):https://en.cppreference.com/w/cpp/language/enum
- C++ 指定初始化(designated initializers,支撑 `gui_start()` 里 `DesktopIcon{.x=40, .y=40, ...}` 这种按成员名构造的写法):https://en.cppreference.com/w/cpp/language/aggregate_initialization
- 030 章确立的合成分层顺序(`clear` → blit 窗口 → `draw_cursor` → `flip`),033 在 `clear` 与 blit 之间插入 `draw_desktop_icons`:见 [030 · 窗口管理器](030-gui-wm-basic.md)
- 032 章的位图原语(`Canvas::draw_bitmap` 透明像素跳过)与 `DesktopIcon::contains` 左闭右开命中框:见 [032 · 位图图标](032-gui-bitmap-icon.md)
