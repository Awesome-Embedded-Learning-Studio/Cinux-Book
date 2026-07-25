---
title: 02 · 设计图:cwd、stat 与路径解析
---

# 设计图:cwd、stat 与路径解析

## 设计图

一次相对路径的解析,从用户态字符串到内核绝对路径:

```text
用户敲: cat hello.txt         (当前在 /etc)
  │
  ▼  path = "hello.txt" (相对)
sys_xxx(path_virt)
  ▼
resolve_user_path(path_virt, out[])            [path_util.cpp]
  │  ① validate_user_ptr(path_virt)           ← canonical 地址校验
  │  ② cwd = Scheduler::current()->cwd        ← 取本进程 cwd ("/etc")
  │  ③ path_resolve(cwd, "hello.txt", out)
  │       相对 → 拼成 "/etc/hello.txt"
  │       path_canonicalize → "/etc/hello.txt"
  ▼
out = "/etc/hello.txt"  (规范绝对路径)
  ▼
vfs_resolve(out) → fs + rel_path              [交给 003 的挂载层]
  ▼
fs->lookup(rel_path)                          [交给 005 的 ext2]
```

`path_canonicalize` 的核心是一个「栈式」的处理:逐个读路径分量,普通分量压进结果缓冲,遇到 `.` 跳过,遇到 `..` 就把最后一个分量弹掉:

```text
输入 /a/b/../c/./d//
分量序列:  a   b   ..   c   .   d
处理:      a   b   (弹 b) c   (跳) d
结果:      /a/c/d
```

## 代码路线
