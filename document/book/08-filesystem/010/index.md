---
title: 010 · DevFS:用一个虚拟文件系统把设备挂进 /dev
---

# 010 · DevFS:用一个虚拟文件系统把设备挂进 /dev

> 立一个纯内存的虚拟文件系统 DevFS,把设备行为包成一种特殊的 inode(device inode),挂在 /dev 下——ls /dev 见设备,读写触发设备行为。

## 本章路线

- [01 · DevFS:把设备挂进 /dev](01-devfs.md)
