# 2026-06-30 · P0-b2 · offscreen host + PPM dump（人眼看到第一张图）

> P0-b 第二批。b1 字体通了之后，本批画一张静态桌面场景 dump 成 PPM——**本仓第一次端到端画出像素、人眼可见**。证明 `pump → core.staging → swraster → glyph_blit → PPM` 全链路。

## 背景

b1 让 `PsfFont.glyph()` → `swraster::glyph_blit()` 通了（单字单测）。但 swraster 还没在「真渲染路径」上画出过一张完整图。b2 搭一个 offscreen host，画带窗口/标题栏/文字/光标的静态场景，dump 一帧 PPM，眼见为实。

## 目标

1. offscreen host（独立 main）用 swraster + PsfFont 画静态场景进 `GuiCore.staging`。
2. PPM dump（P6），落盘可看。
3. 端到端验证：pump 一次 → staging 被画 → PPM 落盘 → 人眼/视觉模型确认场景。

## 设计

### `host/offscreen_host_main.cpp`（独立 main）

- 构造 `GuiCore(320×240, XRGB8888)` + 填 Host 表。
- `render_frame` 回调（host 侧）cast `frame->pixels` 成 `Surface`，用 swraster 画：bg → 窗口 face → 标题栏 → 边框 → 标题文字 → 多行 body 文字 → 光标。
- `pump()` 一次 → `write_ppm(core.staging())`。
- `verify_scene`：读 staging 三结构像素（bg 角 / 窗口中心 == `kWinFace` / 标题栏 == `kTitleBar`），自校验场景画了。

### `host/ppm_writer.{hpp,cpp}`

`write_ppm(path, w, h, pixels, stride)`：XRGB8888(`0x00RRGGBB`) → P6 binary。每像素 `memcpy` 出 uint32 + 移位取 RGB（无别名 UB、endian-agnostic 值层面），丢 X byte。host 侧 `fopen/fwrite`——**core 永不写文件**。

### 关键 host 回调契约

- `render_frame`：预填了 `frame->pixels`（core 拥有的 staging），host 往里画 + 报脏区（首帧全屏 dirty，count=1）。
- `flush`：**空实现**。offscreen 无真屏；dump 在 pump 后直接读 `core.staging()`。

### 场景（用户选「加料」）

深色 bg + 窗口（`fill_rect` face + `draw_rect_outline` 边框）+ 蓝色标题栏（`fill_rect`）+ 标题 "Cinux"（`draw_text`）+ body "Hello\nCinux-GUI"（多行）+ 白色光标块。两个画图 helper 嵌 main：`draw_rect_outline`（4 条 `draw_line`）、`draw_text`（逐字 `glyph_blit`，遇 `\n` 换行）。

## 决策

- **独立 main vs 扩 `fake_host_main`**：独立。`fake_host_main` 是 host-neutrality smoke（纯逻辑断言），混进场景渲染 + PPM 会臃肿（§7b 单文件聚焦）。两 main 各司其职。
- **空 flush**：`GuiCore::pump()` 要求 `flush != nullptr` 才调 `render_frame`（`if (render_frame==null || flush==null) return`）。offscreen 无屏，给空 flush 让 pump 跑通；dump 走 pump 后读 staging。
- **加料场景**（标题栏 + 多行文字）：用户拍板。比最小集更像真窗口，`draw_text` 顺手支持了 `\n` 多行（b3/beyond 复用）。
- **dump 时机**：pump 后直接读 `core.staging()` 整屏 dump。首帧整屏 dirty；细粒度脏区到 b3。

## 陷阱

- **pump 要 flush 非 null**：第一次写 offscreen host 时若 `flush=nullptr`，pump 在 `render_frame` 前就 return，场景根本不画——得给空 flush。
- **产物路径**：ctest 跑时 cwd=build dir，PPM 写 `build/`（已 ignore）；手动 `./build/offscreen-dump` 在仓库根跑会写根。已加 `*.ppm` 到 `.gitignore` 保险；手动 run 的根产物清掉。
- **PPM→PNG 看图**：本机无 ImageMagick `convert` / Python PIL。用 **Python 标准库 `zlib`+`struct`** 手写最小 PNG encoder（uncompressed→deflate stored via `zlib.compress`），转完用 Read 工具 + 视觉模型确认场景。
- **颜色判读**：`kText=0x00181818`（深色）画在浅灰窗口（`kWinFace`）上，对比度好；视觉模型把它说成 "white text" 是判色误差，实际深色，不影响可读性。

## 验证

- **ctest 全绿**（2 test）：`cinux-gui-smoke` + `offscreen-dump`。后者内部 `verify_scene` 校验 bg/window/titlebar 像素 + `write_ppm` 成功。
- **ASAN 干净**：`-fsanitize=address` 构建 + ctest + 直接 run 均 exit 0。
- **PPM 落盘**：320×240，P6 头 `P6\n320 240\n255\n`，230 KB。
- **视觉确认**：PPM→PNG，视觉模型逐元素确认——深色 bg、蓝色标题栏、"Cinux" 标题、窗口内 "Hello"/"Cinux-GUI" 两行可读、白色光标块，无渲染瑕疵。**管线端到端打通。**

## 后续

- **P0-b3（🔜）**：事件回放（文本事件流 → `poll_event`）+ 拖拽交互（光标/窗口随事件动）+ 多帧 dump + 金帧/脏区断言。b2 的 offscreen host + `ppm_writer` 直接复用。
- CinuxOS 零改动。
