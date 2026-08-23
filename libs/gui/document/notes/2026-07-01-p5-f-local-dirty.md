# P5-f — per-widget 局部 dirty（只刷脏区，砍 upload 量）

> P5-c(idle 0 flush)的深一步。terminal-host 在 WSLg 下卡 —— 全屏 upload 720×440(316k 像素)/帧 + core 全屏 render 2000 glyph。host 调优(SOFTWARE/限速/降帧)失败:SOFTWARE 反而未响应,ACCELERATED 拖死 X compositor。正路在 core:**per-widget 局部 dirty**,只 composite + upload 变化区。

## 背景

P5-c 让 idle(shell 不输出)时 0 flush。但 shell 一输出 → desktop.render 全屏 execute + host 全屏 upload。WSLg 扛不住整屏 streaming texture upload(GPU 路径阻塞 X)。要让"变化一行字"只 upload 那一行(704×16=11k 像素,省 96%),而非整屏。

## 目标

core per-widget dirty rect 收集 + execute clip 脏区 + host per-rect upload。terminal per-row(shell 输出只脏变化行)。Host ABI 零改动。

## 设计

**Widget 层**(widget.hpp/cpp):
- 去 P5-c 的 `dirty_` 传播 + `is_dirty()`;改 `dirty_self_`(self 脏,不传播)+ `dirty_rect_`(累积脏区 union)。
- `invalidate()` = `invalidate(rect_)`;新增 `invalidate(Rect r)`(标任意 rect 脏,union 进 dirty_rect_)。
- `virtual collect_dirty(Region& sink)`:if dirty_self_ → sink.add(dirty_rect_);递归 children。**不传播 parent**(传播会让 root union = 全屏,退化)。
- `virtual clear_dirty()`:清 dirty_self_ + dirty_rect_ + 递归。

**Desktop.render**(widget.cpp):
- `first_` force 首帧全屏(因 dirty_self_ 初 true 但 dirty_rect_ 空 degen,首帧靠 first_)。
- 否则 `root->collect_dirty(dirty)` 收 per-widget 脏 rects → Region。
- idle(空)→ 0 rect(0 flush)。
- 否则 flatten list + **对每 dirty rect `execute(list, font, &clip)`**(P2-c 模式:整 list clip 到脏 rect 重画,幂等)。

**compositor execute**(compositor.hpp/cpp):加 `const ClipRect* outer` 参数,作 base clip(kClipPush 与之 intersect)。

**TerminalWidget per-row**(terminal.hpp/cpp):
- `dirty_rows_[kMaxRows]` + `dirty_all_`。write 改 cell → `dirty_rows_[row]=true`;scroll/clear → `dirty_all_=true`。
- `collect_dirty` 覆盖:dirty_all_ → add(self 整);否则每个 dirty_rows_[r] → add(一行 rect:x 全宽,y0+r×16 到 +16)。
- `clear_dirty` 覆盖:清 dirty_rows_/dirty_all_ + base。

**Window move_to_**(window.cpp):`invalidate(old)` + `invalidate(new)` —— 移动露背景(old footprint 要重画 bg)。

**WM process_pointer**(window_manager.cpp):cursor 移 → `invalidate(old cursor rect)` + `invalidate(new cursor rect)`(4×4 小块,非全屏);raise → `invalidate(window rect)`(Z 序变)。

**terminal-host**(terminal_host_main.cpp):`SDL_UpdateTexture` 改 per-rect —— 对每 dirty rect 上传 `buf + y*pitch + x*4`,而非整张 `nullptr`。

## 决策

1. **collect_dirty 递归,不传播 parent**:P5-c 传播是为了 root `is_dirty()` 反映子;局部 dirty 要 per-widget rect,传播会让父 dirty_rect_ union 到 root = 全屏。改 collect_dirty 递归查每 widget dirty_self_,各自贡献 rect。
2. **terminal per-row 是关键**:shell 输出一行 → 1 个 704×16 rect upload(11k 像素)vs 全屏 316k,省 96%。整 terminal rect(P5-c 的 self)≈ 全屏,省不了;per-row 才省。
3. **execute per-rect clip(P2-c 模式)**:对每 dirty rect,execute(整 list)clip 到它。N × execute(N dirty rects)。N 通常 1-3(一行 + cursor)。clip 外 cmd 跳过,实际画脏区内。
4. **Window move old+new**:移动 old rect 露背景,必须 dirty(P5-c 全屏自动重画,局部要显式 old)。
5. **WM cursor rect**:cursor 移动最频繁,2 个 4×4 rect 而非全屏,鼠标移动不再卡。

## 陷阱

- **dirty_self_ 初 true + dirty_rect_ 空**:Widget 构造 dirty_self_=true 但 dirty_rect_ degenerate(空)。collect 首 frame add 空(ignored)→ idle(不画)。Desktop `first_` force 首帧全屏解。
- **clear_dirty virtual**:terminal override 清 dirty_rows_/dirty_all_(base 不知)。Widget clear_dirty 加 virtual。
- **clangd include path 误报**:swraster/event_payload not found(clangd 没认 core/ path)。gcc 编译(cinux-gui PUBLIC include)OK。
- **build-asan 目录偶发消失**:重 cmake configure。

## 验证

- standalone ctest **19/19** + ASAN 干净(零回归;collect/execute-clip/per-row 逻辑对)。
- terminal-host 编译链接(per-rect upload)。
- **性能眼检**(用户):per-rect upload 砍 90%+ 量(shell 输出一行只上传该行)。WSLg 下应从"卡死/拖死 X"到可用。若 WSLg 仍卡(极小 upload 也慢),那是 WSLg SDL 环境极限,core 已尽力。
- Host ABI 零改动;core 仍 host-neutral;纯整数 + 定长。

## CinuxOS 侧

零改动。

## 收尾 / 下一步

- **Scene 退役前提达成**:per-widget dirty + execute-clip 是控件树的帧间 diff(等价 P2-c Compositor 的 Scene diff)。offscreen/replay host 可迁控件树后,Scene 可退役(P4-e follow-up)。
- terminal-host 性能若仍不足(WSLg),候选:缩 staging(终端 60×18)+ SDL 软件(已知 WSLg 反例,谨慎)/ 局部 dirty 已是正路,剩余卡顿属 WSLg SDL 环境。
