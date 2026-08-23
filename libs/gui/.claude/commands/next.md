---
description: 脚手架化一批（默认 PLAN 的 🔜；或 $1 指定）
argument-hint: "[批-id]"
allowed-tools: Bash(grep:*), Bash(ls:*), Bash(git log:*), Read, Grep, Glob
---
@document/ai/PLAN.md
@document/ai/DIRECTIVES.md
为「批 $1」（默认 🔜 NEXT）产出脚手架：①范围 ②触及文件(grep 定位给绝对路径,限 `core/` `host/`;批不存在则报错停) ③API/签名草案（Host ABI 表项 / core 接口；core host-neutral） ④完成门(standalone ctest 全绿;真 host 路径加 Probe-1 QEMU 手动冒烟) ⑤提交草案(无 Co-Auth) ⑥相关 gotcha。停下等确认，不开改。
定位示例：!`grep -rn "flush\|render_frame\|staging" core/ host/ 2>/dev/null | head -30`
