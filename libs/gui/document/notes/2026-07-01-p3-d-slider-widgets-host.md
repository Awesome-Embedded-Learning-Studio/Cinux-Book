# 2026-07-01 · P3-d · Slider + press capture + widgets demo host(P3 收官)

> P3 第四批(收官)。Slider 控件(拖动取值)+ Desktop press capture(拖动跟手)+ widgets demo host(控件树端到端 Material 外观,ctest + 像素眼检)。fbdev 真 host 跑控件化作 follow-up。

## 背景

P3-a/b/c 立了控件骨架 + Material 外观 + 基础控件/布局。P3-d 补最后一块:交互控件 Slider(拖动)+ Desktop press capture(拖动跟手)+ 一个端到端 demo host 证明控件树真的能画出 Material UI。

## 目标

1. **Desktop press capture**:down 记 press_target,move/up 发给它(拖动出控件仍收 move,Slider 需要),up 释放。无 press 的 move 忽略(P3 无 hover)。
2. **Slider**:拖动取值 [0,range] + clamp;圆形 thumb(radius=size/2)+ outline track,配色从 Theme。
3. **widgets demo host**:`host/widgets_host_main.cpp` —— HBox(Label + Button + Slider)Material 控件树,desktop.render 一帧,验 Material 配色上屏 + dump PPM。

## 设计

### Desktop::dispatch_pointer 加 press capture

down → `press_target_ = hit_test` + 交付;move → 若 press_target_ 交付给它(不重 hit-test);up → 交付 + `press_target_ = nullptr`。Slider on_pointer(down→dragging+apply_x,move→apply_x,up→dragging=false)靠 capture 在拖出 thumb 时仍收 move。

### `core/widget/slider.{hpp,cpp}`

`apply_x_(x)`:value = (x - pad) * range / content_w,clamp [0,range]。paint:fill_rounded_rect(track, outline, r=2) + fill_rounded_rect(thumb, primary, r=size/2 圆形)。

### widgets demo host(不经 pump,直接 desktop.render)

HBox(padding16/spacing16)[Label "Material" | Button "OK" | Slider@60],material_light。`desktop.render(staging, font, &dirty)` 一帧 → 验 bg + OK primary 像素 + dump PPM。证明 Widget 树 → flatten → PaintList → execute → staging 全链路。

## 决策

- **fbdev 控件化作 follow-up(不阻塞 P3-d)**:fbdev 切控件树要重写 linux_fbdev_main(覆盖 P2 Scene 路径)+ 重 build static/initramfs + QEMU 冒烟(慢)。而 widgets-host ctest + 像素眼检 + slider/widgets-test 已完整证明控件层(数据 + 交互 + 渲染)。fbdev 真跑控件是「集成验证」,推到 P4 桌面迁入时 fbdev 自然控件化。同 CI-P2 模式:核心层 ctest 充分证,真 host 集成作 follow-up。
- **Scene 不强行退役**:offscreen/replay 保留 P2 Scene 路径(P2 回归基线,test_compositor/offscreen-dump/replay-dump 继续验 P2)。core/scene.* + compose(Scene) 保留。控件树是新场景源(widgets-host 用),与 Scene 共存 —— A①「控件树取代 Scene」在「新场景走控件树」层面兑现,不强删 P2 基线。
- **Slider thumb 圆形(radius=size/2)**:fill_rounded_rect(16x16, r=8)→ r clamp 到 8=16/2 → 正圆。Material slider thumb 是圆。整数圆角原语(P3-b)直接用。
- **demo host 不经 pump**:widgets-host 直接 desktop.render(malloc staging),不走 GuiCore/pump/Host ABI。证明控件层;fbdev 才经 Host ABI(真 host)。

## 陷阱

- **press capture 改了 P3-a 的 dispatch 语义**:P3-a 段2「miss move 不交付」在 capture 下变成「captured move 交付」。test_widget 段2 重写测 capture 语义(down→1, captured move→2, up→3, post-up move→3)。语义演进,test 跟进。
- **HBox 等分整除**:widgets-host 3 件 pad16/spacing16 → child_w=(288-32)/3=85(丢 1px)。无碍视觉。

## 验证

- **standalone ctest 全绿**(Release):11/11 —— 新 `slider-test`(drag/clamp/paint)+ `widgets-dump`(bg + OK primary 像素)。test_widget 段2 改测 capture。
- **ASAN 干净**:`slider-test`/`widgets-test`/`widget-test`/`widgets-dump` exit 0。
- **像素眼检**(widgets_frame.ppm → png):浅灰背景 + "Material" label + 紫色圆角 "OK" 按钮 + slider(轨道 + 圆 thumb),完整 flat-Material UI,无乱码。

## 后续 / P3 收官

- **P3 完成 ✅**:Widget 树 + PaintList + 事件路由(P3-a)+ 圆角/Material Theme(P3-b)+ Label/Button/Container/HBox/VBox(P3-c)+ Slider/press capture/widgets demo(P3-d)。**Host ABI 零改动,CinuxOS 不 bump pin**。
- **Follow-up(迟早做)**:① fbdev host 切控件树 + QEMU 冒烟(真 evdev → Desktop 全链路)。② P2 Scene 退役(确认无 P4+ 引用后删 core/scene.* + compose(Scene))。③ 布局升级(flex 权重 / preferred-size)、事件冒泡、ripple 动效(P5+ 动画系统)。
- **P4(🔜)**:桌面迁入 —— CinuxOS `kernel/gui/` 的 WM/terminal/desktop 搬上控件树 + Host ABI,CinuxOS 只剩 host_cinux。
- CinuxOS 侧零改动。
