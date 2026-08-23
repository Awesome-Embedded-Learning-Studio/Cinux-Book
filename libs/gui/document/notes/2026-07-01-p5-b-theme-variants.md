# P5-b — 主题配色变体 + 运行时切换

> P5 第二批。Theme 加 Material 配色变体（primary_container / surface_variant 等）+ sdl-host 按 T 运行时切 light/dark。

## 背景

P3-b 的 Theme 只有 primary/surface 等基色，控件配色选择窄（按钮只能 primary，无"次要/柔和"变体）。Material 实际有 container / variant 色阶（M3）。P5-b 补齐 + 让 sdl-host 能运行时切 light/dark 验眼检。

## 目标

Theme 加 primary_container / on_primary_container / surface_variant（M3 值）；sdl-host 按 T 切 light↔dark（运行时重渲染）；theme-test 扩展。Host ABI 零改动。

## 设计

- **Theme struct 加 3 字段**：`primary_container` / `on_primary_container` / `surface_variant`（插在 on_primary 后 / surface 后）。聚合初始化 `material_light`/`material_dark` 同步加值。
- **M3 配色值**：light primary_container #EADDFF / on #21005D / surface_variant #E7E0EC；dark primary_container #4F378B / on #EADDFF / surface_variant #49454F。
- **运行时切换**：控件已持 `theme_` 指针（P3）。换 theme = 换 Theme 对象 + 遍历控件 `set_theme(&t)`；`Desktop::render` 每帧全屏重画（P3-a），下帧自动用新色。sdl-host 加 `bool dark`，SDLK_t 切 `t = dark ? dark : light` + 遍历 set_theme/set_color/set_bg。
- **theme-test 段 6**：断言 container/variant 非 0、有对比、light/dark 不同。

## 决策

1. **加字段而非新 Theme 变体类**：Theme 是数据 POD，加字段 + light/dark 填值最简；控件按需读新字段（如 Button Tonal 用 primary_container，留后续）。不破坏现有 set_theme。
2. **运行时切换 = 换指针 + 遍历 set_theme**：Desktop.render 全屏重画（P3-a）天然支持 —— 换 theme 不需 dirty 通知，下帧自动新色。代价：要遍历控件 set_theme（sdl-host 手动列）。将来 P5-c per-widget dirty 不影响（重画全做）。
3. **sdl-host demo 作眼检**：core theme 改动靠 theme-test（ctest）；运行时切换的"好看"靠 sdl-host 手动（T 键 light↔dark，WSLg 眼检）。同 P3 widgets-host 模式。

## 陷阱

- **聚合初始化字段顺序**：Theme 加字段后，`material_light`/`dark` 的 `Theme{...}` 必须按新顺序填全（漏字段 → 聚合警告/错位）。按 struct 声明顺序对齐 + 注释每行。
- **sdl-host mouse event 对齐空格**：format 后 `p.kind    =`（多空格对齐），Edit old 要精确含对齐，否则不匹配。
- **Label 无 set_theme**：Label 只有 set_color（无 theme_），运行时切换要单独 set_color(t.on_surface)，不能 set_theme。Button/Slider 有 set_theme。

## 验证

- theme-test 段 6：primary_container/surface_variant 非 0、有对比、light/dark 不同。**ctest 16/16 + ASAN 干净**。
- **sdl-host 编译通过**（T 键 KEYDOWN case；opt-in `-DCINUX_HOST_SDL=ON`）。
- **手动眼检**：`./build/sdl-host` → 按 T 切 light/dark，WSLg 看控件配色实时换。
- Host ABI 零改动；core 仍 host-neutral。

## CinuxOS 侧

零改动。

## 下一步

P5-c per-widget dirty + 帧间 diff（控件树从全屏 dirty 收紧到局部，省 composite/flush；Scene 退役前提）。
