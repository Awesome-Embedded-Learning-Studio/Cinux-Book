---
title: 004 · 基建加固:lockdep 与 NotNull
---

# 004 · 基建加固:lockdep 与 NotNull

> 在多核到来之前,先给并发上两道保险:lockdep 把"持着自旋锁去调度"这类死锁源变成开发期当场炸;NotNull 把"这个指针不能为空"的注释契约写进类型——外加一批看不见但扛事的构建加固。

## 本章路线

- [01 · lockdep 与 NotNull:把纪律写进类型与构建](01-lockdep-notnull.md)
- [02 · 一批"看不见但扛事"的加固,与验证](02-hardening-and-verify.md)
