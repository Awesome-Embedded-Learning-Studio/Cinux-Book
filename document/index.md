---
layout: home

hero:
  name: "Cinux"
  text: "64位OS教程"
  tagline: C++17 驱动，从 Bootloader 到多终端桌面的完整操作系统学习路径
  actions:
    - theme: brand
      text: 主书 · 从零读起
      link: /book/
    - theme: alt
      text: 实验册 · 动手巩固
      link: /labs/
    - theme: alt
      text: GitHub
      link: https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book

features:
  - icon: 📖
    title: 主书 · tag 绑定的主线教程
    details: 每章绑定一个 git tag、从源码提炼，从 Real Mode bootloader 一路到多终端桌面，讲清设计、代码、踩坑与验证
    link: /book/
  - icon: 🛠️
    title: 实验册 · 理解 + 手算 + 排错
    details: 不贴答案，把每个子系统拆成可推导的任务——给你输入与接口，你来算、来验证、来定位故障
    link: /labs/
  - icon: 🔍
    title: 参考 · 子系统速查表
    details: 中断、内存、进程、文件系统、存储的跨章节速查：接口、寄存器、位常量、边界，附源码索引与权威依据
    link: /reference/
  - icon: ⚡
    title: C++17 现代内核
    details: freestanding C++17，ErrorOr<T> 错误处理，concepts 约束模板，零开销抽象
    link: /book/
  - icon: 🖥️
    title: 完整系统栈
    details: 覆盖 Bootloader、内存管理、进程调度、VFS、设备驱动、GUI 到 Shell 的全栈实现
    link: /book/
  - icon: 🐛
    title: 调试笔记 · 真实排错现场
    details: 从真实开发笔记提炼的排错故事：症状 → 定位 → 根因 → 修复 → 防复发，不照抄原始笔记
    link: /debug-notes/
---
