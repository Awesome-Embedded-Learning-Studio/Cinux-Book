# Cinux-GUI — Claude Code 工作指引

CinuxOS 的 GUI 系统，独立开发。Host-neutral `core/` + 多 `host/`；`core/host.hpp` 函数指针表（Host ABI）是唯一硬缝。零 CinuxOS 依赖。P0–P7 长弧（P0=Probe-0 探针）。Claude 是长期主力开发。

## 文档分层（按耐久度，按需读）
- `document/ai/DIRECTIVES.md` — 架构铁律 + 约定 + 操作模型（年，最稳）
- `document/ai/ROADMAP.md` — 里程碑全树 P0–P7 + 状态（里程碑级）
- `document/ai/PLAN.md` — 当前焦点批级进度（批级，最易变）
- `document/ai/CODING-TASTE.md` — 编码/注释风格权威（写代码前读）
- `document/notes/` — 工作记录（正式发布文档，`<date>-<milestone>-<batch>.md`；**完成一批立即补一篇**，不堆到里程碑末尾，参考既有 notes 风格）

## 始终遵守（每条便宜，违规代价大）
- **无异常**：C++17，`no exception / no RTTI`；错误用返回码/简单错误处理（GUI 核心无 syscall、无 trap、当前不用 ErrorOr）。
- **验证默认用 standalone ctest**：`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`（绿才提交）。host 单测 push 前开 ASAN 自验：`-DCMAKE_CXX_FLAGS="-fsanitize=address" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"`（本地默认 ctest 不开 ASAN 会漏判）。Probe-1 真 fbdev 路径用 QEMU-Linux 手动冒烟（非 CI，结果记笔记；`timeout` 包裹）。
- **本仓与 CinuxOS 解耦**：开发都在本仓；CinuxOS 经 submodule pin 追踪。GUI 改动**不动 CinuxOS 内核**。
- 一批一 commit 一验证；ctest 绿才提交。
- 提交信息 `<type>(<scope>): <中文简述>`（纯描述变更；里程碑归属看 PLAN.md，不入 msg），**不带 Co-Authored-By / AI 署名**。
- 改前查牵连（grep 引用方，限 `core/` `host/`），同步 ROADMAP↔PLAN↔git。
- 新里程碑/跨子系统大改：propose-then-execute。
- 写代码遵循 `document/ai/CODING-TASTE.md`：标识符+注释一律英文、私有成员 `_`、常量/枚举值 `k`前缀（目标）、提交前跑 clang-format。
- **架构不变量铁律**（详见 DIRECTIVES）：① Host ABI 是唯一硬缝（`core/host.hpp`），core 对 host 一无所知——**zero host includes**，只 `cstdint`/`cstddef`；换 host = 换表填充，core 不变。② **flush 显示模型**：host 提供 `flush(area, pixels, stride, fmt)`，core 拥 staging buffer 按脏 rect 推；非 `begin_frame→pointer`。③ **Region 永不欠覆盖**：容量溢出坍缩为包围盒（过覆盖=性能成本，欠覆盖=stale-pixel bug）。④ **单 GUI 线程 + 输入 SPSC 队列**：ISR/其他上下文只 push，GUI 线程 pump 只 pop，core 不持锁、不 busy-wait。⑤ swraster **纯整数**（Q8.8 定点，不引浮点）。⑥ **optional 功能归 CMake，不归源码 `#ifdef`**（调用点零 #ifdef）。

## 命令（`.claude/commands/`）
战术：`/resume` `/status` `/next [批]` `/done`
战略：`/roadmap` `/milestone [M]` `/audit`

## 回到仓库
`/resume`（读 `document/ai/PLAN.md` + git log）。
