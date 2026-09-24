# 前置卷(primer)· 开发计划 v2 —— 「操作系统的第一课」

> 本文件是 primer 卷的**路线图 + 写作规范**,供后续(单会话或并行)写作与审查直接取用。
> v2(2026-09-13)重写:定位从「主线的扫盲班」升级为**「操作系统的第一课」**——前置卷独立可读,是站点的门面;Cinux 主线是「理论的实践场」。朋友们希望本站成为操作系统的学习站,前置卷是入口。
> 放 `meta/` 的原因不变:不在 `document/` 下、永不被侧栏扫描。
> v1 的模块 4/5 设计被本版吸收;写作规范(§2)与参考来源(§6)沿用 v1。

---

## 0. 定位与读者(2026-09-13 用户拍板)

| 决策点 | 结论 |
|---|---|
| 读者基线 | **会 C(课程作业级)、没碰过系统编程**、用过一点 Linux 命令行;不教 C 语言本身 |
| 重写范围 | 现有 3 模块(10 章)**逐章重新打磨**(定位、结构、深度全面过一遍,不是只补弱项);新写 3 个模块 |
| 首开模块 | **06-unix(Unix 使用侧)**,其余按依赖排 |
| 与主线概念节的边界 | 前置卷讲**使用侧心智模型**(是什么/为什么/用起来什么样);主线嵌入概念节讲**实现前夜的理论**。分层不重复 |
| 目录编号 | 沿用 v1 预留号(04/05/06),**不重排**现有 01/02/03——避免破坏刚修完的链接与 sidebar |

每章遵守「概念 → 用户侧动手实验(宿主 Linux 真命令)→ 这在 Cinux 哪卷兑现」三段式;挂主线索引(如 `07-userland/004`)。

## 1. 卷结构(六模块 + 导览落地页)

```text
document/primer/
├─ index.md          导览(改写):学习路径图 / 学 OS 的三种姿势 / 怎么用这个站
├─ 01-toolchain/     现有 4 章;★逐章重新打磨:定位升为「OS 开发的武器库」
├─ 02-assembly/      现有 3 章;★逐章重新打磨(骨架保留,内容重过)
├─ 03-cpp/           现有 3 章;★逐章重新打磨
├─ 04-arch/          ★新写 3 章:计算机如何工作(吸收 v1 模块 4,起点更低)
├─ 05-os-concepts/   ★新写 1-2 章:OS 全景(吸收 v1 模块 5,升格为门面)
└─ 06-unix/          ★新写 4 章:Unix 使用侧(全新,v1 计划没有;审计最大缺口)
```

阅读顺序由 index.md 的路径图引导(不强制物理排序):新手 `04 → 05 → 01 → 02 → 03 → 06 → 主线`;只想跑起来的 `01 → 主线`。

## 2. 写作规范(沿用 v1,全部继续有效)

**标题**
- frontmatter:`title: NN · 名`(**短**,无副标题、无文件名 slug)——侧栏读这个
- H1:`# NN · 名:副标题`(可带副标题)
- `NN` 按模块重新编号(每模块从 01 起);模块 `index.md` 的 `title:` 即组名

**文件粒度**:一章一文件,~150–300 行,不堆巨石。模块目录必须有 `index.md`。

**硬约束**
1. **GAS/AT&T 为本位**(汇编模块):示例全 AT&T;NASM 笔记必须翻译后再用
2. **简短边界**:C/C++ 只讲 freestanding 内核子集;CMake 只到 OS 手搓;06-unix 只讲使用侧语义、不深入内核实现(那是主线的事)
3. **不臆造**:文件名/函数/行号/命令/输出必须真实,逐条 `grep`/`read` 过;06-unix 的动手实验必须在宿主 Linux 真跑过、贴真实输出
4. **纯概念零 Cinux 源码**:新模块正文不贴 Cinux 源码(沿用概念文章模式);只在收尾挂「这在 Cinux 哪卷兑现」跳转
5. **章首源码对齐**(2026-09-13 用户定):每章 H1 下的导语 blockquote 首行标注对齐的 commit(短哈希+日期),正文行号/行为以该 commit 为准,参考节的仓库源码链接也用 `blob/<短哈希>/` 而非 `blob/main/`。primer 不绑 tag,commit 锚就是它的可复现性来源(主线章绑 tag 不需要这条)。范本:01-toolchain/04 试用稿
6. **文风基准 = content_forge 的 writing_style.md**(2026-09-13 定):人称锁定笔者+咱们(+您),机器侧用 `~/.claude/tools/content_forge/tools/humanizer_lint.py` 扫,交稿必须 ERROR 0/WARN 逐条复核;「不是X而是Y」「兑现」「闭环」「→进正文」「」直角引号」等禁区见该档案 §11。注意与项目根 CLAUDE.md 的「用我们」冲突时以本条为准(primer 卷起)

**风格**:中文、「我们」、讲 why、保留折腾感;不是答案堆。

**侧栏机制**:别往 `document/primer/` 放非 `index` 的规划/笔记 `.md`(会被侧栏扫出来);规划放 `meta/`。

## 3. 模块设计

### 06-unix · Unix 使用侧(★首开,4 章 + index)

> 审计结论:主线 `08-filesystem/003` 裸用 fd、`09-gui/003` 直接假设懂 PTY、`14-process-advanced/001` 默认用过 sigaction——**「Unix 系统编程使用侧」是全书最大前置缺口**。本模块教读者**当 OS 的用户**,主线教**当 OS 的作者**。

- `01-shell-and-command.md` — **你敲下一行命令后发生了什么**
  shell 是个普通程序;PATH 查找、argv、退出码、`;`/`&&`;strace 第一眼(实验:`strace -f -e execve bash -c 'ls'`);对应主线 `07-userland/005`(shell)
- `02-everything-is-a-file.md` — **一切皆文件:fd 表**
  fd 是进程私有表的下标;0/1/2;open/read/write/close 最小闭环;重定向 `>` 的本质(dup2);管道 `|`;实验:写 20 行 C 用 fd 拷文件、`ls -l /proc/self/fd`;对应主线 `08-filesystem/003`(FDT)、`14-process-advanced/005`(pipe)、`17-net`(socket 同抽象)
- `03-process-and-signals.md` — **进程的一生与信号**
  fork/exec/wait 用户侧语义;僵尸为什么存在(不 wait 会怎样——实验做出来);Ctrl+C 全链路(终端→SIGINT→前台进程组);`kill -9` vs `-15`、`&`、jobs/Ctrl+Z;对应主线 `10-multitasking/001`、`14-process-advanced/001`/`003`
- `04-terminal-tty.md` — **终端:TTY、行规程与转义序列**
  终端的前世(物理设备)→TTY/PTY 主从两端;行规程(为什么 getchar 按行返回、回显是谁做的、^C/^Z/^D 是谁截的);ANSI 转义(颜色/光标);实验:`script`/`showkey`、用 C 写 5 行禁用回显;对应主线 `07-userland/007`(PTY)、`09-gui/003`(原生终端)、`03-big-kernel/008`(键盘)
- `index.md` — 模块导语 + 一张「用户侧概念 → Cinux 卷章」对照表(或并进最后一章收尾)

### 05-os-concepts · 操作系统全景(★新写,1-2 章 + index)

> v1 模块 5 原设计。一张「我们在造什么」的地图,可作全站导论。
- `01-os-big-picture.md` — 内核是什么;用户态/内核态;中断与异常;系统调用;进程与调度;内存(PMM/VMM);文件系统。每个概念挂「这在 Cinux 哪卷兑现」。
- (可选)`02-os-developer-map.md` — 学 OS 的三种姿势与这个站的用法(若 index.md 装不下再拆)。
- ♻️ 素材:`NoteBookProject/操作系统/通用概念/操作系统导论.md`(OSTEP)、《从 xv6 速通出发了解操作系统概貌》

### 04-arch · 计算机如何工作(★新写,3 章 + index)

> v1 模块 4 原设计 + 起点降低 + 新增硬件视角一节。
- `01-bare-metal.md` — **上电那一刻**:CPU 只会取指;寄存器与栈;内存是字节数组、其余都是约定;没有 OS 的计算机长什么样。(新,v1 没有——基线读者需要)
- `02-real-mode-segmentation.md` — **实模式与段机制**(v1 M4-1 原设计;📎 锚点接地后写)
- `03-protected-long-mmio.md` — **保护模式·长模式·分页预览 + CPU 之外的世界**(v1 M4-2 原设计 + **新增**:MMIO vs 端口 IO、缓存一致性与 DMA 的硬件视角预览——主线 `08-filesystem/001` FLAG_PCD、`15-smp/005` 都撞这块,原埋太晚)
- ⚠️ 写前需「锚点接地」:grep `boot/*.S`、`kernel/linker.ld` 真实行号(v1 M4-S 任务)

### 01-toolchain 模块重构(2026-09-13 定稿:按读者问题划线,不按工具划)

> 走查发现 v1 分章病:01 章文体横跳(手册↔原理课)、01↔02↔03 大面积重复(Generic/_INIT/三层骨架各讲两遍)、原理有前置倒挂只能「点到为止」——点到为止正是断裂感来源。重构为四章四问:01「我怎么把它跑起来?」(装机与首胜,纯手册)/ 02「这套工具凭什么能编内核?」(交叉编译、freestanding、flag 逐条 why、multilib 真相)/ 03「代码怎么变成裸二进制?」(保留,去从属口吻)/ 04「怎么知道它跑对了?」。

- ✅ `index.md` 重写为问题线导航
- ✅ `01-toolchain-install.md` → 装机与首胜(重写,lint 0;真跑证据:check_toolchain 输出、干净 configure 摘要、flag 注入 grep 359 处;修正旧文错误:compile_commands 的 flag 是数组格式,旧 grep 空格串匹配不到)
- ✅ `02-cmake-skeleton.md` → `02-compiler-first-lesson.md`(工具链第一课,重写,lint 0;修正漂移:flag 已不在 toolchain file 而按目标分家 kernel/mini/user 三处、mcmodel 三兄弟 large/kernel/small、大内核现在自己开 -fstack-protector-strong+global guard、-U_FORTIFY_SOURCE 退订故事、项目版本 0.1.0→1.0.0)
- ⬜ `03-targets-linker-objcopy.md` 保留骨架,做去从属口吻+lint 清洗(lint 现状 ERROR 数十)
- ✅ `04-qemu-image-test.md`(前一轮试用稿)

### 02/03 现有模块重新打磨(REW,其余 6 章逐章过)

> 打磨维度(每章都过):

1. **定位重述**:导语按新定位重写(不再「正文 001 假设你懂 X」从属口吻);收尾挂「这在 Cinux 哪卷兑现」跳转
2. **骨架对齐**:「概念 → 动手 → 挂主线」三段式;补真实验证段
3. **深度与引源**:`02-assembly/01-gas-syntax-skeleton.md` 168 行偏薄、`03-cpp` 引源
4. **文风基准**:content_forge 基准 + lint ERROR 0
5. **交叉缝合**:与 06-unix(宿主使用侧)、04-arch(体系结构视角)互挂;GAS 本位/锚点逐章核对

## 4. 任务认领表(v2)

| ID | 任务 | 依赖 | 状态 |
|----|------|------|------|
| M6-0 | `06-unix/index.md`(模块导语+对照表) | M6-1..4 初稿 | ⬜ |
| M6-1 | `06-unix/01-shell-and-command.md` | — | ⬜ **首开** |
| M6-2 | `06-unix/02-everything-is-a-file.md` | M6-1(叙事顺序) | ⬜ |
| M6-3 | `06-unix/03-process-and-signals.md` | M6-2(fd/进程) | ⬜ |
| M6-4 | `06-unix/04-terminal-tty.md` | M6-3(信号) | ⬜ |
| IDX | 改写 `primer/index.md` 为导览+学习路径图(含 04/05/06 导航) | M6 初稿 | ⬜ |
| M5-1 | `05-os-concepts/01-os-big-picture.md` + index | —(可与 M6 并行) | ⬜ |
| M4-S | 04-arch 锚点接地(grep boot/*.S、linker.ld 行号) | — | ⬜ |
| M4-1/2/3 | `04-arch/` 三章 | M4-S | ⬜ |
| REW-T1 | 01-toolchain/01 装机与首胜(问题线重构) | — | ✅ 2026-09-13,lint 0 |
| REW-T2 | 01-toolchain/02 工具链第一课(问题线重构,原 cmake-skeleton 更名) | — | ✅ 2026-09-13,lint 0 |
| REW-T3 | 01-toolchain/03 链接脚本章清洗(保留骨架,去从属口吻+lint) | — | ⬜ |
| REW-T4 | 01-toolchain/04 QEMU/镜像/测试(问题线重构) | — | ✅ 2026-09-13,lint 0 |
| REW-A1..A3 | `02-assembly/` 3 章逐章重新打磨 | 06 定稿后 | ⬜ |
| REW-C1..C3 | `03-cpp/` 3 章逐章重新打磨 | 同上 | ⬜ |
| OPT | gdb 速查章 | 低优 | ⬜ |
| FIN | 全卷终审 + `npm run build` 零死链 + 主线反向挂链(主线章导语引 primer) | 全部 | ⬜ |

并行规则:一人一章/一审查;勿并发改全局(`project.config.ts`、本文件);勿两人碰同一 `.md`。

## 5. 与主线的缝合(FIN 阶段)

- 主线相关章导语加一句「使用侧前置见 primer/06-unix/0N」(fd→08-fs/003、TTY→09-gui/003、信号→14-pa/001)
- 嵌入概念节(dcache 等)与前置卷的引用关系:前置卷收尾「跳转」,嵌入节开头「更早的使用侧铺垫见 primer」
- 站点首页入口:前置卷升到「从这里开始」(动 `site/.vitepress/config/sidebar.ts` 与首页文案,最后做)

## 6. 权威参考来源(写作时核对引用)

- **06-unix**:man7.org(`tty(4)`、`proc(5)`、`signal(7)` 概览页)、《Unix 环境高级编程》(APUE)相关章、OSTEP 虚拟化/并发卷、`console_codes(4)`(ANSI 转义)
- **05-os-concepts**:OSTEP、《操作系统导论》中译、xv6 book
- **04-arch**:Intel SDM Vol.2+Vol.3(本地 `document/reference/intel/`)、OSDev wiki、♻️ NoteBookProject 计组笔记
- **工具链/汇编/C++**:沿用 v1 清章(GCC《Using as》、ld 手册、cmake.org、OSDev「C++」等)
- 本机工具(2026-09-13 验证):内置 WebSearch/WebFetch 可用;PDF 用 pdf-reader;Intel SDM 本地始终可用
