---
title: 012 · ext2 间接块:把 double-indirect 真做了
---

# 012 · ext2 间接块:把 double-indirect 真做了

> 兑现 065 的承诺:把 ext2 的 double-indirect(i_block[13])真做了,撤掉块大小 workaround,让 822 KB 文件真能在 1024 块的 ext2 上完整读写。

## 本章路线

- [01 · ext2 间接块:double-indirect 真做](01-indirect.md)
