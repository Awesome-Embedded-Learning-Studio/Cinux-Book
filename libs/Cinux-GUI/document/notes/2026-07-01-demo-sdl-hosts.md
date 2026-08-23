# 2026-07-01 · demo-hosts · Material 控件展示(demo-dump + SDL2 交互 host)

> P3 收官后加的两个展示 host:① `demo-dump` 把 VBox/HBox 嵌套的 Material UI dump 成一张 PPM(丰富版静态展示);② `sdl-host` 开 SDL2 桌面窗口,鼠标实时喂 `Desktop::dispatch_pointer`,让你能真点按钮、拖 slider。 SDL2 是本仓第一个「高层窗口 host」(P1 的 fbdev 是「低层 fbdev host」)。

## 背景

P3 控件层做完后只有静态 PPM(widgets-dump)和脚本驱动(replay),没法「坐进去鼠标操作」。要直观感受 Material 控件 + press capture + slider 拖动,需要一个窗口化 host:把 staging 渲染到桌面窗口,鼠标事件喂控件树。

## demo-dump(静态丰富展示)

`host/demo_host_main.cpp`:VBox[title `Cinux-GUI` + HBox[New/Open/Save] + `Volume` label + Slider@60 + HBox[OK/Cancel]] Material light,`desktop.render` 一帧 dump `demo.ppm`。比 P3-d 的 widgets-dump(3 控件平铺)更展示嵌套布局 + 多控件。CMake `demo-dump` target(不进 ctest,纯 dump 看外观)。

## sdl-host(SDL2 交互 host)

`host/sdl_host_main.cpp`:
- SDL2 开窗(320×240 staging 经 `SDL_RenderSetLogicalSize` 拉伸到 640×480,鼠标坐标 1:1 映射回 staging)。
- 主循环:`SDL_PollEvent` → 鼠标 motion/down/up 转 `PointerPayload` → `desktop.dispatch_pointer`;然后 `desktop.render(staging)` → `SDL_UpdateTexture` + `SDL_RenderPresent`(60fps cap)。
- 控件树同 demo-dump。鼠标点按钮(按住变白底紫字,press capture 跟手)/ 拖 slider(圆 thumb 跟鼠标,clamp 0–100)。
- **opt-in** `-DCINUX_HOST_SDL=ON`(`find_package(SDL2)` + `SDL2::SDL2`)。SDL2 依赖在 **host 层,core 零 SDL**(守 Host ABI 缝 —— core/ 不 include SDL,sdl-host 是 host 适配器,跟 fbdev/offscreen 同海拔)。

## 决策

- **SDL2 作交互 demo host(ROADMAP「高层 host 以后再加」的首次落地)**:SDL/X11/Wayland 在 ROADMAP 标「以后再加,不用于调试核心」—— 这里加 SDL2 不是调试核心(core 已有 ctest + fbdev 验证),是**展示/交互**(让控件层可被人坐进去操作)。host 层依赖,不污染 core。
- **opt-in CMake option(默认 OFF)**:CI runner 无 SDL2(也不该为展示 host 加重 CI);`-DCINUX_HOST_SDL=ON` 时才 find_package + build。默认 build/ctest 不受影响。
- **sdl-host 不经 pump/GuiCore**:直接 `desktop.render(malloc staging)` + SDL 上传。跟 widgets-dump 同路径(Desktop::render → execute PaintList),证明控件层;fbdev 才经 Host ABI(pump)。
- **逻辑尺寸 320×240 + 拉伸**:staging 是 320×240(同其他 host),SDL 窗口 640×480 + logical size 保持鼠标坐标 1:1。像素被 SDL 拉伸放大(桌面看着像大像素),无碍交互。

## 陷阱

- **WSLg + SDL2**:`DISPLAY=localhost:0`(WSLg),SDL 经 X11 开窗到 Windows 桌面。`timeout 3 ./build/sdl-host` 验证 rc=124(timeout 杀 = SDL 起来跑满 3s,窗口正常)。无 WSLg 的纯 WSL2 跑不了(需 X server)。
- **SDL2 cmake config**:`find_package(SDL2)` + `SDL2::SDL2` imported target(SDL2 2.32 提供)。老版 SDL2(无 config)要改 `pkg_check_modules` 或 `sdl2-config` —— 本机 2.32 有 config,直接用。

## 验证

- **demo-dump**:dump `demo.ppm` → png,眼检 Material UI(标题 + 3 按钮 + Volume + slider + OK/Cancel,无乱码/溢出)。
- **sdl-host**:`timeout 3 ./build/sdl-host` rc=124(窗口起,WSLg 预览);用户 `./build/sdl-host` 实跑,鼠标点按钮/拖 slider 交互(手动验,非 CI)。
- 默认 build(无 `-DCINUX_HOST_SDL`)+ ctest 11/11 不受影响(sdl-host opt-in 不编)。

## 后续

- 这俩是 **P3 后的展示工具**,不进 P3 批表。
- sdl-host 是 SDL/X11/Wayland 高层 host 的起点;以后可加 X11/Wayland host(同 pattern,host 层适配)。
- 真要「跑控件树到真 fb」:fbdev host 切控件树(P3 follow-up)+ QEMU 冒烟。
