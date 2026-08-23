# 2026-06-30 · P0-b3 · replay host（事件回放 + 拖拽 + 脏区纪律断言）

> P0-b 第三批，P0 收尾。第一次用**真输入**通过事件 ABI 驱动核心：硬编码拖拽脚本 → `poll_event` → `dispatch_event` → host 状态机拖窗口 → `render_frame` 报真脏区 → 断言终态几何 + 脏区纪律。**P0 Probe-0 至此完成。**

## 背景

b2 画出第一张静态图。P0 探针验收还差「输入驱动 + 光标/窗口动 + 脏区只刷该刷的」（PLAN 验收三条）。b3 把 offscreen host 升级成 replay host：喂脚本事件，host 维护动态状态，render_frame 报真脏区，ctest 断言几何 + 脏区纪律。

## 目标

1. 事件 ABI 实战：文本/脚本事件 → `poll_event`（构造 EventHeader + PointerPayload）→ `dispatch_event`（memcpy 解析 + 应用）。
2. host 状态机：cursor 移动；down 在标题栏 → 拖拽；move+拖拽 → 窗口跟 delta；up → 停。
3. 真脏区：首帧全屏；后续只报 old+new cursor/win bbox；idle（无像素变化）count==0。
4. 断言：每帧 cursor/win 位置 + 脏区纪律（idle 0 / 首帧全屏 / 后续 < 全屏 + 新 cursor 在 flushed 区）。
5. 多帧 dump PPM 供人眼。

## 设计

### `host/replay_host_main.cpp`（独立 main，平行 offscreen host）

- 回放脚本 `kScript[]`：6 个 pointer 事件（move→down→drag×2→up→move away），每帧喂 1 个（`frame_quota`）。
- `HostState`：cursor/win 位置、dragging、prev 位置（脏区算）、replay_idx、frame_quota、flushed[] 记录。
- `poll_event`：从脚本取下一事件，构造 `EventHeader`（magic/version/type=kPointer）+ `memcpy` `PointerPayload` 到 header 之后（照 CinuxOS host_cinux 范式）。
- `dispatch_event`：校验 magic，memcpy 出 PointerPayload，按 kind 应用（down 命中标题栏→dragging；up→停；move+dragging→win += cursor delta）。
- `render_frame`：检测 cursor/win 是否移动；未移动且非首帧→`count=0`（idle）；否则整屏重画 staging + region 收脏区（首帧全屏 / 否则 old+new cursor/win bbox）+ 更新 prev。
- `flush`：记 rect 到 `st->flushed[]` 供断言。
- `main`：每帧 `frame_quota=1` → `pump` → 校验 `kExpect[f]`（cursor/win/idle）+ 脏区纪律 → dump 关键帧。

### 脏区纪律断言

- `e.idle` → `flush_count == 0`
- `f==0` → `flushed_area >= 全屏`
- 其他非 idle → `flushed_area < 全屏` **且** `flushed_contains(新 cursor)`
- 脚本后再一帧（无事件）→ 必须 idle

## 决策

- **结构断言 vs 二进制金帧 fixture**：选结构断言（cursor/win pos + 脏区面积/覆盖）。二进制金帧怕抗锯齿/字体差异 flake，且首次正确性仍靠人眼；结构断言更鲁棒、直接验「该动的动了 + 该刷的刷了」。P0-c 的「金帧」精神由此吸收。
- **整屏重画 staging + 只 flush 脏区**：b3 的简化双缓冲（staging 每帧整屏重画保证正确，flush 只推脏区省传输）。真差分合成留 P2。
- **`frame_quota=1`（每帧 1 事件）**：pump 的 poll_event while 会 drain 全部可用事件；为逐帧看变化 + 验脏区，用 `frame_quota` 把每帧限制在 1 事件。真 GUI 一帧可多事件，但 b3 的逐帧是验证需要。
- **独立 main vs 扩 offscreen**：独立（replay_host_main）。offscreen 是静态一帧（b2），replay 是动态多帧（b3），职责不同；画图 helper 平行（未提取共享，P0 不过度抽象）。

## 陷阱

- **down/up 是 idle**：down/up 只改 dragging 状态，不改像素（cursor 同位、win 同位）→ render_frame 检测无移动 → `count=0`。这是对的（屏幕没变）。断言 `f1/f4 idle flush==0` 验证了这点。
- **drag delta 用绝对坐标差**（`p.x - old_cx`），不用事件的 dx 字段——`old_cx` 是 dispatch 前的 state，可靠；dx 字段要脚本额外维护。
- **poll_event 的 out_cap 校验**：`cap < sizeof(EventHeader)+sizeof(PointerPayload)` 才写（防 overrun）。pump 传的 buf 是 8+24=32 ≥ 8+18=26，OK。
- **render_frame 先算 dirty 再更新 prev**：prev 必须是「上一帧终态」，所以在帧尾更新；用 prev 与 cur 比算变化区。

## 验证

- **ctest 全绿**（3 test）：smoke + offscreen-dump + replay-dump。
- **replay-dump stdout**（几何 + 脏区全对）：
  ```
  f0: cursor(100,48) win(60,40)  flush=1   (首帧全屏)
  f1: idle                       flush=0   (down 不变)
  f2: cursor(130,48) win(90,40)  flush=4   (拖拽: old+new cursor/win)
  f3: cursor(160,52) win(120,44) flush=4   (拖拽)
  f4: idle                       flush=0   (up 不变)
  f5: cursor(250,200) win(120,44) flush=2  (cursor 走、窗口留)
  ```
- **ASAN 干净**：3 test + 直接 run 均 exit 0。
- 拖拽终态：窗口 60,40 → 120,44（+60x +4y，两次 drag delta 累计）；cursor → 250,200；up 后窗口停。

## P0 Probe-0 完成 ✅

验收三条全过：
1. 回放事件驱动，dump 帧序列光标/窗口动 ✓（stdout + 多帧 PPM）
2. 脏区只刷该刷的（非全屏）✓（断言：idle 0 / 首帧全屏 / 后续 < 全屏 + 新 cursor 在 flushed）
3. 金帧对比 ✓（结构断言代替二进制 fixture，更鲁棒）

**全程零 CinuxOS 依赖**——本仓 standalone 构建 + 跑 + 验证。

## 后续

- **P1 Probe-1（待启动）**：QEMU 真 fbdev + evdev（第二个真 host，证解耦缝）。手动冒烟（非 CI），`timeout` 包裹。b2/b3 的 host 范式直接指导 fbdev host 写法。
- **P2 渲染收敛**：swraster 正式接管，compositor dirty-region 双缓冲差分（b3 的「整屏重画」在此优化成真差分）。
- CinuxOS 零改动。
