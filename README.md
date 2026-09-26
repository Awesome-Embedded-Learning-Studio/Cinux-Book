<div align="center">

# 🐧 Cinux

### 从零手搓 x86_64 操作系统 · 中文教程 · 现代 C++

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)]()
[![CMake](https://img.shields.io/badge/CMake-supported-blue)]()
[![VitePress](https://img.shields.io/badge/docs-VitePress-42b883)]()

**🚧 正在积极建设中**

</div>

---

## 现在是什么状态

这个仓库正在建设一部新的操作系统教程:跟着真实的开发史,从零开始把系统一站一站建起来。每一章都对应一个可以 checkout 的代码标签,亲手构建、亲手验证。

- 📖 在线阅读:[Cinux 教程](https://awesome-embedded-learning-studio.github.io/Cinux-Book/)
- 🧰 主线第一站「武器库」已发布——在写第一行内核代码之前,先把错误处理、格式引擎、断言、测试框架四件工具造好

> 旧版内容——完整可跑的 v1 内核(Bootloader → GUI 桌面 → 多终端)与旧版教程——全部保留在 [`legacy/old_site`](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/tree/legacy/old_site) 分支,随时可以翻阅;完整开发历程的历史 tag 也都还在。

---

## 快速上手

主线需要 GCC ≥ 14 与 CMake ≥ 3.16:

```bash
cmake -B build
cmake --build build --target test_host -j$(nproc)
ctest --test-dir build/test --output-on-failure
```

---

## 为什么叫 Cinux

- C/C++'s Linux:再写一个基于 C/C++ 的 Linux
- CharlieChen's *nix(逃)

---

## 相关仓库

| 仓库 | 定位 |
| --- | --- |
| [Awesome-Embedded](https://github.com/Awesome-Embedded-Learning-Studio/Awesome-Embedded) | 组织总导航与项目索引 |
| [Cinux-Book](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book)(本仓) | x86_64 OS 中文教程,主线建设中 |
| [Cinux](https://github.com/Awesome-Embedded-Learning-Studio/Cinux) | 更前沿的 Cinux 开发线 |
| [PenguinLab](https://github.com/Awesome-Embedded-Learning-Studio/PenguinLab) | Linux/Embedded Linux 内核到用户态实验 |

---

## 许可证

本项目采用 [MIT License](LICENSE) 开源协议。感谢 [OSDev Wiki](https://wiki.osdev.org/) 与所有为开源社区贡献的开发者。

---

<div align="center">

**⭐ 如果这个项目对你有帮助,请给一个 Star!**

Made with ❤️ by [CharlieChen114514](https://github.com/Charliechen114514)

</div>
