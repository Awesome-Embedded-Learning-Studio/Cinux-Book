---
title: 02 · 桌面图标实现
---

# 桌面图标实现

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

这一章里,`consume_pending_icon_action` 实现好了,也接进了 `gui_tick_callback`:tick 每个滴答来问一句"有没有人点了图标",取到 `OpenShell` 就调 `create_shell_terminal` 弹出终端,取到别的就忽略。`create_shell_terminal` 的具体实现(new Terminal、绑管道、add_window)留到 [007](../007/) 展开,这一章先把它当作"点 Shell 会触发的那个动作"。`test_desktop_click_sets_and_consumes_action` 验证的就是这套语义:点一次图标,`consume` 得到 `OpenShell`,再 `consume` 一次得到 `None`。

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

