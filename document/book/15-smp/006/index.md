---
title: 006 · SMP 竞态——从发现到根治
---

# 006 · SMP 竞态——从发现到根治

> 053 把「调度迁移写花 ctx」这个最显眼的雷扫了,可 SMP 上会写花状态的地方远不止 runqueue。这一章把 SMP 上「抓竞态」这件事拧成一条红线,贯穿四个阶段:造跨核交错报警器(race-detect)、拿 ext2 inode_cache 当靶子验证、给病灶上锁并清旧债、推导出 deferred CoW 范式。

## 本章路线

- [01 · 导引:点亮什么](01-intro.md)
- [02 · 主线一 · race-detect 基建:一台跨核交错报警器](02-race-detect.md)
- [03 · 主线二 · 拿 inode_cache 当靶子:验证报警器真能抓](03-inode-cache.md)
- [04 · 主线三 · 病灶上锁:inode_cache 套大锁,顺手清三笔旧债](04-locking.md)
- [05 · 主线四 · 纵深期:IPI shootdown 与 deferred CoW 范式](05-shootdown.md)
- [06 · 收尾:范围边界与四块地基拧成一根绳](06-wrapup.md)
