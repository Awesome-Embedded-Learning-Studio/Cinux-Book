---
title: 016 · ext2 独立成库——把共享 buffer 治成 SMP-safe
---

# 016 · ext2 独立成库——把共享 buffer 治成 SMP-safe

> 把 ext2 那块 4KB 共享 block_buf_ scratch 治成 SMP-safe:per-call KmBuf、双参重载、调用方迁移,再补 unlink indirect 快照、bitmap 锁、内核 parity,最后挂上 host PAL 让 TSAN 秒抓 race。

## 本章路线

- [01 · 导引:病灶与治法](01-intro.md)
- [02 · 主线一:病灶——block_buf_ 共享 scratch](02-bug.md)
- [03 · 主线二:治法 1——KmBuf RAII](03-kmbuf.md)
- [04 · 主线三:治法 2——双参 SMP-safe 重载](04-overload.md)
- [05 · 主线四:治法 3——调用方迁移](05-migration.md)
- [06 · 主线五:配套纪律 1——unlink 的 indirect 快照](06-unlink-snapshot.md)
- [07 · 主线六:配套纪律 2——block_alloc_lock_ 串行 bitmap RMW](07-bitmap-lock.md)
- [08 · 主线七:内核 parity——truncate 虚槽 + invalidate_range](08-parity.md)
- [09 · 主线八:host PAL——脱开 QEMU 上 TSAN 抓 race](09-host-pal.md)
- [10 · 主线九:ext4 extent 读路径](10-ext4-extent.md)
- [11 · 收尾:范围与边界](11-wrapup.md)
