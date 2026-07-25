---
title: 003 · 给内核一个能对话的用户态:shell
---

# 003 · 给内核一个能对话的用户态:shell

> 把 `hello` 换成一个能读键盘、回显、退格、按回车收一行、就地切参、查表派发的最小 REPL shell——常驻 Ring 3,你敲 `echo hello`,它老实回你 `hello`。

## 本章路线

- [01 · 给内核一个能对话的用户态:shell](01-intro.md)
- [02 · 代码路线:REPL、tokenize、sys_read、GDT 重排与 ANSI CSI](02-implementation.md)
- [03 · 调试现场:PIT tick #GP 与命令集体失声](03-debug.md)
- [04 · 收尾:验证、下一站与参考](04-wrapup.md)
