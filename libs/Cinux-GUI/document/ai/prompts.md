# Codex 等价命令（粘贴式 prompt）

> Codex 无 slash 命令；复制下列整段进对话即可。Codex 会自动读 AGENTS.md。
> 本仓是 CinuxOS GUI 系统的独立仓（host-neutral core + 多 host），**与 CinuxOS 解耦**——验证走 standalone ctest，不是 `run-kernel-test`；**Host ABI 那张函数指针表（`core/host.hpp`）是唯一硬缝**。

## /resume
读 document/ai/PLAN.md 与 document/ai/DIRECTIVES.md，跑 git log --oneline -10。三行回答：①你在哪(最近✅阶段+commit) ②下一步(🔜阶段+范围,grep 定位文件,限 `core/ host/`) ③相关陷阱(OPEN GOTCHAS)。git 与 PLAN 不一致先指出。只读不改。

## /status
读 document/ai/PLAN.md，紧凑打印阶段表 P0-P7(状态/范围/commit/测试)+最近一条 git log --oneline -1。只读。

## /next [阶段-id]
读 document/ai/PLAN.md 与 document/ai/DIRECTIVES.md。为「阶段<id,留空=🔜NEXT>」产出脚手架：①范围 ②触及文件(grep 绝对路径,限 `core/ host/`;阶段不存在报错停) ③API/签名草案(Host ABI 表项 / core 接口;core host-neutral) ④完成门 standalone ctest 全绿(真 host 路径加 Probe-1 QEMU 手动冒烟) ⑤提交草案(无 Co-Auth) ⑥gotcha。停下等确认,不开改。

## /done
跑 `cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`（standalone ctest 是唯一完成门;**别把 CinuxOS 的 `run-kernel-test`/`make run` 当门**——那是另一仓）。passed 且 0 failed=绿。
- 红:列失败项,不改 PLAN/ROADMAP,不提交,给方向后停。
- 绿:①更新 PLAN.md(批标✅+commit短hash+测试数,挪 🔜;阶段完成则同步 ROADMAP 状态) ②起草提交 `<type>(<scope>): <简述>` 无 Co-Auth,不自动提交 ③写 `document/notes/<date>-<phase>-<batch>.md`(正式发布质量:背景/目标/设计/决策/陷阱/验证,非 diff 复述) ④报告改动+测试+建议 git 命令。
host 单测 push 前开 ASAN 自验（`-DCMAKE_CXX_FLAGS="-fsanitize=address"`）。真 host 路径(Probe-1 fbdev/evdev)的 QEMU 手动冒烟不进 CI,但须在笔记记录结果（QEMU 命令 `timeout` 包裹防挂死）。

## /roadmap
读 document/ai/ROADMAP.md,紧凑打印里程碑树 P0-P7 + MCU 线(状态)+ 两仓契约,指出"当前焦点之后下一个可启动的阶段"。只读。

## /milestone [阶段-id]
读 document/ai/ROADMAP.md 与 document/ai/DIRECTIVES.md。为「<id,默认下一未启动阶段>」propose:①目标范围 ②批分解(每批≈一commit+完成门 ctest) ③触及文件/子系统(grep 定位 `core/ host/`) ④与 GUI 架构不变量契合点(Host ABI 唯一缝 / core host-neutral / flush 显示模型 / region 永不欠覆盖 / 单 GUI 线程 SPSC / swraster 纯整数 / optional 归 CMake) ⑤风险/依赖。草案停下等确认;确认后写入 PLAN 并在 ROADMAP 标🔄。不开改。

## /audit
跑 git --no-pager diff --stat 与 git --no-pager diff。对照 document/ai/DIRECTIVES.md「A.架构不变量」+ CLAUDE.md 逐条查:命名/注释/无异常/no-RTTI/Host ABI 边界/core host-neutral(zero host includes)/flush 模型/region 不欠覆盖/整数绘制/optional 归 CMake/500 行限/提交格式(无Co-Auth)。列违规+定位+建议。只报告(除非要求改)。

## /preflight [目标]
读 document/ai/PLAN.md、document/ai/DIRECTIVES.md。针对「目标」做改前预审:①范围/非范围 ②触及文件与调用方(`grep`,限 `core/ host/`) ③风险等级(R0-R5) ④命中的 GUI 风险域(Host ABI 缝 / host-neutral 边界 / flush 模型 / region 覆盖 / SPSC 输入 / 整数绘制) ⑤必须守住的不变量 ⑥验证矩阵(standalone ctest + 可选 ASAN + 真 host 路径 QEMU 手动冒烟) ⑦需同步文档(PLAN/ROADMAP/notes)。若属新里程碑/跨子系统大改,停下等确认;否则给可执行批计划。

## /quality-review
跑 git status --short、git --no-pager diff --stat、git --no-pager diff；按 `document/ai/QUALITY-GATES.md` 的提交前门禁 G0-G8 逐条判 pass/fail/n/a（门禁的验证项 = standalone ctest + 可选 ASAN + clang-format;无 -smp/无 run-kernel-test）。高危 findings 先列 file:line;给最小补救建议。只报告,除非明确要求修。

## /infra-audit [维度]
**TBD：本仓尚未建 debt 登记表 / audit-guide（CinuxOS 的 `document/todo/quality/debt.md` 不适用本仓）。** 等本仓立起 `document/todo/quality/` 体系（debt.md + audit-guide.md）后再启用此 prompt。当前如需审计，按 DIRECTIVES.md「A.架构不变量」逐条人工核对即可,产出到 notes。

## /fix-debt [DEBT-NNN]
**TBD：本仓尚未建 debt 登记表（CinuxOS 的 `document/todo/quality/debt.md` 不适用本仓），无 DEBT-NNN 可引用。** 等 `document/todo/quality/debt.md` 立起后再启用此 prompt。
