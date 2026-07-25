---
title: 013 · ProcFS 的 TOCTOU:锁内拷值,指针不逃出锁
---

# 013 · ProcFS 的 TOCTOU:锁内拷值,指针不逃出锁

> 兑现 011 留的债:把 ProcFS read 的「锁内拿指针、锁外解引用」TOCTOU 改成「锁内拷 snapshot,锁外用 snapshot」——比 RCU 轻,但根因比当年以为的更严重。

## 本章路线

- [01 · ProcFS 的 TOCTOU:锁内拷值,指针不逃出锁](01-toctou-fix.md)
