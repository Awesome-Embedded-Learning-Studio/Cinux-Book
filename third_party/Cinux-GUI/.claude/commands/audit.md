---
description: 当前 diff 对照 DIRECTIVES 自检（约定+架构不变量）
allowed-tools: Bash(git diff:*), Bash(git status:*), Bash(git --no-pager diff:*), Read, Grep
---
@document/ai/DIRECTIVES.md
当前改动：!`git --no-pager diff --stat`
!`git --no-pager diff`
对照 DIRECTIVES 逐条检查：Host ABI 唯一缝 / core host-neutral(zero host includes) / no-exc no-RTTI / flush 模型 / region 不欠覆盖 / 单线程 SPSC / 纯整数 / optional 归 CMake / 500 行限 / 命名注释 / 提交格式(无 Co-Auth)。列违规+定位+建议修法。只报告，不自动改（除非要求）。
