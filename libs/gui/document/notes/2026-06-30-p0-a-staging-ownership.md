# 2026-06-30 · P0-a · staging Surface 所有权归 core + swraster 接入渲染路径

> P0 探针第一批。扳正 DIRECTIVES A「core 拥有 staging」铁律——之前 staging 物理上由 host 在 `render_frame` 里填，跟铁律相反；本批把所有权挪进 core，swraster 由此进入真实渲染路径，region 代数也挂上。

## 背景

P0 启动前，`core/` 有 pump / region / swraster / event / Host ABI，standalone ctest 绿。但有一处铁律违反：

- DIRECTIVES A 的 flush 显示模型要求 **core 拥有一块 staging Surface** → host 画进去 → region 算脏 → flush 推 host。
- 实际代码（旧 `pump.cpp`）：pump 调 `host->core.render_frame(ctx, &frame)`，**`frame.pixels` 由 host 在 render_frame 里填**（`fake_host_main.cpp` 的 `static uint32_t stage[4*4]; frame->pixels = stage;`）。即 staging 物理上 **host 分配、host 拥有**，与「core 拥有」相反。
- swraster 因此自承 "NOT wired into pump yet"——绘制能力悬着，本仓 standalone 画不出像素。

## 目标

1. 把 staging Surface 的**物理所有权**从 host 挪到 core（落地「core 持有 staging」）。
2. 让 swraster 从「只被单测调」进入真实渲染路径（host 经 Frame 拿到 core 的 staging 往里画）。
3. 让 region 代数挂上渲染路径（host 报脏 → Region 收 → flush），为 P2 差分算脏埋点。
4. P0-a 不画真场景（P0-b 干），只打通所有权 + 可写通路 + 数据流。

## 设计

### 新增 `core/gui_core.{hpp,cpp}` —— core 会话状态

`class GuiCore`：core 侧会话对象，持有 staging Surface 描述 + owned backing buffer + dirty_rects 数组 + host 指针。构造按 display 尺寸分配一次 staging（跨帧复用），析构释放，不可拷贝。

### pump 状态化：`pump(Host*)` → `GuiCore::pump()`

pump 从 stateless free function 变成 `GuiCore` 的成员方法（状态化后必须有持有者）。原 `pump.hpp`/`pump.cpp` 合并进 `gui_core.{hpp,cpp}`。pump 体：

1. drain input（poll_event → dispatch_event）
2. **预填 Frame**：`frame.pixels/stride/width/height/format = core->staging`（core 拥有的），`frame.rects = core->dirty_rects_`
3. 调 `render_frame`：host 往 staging 画 + 报 rects/count
4. **region 收脏**：把 host 报的 rects `add` 进一个 `Region`
5. 遍历 `Region.rects()` 逐个 `flush`

### `Frame::pixels`：`const void*` → `void*`

host 要用 swraster（非 const `Surface&`）往 staging 写，`const void*` 写不进。Frame 是 core↔host 的 **in-process 契约**（非跨进程 wire——跨进程的是 EventHeader/Payload），改 const 不破坏 ABI（size 不变），`abi_check` 的 static_assert 全过。flush 接收侧 `void*` → `const void*` 隐式安全。

### 不 trap：`bytes_per_pixel(fmt) == 0`

`core/host.hpp` 加 `inline constexpr bytes_per_pixel(PixelFormat)`。GuiCore 构造遇不支持的格式（返回 0）不 abort（守 DIRECTIVES「core 运行期不 abort」+ CODING-TASTE §8），而是 staging 留空、pump 开头 `staging_.pixels == nullptr` 检查 no-op。

## 决策

- **为何 GuiCore 状态化而非 pump 保持无状态**：DIRECTIVES「core **持有** staging」= 跨帧复用 = 必有状态。无状态方案要么每帧 new 全屏缓冲（贵），要么 host 先报尺寸再分配（鸡生蛋 + 新回调）。状态化一次分配、跨帧复用，且为 P2 compositor 双缓冲差分铺路。代价是 pump 签名变——但探针阶段 CinuxOS pin 旧 commit 零改动，只伤 fake_host，**越早改越便宜**。
- **为何 region 进路径（而非 P0-a 只扳所有权）**：把 region 代数挂上真实数据流（render_frame → Region → flush），P2 差分算脏可直接接上。P0-a 的 region 价值有限（disjoint rect 不合并），但数据流打通是必要前置。
- **为何 pump.hpp/cpp 合并进 gui_core**：状态化后 pump 不再是独立 stateless 函数，做成 `GuiCore::pump()` 方法最自然；独立 pump.hpp 只剩一个声明 + 哲学注释，合并减少文件数，注释迁移到 gui_core.hpp。

## 陷阱

- **`Frame::pixels` 必须可写**：`const void*` 是隐藏陷阱——host 拿到 `Frame*` 想画却写不进。in-process 契约可改 const；跨进程 wire（Event/Payload）绝不可这样改。
- **缺 `.clang-format` 文件（pre-existing debt）**：CODING-TASTE §0/§4 把 `.clang-format`（Google 基底）列为机械风格权威层级第 1，**但仓库里文件不存在**。`clang-format --dry-run` 退回 LLVM 默认风格报一堆 false violation。本批代码手动对齐现有风格（4 空格/K&R/namespace 不缩进/指针左/include 字母序）。**建议专门一批**：补 `.clang-format`（Google 基底 + §4 要点）+ 全仓 `clang-format -i`。
- **IDE clangd 诊断滞后**：改 include（pump.hpp → gui_core.hpp）后 clangd 沿旧 compile_commands.json 解析，报一片 "file not found / unknown type"。`cmake -B build` 重生成 compile_commands 后自消，非真实编译错误。

## 验证

- **standalone ctest 全绿**（Release）：6 断言——null-host 安全 / idle skip / dirty flush 几何 / **staging 所有权**（host 写 marker 进 frame->pixels → core.staging() 读到同一 marker）/ **region-on-path**（两 disjoint rect → 两次 flush）/ region 代数。
- **ASAN 干净**：`-fsanitize=address` 构建 + ctest + 直接运行均 exit 0，无 leak / 无越界。
- `abi_check` static_assert 全过（Frame 仅 const→非 const，布局 size 不变）。

## 后续

- **P0-b（🔜）**：offscreen host——真用 swraster 画极简场景（窗口 + 光标 + 文字），影子缓冲 dump PNG，事件回放驱动光标动。本批打通的所有权链路此时真正「画出像素」。
- **Debt**：补 `.clang-format` + 全仓 format（见陷阱）。
- CinuxOS 侧零改动（探针阶段 pin 旧 commit）；P2 起 Host ABI 若再变才 bump pin。
