---
description: 为下一阶段拆批 — propose 草案等确认（roadmap→plan 桥）
argument-hint: "[阶段-id，默认 ROADMAP 下一未启动]"
allowed-tools: Bash(grep:*), Bash(ls:*), Read, Grep, Glob
---
@document/ai/ROADMAP.md
@document/ai/DIRECTIVES.md
为「$1」（默认下一未启动阶段）propose：①目标与范围 ②批分解（每批≈一 commit + 完成门 ctest） ③触及文件（grep 定位 `core/` `host/`） ④与 DIRECTIVES 架构不变量的契合点（Host ABI 唯一缝 / host-neutral core / flush 模型 / region 不欠覆盖 / 单线程 SPSC / 纯整数 / #ifdef→CMake） ⑤风险/依赖。
草案停下等确认；确认后再写入 PLAN.md 并在 ROADMAP 标该阶段 🔄。不直接开改。
