---
title: 001 · Cinux-Base 与 ErrorOr
---

# 001 · Cinux-Base 与 ErrorOr

> 给内核的错误一个名字、一个类型(`ErrorOr`),把散在各处的公共类型收进共享库 `Cinux-Base`,为后面文件系统、多进程、网络这些大特性夯实地基。

## 本章路线

- [01 · 病在哪儿:-1 是个什么都能装的筐](01-intro.md)
- [02 · ErrorOr:让错误变成类型,并在 syscall 关口翻译](02-erroror.md)
- [03 · Cinux-Base 收公共类型,加两个真坑与验证](03-base-and-pitfalls.md)
