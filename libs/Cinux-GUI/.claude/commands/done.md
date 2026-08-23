---
description: 验证当前批 — standalone ctest 绿则更新 PLAN 并起草提交(无 Co-Auth)，红则只报
allowed-tools: Bash(cmake:*), Bash(ctest:*), Bash(git:*), Edit
---
@document/ai/DIRECTIVES.md
跑唯一验证（首次/改 CMake 先 `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`）：
!`cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`
判定：末尾全 passed、0 failed = 绿。（**别把 CinuxOS 的 run-kernel-test / make run 当门——那是另一仓。**）
- 红：列失败项，不改 PLAN、不提交，给方向后停。
- 绿：① 更新 `document/ai/PLAN.md`（批标✅+commit 短 hash；挪 NEXT；阶段完成则同步 ROADMAP 状态） ② 起草提交 `<type>(<scope>): <简述>`（纯描述，里程碑归属看 PLAN），**不带 Co-Authored-By/AI 署名**，不自动提交 ③ **写 `document/notes/<date>-<milestone>-<batch>.md`**（正式发布质量：背景/目标/设计/决策/陷阱/验证，参考 `document/notes/` 既有风格；非 diff 复述） ④ 报告改动+测试+建议 git 命令。
真 host 路径(Probe-1 fbdev/evdev)的 QEMU 手动冒烟不进 CI，但须在笔记记录结果。
