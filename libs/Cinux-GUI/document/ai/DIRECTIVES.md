# Cinux-GUI DIRECTIVES — 架构铁律 / 约定 / 操作模型

> Tier 1（年级稳定，改动稀少）。本文是 CinuxOS `document/ai/DIRECTIVES.md` 的克隆 + 修剪版：通用规范两仓一致，内核专属项按翻译规则换成 GUI 不变量。`[待补]`=需填实。改本文件前按操作模型 L4 查牵连。

## A. 架构不变量（跨切面，所有代码适用）
- C++17，不用 C++20。**禁异常（throw/try/catch）** → 错误经返回码 / 简单错误处理（GUI 核心无 syscall、无内核 trap，当前不用 `ErrorOr`；保留"无异常"不变量）。禁 RTTI（dynamic_cast/typeid）。
- **Host ABI = 唯一硬缝**：`core/host.hpp` 那张函数指针表（`HostCore`：flush / flush_complete / poll_event / dispatch_event / render_frame / now_ms / alloc / free / log；`HostDesktop`：spawn，可选）是桌面层与 host 的唯一边界。桌面层 / core 住表**上面**，对 host 一无所知，只调表里的回调；换 host = 换表填充，核心不变。每个解引用的回调都 NULL-check。
- **core host-neutral**：`core/` 零 host include——只允许 `<cstddef> <cstdint> <cstring>`；禁任何平台头（`<SDL*>`、`<X11/...>`、`<linux/...>`、`<windows.h>`…）。host 专属代码只允许出现在 `host/`。`host/` 的对应物在 `host/` 实现并填表，不回染 `core/`。
- **flush 显示模型**：host 提供 `flush(area, pixels, stride, fmt)`（脏 rect 推送），**core 拥有一块 staging `Surface`** → swraster 把场景画进去 → region 算脏区 → 脏 rect 经 `flush` 推 host。**不是 begin_frame→pointer 模型**——core 持有 staging，host 只接收脏帧（`Frame` 脏帧契约）。idle 帧 0 写屏。
- **Region 永不欠覆盖**：`core/region.*` 容量溢出（`kMaxRects=32`）时**坍缩为包围盒**，宁可多画也不许漏画（欠覆盖 = stale-pixel bug）。这是 region 代数（intersect / union / subtract / translate）的不变量。
- **单 GUI 线程 + 输入 SPSC 队列**：core 不持锁、不 busy-wait。ISR / 其他上下文（host 输入线程、内核中断）**只 push** SPSC 队列；GUI 线程 pump **只 pop**。事件 ABI（`core/event*.hpp`）定长、版本化、跨特权稳定（`EventHeader` 8B + `PointerPayload`/`KeycodePayload`）。
- **swraster 纯整数**：`core/swraster.*` 仅 XRGB8888，Q8.8 定点 alpha（`blit_blend`），**不引浮点**。5 个纯 CPU 整数原语：fill_rect / blit / blit_blend / glyph_blit（1-bpp mask → 实色）/ draw_line（Bresenham 全象限）。
- **optional 功能归 CMake，不归源码 #ifdef**：调用点**零 #ifdef**。功能开关（如多 host、debug harness、ASAN）由 CMake 选项决定编译进哪些 TU / 链哪些 host，源码里不散落 `#ifdef CINUX_xxx`。
- 子系统架构细节见 `document/ai/`；debt 登记与审计基建见 `document/todo/quality/`（**TBD：本仓尚未建**，CinuxOS 的 `debt.md` / `audit-guide.md` 是内核仓的，P0 探针稳定后再立）。

## B. 编码 / 注释约定
详见 `CODING-TASTE.md`（单一权威：命名 / 注释 / clang-format / 测试）。要点：
- 命名：类型 `PascalCase`、函数/变量 `snake_case`、私有成员后缀 `_`(必须)、常量与枚举值 `kPascalCase`(目标，legacy UPPER_SNAKE/PascalCase 迁移中)、宏 `UPPER_SNAKE`。
- 注释一律英文（Doxygen 文件/API 头 + `//` 行内）。
- 机械风格以 `.clang-format` 为准（4 空格/K&R/100 列/namespace 不缩进/指针左），跑 clang-format 不手调。

## C. 操作模型（长期，Claude 主力开发）
- **L1 一批一commit一验证**：`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure` 全绿才提交；红则不提交、不更新 PLAN。
- **L2 提交信息** `<type>(<scope>): <中文简述>`——纯描述变更（里程碑/批归属由 PLAN.md 的 commit 列跟踪，不入 commit msg），**不带 Co-Authored-By 或任何 AI 署名 trailer**。
- **L3 propose-then-execute**：新里程碑 / 跨子系统大改，先出草案等确认；已确认的批内可自主推进。
- **L4 改前查牵连**：改任何模块或文档前，grep 引用方与依赖；ROADMAP / PLAN / `document/todo` / git 状态变更需同步，降不一致。
- **L5 验证**：本仓是 standalone 用户态库，**默认用 standalone ctest**：`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`。host 单测可开 ASAN（`-fsanitize=address`）验内存干净。**无 SMP 变体**（GUI 核心单线程；CinuxOS 的 `-smp 2` 两 leg 是内核的事，不适用本仓）。**防挂死**：ctest 有界、不启动长跑进程，**不必 `timeout` 包裹**；唯独 **Probe-1 QEMU-Linux 冒烟**（真 fbdev + evdev，启动 QEMU）需 `timeout` 包裹（参考 CinuxOS 的 `timeout 40` 节奏——QEMU 崩在死循环 / 死锁会挂死终端不返回）。**手动冒烟**：指 standalone host 跑 / Probe-1 QEMU-Linux 冒烟，**不是** CinuxOS 侧的 `make run`（那是 CinuxOS 仓的 GUI 冒烟 target，属另一仓库）。
- **L6 省 token**：命令与文档保持紧凑，不堆仪式；`CLAUDE.md` 常驻须薄，重内容按需读。
- **L7 编译并行**：所有 `cmake --build` 都带 `-j$(nproc)`（本机多核）；验证即上面那条 ctest 流程，大幅省编译时间。
- **L8 工作记录**：**每完成一批立即写一篇 `<date>-<milestone>-<batch>.md`**（不堆到里程碑末尾合并；完成一批补一批笔记——参考 CinuxOS `document/notes/2026-06-16-f1-m3-dma-{buffer,pool,prdt-builder}.md` 各一批一篇的范式）。正式发布质量：背景/目标/设计/决策/陷阱/验证。**笔记目录 = `document/notes/`**（与 CinuxOS 对齐）。
- **L9 质量门禁**：改代码前做预审（风险等级 / 风险域 / 验证矩阵）；提交前过基础审查（ctest 绿 + ASAN/UBSAN 干净 + clang-format）。质量门禁见 `document/ai/QUALITY-GATES.md`；系统性审计产出报告到 `document/todo/quality/reports/`，发现登记到 debt 表，修复一债一批闭环。**TBD：本仓尚未建 debt.md / audit-guide.md**（CinuxOS 的 `document/todo/quality/` 是内核仓的，P0 探针稳定后再立）。

## D. 子模块视角（本仓 vs CinuxOS）
- **本仓就是被 CinuxOS pin 的子模块**（`third_party/Cinux-GUI`，CinuxOS 侧 `add_subdirectory` 只出 `cinux-gui` STATIC lib）。CinuxOS 那条「子模块勿在 `kernel/` 重写」在本仓的反面表述：**本仓的核心（`core/`）是事实来源，CinuxOS 侧不重写 GUI 核心**。
- **CinuxOS 侧唯一对接点**：它的 host 适配器（`kernel/gui/host_cinux.cpp`，填 `core/host.hpp` 那张表）+ framebuffer handoff。Host ABI 若变，CinuxOS bump pin + 改这一处适配器。
- **standalone 双构建契约**：同一份 `CMakeLists.txt` 既是本仓独立项目（含 harness + ctest）也是 `add_subdirectory` 子目录（只出 `cinux-gui` STATIC lib，不进 host / harness / ctest）。改 core 公共接口后，两个构建形态都要绿。
- 探针阶段（P0/P1）：**CinuxOS 内核零改动**，活儿全在本仓 standalone 构建 + 跑。
