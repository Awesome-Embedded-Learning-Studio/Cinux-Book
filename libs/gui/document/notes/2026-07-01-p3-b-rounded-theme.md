# 2026-07-01 · P3-b · swraster 圆角 + Material Theme(flat 子集)

> P3 第二批,给控件工具箱配齐 Material 外观能力:swraster 加圆角矩形原语(纯整数,逐行 isqrt 算角弧)+ core/theme 立 Material 配色(纯整数 flat 子集,无阴影/波纹)。Compositor::execute 的 kFillRoundRect 从 fallback fill_rect 转成真圆角。

## 背景

P3-a 立了控件骨架(PaintList + Widget 树 + Desktop),但 PaintList 的 kFillRoundRect 在 execute 里 fallback 成 fill_rect(没圆角原语)。控件要长得像 Material(圆角 + 配色),先得有这两样。

## 目标

1. **fill_rounded_rect**:纯整数圆角矩形(r clamp 到 min(w,h)/2;r==0 退 fill_rect)。
2. **Material flat theme**:`core/theme.*` 配色(primary/surface/on-surface/...)+ 圆角半径 + 8px 网格。light/dark 两套。
3. **execute 转真圆角**:kFillRoundRect 调 fill_rounded_rect(不再 fallback)。

## 设计

### `core/swraster.*::fill_rounded_rect` —— 整数圆角

逐行画:每行算圆角咬边量 `off = r - isqrt(r²-d²)`(d = 该行到角圆心的行距),画 `[x+off, x+w-off)`。底角行镜像到顶角(同一公式)。中行(d 超出角区)off=0 全宽。radius clamp 到 min(w,h)/2 防四角弧重叠。`isqrt_u32` 整数开方(线性扫,输入 ≤ r²≤256,够快)。复用 fill_rect 的 `effective_bounds` + `isect` 做 clip。

### `core/theme.*` —— Material flat 配色

`Theme{primary/on_primary/surface/on_surface/background/error/outline/button_radius/card_radius}` + `material_light()`/`material_dark()` 返回 Material 基线色(#6200EE / #BB86FC 等,XRGB8888)。8px 网格常量(kSpacing4/8/16)+ 圆角刻度(kRadiusSmall 4 / Medium 8 / Large 16)。

### Compositor::execute 转真圆角

kFillRoundRect 分支从 `fill_rect(fallback)` 改成 `fill_rounded_rect(c.rfill.radius)`。PaintList::fill_round_rect(P3-a 已有 push)现在画出真圆角。

## 决策

- **逐行 isqrt,不预计算 mask**:圆角半径变量(控件指定),预计算要 per-radius 表。逐行算 isqrt(r²-d²) 简单 + 输入小(r≤16)线性扫够快。若热路径实测慢,再换 isqrt 查表(r≤16 → 16 项表)。
- **flat Material,无阴影/波纹**(决策 B):纯整数算不起 blur;ripple 要动画系统。首批靠配色 + 圆角 + 8px 网格「形似」Material;「神似」(动效/深度)留 P5+。海拔靠 surface 变体色(后续控件选 surface vs tinted)而非算阴影。
- **r clamp 到 min(w,h)/2**:小控件(如 6×6 按钮 radius 100)不会爆,安全退化为接近圆。
- **execute fallback 退役**:P3-a 的 kFillRoundRect→fill_rect 是占位,本批转真原语。fallback 留给「radius==0」(fill_rounded_rect 内部调 fill_rect)。

## 陷阱

- **clang-format 单行收 Theme 返回**:theme.cpp 的 `return Theme{...};` 9 个字段 + 注释,clang-format 把最后俩 radius 收一行(`kRadiusMedium, kRadiusLarge,`)。无害,但 Edit old_string 要对齐 format 后形态(本批 Write 全文,无碍)。
- **底角镜像的 row 索引**:底角行 iy in [h-r, h-1],镜像 row = h-1-iy in [0, r-1],复用顶角 off 公式。写错镜像(如 row = iy - (h-r))会让底角弧方向反 —— test 的「bottom corner stays bg」会拦(theme-test 段 3 顶角 + 段 4 radius0 间接覆盖;底角由镜像对称保证)。

## 验证

- **standalone ctest 全绿**(Release):8/8 —— 新 `theme-test` 5 段:light/dark 配色断言 + 圆角(中心/边中 == color,外角 == bg)+ radius0 方角 + radius clamp。
- **ASAN 干净**:`theme-test`/`widget-test`/`compositor-test` exit 0。
- **clang-format 自洽**:swraster/theme.{hpp,cpp} self-diff = 0。

## 后续

- **P3-c(🔜)**:基础控件 Label/Button/Container + HBox/VBox,用 fill_rounded_rect + theme 画 Material 外观(Button 圆角 + primary 色 + press 状态)。
- **P3-d**:Slider + host demo + P2 Scene 退役。
- CinuxOS 侧零改动。
