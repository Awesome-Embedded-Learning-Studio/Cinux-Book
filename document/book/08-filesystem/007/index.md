---
title: 007 · 工作目录与文件信息:让文件系统「认得」相对路径
---

# 007 · 工作目录与文件信息:让文件系统「认得」相对路径

> 给进程一个工作目录(cwd),给文件系统一套查文件元信息(stat)的能力,顺手把分散的路径拆分逻辑收拢成一个公共的路径解析模块。

## 本章路线

- [01 · 导引:点亮什么与为什么](01-intro.md)
- [02 · 设计图:cwd、stat 与路径解析](02-design.md)
- [03 · 实现:resolve_user_path、规范化、Task::cwd、stat](03-implementation.md)
- [04 · 设计现场:cd /.. 与 set_current 的顺序](04-design-notes.md)
- [05 · 收尾:验证与下一站](05-wrapup.md)
