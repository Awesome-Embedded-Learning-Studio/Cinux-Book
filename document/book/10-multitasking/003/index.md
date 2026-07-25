---
title: 003 · 多终端:每个终端一个独立的 shell
---

# 003 · 多终端:每个终端一个独立的 shell

> 把通电后的 fork/execve 接进 GUI:点终端图标 → fork 出子进程 → execve 成 `/bin/sh` → 每个终端背后一个独立的 shell(私有 pipe + 私有 fd 表),互不串扰。

## 本章路线

- [01 · 导引:点亮什么、为什么、设计图](01-intro.md)
- [02 · 代码路线:create_shell_terminal / 私有 pipe / 私有 FDTable / 收尸 / gui_worker / 不要 sti](02-implementation.md)
- [03 · 收尾:验证 + 没做的 + 下一站 + 参考](03-wrapup.md)
