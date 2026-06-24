# Cinux 前置卷(`primer`)写作规划与 TODO

> 状态:**规划已定,待开写**。本文件是前置卷的设计依据与执行清单,供后续写作(单会话或并行)直接取用。
> 创建于 2026-06-24。本卷不绑 git tag,独立成 volume。

---

## 0. 为什么需要前置卷

新 `document/book/` 目前**零前置内容**。第一章 [001 · 实模式引导](../book/01-boot/001-boot-real-mode.md) 开篇第一段就说"前面 `000` 只是把工具链摆好",但那个 `000` 只活在旧目录(`hands-on/000-env-toolchain-1.md` 等,见 git tag `archive/legacy-tutorials`),从未迁进新双轨书。而 001 一上来就假设读者能读 AT&T 汇编、懂段式寻址、会 CMake + 链接脚本 + objcopy——**这是最大的劝退点,也是前置卷最该补的地方。**

好在:① 旧 `000-env-toolchain` 已是 Cinux 自己的声音,迁移刷新即可;② `~/NoteBookProject/` 下有大量本人写的、风格天然匹配的现成素材(见文末素材索引)。

---

## 1. 卷定位与约束

- **目标读者**:会一点 Linux 命令行、写过简单 C/C++,但**没读过 AT&T 汇编、不懂段/分页、没用过链接脚本**的人。读完能**无障碍进入 [001](../book/01-boot/001-boot-real-mode.md)**。
- **不绑 tag(重要)**:其余卷都是 tag-bound(章节绑 git tag、靠 tag-diff 写)。前置卷讲的是横切基础知识,不属于任何 tag 的"新增",**不走 `tools/tutorial_rewrite.py` 的 context-pack 流程**。写作依据:NoteBookProject 现成笔记 + Cinux 仓库当前源码(核对真实文件名/函数)+ 外部权威(OSDev / Intel SDM,可核)。
- **风格**(沿用 CLAUDE.md):中文、"我们"、讲 why、保留折腾感;**不是答案堆**;**不臆造**文件名/命令/输出。
- **防重复铁律**:前置卷只讲"通用基础";正文 001–010 已讲透的不重写;正文用到的具体细节只给"够读懂"的最小集 + 一个"详见正文 NNN"的跳转。

---

## 2. 卷结构

注册:[project.config.ts](../../project.config.ts) 的 `volumes` 数组顶部加一行:
```ts
{ name: 'primer', srcDir: 'primer', urlPrefix: '/primer' },
```
侧栏由 [sidebar.ts](../../site/.vitepress/config/sidebar.ts) 自动扫描目录生成,无需手写。

```text
document/primer/
├── README.md            # 本文件(规划与 TODO)
├── index.md             # 卷导言 + 阅读路径图(开写时补,作为落地页)
├── 01-toolchain/        # 模块1 环境与工具链
├── 02-assembly/         # 模块2 x86 汇编速通(AT&T↔Intel 核心)
├── 03-cpp/              # 模块3 内核向 C/C++
├── 04-arch/             # 模块4 x86 体系结构与三种模式
└── 05-os-concepts/      # 模块5 操作系统前置概念
```

**建议阅读顺序**:01 工具链(先能跑)→ 04 体系结构(知道机器长啥样)→ 02 汇编(能读底层)→ 03 C/C++(能写内核)→ 05 OS 概念(知道在造什么)。模块 05 也可作前置导论放最前(备选)。

---

## 3. 逐模块规划

### 模块 1 · 环境与工具链 `01-toolchain/`
**目标**:装好工具、`cmake --build` 出 `cinux.img`、`make run` 起来,并理解每一步在干嘛。

| 小节 | 复用素材 | 备注 |
|------|---------|------|
| 1-1 工具链安装与验证脚本 | 迁移 `000-env-toolchain-1`(GCC/CMake/QEMU/clangd,为何不交叉编译) | 内容已存在,刷新即可,成本最低 |
| 1-2 CMake 构建骨架(toolchain file + 顶层 CMakeLists + QEMU 集成) | 同上 + `NoteBookProject/常用编程工具学习与环境配置/简单CMake教程.md`、`CMake教程/` 系列 | 正文用 CMake 4.x 现代写法 |
| 1-3 链接、objcopy 与磁盘镜像 | `常用编程工具学习与环境配置/静态链接和静态库实践指北.md`(VMA/LMA/重定位/查看段) + `编译系列——原理与技术/C++编译技术` | **重点**:讲清 `. = 0x7C00` 链接脚本、ELF→裸二进制、`build_image.sh` 拼扇区——001 反复依赖却没展开 |
| 1-4(可选)命令行与 GDB 速查 | `常用编程工具学习与环境配置/GDB快捷使用小记.md` + `The Missing Semester...`(shell/路径) | 速查定位,可并入 reference |

**衔接**:收尾"现在你能 `make run` 看到 QEMU 黑屏,下一站去 [001](../book/01-boot/001-boot-real-mode.md) 让它亮起来"。

### 模块 2 · x86 汇编速通 `02-assembly/` ⭐(AT&T↔Intel 对照为核心)
**目标**:读完能直接看懂 `mbr.S`/`stage2.S` 里 `movw %cs, %ax`、`ljmp $0,$real_start`、`int $0x13`。

| 篇 | 内容 | 复用素材 |
|----|------|---------|
| 2-1 寄存器、内存与寻址 | 通用/段/标志寄存器,`段<<4+偏移`,`DS:SI`/`SS:SP`,常见寻址方式 | `程序语言设计/汇编语言设计/汇编语言入门.md` + `…/系统学习汇编/基于x86_64汇编语言简单教程3`(寄存器/指令格式) + `操作系统/通用概念/操作系统学习前置.md`(x86-64 寄存器全表) |
| 2-2 控制流、栈与函数调用 | `jmp/call/ret`、栈帧、`push/pop`、转移指令原理 | `汇编语言入门.md`(转移指令/call·ret) + `基于x86_64汇编语言简单教程5`(寻址模式/mov) |
| 2-3 ⭐ **AT&T ↔ Intel 对照 & GAS 实战** | 本卷 centerpiece | 见下 |

**2-3 对照篇必须覆盖**:
- 操作数顺序:AT&T「源,目」vs Intel「目,源」
- 前缀:`$`立即数 / `%`寄存器 / 无前缀内存
- 指令后缀:`movb/movw/movl/movq` 表长度(Intel 靠 `byte/word ptr`)
- 寻址语法:`%es:0x28(%di)` ↔ `es:[di+0x28]`、`disp(base,index,scale)`
- 远跳/远调用:`ljmp $0x8000>>4,$0` ↔ `jmp 0x0800:0x0000`
- 伪指令/段:`.section/.text/.asciz/.org` vs NASM `section/db/times`
- **每个对照配一句正文真实例子**(从 `mbr.S` 取)

> ⚠️ 笔记几乎全是 NASM(Intel),正文 `.S` 是 GAS(AT&T)。**不能直接搬运**,关键例子要翻成 AT&T 并逐句核对——本模块最大工作量也是最大增值。

### 模块 3 · 内核向 C/C++ `03-cpp/`
**目标**:看懂 004 起的 C++ 内核源码,理解内核里 C/C++ 能用什么、不能用什么。

| 篇 | 内容 | 复用素材 |
|----|------|---------|
| 3-1 C 核心(内核视角) | 指针/数组退化、位运算与位域、结构体内存布局、`extern "C"`/链接可见性、C 常见陷阱 | `程序语言设计/C&C++/C语言高级特性(三剑客总结).md`(指针数组/边界/溢出/链接 声明vs定义) + `C&C++/嵌入式C语言教程` |
| 3-2 freestanding C++ 子集与内联汇编 | `-ffreestanding -nostdlib`、无异常/无 RTTI、`new` 的安置、`__attribute__`、内联汇编语法、为何内核敢用 C++ | `C&C++/嵌入式C` + `C&C++/编译和链接` + 正文 [004](../book/01-boot/004-boot-load-mini-kernel.md) 实际用的子集 |

### 模块 4 · x86 体系结构与三种模式 `04-arch/`
**目标**:在读 001–003(实/保护/长模式)前,建立"CPU 上电后长啥样、为何有三种模式"的整体图。

| 篇 | 内容 | 复用素材 |
|----|------|---------|
| 4-1 实模式与段机制 | 处理器/寄存器/指令/段寄存器由来/上电与复位/1MB 限制 | `计算机组成原理/从实模式到保护模式/从实模式到保护模式——系统追溯X86体系架构1.md`(完美对口) |
| 4-2 保护模式·长模式·分页预览 | GDT/选择子、CR0/CR4 切换、为何进 64 位、分页最小概念(细节留给正文卷 05) | `计算机组成原理/CSAPP/CSAPP.md` + `计算机架构/AMD/AMD架构简单读书笔记{1-4}`(x86-64) |

### 模块 5 · 操作系统前置概念 `05-os-concepts/`
**目标**:一张"我们在造什么"的地图——内核、特权 ring、中断/异常、内存、进程、系统调用各是什么,以及它们在 Cinux 后面哪一卷兑现。**可作全卷导论放最前。**

| 小节 | 复用素材 |
|------|---------|
| 内核是什么、用户态/内核态、特权 ring | `操作系统/通用概念/操作系统导论.md`(进程/受限直接执行/受限制的操作) |
| 中断与异常、系统调用 | `操作系统/通用概念/从xv6速通出发了解操作系统概貌.md`(系统调用/陷入/中断/驱动) |
| 进程、调度、内存、文件系统 概览 | `操作系统导论.md`(调度) + `从xv6速通…`(页表/FS/锁) |

**衔接**:每个概念后挂一句"这在 Cinux 的 [卷 NN] 兑现",形成全书导览。

---

## 4. 执行 TODO(按建议顺序)

> 并行规则(沿用 CLAUDE.md):不同会话可写不同模块;勿并发改全局文件(如 [project.config.ts](../../project.config.ts));勿让多会话写同一篇。

### P0 — 卷骨架
- [ ] 在 [project.config.ts](../../project.config.ts) `volumes` 顶部注册 `{ name: 'primer', srcDir: 'primer', urlPrefix: '/primer' }`
- [ ] 写 `document/primer/index.md`:卷导言 + 阅读路径图(谁是前置卷、怎么用、5 模块导航)
- [ ] 本地构建验证侧栏出现 `/primer/`(`npm run build` 或 dev server)

### P1 — 模块 1 工具链(零成本,先打通"能跑起来")
- [ ] 1-1 工具链安装与验证(迁移刷新 `000-env-toolchain-1`)
- [ ] 1-2 CMake 构建骨架(迁移刷新 `000-env-toolchain-1` + CMake 教程)
- [ ] 1-3 链接 / objcopy / 磁盘镜像(迁移刷新 `000-env-toolchain-2/3` + 静态链接实践指北)

### P2 — 模块 2 汇编(用户点名,增值最高)
- [ ] 2-1 寄存器、内存与寻址
- [ ] 2-2 控制流、栈与函数调用
- [ ] 2-3 ⭐ AT&T ↔ Intel 对照 & GAS 实战(从 `mbr.S`/`stage2.S` 取真实例子逐句核对)

### P3 — 模块 4 体系结构
- [ ] 4-1 实模式与段机制
- [ ] 4-2 保护模式·长模式·分页预览

### P4 — 模块 3 C/C++
- [ ] 3-1 C 核心(内核视角)
- [ ] 3-2 freestanding C++ 子集与内联汇编

### P5 — 模块 5 OS 概念
- [ ] 5-1 内核 / ring / 中断异常 / syscall / 进程内存 FS 概览(+ 全书导览跳转)

### P6 — 收尾
- [ ] 全卷过 [cinux-review-doc](#) 4 视角审查(衔接正文、无臆造、外部链接可核)
- [ ] 把旧 `000-env-toolchain-*` 在新卷上线后,评估是否从旧目录下线(等双轨稳定再动)

---

## 5. 风险与注意

- **NASM → AT&T 翻译要逐句核对**:汇编笔记不能直接搬,必须翻成 GAS 语法并与正文 `.S` 对齐。
- **别和正文重复**:前置卷是"够读懂"的最小集 + 跳转,不是把 001–010 重讲一遍。
- **外部引用可核**:Intel SDM 章节号、OSDev 链接照正文惯例标注(范例见 [001 参考](../book/01-boot/001-boot-real-mode.md))。

---

## 6. 复用素材索引(NoteBookProject 路径速查)

> 根目录 `~/NoteBookProject/`,下为相对路径。

**汇编**
- `Computer_Science/程序语言设计/汇编语言设计/汇编语言入门.md` — 寄存器/寻址/DS·SS·SP/转移/call·ret(王爽风,8086+NASM)
- `Computer_Science/程序语言设计/汇编语言设计/系统学习汇编/基于x86_64汇编语言简单教程{,2,3,4,5,6…12}.md` — 64 位补集(环境/分段/指令格式/寻址/GDB,NASM)

**C / C++**
- `Computer_Science/程序语言设计/C&C++/C语言高级特性(三剑客总结).md` — C 陷阱与缺陷 + 链接
- `Computer_Science/程序语言设计/C&C++/嵌入式C语言教程`、`…/嵌入式C`、`…/编译和链接`

**体系结构 / 组成原理**
- `Computer_Science/计算机组成原理/从实模式到保护模式/从实模式到保护模式——系统追溯X86体系架构1.md` — 实模式段机制,完美对口
- `Computer_Science/计算机组成原理/CSAPP/CSAPP.md`
- `Computer_Science/计算机架构/AMD/AMD架构简单读书笔记{1-4}.md` — x86-64

**操作系统**
- `Computer_Science/操作系统/通用概念/操作系统学习前置.md` — x86-64 寄存器全表 + GCC 工具链 + ELF + 汇编格式(迷你前置卷,骨干素材)
- `Computer_Science/操作系统/通用概念/操作系统导论.md` — 进程/受限直接执行/调度(OSTEP)
- `Computer_Science/操作系统/通用概念/从xv6速通出发了解操作系统概貌.md` — syscall/中断/页表/FS/锁

**工具链 / 构建**
- `Computer_Science/常用编程工具学习与环境配置/简单CMake教程.md`、`…/CMake教程/`、`…/CMake学习日志.md`
- `Computer_Science/常用编程工具学习与环境配置/静态链接和静态库实践指北.md` — VMA/LMA/重定位/段查看
- `Computer_Science/常用编程工具学习与环境配置/GDB快捷使用小记.md`、`…/The Missing Semester of Your CS Education By MIT.md`
- `Computer_Science/编译系列——原理与技术/C++编译技术.md`、`…/编译技术再论/高级CC++编译技术读书笔记.md`
