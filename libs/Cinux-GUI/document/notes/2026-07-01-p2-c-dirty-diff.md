# 2026-07-01 · P2-c · Compositor 帧间 dirty diff(只 composite 变化区)

> P2 渲染收敛第三批,核心增值。P2-b 的 `compose()` 每帧全画 staging;本批给 Compositor 加状态——持上一帧 Scene 快照,只 composite 变化区,返回 dirty Region。省的是 **composite 本身(CPU 画像素)**,不止省 flush(上屏带宽)——这是 P2 的点睛之笔。

## 背景

P2-b 之后,「Scene → 像素」收敛进 host-neutral 的 `compose()`,但每帧整场景重画。replay_host 早就证明 dirty 只省 flush 不省 composite([replay_host_main.cpp:237](../../host/replay_host_main.cpp#L237) `paint whole staging (correct); flush only dirty`)。本批把帧间 diff 也搬进 core,host 手写的 `prev_cx/prev_wx/...` 脏区逻辑([:228-263](../../host/replay_host_main.cpp#L228-L263))随之收敛。

## 目标

1. **stateful Compositor**:持 `prev_` Scene,`compose(staging, scene, font, &dirty)` 算 dirty 并只重画脏区。
2. **永不欠覆盖**(DIRECTIVES A 铁律):移动元素的**旧 ∪ 新** footprint 都进 dirty——旧位置重画(露背景),新位置重画(新内容)。
3. **idle 0 写**:Scene 没变 → dirty 空 → 不画。
4. **保留 P2-b stateless `compose()`**:零回归基线 + 简单场景/测试入口;两者共享同一画核。

## 设计

### 两个入口,一个画核

- `paint_scene_clipped(s, scene, font, clip)`:bg → windows(z 序)→ cursor,**每个原语 clipped 到 @p clip**。clip=nullptr = 全屏。
- stateless `compose()` = `paint_scene_clipped(nullptr)`(P2-b 行为不变)。
- `Compositor::compose()` = diff → dirty Region → 对每个 dirty rect 调 `paint_scene_clipped(&rect_clip)`。

### Scene diff(`diff_scene`)

逐 index 比较 prev vs cur 的 window 栈(取 `max(prev_count, cur_count)`):
- bg 变 → 全屏 dirty。
- window 变(几何/配色/文本)→ **add 旧 rect + add 新 rect**。
- window 新增 → add 新 rect;window 消失 → add 旧 rect。
- cursor 变 → add 旧 cursor rect + add 新 cursor rect。
- 全等 → dirty 空(idle)。

`window_equals`/`cursor_equals` 逐字段比 + `str_eq`(手写 NUL 比较,无 `<cstring>`)。

### 每个 dirty rect「全场景 clipped」

不按 window 拆分画,而是**对每个 dirty rect 跑一遍 paint_scene_clipped**(clipped 到该 rect)。正确性靠:① bg 先画 → 移动 window 的旧 footprint 里 bg 被重画(露背景)② z 序 → 后画 window 盖前画 ③ 幂等 → rect 重叠区重画同像素。性能:dirty rect 少(光标移动 = 2 rect,窗口移动 = 2 rect),可接受;P2-d 之后再优化。

## 决策

- **dirty = old ∪ new,非包围盒**:移动 window 的旧/新 rect 分别 add(可能 2 个分离 rect),而非 union 成一个大 bbox。前者更精确(省画),但都满足「不欠覆盖」。Region 容量溢出坍缩包围盒是最后保险(过覆盖,非欠覆盖)。
- **每个 dirty rect 全场景 clipped,而非 per-window per-rect**:实现极简(一个画核)、正确性易证(z 序 + 幂等)。复杂的有向脏区(只画「实际暴露的」)留给需要时——P2 当前场景 dirty rect 极少,不值。
- **保留 stateless `compose()`**:不把 P2-b 入口删掉。理由:test_compositor 的 P2-b 零回归段仍用它(基线锚点),且简单 host(只想全画)不必持状态。两入口共享 `paint_scene_clipped`,零冗余。
- **`Compositor::compose` 的 `dirty` 可 nullptr**:host 若不关心 dirty(只想画),传 nullptr,内部用 sink region。一个入口两用。

## 陷阱

- **IDE clangd 诊断滞后(复发)**:P2-c 给 compositor.hpp 加 `Compositor` 类 + 新 include,clangd 沿旧 compile_commands 报「compositor.hpp file not found / Unknown type Region」一片。`cmake --build` 实际通过、ctest 6/6 绿。p0-a/P2-b 笔记都记过,**以实际编译为准**。
- **露背景的正确性依赖画序**:「window 移动后旧位置露 bg」只有当 paint_scene_clipped **先 bg 后 window** 时才对——若先画 window 再 bg,bg 会盖掉 window。P2-b 的画序(bg→window→cursor)本就对,本批复用,test_compositor 段 8 显式断言旧 footprint 像素 == bg 锁死。

## 验证

- **standalone ctest 全绿**(Release):6/6——`compositor-test` 扩到 9 段(P2-b 4 + P2-c 5):
  - P2-c:首帧全屏 / 同帧 idle(0 dirty)/ cursor 移(脏 < 全屏,旧∪新都在 dirty,新光标像素画对)/ window 移(脏 < 全屏,新中心 == face,**旧 footprint == bg 露背景**)/ bg 变(全屏)。
- **ASAN 干净**:`compositor-test` 直接运行 exit 0,无 leak/越界。
- **clang-format 自洽**:`compositor.{hpp,cpp}` format 后 self-diff = 0。

## 后续

- **P2-d(🔜)**:offscreen/replay/fbdev 三 host 的 `render_frame` 切到「Scene + `Compositor::compose`」,删各自 `paint_scene`/`draw_rect_outline`/`draw_text` 重复;replay_host 的手写 `prev_*` 脏区逻辑退役。fbdev 重跑自构建 kernel+initramfs QEMU 冒烟验像素无回归。
- CinuxOS 侧零改动(Host ABI 没动)。
