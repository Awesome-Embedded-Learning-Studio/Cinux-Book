# Cinux-GUI Quality Gates — AI 预审 / 审查 / 修复流程

> Tier 1.5（工程流程，稳定但可演进）。本文定义每轮代码变更前后必须回答的问题，让本地 Claude Code / Codex 能按清单预审、审查、修复，而不是只靠经验。
>
> 事实源关系：`DIRECTIVES.md` = 架构铁律；`CODING-TASTE.md` = 代码风格；`document/todo/quality/debt.md` = 债务登记（TBD，本仓尚未建）；本文 = 每轮执行门禁；`document/todo/quality/audit-guide.md` = 深度审计方法（TBD，本仓尚未建）。

## 0. 使用模式

### A. 预审（改代码前）

目标：判断这次改动属于哪些风险域，提前列出必须检查的不变量。

Claude Code 必须输出：
- **范围**：本轮要改什么，不改什么。
- **触及文件**：用 `rg`/`git grep` 找调用方、共享状态、生命周期边界。
- **风险域**：从本文第 2 节选择。
- **必须验证**：最小测试 + 额外矩阵。
- **文档同步**：是否需要 PLAN/ROADMAP/DEBT/notes/todo。

### B. 审查（提交前）

目标：确认改动没有破坏 Host ABI 缝、生命周期、渲染/输入模型、Region 覆盖和文档一致性。

Claude Code 必须输出：
- **发现**：按严重性列 file:line，先 bug 后风格。
- **门禁结果**：第 3 节逐项 pass/fail/n/a。
- **验证结果**：命令、测试数、失败项。
- **提交状态**：绿才允许建议 commit message。

### C. 专项修复（处理 `DEBT-NNN`）

目标：一条债务一批闭环，避免“顺手修一大片”造成新不确定性。

修复流程：
1. 读 `document/todo/quality/debt.md` 对应条目（本仓尚未建时先在 notes 里立条），确认位置、根因、建议。
2. `rg` 查所有引用方和相邻不变量。
3. 写小设计：owner、同步策略、错误路径、测试计划。
4. 改代码 + 测试。
5. 更新 `document/todo/quality/debt.md` 状态（建表后），写 `document/notes/<date>-<topic>.md`。
6. 绿才提交。

## 1. 风险等级

| 等级 | 判据 | 门禁强度 |
|------|------|----------|
| R0 | 文档/注释/只读说明 | 可不跑 ctest，但需说明未跑原因 |
| R1 | 局部函数、无共享状态、不碰 Host ABI | standalone ctest |
| R2 | 改公共接口、Host ABI 函数指针表、CMake | standalone ctest + 全量 build；host 改动补 host 单测（开 ASAN 自验） |
| R3 | Region 代数、swraster 像素路径、staging buffer 生命周期 | R2 + 容量溢出/覆盖率/边界测试 |
| R4 | 输入 SPSC 队列、pump 主循环、flush 协议、多 host 并存 | R2 + 输入往返 / flush 顺序验证；可开 ASAN 验证 |
| R5 | Probe-1 QEMU-Linux 真机冒烟、新 host 接入 | R2 + standalone host / Probe-1 QEMU 手动冒烟；必要时留截图/日志证据 |

## 2. 风险域映射

| 改动位置/主题 | 必查风险域 |
|---------------|------------|
| `core/host.hpp`、`HostCore`/`Host`/`HostDesktop` 结构 | 唯一硬缝：跨进程布局契约、回调 NULL 安全、函数指针表填充完整性 |
| `core/region.*` | Region 永不欠覆盖（容量溢出坍缩包围盒）、整数运算、无分配 |
| `core/swraster.*` | 纯整数 Q8.8 定点、clip 严格、无浮点泄漏 |
| `core/pump.*`、主循环、输入队列 | SPSC（ISR/其他上下文只 push，GUI 线程 pump 只 pop）、core 不持锁不 busy-wait、flush 脏 rect 顺序 |
| `core/event*`、`EventHeader`、payload | wire 布局（8B header / 18B PointerPayload / 3B KeycodePayload）、packed-ness、`abi_check.cpp` static_assert |
| `host/*`、新 host 接入 | Host ABI 表填充、flush/staging 契约、可选回调 NULL、host 单测 ASAN |
| CMake/CI/tooling | 本地/CI 对等、optional 功能归 CMake 不归源码 #ifdef、产物路径 |

## 3. 提交前门禁

### G0 上下文门

- [ ] 已读 `PLAN.md` 当前焦点和最近 git log。
- [ ] 知道本轮是否属于新里程碑/跨子系统大改；若是，先 propose。
- [ ] 未覆盖用户未提交改动；`git status --short` 已核对。

### G1 范围门

- [ ] 本轮范围能用一句话描述。
- [ ] 非目标改动没有混入。
- [ ] 公共接口改动已 grep 所有调用方。

### G2 架构铁律门

- [ ] C++17，无异常、无 RTTI。
- [ ] **Host ABI = 唯一硬缝**：core 只通过 `core/host.hpp` 的函数指针表碰 framebuffer / 输入 / 时间，不直接触达。
- [ ] **core host-neutral**：`core/` 零 host include（只 `<stdint.h>`/`<stddef.h>`），不引 SDL/X11/Wayland/内核头。
- [ ] **optional 功能归 CMake**，不归源码 `#ifdef`；调用点零 `#ifdef`。
- [ ] 不在 `core/` 重写 host 已有的渲染/输入类型。

### G3 生命周期门

- [ ] 新增 owner 时写清谁释放（core 拥 staging buffer；host 拥输入 dispatch + 渲染）。
- [ ] 共享对象有明确的释放路径；core 不持内核式 refcount/锁（GUI 核心单线程）。
- [ ] error path 与正常 path 都释放资源。
- [ ] move/copy 语义明确；禁止隐式双 owner。
- [ ] 对 staging buffer、dirty rect buffer（`Frame.rects`/`max_rects`/`count`）、Surface、host ctx 做了释放路径审查。

### G4 输入 / SPSC 同步门

- [ ] **单 GUI 线程 + 输入 SPSC 队列**：ISR 或其他上下文只 `push`，GUI 线程 pump 只 `pop`，core 不持锁、不 busy-wait。
- [ ] 新增或触碰共享可变状态时写明同步策略；普通 bool/int 不承载跨线程同步语义。
- [ ] flush 协议使用脏 rect 推送（非 begin_frame→pointer）；count==0 即 idle skip，core 不 flush。
- [ ] memory barrier / acquire-release（若 host 侧需要）必须有注释解释“为什么”。

### G5 Host ABI 边界门

- [ ] 不直接信任 host 回调的越界输入；host 填的 rect/坐标在 core 侧 clip 前按整数范围 sanity 检查。
- [ ] Host 函数指针表允许部分填充（NULL-safe）；pump 对每个 callback NULL-check 再解引用。
- [ ] 跨进程布局（`EventHeader`/`Rect`/payload/`Host`）改动后 `core/abi_check.cpp` 的 static_assert 仍过。
- [ ] 偏移、长度、宽度、计数有上限与溢出检查。

### G6 错误与崩溃门

- [ ] 可恢复错误返回返回码/简单错误处理（**无异常**不变量）；不变量破坏用 assert/fatal。
- [ ] 半初始化路径可诊断，不静默吞错。
- [ ] 致命路径不递归依赖危险状态或分配。
- [ ] 日志走 host `log` 回调，格式串不泄漏 host 专属类型。

### G7 测试门

- [ ] 基础验证**默认 `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`**（standalone ctest，单 leg，**无 -smp 变体**——GUI 核心单线程，SMP 是 CinuxOS 的事）。
- [ ] 改公共接口/Host ABI 表/host mock 相关代码时补全量 build 或 host 单测；**host 单测本地开 ASAN 自验**（`-fsanitize=address`），别只靠默认 ctest（默认 ctest 不开 ASAN，会漏判泄漏/UAF）。
- [ ] 改 Region/swraster/flush 协议时补覆盖率/边界/容量溢出相关验证。
- [ ] 新行为有测试；无法测试的路径（如真机 host 冒烟）写明人工冒烟和日志/截图证据。
- [ ] **Probe-1 QEMU-Linux 手动冒烟**（非 CI）：跑 standalone host 或 Probe-1 QEMU 看渲染/输入往返；**ctest 有界不必 timeout，但 Probe-1 QEMU 冒烟须 `timeout` 包裹**防挂死。

### G8 文档门

- [ ] 完成批次更新 `PLAN.md`；里程碑级变化同步 `ROADMAP.md`/`document/todo`。
- [ ] 修债务更新 `document/todo/quality/debt.md` 状态（本仓建表后），保留原条目和 commit 指针。
- [ ] 每批写 `document/notes/<date>-<topic>.md`，记录背景、设计、陷阱、验证。
- [ ] 新 GOTCHA 写入 PLAN 或对应 notes，防复发。

## 4. 验证矩阵

| 改动类型 | 必跑 | 条件加跑 |
|----------|------|----------|
| 文档-only | 可不跑；需说明 | markdown / 链接人工检查 |
| 普通 core 代码 | standalone ctest | 全量 build |
| 公共接口/Host ABI 表/host mock | standalone ctest | `cmake --build build -j$(nproc)`；host 单测开 ASAN |
| Region/swraster/生命周期 | standalone ctest | 容量溢出 / 覆盖率 / 边界压力 |
| 输入/SPSC/flush/pump | standalone ctest | 输入往返 / flush 顺序；开 ASAN |
| 新 host / Probe-1 | standalone ctest | standalone host 冒烟 / Probe-1 QEMU 手动冒烟（`timeout` 包裹） |

## 5. Claude Code 输出模板

### 预审模板

```text
范围：
触及文件：
风险域：
必须不变量：
验证计划：
文档同步：
停等确认/可以执行：
```

### 审查模板

```text
Findings:
- [severity] file:line — issue

Gate results:
G0 context: pass/fail/n/a
...

Verification:
- command: result

Residual risk:
```

### 修债模板

```text
Debt:
Root cause:
Design:
Files:
Tests:
Docs:
Commit message:
```

## 6. 参考基线

本流程对齐 host-neutral GUI 库的工程分层（core/host 缝、flush 显示模型、SPSC 输入、纯整数渲染），按 Cinux-GUI 当前体量裁剪：
- Host ABI 函数指针表模式（`core/host.hpp`）：跨 host 切换 = 换表填充，core 永不触达底层。
- 编译期 ABI 自检（`core/abi_check.cpp`）：`static_assert` 锁死跨进程 wire 布局，drift 在编译期即暴露。
- Region 代数不变量（`core/region.hpp`）：容量溢出坍缩包围盒，永不欠覆盖。
- 渲染管线对齐分阶段合成 + dirty-rect 脏区推送（整数 Q8.8）。
