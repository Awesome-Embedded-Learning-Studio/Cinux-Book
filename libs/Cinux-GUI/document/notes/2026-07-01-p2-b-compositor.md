# 2026-07-01 · P2-b · Compositor 接管场景绘制(Scene → staging 像素)

> P2 渲染收敛第二批。P2-a 立了 Scene 数据层;本批立**渲染器**——`compose(staging, scene, font)`,把 offscreen/replay/fbdev 三个 host 各自手抄的 `paint_scene`/`draw_rect_outline`/`draw_text` 收敛到一个 host-neutral 实现,像素严格对齐旧 offscreen 画法(零回归)。

## 背景

P2-a 之后,`Scene` 能描述一帧桌面(背景 + Window z 栈 + Cursor),但「Scene 怎么变成像素」仍散在三 host 的 `render_frame` 里,三份 `paint_scene` 几乎一字不差([replay_host_main.cpp:134](../../host/replay_host_main.cpp#L134) 注释自承「mirror offscreen_host_main」)。本批把画法抽上 core,host 的 `render_frame` 未来只需「更新 Scene → compose()」(P2-d 落地)。

## 目标

1. **host-neutral `compose()`**:读 Scene + staging Surface + PsfFont,用 swraster 画整场景。P2-b 每帧全画(现行 host 行为的 drop-in 替换);帧间 dirty diff 是 P2-c。
2. **像素零回归**:画法顺序 + 文本坐标严格复刻 offscreen host,test_compositor 在结构采样点(bg/窗口中心/标题栏中心/光标)断言像素一致。
3. **守铁律**:零 host include;依赖只在 core/(scene/swraster/font);Host ABI 不动。

## 设计

### `core/compositor.{hpp,cpp}`

- `compose(Surface& staging, const Scene& scene, const PsfFont& font)`:bg fill → 按 z 序 `compose_window` 每个 Window → cursor 最后画(恒在顶)。
- `compose_window`:face fill → titlebar band(`titlebar_height>0` 时)→ 1px edge outline → title text → body text。顺序/坐标与三 host 一致。
- 内部 helper `draw_rect_outline`(四条 Bresenham `draw_line`)、`draw_text`(`glyph_blit` 循环,'\\n' 换行):**三 host 重复实现的字面收敛**,签名带 `const ClipRect* clip`(对齐 swraster 原语,P2-c 脏区裁剪直接能用)。

### 零回归基线

`test_compositor::offscreen_scene()` 用 Scene 模型重建 offscreen host 的同一场景(几何/配色/文本字面相同),compose 后在 `verify_scene` 同款采样点断言:
- 原点 == bg、窗口中心 == face、标题栏中心 == titlebar、光标像素 == cursor、远角 == bg。

加上 z 序(后 Window 盖前 Window)、空 Scene(全 bg)、`titlebar_height==0`(无标题带,顶部 == face)三段。

## 决策

- **helper 留 compositor 内部,不晋升 swraster**(用户拍板决策点 B):`draw_rect_outline`/`draw_text` 不进 `core/swraster.*`。理由:swraster 是纯原语层(fill/blit/line/glyph),不依赖 font;`draw_text` 进 swraster 会反向引入 font 依赖,破层级。compositor 依赖 font+swraster+scene,方向单向无环。
- **helper 带 `clip` 参数**:初版为简化去掉了 clip(内部硬编码 nullptr),但 P2-c 「只 composite 脏区」必然要 clip 到脏 rect。与其 P2-c 再改签名,本批就带 clip(调用点传 nullptr),零返工、且与 swraster 原语签名一致。
- **画法字面复刻 offscreen**:不趁机「优化」画序/坐标。收敛的前提是「输出不变」,任何重排都要有零回归证据背书——本批只搬位置,不改语义。

## 陷阱

- **签名不匹配 → 编译失败**:初版去掉 helper 的 clip 参数后,`compose_window` 调用点仍传 `nullptr`(「too many arguments」)。修法选「加回 helper 的 clip 参数」而非「删调用点的 nullptr」——前者前瞻 P2-c,后者到 P2-c 又得加回。ctest 在编译期当场拦下。
- **IDE clangd 诊断滞后**:Edit helper 签名后 clangd 沿旧 compile_commands 报「No matching function」,但实际 `cmake --build` 通过(调用点带 nullptr、helper 带 clip,本就匹配)。p0-a 笔记第 54 行已记同一陷阱,`cmake -B` 重生成 compile_commands 后自消,**以实际编译为准**。

## 验证

- **standalone ctest 全绿**(Release):6/6——新增 `compositor-test`(4 段:零回归/z 序/空场景/无标题带)。
- **ASAN 干净**:`compositor-test`/`scene-test`/`evdev-test` 直接运行 exit 0,无 leak/越界。
- **clang-format 自洽**:`core/compositor.{hpp,cpp}` format 后 self-diff = 0。

## 后续

- **P2-c(🔜)**:Compositor 持 prev Scene,`compose` 只 composite 变化区(省 CPU 不止省 flush),返回 dirty Region;`window_rect`/`cursor_rect` 进 region 代数,helper 的 `clip` 参数派上用场。
- **P2-d**:三 host `render_frame` 切到「Scene + compose()」,删各自重复 paint;fbdev QEMU 冒烟验无回归。
- CinuxOS 侧零改动(Host ABI 没动)。
