# CinuxOS → Cinux-Book 回迁调研报告(终稿)

> 调研范围:只读比对 `/home/charliechen/Cinux`(Cinux-Book,教学线,根 commit `10eb3e8a`)与 `/home/charliechen/CinuxOS`(CinuxOS,开发线 v1.0.0,根 commit `cde30d1`)。两仓 git 历史独立、对象库互不相通(`git fetch` 实测互不可达),只能内容级手工搬运。本报告对作者晚上回来拍板"怎么开干"负责。
>
> **终稿说明**:本稿基于三视角对抗审查(技术可行性 / 完备性 / 教学价值)修订。修订原则是吸收所有合理 gaps、降过乐观断言、修分期与量级,把 must_ask_user 收拢进「风险与开放问题」段并给选项 + 推荐。修订面较大的条目用 **【v2 改】** 标注,新增的硬核实用条目用 **【新增】** 标注。本稿所有数字、文件路径、机制陈述均经实测复核。

---

## 0. 先纠简报与初稿里会误导分期的几条事实

调研起步前先把简报与初稿里几条与实测不符的事实钉死,否则分期会基于错误前提:

1. **CinuxOS 不是 43 tag,也没有线性 tag 序**。实测 `git tag --list` 在 CinuxOS 只有 `v1.0.0-rc1` 与 `v1.0.0` 两个。所谓"715 提交/43 tag"是 Cinux-Book 的口径错位记到了 CinuxOS 头上。CinuxOS 的特性演进记录在 `document/notes/`(309 篇 HEAD,308 篇 v1.0.0)+ `document/todo/f<N>-*/` 立项文档 + `document/ai/PLAN.md`(293KB 批级日志)里。**这直接决定了"回迁不能用 tag→tag diff 驱动,要用弧+里程碑驱动"**。

2. **【v2 改】两仓不共享根 commit**。Cinux-Book 根是 `10eb3e8a Initial commit`,CinuxOS 根是 `cde30d1`。简报说"共享根 cde30d1"与"对象库不通 → 不能 cherry-pick/merge"的结论并存——后者**实测仍成立**(证据不是"根不同",而是 `git fetch` 互不可达),但根 commit 确实不同(可能是 CinuxOS 重写过根,或 fork 后 rebase)。回迁上不影响(都只能手工搬)。

3. **Cinux-Book 没有 `035b`、`033b` 子 tag**。实测 43 个 git tag 里子 tag 只有 `004_{A,B,C}`、`027b`、`028{b,c,d,e}`。`035b-multi-terminal.md` 与 `033b-gui-lazy-terminal.md` 是 book 章节,不是 git tag。续编号应沿用 `028b/c/d/e` 风格(主号 + 字母后缀无下划线)。

4. **【v2 改】`document/document/{kernel,user}` 实测 34 个文件,在 v1.0.0 与 HEAD 都是 34**——简报"34 篇"是对的。它**不是 flat 文件**,kernel 下按 arch/drivers/fs/gui/ipc/mm/proc + lib/mini/syscall/test 分目录,user 下只有 libc/shell。初稿曾以 HEAD 结构反驳简报的"34"是自相矛盾,本稿撤销:简报"34"正确,目录结构也正确。关键定性不变:**这批文档对应 Cinux-Book 旧 tag 体系**(ext2 读写、ramdisk、PS/2 keyboard/mouse、PIO AHCI、wm/canvas/terminal/desktop_icon 全在),**对"035 之后新特性"零贡献**,盘点不应计入回迁原料。

5. **【v2 改·重要】Cinux-Book 的 `document/todo/` 在 git 历史里可恢复**。实测 commit `f32c51c`(2026-05-05)曾提交了一份完整的 F1-F13 phase 路线图(`document/todo/f4-smp/{00-acpi,01-apic,02-ap-boot,03-smp-sched,04-smp-sync}.md` 等),后于 commit `9ceefff`(2026-05-25)被一次性删除。这意味着:**CLAUDE.md 引用的脚手架不是"从未提交、已丢",而是"曾提交后被有意删除,git 历史里 `git checkout f32c51c -- document/todo/` 一条命令即可恢复"**。这是初稿最大的误判之一,直接影响 Stage 0 的性质与量级(见 §4.5、§6 Stage 0)。

6. **【v2 改】Cinux-Book 035..HEAD 不是 10 个 commit,是 42 个**;且不全是 docs/refactor/deploy——含 `f32c51c add flat feature-domain roadmap`、`e41673c Optimize/phase0a refactor`、`344f3cf Optimize tutorials for release`、`a3b4de4 migrate to default compiler compilable`、`fd7bbca chore fix: gcc 14` 等实质性结构 commit。教学线"冻在 035"的结论仍成立(tag 没新增),但"035 之后只是文档维护"的叙事偏简化。

7. **【v2 改】CinuxOS v1.0.0 → HEAD 漂移极小**:notes 308→309(只多 `2026-07-16-nvme-async-io.md`),kernel 文件 597 不变,SYS_ 115 不变。**结论:非 NVMe 弧的回迁直接用 HEAD 也基本安全,锚定 v1.0.0 的纪律主要约束 F5 NVMe 弧**。

---

## 1. 执行摘要

**是什么**:把 CinuxOS v1.0.0(2026-07-16 发版)在 Cinux-Book 教学 tag `035_multi_terminal` 之后长出来的特性——SMP 多核、网络全栈、NVMe/VirtIO/xHCI 驱动、musl 动态链接、TTY/PTY、安全机制、GCC 自举——以及配套的 dev notes / todo 立项 / PLAN-ROADMAP-changelog,作为教学原料回迁到 Cinux-Book,延续其 tag-bound 教学线。

**【v2 改·量级】实测基准**(初稿数字多处偏大,本稿全部用实测值重估):

| 项 | 初稿值 | 实测值 | 备注 |
|---|---|---|---|
| Cinux-Book book 内容篇 | 46 | **45**(9 内容卷 + index) | 卷号 04 刻意留空,`document/book/index.md` 自述"九卷" |
| Cinux-Book labs | 44 | **43** | |
| Cinux-Book primer 内容篇 | 14 | **10**(另 4 个 index) | 04-arch/05-os-concepts 未写 |
| Cinux-Book debug-notes | 17 | **18**(含 index) | |
| Cinux-Book reference | 5 | **7**(含 index 与 intel SDM) | |
| Cinux-Book kernel 源文件 | 242 | **230**(HEAD,tag 035 时点更少) | |
| Cinux-Book SYS_ | 22 | **21** | |
| Cinux-Book kernel/gui 行数 | 2698 | **2898**(HEAD) | |
| Cinux-Book CI job 数 | 4 | **5**(format/host-tests/line-limits/docs/kernel-tests) | |
| CinuxOS v1.0.0 kernel 文件 | 597 | **597** | 初稿值偶然正确 |
| CinuxOS v1.0.0 SYS_ | 113 | **115** | |

**量级估算**:全弧回迁约等于把 Cinux-Book 从 **9 卷 45 篇 book / 43 lab / 10 primer / 7 reference** 扩到约 **15-16 卷 88-95 篇 book / 75-80 lab / 25 reference**,book 体量约翻倍。与 CinuxOS kernel 230→597(2.6×)、SYS_ 21→115(5.5×)的扩张幅度大致匹配。

**【v2 改】关键结论**:

1. **dev notes / todo / PLAN-ROADMAP 是回迁的顶层骨架,但不是"近原生改写"的富矿**。309 篇 notes 的**长度分布实测**: <50 行 **127 篇(41%)**、50-99 行 **157 篇(51%)**、100-149 行 19 篇、**150+ 行仅 6 篇**。真正能给 book 喂教学叙事(背景/设计/踩坑/GOTCHA 齐)的"金矿 note"约 **25 篇(<10%)**,绝大多数是 propose-log(决策/实现/验证/下一步四段,无 Why 教学化裁剪)。**PLAN.md(293KB)+ ROADMAP.md(30KB)+ A-rewrite-handoff.md(3.3KB)是顶层骨架**,但 PLAN 是"批级工程日志"不是"教学叙事",改写仍需补 Why + 教学化重构。**因此:每个 Stage 的真实工作量应在"弧本体代码量"基础上 +30-50% 用于 propose-log 教学化**,初稿的"9 个 A 级弧"口径过乐观,本稿降为"9 个弧原料齐全但需重构,其中 F4/F5/F7/F9-SMAP 等 4-5 个弧有金矿 note 可作叙事锚点"。

2. **回迁不是"搬代码",是"重讲发展弧"**。两仓 git 历史独立、对象库不通(`git fetch` 互不可达),不能 cherry-pick/merge/rebase;且 CinuxOS 还在 `feat/nvme-async-io` 往前跑,v1.0.0 快照会过时(v1.0.0→HEAD 漂移极小,但 NVMe 弧已在改)。教学上要把每个弧重新按 tag 序铺开,用 dev notes 当调试叙事。

3. **最大教学差异化在两处:Cinux-Book 完全空白的 SMP(F4)与网络(F7)**。这两弧概念密度最高、CinuxOS 原料最厚(F4 25 note + 40K todo 含 f-verify-m3-smp-wake,F7 15 note 按协议层递进),回迁后能让 Cinux-Book 从"单核教学 OS"跨进"现代化教学 OS"梯队。

4. **【v2 改】CLAUDE.md 脚手架不是"从零重建",而是"从 git 历史恢复 + 决定迁移到 meta/"**。`document/todo/` F1-F13 路线图在 `f32c51c` 提交过,`git checkout f32c51c -- document/todo/` 一条命令即可恢复。Stage 0 的真正工作是:(a) 决定恢复还是重建;(b) 决定放 `meta/` 还是 `document/todo/`(后者会被 sidebar.ts 扫,需加排除规则);(c) 按恢复的里程碑重写弧驱动 context-pack 工具。初稿"从零写工具 + 1-2 天"低估也高估:若选恢复可能 0.5 天,若选重建有里程碑 md 作蓝本也比从零快。

5. **【v2 改·最关键】分期最大盲点:F3 信号(sigreturn 栈注入)与 F9 NXE 是反向耦合**。F3-M1 note GOTCHA#10 明写:"handler 返回地址指向栈上 `int $0x80`(cd 80 + nop,8B 槽)。依赖栈页可执行——**NXE 未启用故可行。F9 启用 NXE 时栈不可执行,trampoline 失效,必须迁 vdso / 独立可执行页**"。初稿把 F3 排 Stage 1、F9 排 Stage 7,暗示 F3 先做——但 F9 一回迁,Stage 1 写的 sigreturn 实现与章节就要返工。F10 musl 动态链接会带 vdso 机制,vdso 正好是 F9 NXE 启用后 F3 sigreturn 的解法。**三个弧(F3/F9/F10)的 sigreturn 实现会互相牵引**,这是分期必须显式处理的开放问题(见 §7 决策点 2)。

**推荐首期(已修订)**:初稿推"F3 信号三部曲破冰"——**实测不支持**:F3-M1 note 仅 53 行,且明确标注"Custom handler 真用户态 round-trip 留后续",即 M1 实际只交付 Default/Ignore 路径 + 内核投递架构,读者最想看的"我注册 sigaction 捕获 SIGSEGV 程序继续跑"在 M1 没做完。**本稿改为推荐"F9-M1 安全(NX/SMEP/SMAP 机制回读)破冰"**:独立性最高(只需 CPUID/CR4/EFER)、note 最现成(240 行金矿)、读者可见感最强(跑一道 CPUID 看到 SMEP 真开了)、教学差异化最高("怎么证明硬件保护真生效"方法论)。详细论证见 §6 Quick Win 与 §7 决策点 1。

---

## 2. 两仓血缘与现状

### 2.1 血缘

- **Cinux-Book** = `/home/charliechen/Cinux`,远端 `Awesome-Embedded-Learning-Studio/Cinux-Book`,稳定教学版。根 commit `10eb3e8a`,43 个 git tag,教学线收于 `035_multi_terminal`(commit `094feef`),HEAD 在 035 之后有 **42 个 commit**(实测,非初稿的 10),含 `f32c51c add flat feature-domain roadmap`、`e41673c phase0a refactor`、`344f3cf optimize tutorials for release`、`a3b4de4 migrate to default compiler`、`fd7bbca gcc 14` 等结构性 commit,**但没有新教学 tag**——教学线确实冻在 035,只是"035 之后纯维护"的叙事偏简化。
- **CinuxOS** = `/home/charliechen/CinuxOS`,远端 `Awesome-Embedded-Learning-Studio/Cinux`,前沿开发线。根 commit `cde30d1`,只有 `v1.0.0-rc1`/`v1.0.0` 两个 tag,2026-07-16 发 v1.0.0 正式,当前在 `feat/nvme-async-io` 往前跑。
- **两仓对象库互不通**——`git fetch` 实测互不可达(这才是不能 cherry-pick/merge/rebase 的真正证据,与"根 commit 是否相同"无关)。**只能内容级手工搬运**。

### 2.2 Cinux-Book 现状(tag 035 收口)

- **kernel 230 源文件**(HEAD,tag 035 时点更少),21 syscall,无 SMP/网络/NVMe/USB/安全/GCC 自举。grep 实测全 0 命中:`kernel/{smp,net}`、`kernel/drivers/{nvme,usb,acpi,apic,virtio}`、`kernel/drivers/tty` 全无。
- **drivers**:`{ahci, pci, pit, keyboard, serial, video}` + 顶层 `mouse.cpp`(PS/2 三字节版)+ `canvas.cpp/hpp`。无 IBlockDevice、无 DmaPool、无 MSI-X。
- **fs**:ext2(读写但通过 `Ext2(AHCI&, port_index)` 直挂)+ ramdisk + vfs_mount,**无 DevFS/tmpfs/ProcFS、无独立块设备抽象层**。`kernel/fs/ext2.hpp` 直持 `AHCI&`——任何第二块盘都要改 Ext2 构造。
- **ipc**:仅 `pipe*`(自管 char buffer,sti/hlt 自旋,无 O_NONBLOCK/无 SIGPIPE,因为根本没有信号子系统)。
- **proc**:单一全局 `g_per_cpu`(单核调度),`Scheduler::current_` 静态全局;per_cpu.hpp 只有 current/kernel_stack/gs_page_vaddr 三个字段。无 clone、无 futex、无 signal、无 setpgid。**【v2 改】021-proc-sync 已经主动埋了信号债**(原文"拿自旋锁进信号 handler 是另一套麻烦""async-signal-safe 本章没这个保证")——但 019-021 全程是协作式内核线程,0 ring3、0 syscall 信号路径、0 clone、0 futex。**F3-M1 信号要往 021 之上接,不是"扩充 06-process"是"新建一整个子系统"**(信号帧/sigreturn trampoline/中断路径投递),初稿"扩 06-process 为 06b"低估了跨度。
- **gui**:kernel/gui/ **2898 行**(HEAD,非初稿的 2698)全 ring0(window/wm/terminal/desktop_icon 都编进 big_kernel),无 /dev/fb0、无 /dev/event0、无 PTY、无 mmap。
- **【v2 改】教学产出**(实测):
  - book **9 内容卷 45 篇** + index(01-boot 4 / 02-mini-kernel 4 / 03-big-kernel 8 / 05-memory 4 / 06-process 3 / 07-userland 3 / 08-filesystem 9 / 09-gui 7 / 10-multitasking 3,卷号 04 刻意留空,`document/book/index.md` 自述"九卷")。
  - labs **43**(卷结构与 book 对齐)。
  - primer **10 内容篇** + 4 个 index(01-toolchain 4 + 02-assembly 3 + 03-cpp 3,**04-arch / 05-os-concepts 未写**,index 已诚实声明)。
  - debug-notes **18**(含 index)、reference **7**(含 index 与 intel SDM 子项)、notes 50。
- **构建**:VitePress 1.6.4 + 自研 `scripts/build.ts`(分卷并发,已治 OOM)。sidebar.ts 扫 `document/` 下 `.md` 自动入侧栏——**规划/交接文档绝对不能放 document/,只能放 meta/**(或放 document/ 下被 sidebar 排除的子目录)。
- **【v2 改·重要】CLAUDE.md 引用的脚手架状态**: `TUTORIAL_REWRITE_DESIGN.md` / `CLAUDE_CODE_HANDOFF.md` / `tools/tutorial_rewrite.py` / `meta/rewrite/` / `document/arc/` 在当前 HEAD 全不存在(实测)。**但 `document/todo/` 在 `f32c51c` 曾完整存在(F1-F13 phase 路线图,与 CinuxOS `document/todo/` 同名同结构),`9ceefff` 删除,git 历史可 `git checkout f32c51c -- document/todo/` 恢复**。`meta/` 当前只有 `primer-dev-plan.md`。
- **【v2 改】CI**:5 个 ubuntu-latest job(format/host-tests/line-limits/docs/kernel-tests),无自定义 action。
- **【v2 改】未丢失的写作契约不止 4 个**:`document/ai_prompts/` 实测有 **8 个文件 + templates/ 子目录**(CLAUDE.md.template / code_conventions / prompt_generate_tutorial / **prompt_maintain_project** / prompt_review_tutorial / **prompt_understand_project** / **prompt_write_module** / README / writing_style)。重建 writing-contract.md 时这些都应引用。

### 2.3 CinuxOS v1.0.0 现状

- **kernel 597 源文件**(v1.0.0,HEAD 585——feat/nvme-async-io 重构略减),**115 SYS_**(`kernel/syscall/` 下 grep `SYS_` 唯一号,非初稿的 113)。
- **驱动 10**:AHCI DMA(已升级)/ NVMe / VirtIO-blk/net / xHCI USB HID / e1000 / HPET / RTC / 键鼠(已拆 PS/2 与 USB)。
- **【v2 改】文件系统 5,但 ext2 位置变了**:`kernel/fs/` 实测只有 `{dentry, devfs, file, file_lock, inode, path, procfs, ramdisk, stat, tmpfs, vfs_*}`,**ext2 不在 kernel/fs/,而在 `libs/ext2/`(ext2.hpp/ext2_block/ext2_common/ext2_directory/ext2_dirops/ext2_extent/ext2_init/ext2_inode)**,ctor 已是 `Ext2(IBlockDevice* dev)`(非初稿描述的 `Ext2(AHCI&)`,那是 Cinux-Book 的形态)。`libs/ext2/ext2_common.cpp` 强依赖 `kernel/mm/page_cache.hpp`(is_page_cacheable + g_page_cache.invalidate_range)。**这印证 F6↔F2(PageCache)与 F6↔F5(IBlockDevice)双重强耦合**,初稿 §6 Stage 3 只标了 F2 没标 F5,是分期硬伤(见 §6 Stage 3 修订)。
- **SMP**:`-smp 2` 双核 online(per-CPU + IPI + trampoline + 多核调度 + lockdep)。`kernel/drivers/{acpi,apic}` + `kernel/arch/x86_64/smp.hpp` 全在。
- **TCP/IP 全栈**:`kernel/net/`(ethernet/ARP/IP/ICMP/UDP/TCP/Socket)+ 真 `ping 10.0.2.2` + Ctrl+C。
- **【v2 改】安全(NXE/SMEP/SMAP/ASLR/UID-GID/Canary)并非"独立性高"**:F9-SMAP 推倒重做的根因(`document/notes/2026-06-28-smap-m0-accessor-p0a-p0e-foundation.md`)**明写**:"RFLAGS.AC 是 per-CPU 位,而 `context_switch.S` 不存 RFLAGS。`stac` 之后、任务跨 CPU 迁移后 AC 位丢失,-smp 2 shell /hello 触发 SMAP #PF"。**完整可用的 SMAP accessor 需要 SMP 的 RFLAGS 保存纪律**——F9 与 F4 SMP 是反向耦合,不是"独立"。初稿矩阵把 F9 标 medium/medium 独立性、把 F9 放 Stage 7(SMP Stage 4 之后)顺序是对的,但"独立性高"标签会误导。
- **musl 动态链接 + GCC 自举**:cc1→as→ld→./hello 闭环,默认 PIE,busybox init PID1。
- **GUI 用户态化**:Cinux-GUI 独立 submodule + 薄 host adapter,/dev/fb0 mmap + /dev/event0 + PTY 多终端。`kernel/gui` 收敛到 **372 行**(不含 `data/icon_data.hpp`;含 icon 数据时 646 行——回迁 F13 时 icon 数据归属是隐性决策点)。
- **【v2 改】submodule 2 个**(实测 `.gitmodules`):
  - `third_party/Cinux-Base`(url = `github.com/CinuxOS/Cinux-Base`,**5373 行**,非"小")。ErrorOr/expected 基础设施。
  - `third_party/Cinux-GUI`(url = `github.com/Awesome-Embedded-Learning-Studio/Cinux-GUI`,**9090 行**,非初稿的 7249)。host-neutral GUI 核心。**【v2 改·决策利好】Cinux-GUI 远端与 Cinux-Book 远端同组织(Awesome-Embedded-Learning-Studio)**——权限/归属无障碍,F13 若决定回迁,submodule 引入是组织内操作,不是跨组织协作。
- **【v2 改】教学原料体量与质量**:
  - `document/notes/` **309 篇** HEAD / 308 篇 v1.0.0。**长度分布**:<50 行 127 篇(41%)、50-99 行 157 篇(51%)、100-149 行 19 篇、**150+ 行仅 6 篇**。**真正"详级教学金矿"约 25 篇(<10%)**。
  - `document/todo/` 16 个弧立项目录(f1-f13 + f-eco/f-gui-userspace/f-usability + quality)。
  - **【v2 改·新增】`document/ai/` 实测 8 个文件**(初稿只列 PLAN/DEVLOG/ROADMAP):PLAN.md(293KB 批级日志,极大教学富矿)、ROADMAP.md(30KB 弧级里程碑树,天然教学大纲骨架)、**A-rewrite-handoff.md(3.3KB,正在进行的 fork child-setup A 重写交接 prompt,直接揭示 SMP+fork+ring3+gcc-13+ubsan 组合下 CI 5/6 绿、Release+ubsan deferred #GP 未根治——这是把"回迁最难的坑"写进 handoff 的现成素材)**、**CODING-TASTE.md(13KB)**、**DIRECTIVES.md(5.6KB)**、**QUALITY-GATES.md(8.2KB,含双 leg 绿/docker gcc-13 复现验证纪律)**、prompts.md(3.4KB)、DEVLOG.md(几乎空)。
  - `document/changelogs/v1.0.0.md`:用户视角特性清单,适合做新卷"导览/总览"章。
  - `tools/{gcc-toolchain,musl}/` + `rootfs/{buildroot,overlay}/`:工具链与 rootfs。
  - `document/document/{kernel,user}/`:**对应 Cinux-Book 旧 tag 体系**(实测 34 个文件,ext2/ramdisk/PS2/PIO AHCI/wm 全在),对 035 之后新特性零贡献。
- **【v2 改】CI**:5 job(host-tests 2 cell asan/tsan + line-limits + kernel-tests 6 cell + busybox-smoke + gcc-smoke),有自定义 `.github/actions/setup-cinux/` composite action(musl/busybox/buildroot 三级 cache),**无文档构建**。**回迁带 SMAP accessor/futex/fast-path 的弧需要同步扩 host-tests**(`scripts/check_uaccess_boundaries.sh` / `check_freestanding_headers.py` 这些约束脚本在 host-tests 跑),初稿 §4.6 只讲 kernel matrix 扩展漏估。
- **已 defer 的**:ext4 写/journal、UEFI(F11)、epoll、TCP 重传/RTO/窗口/拥塞、NVMe 偶发 LBA 越界(已兜底 SIGBUS/SIGILL)。**【v2 改·新增】F10 fork 在 v1.0.0 仍有已知 bug**:A-rewrite-handoff.md 揭示 `-smp2 + ring-3 + gcc-13 + ubsan` 下首个 /hello fork 子处 #GP(vector 13),根因是 fork child-setup 的 current_rbp 捕获在 gcc-13 帧布局下算错,CI 5/6(Release+ubsan deferred)。**这直接影响 Stage 5 F10 的回迁质量基线**(见 §7 决策点 6)。

---

## 3. 回迁总范围矩阵

下表覆盖 11 个候选弧。代码/教学复杂度、推荐结论来自弧级评估,均已对源码与文档核实。**【v2 改】复杂度列加了"跨弧耦合"标注,把反向依赖显式化**。

| 弧 | CinuxOS 有(v1.0.0) | Cinux-Book 缺(tag 035) | 代码复杂度 | 教学复杂度 | 硬前置 + 反向耦合 | 建议 tag(036+) | 建议卷 | 原料就绪 | 推荐 |
|---|---|---|---|---|---|---|---|---|---|
| **F4 SMP** | ACPI+APIC+per-CPU+IPI+trampoline+多核调度+lockdep,-smp2 真 online | 全无 | very-high | very-high | 无前置;**反向被 F9-SMAP 依赖(RFLAGS 保存)** | 048-053(6 主 tag:acpi/apic/percpu/trampoline/smp-sched/lockdep) | **新建卷 11-smp** | A(25 note,但仅 ~6 详级;m3-smp-wake 是金矿) | **port-later** |
| **F5 驱动扩展** | AHCI DMA + VirtIO-blk/net + NVMe + xHCI + e1000 + HPET/RTC + DmaPool + MSI-X + InputEventDevice/TTY/PS2 拆分 | 仅 PIO AHCI + 单 PRDT 轮询 + 顶层 PS/2 mouse;028c/lab-030 留了时间戳/双光标债 | very-high | high | **F1 基建(IBlockDevice/DmaPool)+ F4 IRQ(MSI-X/IST2)**;E1000/VirtIO-net 单搬是死代码,需 F7;**F6 ext2 已强依赖 IBlockDevice,F5 子集是 F6 前置** | 061-063(dma-pool+IBlockDevice/hpet-rtc/msix/virtio-blk/nvme/xhci-hid/input/tty) | **新建卷 14-drivers** | A(44 note,弧内最厚) | **port-later** |
| **F8 IPC 扩展** | Pipe 增强(阻塞+SIGPIPE)+ FIFO + Unix Socket + SysV shm + poll/select | 仅 sti/hlt 自旋 pipe;无 errno/信号/FIFO/socket/shm/poll | very-high | high | **F9 信号(SIGPIPE/EINTR)**;F8-M3 Unix Socket 需 F7 Socket 基座;F8-M4 shm 需 F2 mmap/ASLR | 064-065(fifo-unixsocket/shm-poll) | **新建卷 11-ipc** | A(9 note + 28K todo) | **port-later** |
| **F9 安全** | NXE/SMEP/SMAP(机制回读)/ASLR/UID-GID/Stack Canary | 全无;`__stack_chk_fail` stub 但 CMake 用 `-fno-stack-protector`;无 access_ok/uaccess/extable | **medium→high** | medium | 只需 CPUID/CR4/EFER;**【v2 改】反向耦合:F3 sigreturn 栈注入在 NXE 启用后失效需迁 vdso;F9-SMAP 完整版需 SMP RFLAGS 保存纪律;F-EXTABLE 必须同 F9-SMAP 同迁(否则 SMAP accessor 的 -EFAULT 容错契约无法兑现只能 panic)** | 059-060(nxe-smep-smap-vdso/uaccess+extable/aslr/credentials/canary) | **新建卷 13-security** | **A(M1 NX/SMAP 金矿 240 行;M3 SMAP-SMP 耦合需重构)** | **port-now(M1 破冰首选)** |
| **F10 用户态运行时** | musl 静态 + ELF 动态(PT_INTERP/interp/auxv)+ TTY 行规范 + PTY | execve 把 argv/envp 丢弃跳空栈;SYS_chdir=12 撞 SYS_brk=12;无 TTY/PTY/ioctl/mmap | very-high | very-high | F2(brk/mmap)+ F3(clone/setpgid/signal)+ F6(DevFS)+ F9(creds)+ **vdso(F9 NXE 启用后 F3 sigreturn 的解法)**——"F10 + 4 个半弧";**【v2 改】v1.0.0 fork 在 -smp2+ring3+gcc13+ubsan 仍有 #GP 未根治(A-rewrite-handoff)** | 052-054(linux-abi/initial-stack-auxv/musl-static/elf-dynamic/tty/pty) | 扩 07-userland 或新建 11-userland-runtime | A(21 note + 36K todo) | **port-later(等 fork A-rewrite 进 stable)** |
| **F12 开发者生态** | 腿 A:KALLSYMS+frame pointer+panic backtrace;腿 B:GCC 自举(cc1→as→ld→./hello 默认 PIE) | panic 只有 dump_registers+fatal_halt;无 kallsyms/backtrace/GCC 自举 | 腿 A low / 腿 B very-high | 腿 A medium / 腿 B very-high | 腿 A 无前置(实质是 F-INFRA-5 的复刻);腿 B 隐式依赖 F10(PT_GNU_STACK)+ F5-mm(file CoW+mapcount)+ F9(PIE)+ syscall 补全 | 腿 A:036_kernel_kallsyms;腿 B 不建议本轮 | 腿 A 挂 03-big-kernel 或填 04-developer | **【v2 改】腿 A 实质是 F-INFRA-5 复刻;腿 B 有 10 篇专门 note(gcc-b3b/gcc-b4a/gcc-line-finale/syscall-gcc-selfhost 等)分类在 f-usability 下** | **port-now(只腿 A)** |
| **F13 GUI 用户态化** | host-neutral core + 薄 host adapter + /dev/fb0 mmap + /dev/event0 + PTY 多终端 | kernel/gui 2898 行全 ring0;0 用户态 syscall/0 mmap/0 PTY/0 userland | very-high | very-high | F2-mmap(IoPhys)+ F8-poll + F10-ring3 + F-ECO(ioctl/setsid/dup/fcntl)+ DevFS + PTY——**依赖链最长** | 036-042(若本轮做) | 扩 09-gui | A(19 note + 64K todo) | **defer-to-v1.1(决策利好:Cinux-GUI 与 Cinux-Book 同组织,submodule 引入无障碍)** |

**11 弧之外,F2/F3/F6 的"深化"(扩现有卷)** 也是回迁范围,放在分期里单独成 Stage:

| 弧 | CinuxOS 有 | Cinux-Book 缺 | 复杂度 | 建议 tag | 卷 | 推荐 |
|---|---|---|---|---|---|---|
| **F3 进程深化** | 信号/clone/futex/TLS/进程组/waitpid 阻塞/调度类 | 021 只讲基础锁,无信号/clone/futex/pgrp | medium→**high** | 036-039 | 扩 06 或新建 06b | **port-now(M1 信号破冰,但 sigreturn 形态要先定,见决策点 2)** |
| **F2 内存深化** | VMA/mmap/brk/PageCache/Demand Paging/Buddy/Slab | 05-memory 停在 address space | medium→high | 040-043 | 扩 05 或 05b | **port-later(F3 之后)** |
| **F6 FS 升级** | VFS 增强+mount+Dentry+flock/ProcFS/DevFS/tmpfs/**ext2 独立库(libs/ext2)** | 08-filesystem 停在 028 ext2 读 | medium→**high** | 044-047 | 扩 08 或 08b | **port-later(F2 之后,且 F5 IBlockDevice 子集必须前移)** |

---

## 4. 工程机制

### 4.1 内容级手工搬运是唯一通道

两仓 git 历史独立、对象库不通(`git fetch` 实测互不可达),不能 cherry-pick、不能 merge、不能 rebase。回迁的每一个文件都是:**读 CinuxOS 当前 HEAD 源码 → 内容级拷到 Cinux-Book 对应位置 → 调整命名空间/路径/依赖 → 真机验证**。

**【v2 改】锚定纪律修订**:CinuxOS 还在 `feat/nvme-async-io` 往前跑,但 v1.0.0 → HEAD 实测只多 1 篇 note、kernel 文件 597 不变、SYS_ 115 不变。**结论:非 NVMe 弧的回迁直接用 HEAD 也基本安全,锚定 v1.0.0 的纪律主要约束 F5 NVMe 弧**(避免把 async IO 半成品搬进教学)。

### 4.2 【v2 改·重要·机制说反已纠正】构建:CMake toolchain 不能照搬,但论证链要重写

初稿 §4.2 把 toolchain init 的方向说反了。**实测真相**:

- **Cinux-Book** `cmake/toolchain-x86_64.cmake`(init 段):**灌了** `-ffreestanding -fno-stack-protector -mno-red-zone -mcmodel=kernel`。
- **CinuxOS** `cmake/toolchain-x86_64.cmake`(init 段):只设 `CMAKE_CXX_STANDARD 17` + ASM/link flags,**没灌 freestanding**。

**结论"不能照搬 toolchain"仍然成立**,但论证链完全相反:不是"CinuxOS 假设工具链只编内核灌 freestanding",而是 **Cinux-Book 自己的工具链 init 就灌了 kernel 模式标志,CinuxOS 反而把 freestanding 下放到 per-target**。回迁时若要照搬 CinuxOS 的 per-target 风格(更干净),需要先把 Cinux-Book toolchain init 的 freestanding 标志剥掉、下放到 `kernel/CMakeLists.txt` 的 per-target——这是一次 toolchain 重构,不是简单复制。

**【v2 改】风险警告也要降级**:初稿"搬 toolchain 会让 Cinux-Book user/(small model)和 host unit test 错套 `-mcmodel=kernel`"实测**夸大**——Cinux-Book `user/CMakeLists.txt` 用 `USER_COMPILE_FLAGS` 含 `-mcmodel=small` 做 `target_compile_options PRIVATE`,per-target 优先级高于 toolchain init,实际不会错套(它自己的 toolchain 就灌了 kernel,user 一直好好的)。真正的风险是 host unit test 若没显式 per-target flags 会继承 toolchain init 的 `-mcmodel=kernel`,但这是 Cinux-Book 现状已存在的问题,不是回迁引入的。

### 4.3 构建:GCC 门禁的两仓一致(口径修正)

简报与 CinuxOS README badge 都把 GCC 16.1.1+ 当硬要求。**实测 CinuxOS 根 CMakeLists 硬门禁是 `GCC >= 11`**,16.1.1 只是 badge 营销。Cinux-Book 根 CMakeLists **完全没有 GCC 版本门禁**,但 commit `fd7bbca chore fix: gcc 14` 显示 Cinux-Book 自己在 gcc 14+ 上工作。**两仓工具链底线大致一致(Ubuntu 24.04 默认 gcc 即可起步),但"底线一致"是未完全核实的乐观断言**——回迁带 musl 的弧(F10)要复现 CinuxOS 的 docker gcc-13 验证纪律(QUALITY-GATES.md),本机 gcc-16 ≠ gcc-13 不能代表 CI。

### 4.4 【v2 改】子模块:Cinux-Base 与 Cinux-GUI

CinuxOS `.gitmodules` 有两个 submodule,Cinux-Book 一个都没有。**【v2 改】行数实测**:

- **Cinux-Base**(`github.com/CinuxOS/Cinux-Base`,**5373 行**,非初稿的"小"):ErrorOr/expected 基础设施。**【v2 改·决策点 2 升级】引入会带来 ErrorOr/expected 风格,会让 Cinux-Book 现有错误处理代码风格分裂**——三选一:(a) 全仓改用 ErrorOr(工作量大);(b) 新卷用 ErrorOr、旧卷不改(风格割裂);(c) 不引 Cinux-Base 自己重写错误处理(搬运成本上升)。初稿把这个取舍轻描淡写了。
- **Cinux-GUI**(`github.com/Awesome-Embedded-Learning-Studio/Cinux-GUI`,**9090 行**,非初稿的 7249):F13 GUI 用户态化才需要。**【v2 改·决策利好】远端与 Cinux-Book 同组织**,F13 若决定回迁,submodule 引入是组织内操作,权限/归属无障碍。引入会改变 Cinux-Book 仓库形态(`.gitmodules` + `third_party/` + `--recursive` clone 流程)。**教学仓一般倾向避免 submodule,但同组织内 + 已有 v1.0.0 stable 成果(PR#72 合 main)让这个取舍比初稿评估的更可考虑**。

### 4.5 【v2 改·重要】CLAUDE.md 脚手架重建(Stage 0,性质重判)

CLAUDE.md 引用的 5 处脚手架在当前 HEAD 全不存在:`TUTORIAL_REWRITE_DESIGN.md`、`CLAUDE_CODE_HANDOFF.md`、`tools/tutorial_rewrite.py`、`meta/rewrite/`、`document/arc/`。`meta/` 当前只有 `primer-dev-plan.md`。

**【v2 改·关键纠正】脚手架不是"从未提交、已丢",而是"曾提交后被有意删除,git 历史可恢复"**:
- `document/todo/` F1-F13 phase 路线图在 commit `f32c51c`(2026-05-05)曾完整存在(每个域 00/01/02/03/04 里程碑 md,如 `document/todo/f4-smp/{00-acpi,01-apic,02-ap-boot,03-smp-sched,04-smp-sync}.md`),与 CinuxOS `document/todo/` 同名同结构。
- commit `9ceefff`(2026-05-25)一次性删除全部 `document/todo/`。
- **`git checkout f32c51c -- document/todo/` 一条命令即可恢复**。

删除机制印证了 sidebar.ts 扫描陷阱:`document/` 下任何 .md 都会被当页面扫,规划/交接文档要么放 meta/、要么删掉、要么放被排除的子目录。上一轮的路线图走了第二条路且没按 tag 重建到 meta/。

**【v2 改】未丢失的写作契约远不止 writing_style.md**:`document/ai_prompts/` 实测 8 个文件(见 §2.2)。**【新增】CinuxOS `document/ai/` 还有 CODING-TASTE.md(13KB)、DIRECTIVES.md(5.6KB)、QUALITY-GATES.md(8.2KB)、A-rewrite-handoff.md(3.3KB)**——这些是 2026-07 新文档,QUALITY-GATES 含双 leg 绿/docker gcc-13 复现验证纪律,**是回迁质量的硬约束来源**,应与 writing_style.md 合并作为"写作契约"。

**【v2 改】最小重建清单修订**:

| 项 | 现状 | 重建 | 最小形态 / 工作量 |
|---|---|---|---|
| `document/todo/` F1-F13 路线图 | `9ceefff` 删除,git 可恢复 | **从 `git checkout f32c51c -- document/todo/` 恢复,决定放 meta/ 还是 document/todo/(后者需 sidebar 排除规则)** | 0.5 天(恢复)或 1 天(恢复 + 迁 meta/) |
| `tools/tutorial_rewrite.py` | 无 | **是**(单文件 Python,弧/里程碑驱动,3 子命令 matrix/pack/pack-all) | 1 天(有恢复的里程碑 md 作蓝本) |
| `meta/rewrite/writing-contract.md` | 无 | **是**(可勾选清单,引用 `document/ai_prompts/writing_style.md` + CinuxOS `document/ai/{QUALITY-GATES,DIRECTIVES,CODING-TASTE}.md`) | 0.5 天 |
| `meta/rewrite/README.md` | 无 | **是**(目录用途 + 工具用法) | 0.3 天 |
| `meta/rewrite/context-packs/<arc>/` | 无 | 按需生成(pack 工具产出,从 dev notes + todo + PLAN 批段 + Cinux-Book 对应卷 diff 聚合) | 工具就绪后每弧 1-2 小时 |
| `TUTORIAL_REWRITE_DESIGN.md` / `CLAUDE_CODE_HANDOFF.md` | 无 | **否**(并入 `meta/rewrite/README.md`) | — |
| `document/arc/` | 无 | **否**(本轮直接进 book 新卷) | — |
| CLAUDE.md | 引用一堆不存在路径 | **需修订**(删不存在引用,或恢复后更新) | 0.2 天 |

**Stage 0 总工作量**:选恢复路径 1.5-2 天,选从零重建路径 2-3 天(比初稿的"1-2 天"略长,因要决定迁移位置 + 改 sidebar 排除规则)。

### 4.6 【v2 改】CI 扩展建议(含 host-tests 双轨)

Cinux-Book 当前 CI **5 job**(format/host-tests/line-limits/docs/kernel-tests),无自定义 action;CinuxOS 5 job + setup-cinux action(musl/busybox/buildroot 三级 cache)+ 6 cell kernel matrix。

**【v2 改·新增】host-tests 双轨漏估**:CinuxOS CI 有 host unit test(host g++ 编 freestanding)与 kernel-test(QEMU 双 leg)两套不同 harness。回迁带 SMAP accessor/futex/fast-path 的弧需要同步扩 host-tests(`scripts/check_uaccess_boundaries.sh` / `check_freestanding_headers.py` 等约束脚本在 host-tests 跑)。初稿只讲 kernel matrix 扩展漏估这一块。

**建议增量引入(按弧,不要一次性全上)**:

1. **setup-cinux action 引入**:把 5 个 job 的 apt+cmake 样板抽出来,加 ccache cache。最高性价比基建——SMP/buildroot/gcc-smoke 进 CI 后构建时间会暴涨,没 ccache 不可接受。
2. **musl 弧**:setup-cinux 开 `cache-musl: 'true'`;kernel-tests 加 `tools/musl/build-musl.sh`。
3. **SMP 弧**:`run-kernel-test` 升级为 `run-kernel-test-all`(单核 + `-smp 2` 双腿);**SMP 不跑双核等于没测**。
4. **安全弧**:kernel-tests 改 2 cell matrix(加 `-DCINUX_UBSAN=ON`)。
5. **lockdep 弧**:再加 `-DCINUX_LOCKDEP=ON` cell。
6. **busybox/gcc smoke**:独立 job,不进 kernel matrix(避免矩阵爆炸)。
7. **KALLSYMS**:所有 build 步骤改双构建。
8. **【新增】host-tests 约束脚本**:回迁 F9/F3/F2 时同步把 `check_uaccess_boundaries.sh` / `check_freestanding_headers.py` / `check_net_decoupling.sh` 引入 host-tests。

**矩阵控制**:Cinux-Book 教学仓不建议一上来 6 cell,分阶段——先 2 cell,SMP 回迁后升双腿,安全弧回迁后再加 ubsan/lockdep。`docs` job 与 kernel CI 必须保持独立(Cinux-Book 现状),不要让 kernel matrix 失败阻塞文档部署。

### 4.7 【v2 改】tag 续编号规则(修订字母位冲突)

实测 Cinux-Book 43 个 tag 的命名规律:

- 主线:三位数字 + 下划线 + 主题域 + 下划线 + 细分(如 `015_mm_pmm`、`027_fs_vfs`、`034_process_fork_exec`)。
- 子 tag:**主号 + 字母后缀**(无下划线,如 `028b_fs_ext2_write`、`027b_fs_vfs_syscall`),只在 `004_{A,B,C}` 用了大写 A/B/C。
- 已用子 tag 字母位:b/c/d/e(028 已占满 4 位)。

**【v2 改】续编号建议修订**(初稿"每弧预留 4-5 字母位"与 028 已用 b/c/d/e 自相矛盾):

1. **长弧直接用主号区间,不挤字母位**:SMP(F4)这种 6-8 tag 的长弧用 048-053 六个主号,不是 048+048b..048f。字母位只用于"单里程碑内部分批交付"(如 028 写 ext2 时分 b/c/d/e 四批)。
2. **风格一致**:三位数字 + 主题域前缀(尽量复用已用前缀:`mm`/`proc`/`fs`/`driver`/`gui`/`security`/`smp`/`net`/`userland`/`ipc`)。
3. **跨弧依赖用前置 tag 显式标注**:如 GUI 用户态化依赖 musl,musl 弧 tag 必须排在 GUI 弧之前。
4. **跨弧共享前置用独立 tag**:如 IBlockDevice 被 F5/F6 共用,单独开一个 `036_driver_iblock_device` 主 tag,不挤进任一弧。

具体编号表见 §6 各 Stage。

---

## 5. 【v2 改】教学原料就绪度热力图(口径重定)

**【v2 改·口径修订】**:notes 按"日期后第一段"分弧;**质量分级**改为按"详级 note 数 + propose-log 数"双口径:详=可近原生改写(背景/决策/代码/验证/GOTCHA 齐全)、中=需补 Why 与教学化裁剪、略=propose-log 四段(决策/实现/验证/下一步,无 Why 叙事)。**就绪度** A=有金矿 note 可作叙事锚点 + 其余 propose-log 可重构、B=原料够但需大量补 Why、C=原料薄需新写。

### 5.1 主弧

| 弧 | notes 总数 | 详级 note 数 | todo 规模 | 质量 | 就绪度 | Cinux-Book 现状 | 增量性质 |
|---|---|---|---|---|---|---|---|
| **F1 内核基础设施** | 8 | ~2 | 52K | 中 | A | 无 04 卷 | 新增地基卷 |
| **F2 内存管理** | 8 | ~3 | 52K | 详 | A | 05-memory 卷(015-018) | 扩充(VMA/mmap/PageCache/Buddy/Slab) |
| **F3 进程与线程** | 17 | ~4 | 36K | 详(M1 note 仅 53 行) | A | 06-process 卷(019-021) | 扩充(信号/clone/futex/pgrp/调度类) |
| **F4 SMP 多核** | 25 | ~6(m1-m5 多 propose-log) | 40K | 详 | A | 完全无 | **⭐ 最大新增卷**,m3-smp-wake 是金矿 |
| **F5 设备驱动** | 44 | ~8 | 48K | 详 | A | 完全无 | **⭐ 新增驱动卷**,弧内原料最厚 |
| **F6 VFS/FS** | 9 | ~3 | 44K | 中-详 | A | 08-filesystem 卷 | 扩充(ProcFS/DevFS/tmpfs/ext2 独立库/mount) |
| **F7 网络** | 15 | ~4 | 36K | 详 | A | 完全无 | **⭐ 新增网络卷**,按协议层递进 |
| **F8 IPC** | 9 | ~2 | 28K | 详 | A | 08/fs 含 pipe | 扩充(FIFO/UnixSocket/shm/poll) |
| **F9 安全** | 10(SMAP 系列连看 14) | ~3(M1 240 行金矿) | 24K | 中-详 | A | 完全无 | **⭐ 新增安全卷**,机制回读叙事极强 |
| **F10 用户态运行时** | 21 | ~5 | 36K | 详 | A(但 fork #GP 未根治) | 07-userland 卷(022-024) | 大幅扩充(musl/动态链接/TTY/PTY) |
| **F11 平台/UEFI** | 0 | 0 | 16K | — | C | 无 | **跳过**(已 defer) |
| **F12 GCC 自举** | 10(分类在 f-usability/f-eco) | ~2 | 28K | 中 | B(散但可 grep 聚合) | 无 | 腿 A=F-INFRA-5 复刻;腿 B 需从 f-usability+f-eco+ROADMAP 反构 |
| **F13 GUI 分离** | 19 | ~5 | 64K | 详 | A | 09-gui 卷 | 扩充(submodule/host adapter/用户态化) |

### 5.2 【v2 改】横切里程碑(计数修正 + 落点修正)

| 横切弧 | notes 数 | 质量 | 教学价值 | 建议落点 |
|---|---|---|---|---|
| **FO 可观测性** | 1 | 中 | 中 | debug-notes(frame pointer/KALLSYMS/panic) |
| **【v2 改】F-INFRA 基建** | **14**(非 12) | 中-详 | **高**(lockdep/UBSAN/kprintf-format 是 F4/F9 隐性硬依赖,非"可选 reference") | **reference + F12 腿 A 实质是 F-INFRA-5 复刻** |
| **F-QA 质量收敛** | 7 | 中 | 中 | reference(测试矩阵方法论) |
| **【v2 改】F-CLN 债务清理** | **9**(非 8) | 略-中 | 低 | debug-notes 挑 DEBT-008/010 |
| **【v2 改】F-VERIFY 动态验证** | 4 | 详 | 高 | **testing/labs + Stage 4 SMP trampoline 章必读 `f-verify-m3-smp-wake.md`(初稿埋在横切表)** |
| **F-EXTABLE** | 4 | 中-详 | 中 | **reference + 必须同 F9-SMAP 同迁(隐性依赖)** |
| **【v2 改】F-ECO 用户生态** | **8**(非 6,含 busybox-acceptance) | 详 | 高 | **labs**(busybox 试金石挖内核 bug) |
| **【v2 改】F-USABILITY** | **9**(非 8) | 中-详 | 高 | **F12 GCC 自举的实际承载弧** |
| **F-GUI-USERSPACE** | 4 | 详 | 高 | F13 用户态化实操 |
| **F-DYN-COV** | 3 | 中 | 高 | debug-notes/testing(TSAN 抓 ext2 race) |

### 5.3 【v2 改】顶层叙事骨架(增列)

| 文档 | 行数 | 教学价值 | 用途 |
|---|---|---|---|
| `document/ai/PLAN.md` | 293KB(初稿误为 1520 行) | **极高** | 单文件最大教学富矿,每弧每批的背景/方法/决策/GOTCHA/验证全记——**是"第一手为什么"来源,但需教学化裁剪,不是直接可用** |
| `document/ai/ROADMAP.md` | 30KB(初稿误为 95 行) | **极高** | 弧级里程碑表 + 状态 + GOTCHA 编号,**天然教学大纲骨架** |
| `document/ai/A-rewrite-handoff.md` | 3.3KB | **极高** | **F10 fork #GP 未根治的现成交接 prompt,把"回迁最难的坑"写进去——Stage 5 F10 必读** |
| `document/ai/QUALITY-GATES.md` | 8.2KB | 高 | 双 leg 绿/docker gcc-13 复现验证纪律,**回迁质量硬约束** |
| `document/ai/{CODING-TASTE,DIRECTIVES,prompts}.md` | 22KB | 中-高 | 代码品味 + 指令 + prompt 模板,写作契约补充 |
| `document/changelogs/v1.0.0.md` | 85 行 | 高 | 用户视角特性清单,适合做新卷"导览/总览"章 |
| `document/document/{kernel,user}/` | 34 文件 | **零**(对应旧 tag) | 对 035 之后新特性无贡献,只作对照基准 |

### 5.4 【v2 改】结论(降乐观)

- **9 个弧原料齐全(F2/F3/F4/F5/F6/F7/F8/F9/F10/F13),但"近原生改写"口径过乐观**:309 篇 notes 中**真正详级教学金矿约 25 篇(<10%)**,其余多为 propose-log 需补 Why + 教学化重构。**每 Stage 真实工作量应在弧本体代码量基础上 +30-50%**。
- **PLAN.md(293KB)+ ROADMAP.md(30KB)+ A-rewrite-handoff.md + changelog 构成顶层叙事骨架**,弥补单篇 propose-log 偏 dev 日志的碎片感。
- **唯一明显缺口是 F11(已 defer,不该补)和 F12 腿 B(原料散落在 f-usability/f-eco 下,但 git log 一条 grep 即可全捞 10 篇,聚合成本被初稿夸大)**。
- **`document/document/{kernel,user}/` 对新特性回迁零贡献**,盘点时不应计入新原料。
- **原料侧无阻塞,瓶颈在 Cinux-Book 侧的卷结构规划、脚手架恢复、propose-log 教学化重构**。

---

## 6. 推荐分期

总原则:**依赖最低、教学杠杆最高、dev note 最现成的先做;概念密度最高的押后;CinuxOS 自己 defer 的一律不回迁;反向耦合(F3↔F9↔F10 sigreturn/vdso)必须显式处理**。

### Stage 0 — 工具链与脚手架恢复/重建(前置,非可选)

- **【v2 改】内容**:**首选** `git checkout f32c51c -- document/todo/` 恢复 F1-F13 phase 路线图(每弧已有 00/01/02/03 里程碑 md,与 CinuxOS 同名同结构)→ 决定放 `meta/`(不污染 sidebar)还是 `document/todo/`(加 sidebar 排除规则)→ 重建 `tools/tutorial_rewrite.py` 的**弧/里程碑驱动**版(CinuxOS 无线性 tag)→ 在 `meta/rewrite/context-packs/<arc>/` 下为每个弧生成 context pack → 修订 CLAUDE.md 引用。
- **理由**:CLAUDE.md 明确"Do not write tutorial content directly from current working tree files""use context pack"。没有工具,后面每个 stage 都会违反纪律。
- **【v2 改】量级**:选恢复路径 1.5-2 天,选从零重建 2-3 天(初稿 1-2 天偏乐观或偏悲观,取决于是否知道 document/todo 可恢复)。
- **必须先做**:是。

### Stage 1 — 【v2 改】安全弧 M1(NX/SMEP/SMAP 机制回读)破冰

- **弧**:F9-M1(NX/SMEP/SMAP,CPUID/CR4/EFER 机制回读,**不带 SMP RFLAGS 耦合的纯单核版**)。
- **【v2 改】理由(破冰从 F3 信号改为 F9 安全)**:
  - **独立性最高**:只需 CPUID/CR4/EFER,**无任何前置弧、无反向耦合**(F9-SMAP 的 SMP RFLAGS 耦合只在 SMP 启用时触发,M1 单核版不受影响)。
  - **note 最现成且详**:`2026-06-28-smap-m0-accessor-p0a-p0e-foundation.md` **240 行金矿**(初稿推的 F3-M1 note 仅 53 行,且 Custom handler round-trip 留后续)。
  - **读者可见感最强**:跑一道 `cpuid` 看到 SMEP 真开了、写个用户态程序触发 SMAP #PF 看内核优雅返回 -EFAULT——比"内核能投递 Default 终止信号"可见感强一个量级。
  - **教学差异化最高**:"怎么证明硬件保护真生效"的方法论高光,区别于"教科书说设了就设了"。
  - **叙事锚点**:019 中断(#PF)+ 021 同步 → 036 NX/SMEP/SMAP,自然承接。
- **量级**:约 0.4 个卷(2-3 book + 1-2 lab + 1 reference)。
- **tag**:036_nx_smep_smap。
- **卷**:**新建 13-security**(首章破冰)。

### Stage 2 — 进程弧深化(F3 信号 + clone/futex/TLS + 进程组 + 调度类)

- **弧**:F3-M1(信号,**sigreturn 形态按决策点 2 一次定稿**)、M2(clone/futex/TLS)、M3(进程组/waitpid 阻塞)、M4(优先级调度类)。
- **【v2 改】理由修订**:依赖 F1 基建(Cinux-Book 019-021 已覆盖);教学杠杆高(futex 的 userspace fast path 是教学高光);**但 F3-M1 信号不是"独立破冰"——sigreturn trampoline 形态依赖 NXE 是否启用(决策点 2)**。若 Stage 1 已做 F9-M1 NXE,则 F3-M1 必须直接按 NXE-on 设计(用 vdso),Stage 2 工作量比初稿估的 +1 book。
- **【v2 改】首期 F3-M1 范围诚实化**:讲信号投递 + Default/Ignore + kill(pid>0)/sigaction/sigprocmask + sigreturn(vdso 形态);**waitpid/SIGCHLD 阻塞需 wait_queue(F3-M2 futex 同类),首期只做 non-blocking 轮询版,留 TODO**;**进程组 kill(pid<0)留 F3-M3**。
- **量级**:约 0.9 个卷(4-5 book + 3-4 lab + 2 reference)。
- **tag**:037_signals / 038_clone_futex / 039_process_group / 040_scheduler_class。
- **卷**:扩 06-process 为 06b-process-advanced。

### Stage 3 — 内存弧深化(F2 mmap + PageCache + Demand Paging + Buddy/Slab)

- **弧**:F2-M1 VMA、M2 mmap、M3 brk、M4 Page Cache、M5 Demand Paging、M7 Buddy、M7b Slab。
- **理由**:依赖 F1(已铺)+ F3(clone 时地址空间 refcount,Stage 2 已做);Buddy→Slab→VMA→mmap→PageCache→Demand Paging 是教科书级主线,正好补 05-memory 卷停在 address space 的半截。
- **量级**:约 1.2 个卷(6-7 book + 4-5 lab + 2-3 reference)——最大单弧,内存概念密度最高。
- **tag**:041_vma_mmap / 042_page_cache / 043_demand_paging / 044_buddy_slab。
- **卷**:扩 05-memory 为 05b-memory-advanced。

### Stage 3.5 — 【新增】块设备抽象前置(F5 IBlockDevice/DmaPool 子集)

- **【v2 改·新增理由】**:F6 ext2 已在 `libs/ext2/`,ctor = `Ext2(IBlockDevice*)`,ext2_common.cpp 强依赖 IBlockDevice + page_cache。初稿把 F5 全弧排 Stage 8,但 Stage 4(F6 FS 升级)开工时块设备抽象还没回迁,要么临时造 stub 要么 F5 的 IBlockDevice/DmaPool 子集前移——本稿显式前移为独立 Stage。
- **弧**:F5-M1 子集(IBlockDevice 抽象 + DmaPool 基建,**不含具体驱动**)。
- **量级**:约 0.3 个卷(1-2 book + 1 lab)。
- **tag**:045_driver_iblock_device_dma_pool。
- **卷**:挂 14-drivers 首章,或并入 08b-filesystem-advanced 作前置章。

### Stage 4 — 文件系统弧升级(F6 ext2 写 + Dentry + ProcFS/DevFS/tmpfs + mount)

- **弧**:F6-M1(VFS 增强+mount+Dentry+flock)、M2 ProcFS、M3 DevFS、M4 tmpfs、M6 ext2 独立库(libs/ext2)。F6-M5 ext4 读**可选**。
- **【v2 改】理由修订**:**强依赖 F2 PageCache(Stage 3)+ F5 IBlockDevice(Stage 3.5)**;F6-M6 ext2 独立库的 host PAL 测试范式直接复用 F2 的 host TSAN 基建。
- **量级**:约 1.0 个卷(5-6 book + 4-5 lab + 2 reference)。
- **tag**:046_ext2_write_lib / 047_dentry_cache / 048_procfs_devfs / 049_tmpfs_mount。
- **卷**:扩 08-filesystem 为 08b-filesystem-advanced。

### Stage 5 — SMP 多核弧(F4 ACPI + APIC + AP boot + 多核调度 + lockdep)

- **弧**:F4-M1 ACPI、M2 LAPIC/IOAPIC + PIC→APIC、M3 per-CPU + AP trampoline、M4 多核调度 + 迁移、M5 同步 + lockdep。
- **【v2 改】理由修订**:依赖 F1+F3-M4(Stage 2 已做);**为何押后到 Stage 5**:F4 是难度与概念密度最高的弧(trampoline/swapgs/per-CPU GS/IPI/迁移竞态),教学叙事要先讲透单核(Stage 1-4 把进程/内存/FS 三大子系统补齐),再上多核。**必读 note**:`2026-06-27-f-verify-m3-smp-wake.md`(SMP 空转/ap-wake 误判破除,顶级 debug 叙事)+ `2026-06-25-f4-followup-smp-migration-race.md`(迁移竞态 Heisenbug)。
- **【v2 改】量级上调**:约 1.8 个卷(**8-10 book** + 6-7 lab + 3-4 reference)。初稿 7-8 book 偏保守,F4-M4 多核调度+迁移 + M5 lockdep 概念密度极高,单独可能再要 3-4 book。报告自己 §1 说 SMP 是 very-high/very-high,量级估算应按 very-high 上调。
- **tag**:050_acpi_apic / 051_percpu_ap_boot / 052_multicore_sched / 053_lockdep。
- **卷**:**新建 11-smp**。

### Stage 6 — 用户态运行时弧(F10 musl + ELF 动态链接 + TTY/PTY)

- **弧**:F10-M1 musl 静态、M2 ELF 动态、M3 TTY Phase1+2、M4 PTY 真终端会话。
- **【v2 改】理由修订**:依赖 F2 mmap(Stage 3)+ F3 信号(Stage 2)+ F9 creds(Stage 1 只做了 M1,M3 SMAP-SMP 耦合需在 Stage 5 SMP 完成后补);**【v2 改·关键】v1.0.0 fork 在 -smp2+ring3+gcc13+ubsan 仍有 #GP 未根治(A-rewrite-handoff),回迁前应等 fork A-rewrite 进 stable tag**(见决策点 6)。决策对齐 CinuxOS 砍自建 libc 改 musl,本身就是教学点。
- **量级**:约 1.0 个卷(5-6 book + 4 lab + 2 reference)。
- **tag**:054_musl_static / 055_elf_dynamic_link / 056_tty_pty。
- **卷**:扩 07-userland 或新建 11-userland-runtime。

### Stage 7 — 网络弧(F7 TCP/IP 全栈,前置 F5 网卡子弧)

- **弧**:**【v2 改】前置 F5-M3 子集**(e1000/VirtIO-net 网卡驱动,独立 tag)→ F7-M1 以太网、M2 ARP、M3 IPv4/ICMP、M4 UDP、M5 TCP、M6 Socket API。
- **【v2 改·分期硬伤修复】理由修订**:初稿把 F5 全弧排 Stage 8、F7 排 Stage 6,但 F7 依赖 F5 网卡(e1000/VirtIO-net),Stage 6 开工时网卡还没回迁。**本稿把 F5 网卡子集前移到 Stage 7 开头作为前置,或把 F5 全弧拆成 Stage 8a(网卡)+ Stage 8b(存储)**(见决策点 5)。
- **【v2 改】量级与 defer 边界明确化**:约 1.5 个卷(7-8 book + 5-6 lab + 3 reference)。**defer 边界明确为"TCP 三次握手/挥手/最小数据流"**(教学必须讲状态机),不含重传/RTO/窗口/拥塞。
- **tag**:057_driver_nic / 058_ethernet_arp / 059_ip_icmp / 060_udp_tcp / 061_socket_api。
- **卷**:**新建 12-network**。

### Stage 8 — 安全弧补全(F9 ASLR + UID/GID + Canary + SMAP-SMP 耦合)

- **弧**:F9-M2 ASLR、M3 UID/GID、M4 Canary、**【v2 改】M1.5 SMAP accessor SMP-Ready 版**(把 Stage 1 单核版补 RFLAGS 保存纪律,与 Stage 5 SMP 同期/之后做)。
- **【v2 改】理由**:Stage 1 只做了 F9-M1 单核版,完整 SMAP accessor 需要 SMP RFLAGS 保存纪律(`context_switch.S` 改),必须在 Stage 5 SMP 之后补。
- **量级**:约 0.5 个卷(3 book + 2-3 lab + 1-2 reference)。
- **tag**:062_aslr_canary / 063_credentials / 064_smap_smp_ready。
- **卷**:补 13-security。

### Stage 9 — 存储驱动弧增量(F5 NVMe + VirtIO-blk + HPET/RTC)

- **弧**:F5-M1 AHCI DMA 补全、M2 VirtIO-blk、M3 NVMe(**stable polling 版,不搬 async IO 分支**)、M4 HPET/RTC、M5 xHCI(键鼠 USB HID)。
- **理由**:依赖 Stage 3.5(IBlockDevice/DmaPool);教学 leverage 低于 SMP/网络。
- **量级**:约 0.8 个卷(4-5 book + 3-4 lab + 2 reference)。
- **tag**:065_nvme / 066_virtio_blk / 067_hpet_rtc_xhci。
- **卷**:**新建 14-drivers**(填补 04 卷空位也可)。

### Stage 10 — IPC 扩展弧(F8 Pipe 增强 + FIFO + Unix Socket + shm + poll/select)

- **弧**:F8-M1 Pipe 增强(SIGPIPE/阻塞)、M2 FIFO、M3 Unix Socket、M4 SysV shm、M5 poll/select。
- **理由**:独立性高,教学 leverage 中等;放最后收尾填充 userland 卷厚度。
- **量级**:约 0.5 个卷(3 book + 2-3 lab)。
- **tag**:068_fifo_unixsocket / 069_shm_poll。
- **卷**:扩 07-userland 或新建 11-ipc。

### 【v2 改】量级汇总

Cinux-Book 现有卷平均 ≈ 5 book + 4-5 lab + 1-2 reference。全 10 个 Stage 合计:

| Stage | 弧 | book | lab | reference | 折合卷 |
|---|---|---|---|---|---|
| 0 | 工具链恢复/重建 | 0 | 0 | 0 | 0.3 |
| 1 | **F9-M1 安全破冰** | 2-3 | 1-2 | 1 | 0.4 |
| 2 | F3 进程深化 | 4-5 | 3-4 | 2 | 0.9 |
| 3 | F2 内存深化 | 6-7 | 4-5 | 2-3 | 1.2 |
| 3.5 | F5 IBlockDevice 前置 | 1-2 | 1 | 0 | 0.3 |
| 4 | F6 FS 升级 | 5-6 | 4-5 | 2 | 1.0 |
| 5 | F4 SMP | 8-10 | 6-7 | 3-4 | 1.8 |
| 6 | F10 userland | 5-6 | 4 | 2 | 1.0 |
| 7 | F7 网络(含 F5 网卡) | 7-8 | 5-6 | 3 | 1.5 |
| 8 | F9 安全补全 | 3 | 2-3 | 1-2 | 0.5 |
| 9 | F5 驱动增量 | 4-5 | 3-4 | 2 | 0.8 |
| 10 | F8 IPC 扩展 | 3 | 2-3 | 1 | 0.5 |
| **合计** | | **~53** | **~38** | **~21** | **~10.2** |

**【v2 改·口径统一】**:回迁完 Cinux-Book 从 9 卷 45 篇 book 扩到约 **15-16 卷 88-95 篇 book + 75-80 lab + 25 reference**——**新增约 10.2 卷,即现有 9 卷 + 新增 10 卷 ≈ 19 卷总规模,book 篇数翻倍**。初稿"17-18 卷"与"翻倍"口径不统一,本稿明确:**新增 ~10 卷(翻倍),总规模 ~19 卷**。

### 【v2 改】Quick Win(先做,1-2 周破冰)

| 弧 | 理由 |
|---|---|
| **Stage 0 工具链恢复** | 前置,不做后面全违规。**首选 git 恢复 document/todo/,1.5-2 天** |
| **【v2 改·破冰首选】F9-M1 安全**(NX/SMEP/SMAP) | **纯机制回读,零依赖零反向耦合;240 行金矿 note;读者可见感最强;教学差异化最高** |
| **F12 腿 A**(KALLSYMS+backtrace+panic) | 零外部依赖;700 行新文件 + 270 行测试;tag 035 已有的 `fork_frame_pointer_bug.md` 是绝佳引入。**【v2 改】实质是 F-INFRA-5 复刻,非独立 quick win** |
| **F2 Buddy PMM**(F2-M7 单独) | 替换 flat bitmap,概念清晰;note 现成;WSL2 nested KVM 坑是亮点 |
| **【v2 改·撤】F3-M1 信号** | **撤下破冰候选**:note 仅 53 行,Custom handler round-trip 留后续,sigreturn 形态依赖决策点 2。留 Stage 2 |

### 押后(代码依赖深或教学杠杆低)

| 弧 | 押后理由 |
|---|---|
| **F4 SMP 全弧** | 概念密度最高,需 Stage 1-4 把单核讲透;放 Stage 5 |
| **F7 网络全栈** | 依赖 F5 网卡(Stage 7 前置)+ F8 wait-queue;全新卷工作量大;放 Stage 7 |
| **F5 NVMe/VirtIO 存储** | 代码依赖深(MSI-X/DmaPool/split virtqueue);放 Stage 9 |
| **F10 动态链接** | 依赖 F2 mmap + F3 信号 + F9 creds;**且 v1.0.0 fork #GP 未根治,等 A-rewrite stable**;放 Stage 6 |
| **F13 GUI 用户态化** | 依赖链最长(mmap/poll/ring3/DevFS/PTY 五大前置)+ 要重写 029-033 现有 7 章;**【v2 改·决策利好】Cinux-GUI 与 Cinux-Book 同组织,submodule 引入无障碍,但 defer 到 v1.1 仍合理** |

### 建议不回迁或推后

| 项 | 理由 |
|---|---|
| **F6-M5 ext4 写/journal** | CinuxOS 自己 defer(ext4 只读);ext2 已读写够教学;ext4 extent tree 写 + journal 概念重生产价值低 |
| **F7 TCP 重传/RTO/窗口/拥塞** | CinuxOS 自己 defer;最小可用 TCP(握手/挥手/数据流)够教学;拥塞控制是研究生课程,reference 提及即可 |
| **F8-M5 epoll** | CinuxOS 自己 defer(poll/select 已够);差异在性能不在概念,教学 ROI 低 |
| **F11 FAT32 + UEFI** | CinuxOS 推 v1.1+;Cinux-Book 01-boot 三阶段 BIOS bootloader 已讲透,UEFI 是平行方案不增量 |
| **F12-M3 自举闭环 / M4 包管理** | "在 Cinux 编 Cinux"是工程里程碑不是教学概念;包管理超出 OS 教程范畴 |
| **F12 GCC 自举全套**(腿 B) | 跨 F10/F5/F9 多弧,任一前置未回迁则跑不起来;CinuxOS 自己就为此连踩 4 层 bug。**只回迁腿 A,腿 B 等前置弧全回迁后再排期** |
| **NVMe 异步化 feat/nvme-async-io** | 当前开发中未稳定;0x4080 LBA 越界 bug 仅兜底 SIGBUS/SIGILL 未根治;教学上不宜讲半成品 |
| **F13 GUI 用户态化** | 依赖链最长 + 要重写 Cinux-Book 029-033 现有 7 章 + submodule 引入改变仓库形态;**defer 到 v1.1。但【v2 改·新增】至少补一篇 reference 讲"GUI 为何从内核迁用户态"的设计权衡**(决策点 3 选项 C),避免 09-gui 现有"内核态 GUI"叙事与回迁后主线割裂 |
| **F-VERIFY/F-DYN-COV/F-EXTABLE 等横切** | 方法论而非特性,不适合 book 卷;但 note 是绝佳 reference 素材,建议蒸馏 2-3 篇 reference(`testing-smp-coverage.md`/`exception-table.md`/`gui-userspace-design.md`) |
| **document/document/{kernel,user}** | 对应 Cinux-Book 旧 tag,对新特性零贡献,盘点时不计入新原料 |

### 【v2 改】最小可讲故事首期交付

**目标**:1-2 周内出第一个可见成果,重建节奏与信心,同时验证工具链可用。

**【v2 改·首期 = "SMEP 三件套" mini-arc**(Stage 0 + Stage 1):

1. **Stage 0 工具链恢复**(1.5-2 天):**首选** `git checkout f32c51c -- document/todo/` 恢复 F1-F13 路线图 → 迁 `meta/rewrite/`(或 document/todo/ + sidebar 排除)→ 重建 `tools/tutorial_rewrite.py` 弧驱动版 → 生成 F9 context pack → 对齐 sidebar.ts。**验收**:`python3 tools/tutorial_rewrite.py pack f9-security` 能输出可用 pack。
2. **036_nx_smep_smap.md**(book,Stage 1 F9-M1 单核版):CR4.NXE/SMEP/SMAP 置位 + CPUID leaf 7 机制回读 + EFER.NXE + 用户态 #PF/-EFAULT 路径 + accessor 单路(stac/clac)。配 1 lab(写用户态程序触发 SMAP #PF 看内核优雅返回)+ 1 reference(CR4/EFER 位映射 + CPUID 机制验证命令集)。叙事锚点:019 #PF → 021 同步 → 036 NX/SMEP/SMAP。
3. **reference/security-mechanism-verification.md**:从 F9-M1 240 行金矿 note 提炼"怎么证明硬件保护真生效"方法论。

**为何是这个组合**:**零依赖**(只需 CPUID/CR4/EFER)、**零反向耦合**(单核版不触发 SMP RFLAGS 问题)、**note 最现成**(240 行金矿)、**读者可见感最强**(跑命令看到保护真开了)、**独立成篇**(不依赖任何弧)。

**首期不做**:不碰 F3/F4/F7(依赖深概念密);不一次做完 F9 全弧(SMAP-SMP 耦合留 Stage 8);**【v2 改】不碰 F3 信号破冰**(sigreturn 形态需决策点 2 先定)。

**首期后第二推**:F12 腿 A(KALLSYMS/backtrace)+ F2-M7 Buddy PMM + F3-M1 信号(决策点 2 定稿后),三个并行,把节奏稳住再开 Stage 3 内存深水区。

---

## 7. 【v2 改】风险与开放问题(决策点,吸收 must_ask_user)

### 决策点 1:【v2 改·修订】首期破冰到底选 F9-M1 还是 F3-M1?

- **选项 A(推荐)**:Stage 0 + **F9-M1 安全(NX/SMEP/SMAP 单核版)**。独立性最高(零依赖零反向耦合)、note 最详(240 行金矿)、读者可见感最强(跑 CPUID 看保护生效)、教学差异化最高(机制回读方法论)。
- **选项 B(初稿推荐,本稿撤)**:Stage 0 + F3-M1 信号。**撤下理由**:note 仅 53 行,Custom handler round-trip 留后续,sigreturn 形态依赖 NXE(决策点 2),首期交付可见感弱("内核能投递 Default 终止信号"≠"我的 OS 能捕获信号继续跑")。
- **选项 C**:Stage 0 + F12 腿 A(KALLSYMS/backtrace)。代码量最小,但教学杠杆中等。
- **我的推荐**:A。

### 决策点 2:【v2 改·新增·最关键】F3 sigreturn trampoline 形态是否一次定稿?

F3-M1 note GOTCHA#10 明写:sigreturn 栈注入依赖 NXE 未启用,F9 启用 NXE 后栈不可执行,trampoline 失效,必须迁 vdso。三个弧(F3/F9/F10)的 sigreturn 实现互相牵引。

- **选项 A**:F3 直接按 NXE-on 设计(用 vdso)。Stage 2 工作量 +1 book,但 Stage 8 F9 NXE 回迁时 Stage 2 章节不返工。
- **选项 B(推荐,分阶段)**:**Stage 1 先做 F9-M1 NXE**(首期破冰),Stage 2 F3 sigreturn 直接按 NXE-on 设计(vdso)。这样 F3 一次定稿,不返工。
- **选项 C**:F3 先按 NXE-off 栈注入,F9 回迁时返工 F3 章节。返工成本明确,但叙事连贯性差。
- **我的推荐**:B。Stage 1 F9-M1 NXE 破冰 + Stage 2 F3 按 NXE-on 设计,三方耦合一次理清。

### 决策点 3:【v2 改】document/todo 的 F1-F13 路线图恢复还是重建?

- **选项 A(推荐)**:`git checkout f32c51c -- document/todo/` 恢复,迁 `meta/rewrite/`(或不迁,加 sidebar 排除规则)。1.5-2 天。
- **选项 B**:从零重建 meta/rewrite/ + tools/tutorial_rewrite.py。2-3 天,但有恢复的里程碑 md 作蓝本比从零快。
- **选项 C**:连 arc 时间线轨道一起重建(document/arc/)。Stage 0 翻倍,arc 是更大方法论工程。
- **我的推荐**:A。document/todo 已是里程碑驱动蓝本,恢复比重建高效。

### 决策点 4:【v2 改】Cinux-Base(5373 行,非"小")是否在 Stage 0 引入?

- **选项 A**:Stage 0 引入。是 ErrorOr 风格代码的基建前提,先引一次,后续所有弧复用。**但会带来风格分裂**(Cinux-Book 现有错误处理 vs ErrorOr)。
- **选项 B(推荐)**:用到需要它的弧再引,且**新卷用 ErrorOr、旧卷不改**(风格割裂但工作量可控)。
- **选项 C**:不引,把 ErrorOr 改写成 Cinux-Book 现有错误处理风格。搬运成本上升。
- **我的推荐**:B。ErrorOr 风格只在新卷用,旧卷保持现状。

### 决策点 5:【v2 改·新增】Stage 7 网络的 F5 网卡前置怎么排?

F7 依赖 F5 网卡(e1000/VirtIO-net),初稿把 F5 全弧排 Stage 9、F7 排 Stage 7,分期硬伤。

- **选项 A(推荐)**:把 F5 网卡子集前移到 Stage 7 开头作为前置 tag(057_driver_nic),再开 F7 主弧。
- **选项 B**:把 Stage 9(F5 全弧)拆成 Stage 9a(网卡)+ Stage 9b(存储),Stage 7 排在 Stage 9a 之后。
- **选项 C**:Stage 7 网络只做 loopback,F5 网卡留 Stage 9。教学叙事不完整(loopback 没法真 ping 10.0.2.2)。
- **我的推荐**:A。

### 决策点 6:【v2 改·新增】F10 fork 在 v1.0.0 仍有 #GP,Stage 6 何时启动?

A-rewrite-handoff.md 揭示 -smp2+ring3+gcc13+ubsan 下首个 /hello fork 子处 #GP(vector 13),CI 5/6(Release+ubsan deferred),fork child-setup A 重写正在进行。

- **选项 A(推荐)**:等 fork A-rewrite 进 stable tag 再启动 Stage 6 F10。延后但回迁质量基线稳。
- **选项 B**:锚定 v1.0.0 回迁,章节明确标注"fork 在 -smp2+gcc13+ubsan 仍有 #GP,教学版用单核或 gcc-16"。教学仓可接受半成品标注。
- **选项 C**:直接回迁 feat/nvme-async-io 分支(含 A-rewrite)。分支未稳定,违背"stable + 可验证"。
- **我的推荐**:A。

### 决策点 7:Cinux-GUI submodule 与 F13 GUI 用户态化是否在本轮回迁?

- **选项 A(推荐)**:**defer 到 v1.1**。F13 依赖链最长 + 要重写 029-033 现有 7 章。**【v2 改·新增】但至少补一篇 reference 讲"GUI 为何从内核迁用户态"**(决策点 7 选项 C 的最小化版本),避免 09-gui 现有叙事与回迁后主线割裂。
- **选项 B**:本轮做。**【v2 改·决策利好】Cinux-GUI 与 Cinux-Book 同组织,submodule 引入是组织内操作**,但工程周期可能是所有候选弧里最长的。
- **选项 C**:只补一篇 reference(本稿并入选项 A 作为最小化补充)。
- **我的推荐**:A + 一篇 reference。

### 决策点 8:tag 续编号起点是 036 还是清理 04 卷空位?

- **选项 A(推荐)**:036 起新区间,新卷从 11 开始(11-smp/12-network/13-security/14-drivers),04 空位保留(`document/book/index.md` 已自述"九卷"含 04 空位语义)。
- **选项 B**:填补 04 卷空位(如 04-smp)。破坏现有卷号语义。
- **我的推荐**:A。

### 决策点 9:【v2 改】CinuxOS CMake warning 纪律如何引入?

**【v2 改】CinuxOS 不是"全量 -Werror"**:实测是精选 `-Werror=return-type/implicit-fallthrough/undef/duplicated-branches/duplicated-cond/logical-op/format-security` + 一个 GLOBAL `-Werror` 作用在 `big_kernel_common` PRIVATE(不传 test)。且开关走 `cmake/options.cmake` 单一注册表模式("All user-facing build switches live in the single registry")。

- **选项 A(推荐)**:作为可选开关引入,**仿照 options.cmake 单一注册表模式新建**(不是简单加一行 option)。默认关,读者可选开。
- **选项 B**:全量引入。教学仓不友好。
- **选项 C**:完全不引。放弃 F-WARN/F-QA 纪律收益。
- **我的推荐**:A。**注意:options.cmake 是基建工作,工作量被初稿低估**。

### 决策点 10:【v2 改】CI matrix 何时扩到 6 cell?

- **选项 A(推荐)**:分阶段——先 2 cell,SMP 回迁后升 `run-kernel-test-all` 双腿,安全弧后再加 ubsan/lockdep cell。`busybox-smoke`/`gcc-smoke` 独立 job 不进 kernel matrix。**【v2 改·新增】同步把 host-tests 约束脚本(check_uaccess_boundaries/check_freestanding_headers)引入**。
- **选项 B**:一次性引入 CinuxOS 全套 6 cell + setup-cinux action。CI 时间暴涨。
- **我的推荐**:A。

### 决策点 11:【v2 改】F5 NVMe 回迁版本选哪一版?

- **选项 A(推荐)**:回迁 v1.0.0 stable polling 版,不搬 `feat/nvme-async-io` 分支的异步 IO。异步版有未根治的 0x4080 LBA 越界 bug。
- **选项 B**:等 async IO 进 stable 再回迁。延后 Stage 9。
- **选项 C**:回迁 async IO 分支。带来已知 bug。
- **我的推荐**:A。

### 决策点 12:【v2 改·新增】309 篇 notes 多是 propose-log,回迁工作量要不要 +30-50%?

- **选项 A(推荐)**:接受 +30-50% 工作量,给 propose-log 补 Why + 教学化重构。每个 Stage 量级估算按此上调。
- **选项 B**:接受"半成品章节"先铺量后打磨。教学仓质量风险。
- **选项 C**:只回迁有金矿 note 的弧(F4/F5/F7/F9-SMAP/F10),其余 defer。
- **我的推荐**:A。

### 决策点 13:【v2 改·新增】primer 04-arch / 05-os-concepts 两块空白要不要在 Stage 0/1 之前补?

回迁后正文扩到 ~19 卷,新读者面对 SMP/网络/安全/动态链接这些高概念密度内容时,缺"05 OS 前置概念"这张全景图会更痛。

- **选项 A**:保持现状(正文带着讲),赌读者能跟上。
- **选项 B(推荐)**:Stage 0 或 Stage 1 之前先补 primer 04/05 全景图,降低新读者入门门槛。
- **我的推荐**:B。但优先级低于 Stage 0/1,可并行做。

### 决策点 14:【v2 改·新增】翻倍体量是否拆成两个轮回?

教学仓读者路径是从 001 顺序读到 035,翻倍后读者路径长度翻倍,新读者入门门槛骤升。

- **选项 A(推荐)**:拆成两个轮回。第一轮只做 Stage 0-4(扩现有卷 + 工具链 + F9 破冰 + F3/F2/F6 深化,体量 +4 卷),验证读者反馈再决定要不要开 SMP/网络两个全新大卷。**【v2 改·新增】同时引入读者路径分层:基础轨 01-10 + 进阶轨 11-19**(分轨索引,新读者从基础轨入门)。
- **选项 B**:一次性规划全 10 Stage。
- **我的推荐**:A。

### 决策点 15:【v2 改·新增】lab 配套的读者可验证性怎么保证?

- **选项 A(推荐)**:每个 Stage 的 lab 显式列"读者跑不跑得起来"细节: SMP lab 怎么复现迁移竞态(`-smp 2` + sync 调试技巧)、网络 lab 怎么真 ping 通(QEMU user-mode 10.0.2.2 配置)、SMAP lab 怎么证明保护生效(`-cpu host` 透传 CPUID leaf 7)。
- **选项 B**:lab 只给代码 + 期望输出,不细化环境配置。
- **我的推荐**:A。Cinux-Book 现有 035 章节成功靠"每章至少一个真实踩坑 + 验证命令",回迁必须延续。

### 风险清单

1. **Heisenbug 教学可控性差**(SMP 弧最大风险):-smp 2 下并发 bug 时序敏感。必须把 on_cpu 迁移同步、prepare-to-wait、per-CPU deferred-free 一并迁全,不能只迁"启动 AP idle"半截。
2. **重写稳定教学核心的风险**(SMP/F10/F13 共有):Cinux-Book scheduler.cpp/context_switch.S/gdt.cpp/per_cpu.hpp/interrupts.S 都是单核主干,SMP 回迁中间态会破坏 syscall;F10 要把 022/023/024 现有"裸 _start"叙事推翻;F13 要删 029-033 的内核 WM。
3. **swapgs 纪律脆弱**(SMP):NMI/#DB 在 syscall-exit swapgs 窗口,CinuxOS 自己留 paranoid 路径 follow-up。
4. **trampoline 首跑即通难度**(SMP):GAS-direct(非 cpp)、地址折叠、CR3 切换时机、双 SIPI 兜底——任一错都 triple-fault。**必读 `2026-06-27-f-verify-m3-smp-wake.md`**(破 SMP 空转/ap-wake 误判)。
5. **测试基线门槛**(SMP):CinuxOS 有 run-kernel-test-all 单核+-smp2 ~1946 项 + TSAN/ASAN/LOCKDEP 矩阵;Cinux-Book 测试体系若较弱无法兜底 SMP 正确性,回迁后"绿"可能只是没踩。
6. **【v2 改·升级】NVMe 已知 bug + F10 fork #GP 双未根治**:0x4080 status + CQ timeout(F5);-smp2+ring3+gcc13+ubsan fork #GP(F10,A-rewrite-handoff 揭示)。回迁必须先决定搬哪版,Stage 6 F10 等 A-rewrite stable。
7. **跨弧前置债**(F5/F8/F10/F13 共有):F5 无法单弧回迁(需 F1+F4 基建);F8 需 F9 信号子集;F10 需 F2/F3/F6/F9 子集;F13 需五大前置。**实际工作量 = 弧本体 + 半个前置弧集**。
8. **【v2 改·升级】F3↔F9↔F10 三方 sigreturn 耦合**:F3-M1 sigreturn 栈注入依赖 NXE-off,F9 NXE 启用后失效需迁 vdso,F10 musl 动态链接带 vdso。三个弧分期必须显式处理(决策点 2),否则按初稿顺序会在 Stage 8 发现 Stage 2 成果要返工。
9. **【v2 改·升级】SMAP accessor↔SMP RFLAGS 耦合**:F9-SMAP 完整版需 SMP 的 RFLAGS 保存纪律(`context_switch.S` 改),Stage 1 单核版不触发但 Stage 5 SMP 后必须补(Stage 8 F9-M1.5)。初稿"F9 独立性高"标签误导。
10. **F-EXTABLE 必须同 F9-SMAP 同迁**(F9):extable.hpp + linker.ld __ex_table 段 + page_fault.cpp 查表,否则 SMAP accessor 的 -EFAULT 容错契约无法兑现只能 panic。
11. **-fstack-protector-strong 全重编可能暴露隐藏 bug**(F9):原本 `-fno` 下藏着的栈数组越界 bug 会触发 `__stack_chk_fail` panic。
12. **submodule 引入是单向门**(F13):一旦 `.gitmodules` 加了 Cinux-GUI,所有后续 tag checkout 都要 `--recursive`。**【v2 改】但 Cinux-GUI 与 Cinux-Book 同组织,权限/归属无障碍**。
13. **【v2 改·纠正】toolchain-x86_64.cmake 不能照搬**:Cinux-Book 自己的 toolchain init 灌了 freestanding/kernel 标志,CinuxOS 反而把 freestanding 下放到 per-target。照搬需先把 Cinux-Book toolchain init 的标志剥掉下放,是一次 toolchain 重构。**user/host test 错套 -mcmodel=kernel 的风险被初稿夸大**(per-target PRIVATE 优先级高)。
14. **CinuxOS CMake warning 纪律教学仓不宜全量**(机制):实测 CinuxOS 是精选 -Werror=specific + big_kernel_common PRIVATE GLOBAL -Werror,不是全量 -Werror;options.cmake 是单一注册表模式,引入需仿照新建。
15. **tag 035 源码文件名与教学线叙述不完全对齐**(机制):实测 tag 035 内 `kernel/fs/vfs.cpp` NOT in tag(实际是 `vfs_mount.cpp`/`vfs_filesystem.hpp`/`inode.cpp`)、`kernel/gui/canvas.cpp` NOT in tag(canvas 在 drivers/)。**写新章节引用源码时必须 `git ls-tree <tag>:<dir>` 实查,不能凭 HEAD 或记忆**。
16. **【v2 改·新增】propose-log 教学化重构工作量大**:309 篇 notes 仅 ~25 篇详级金矿,其余需补 Why + 教学化裁剪。每个 Stage 量级 +30-50%。
17. **【v2 改·新增】读者路径陡升**:翻倍体量(9→19 卷)让新读者入门门槛骤升,需引入基础轨/进阶轨分轨索引(决策点 14)。

---

## 8. 建议下一步准备动作(不写代码)

### 8.1 拍板(作者本人定)

1. 决策点 1-15(§7)的选择。**最关键:决策点 1(破冰弧)、决策点 2(F3 sigreturn 形态)、决策点 3(恢复 vs 重建)、决策点 6(F10 fork 等不等 stable)、决策点 14(一轮 vs 两轮)**。
2. 是否在本轮就启动 Stage 5 SMP 与 Stage 7 网络(最大教学差异化,但工作量也最大)。
3. 是否接受"翻倍体量(~19 卷)"作为本轮回迁的最终目标,还是只做 Stage 0-4(扩现有卷 + 工具链 + F9 破冰 + F3/F2/F6 深化)作 MVP(决策点 14)。

### 8.2 恢复/重建脚手架(先做,不产教学卷)

1. **【v2 改】首选 `git checkout f32c51c -- document/todo/` 恢复 F1-F13 路线图**,决定迁 `meta/rewrite/` 还是留 `document/todo/` + sidebar 排除。
2. `tools/tutorial_rewrite.py`(单文件 Python,弧/里程碑驱动,3 子命令 matrix/pack/pack-all)。
3. `meta/rewrite/writing-contract.md`(可勾选清单,引用 `document/ai_prompts/writing_style.md` + CinuxOS `document/ai/{QUALITY-GATES,DIRECTIVES,CODING-TASTE}.md`)。
4. `meta/rewrite/README.md`(目录用途 + 工具用法)。
5. 修订 `CLAUDE.md` 删掉不存在引用,或恢复后更新。
6. (决策点 4 选 B)新卷引入 ErrorOr 风格,旧卷不改。

### 8.3 【v2 改】先读哪些 note(为 Stage 1 F9-M1 安全破冰准备)

- `/home/charliechen/CinuxOS/document/notes/2026-06-28-smap-m0-accessor-p0a-p0e-foundation.md`(240 行金矿,SMAP 推倒重做全记录,教学金矿)
- `/home/charliechen/CinuxOS/document/todo/f9-security/`(F9 立项文档,24K)
- `/home/charliechen/CinuxOS/document/ai/PLAN.md` 中 F9-M1 批段
- `/home/charliechen/CinuxOS/document/ai/ROADMAP.md` F9 行
- `/home/charliechen/Cinux/document/book/03-big-kernel/`(中断/异常章节,Cinux-Book 现状锚点,#PF 路径承接处)
- `/home/charliechen/CinuxOS/document/ai/{QUALITY-GATES,CODING-TASTE,DIRECTIVES}.md`(写作契约补充)
- `/home/charliechen/Cinux/document/ai_prompts/{writing_style,code_conventions,prompt_generate_tutorial}.md`(写作契约)

### 8.4 先读哪些 note(为后续 Stage 选向准备)

- Stage 5 SMP:`/home/charliechen/CinuxOS/document/notes/2026-06-19-f4-m3-design.md` + `2026-06-25-f4-followup-smp-migration-race.md`(迁移竞态 Heisenbug)+ **`2026-06-27-f-verify-m3-smp-wake.md`(SMP 空转破除,顶级素材)**
- Stage 7 网络:`/home/charliechen/CinuxOS/document/todo/f7-network/`(弧线完整)
- F12 腿 A:`/home/charliechen/CinuxOS/document/notes/2026-06-19-finfra-5-kallsyms-real-symbols.md` + Cinux-Book `/home/charliechen/Cinux/document/notes/035/fork_frame_pointer_bug.md`
- **【v2 改·新增】Stage 6 F10**:`/home/charliechen/CinuxOS/document/ai/A-rewrite-handoff.md`(fork #GP 根因 + docker gcc-13 验证纪律)

### 8.5 建立的工作流纪律(本轮全程遵守)

1. **不凭印象写**:写新章节引用源码前,必 `git ls-tree <tag>:<dir>` 或 `git show <tag>:<file>` 实查(Cinux-Book tag 035 文件名与教学线叙述不完全对齐)。
2. **规划/交接文档放 meta/**:绝不放 document/(sidebar.ts 会扫出来污染侧栏),或放 document/ 下被 sidebar 排除的子目录。
3. **新卷从 11 起编号**:04 卷空位保留。
4. **CinuxOS 锚定 v1.0.0 tag**:不跟 HEAD(非 NVMe 弧漂移极小可放宽,但 NVMe 弧必须锚 v1.0.0)。
5. **每个弧生成 context pack 再写**:不直接从 working tree 抄。
6. **CinuxOS 已 defer 的不回迁**:ext4 写/epoll/TCP 拥塞/UEFI/NVMe async。
7. **stable + 可验证优先**:NVMe 选 polling stable 不选 async 分支;F10 fork 等 A-rewrite stable;F13 选 defer 不选半成品。
8. **【v2 改·新增】propose-log 必补 Why**:绝大多数 note 是 propose-log 四段,改写成 book 章节必须补"为什么"教学叙事 + 教学化裁剪。
9. **【v2 改·新增】docker gcc-13 验证纪律**(QUALITY-GATES.md):回迁带 musl/fork 的弧,本机 gcc-16 ≠ gcc-13 不能代表 CI,必须 docker 复现。

---

## 关键文件路径索引(供下游取用)

**Cinux-Book**:
- 根构建:`/home/charliechen/Cinux/CMakeLists.txt`、`/home/charliechen/Cinux/cmake/toolchain-x86_64.cmake`(**【v2 改】init 灌 freestanding/kernel 标志**)、`/home/charliechen/Cinux/kernel/CMakeLists.txt`
- 构建入口:`/home/charliechen/Cinux/scripts/build.ts`、`/home/charliechen/Cinux/project.config.ts`、`/home/charliechen/Cinux/package.json`
- sidebar 扫描:`/home/charliechen/Cinux/site/.vitepress/config/sidebar.ts`
- CI:`/home/charliechen/Cinux/.github/workflows/{ci,deploy}.yml`(**5 job**)
- 卷组织自述:`/home/charliechen/Cinux/document/book/index.md`(自述"九卷")
- 唯一存活规划:`/home/charliechen/Cinux/meta/primer-dev-plan.md`
- **【v2 改·新增】可恢复路线图**:`git checkout f32c51c -- document/todo/`(F1-F13 phase 路线图,9ceefff 删除)
- 未丢失写作契约:`/home/charliechen/Cinux/document/ai_prompts/{writing_style,code_conventions,prompt_generate_tutorial,prompt_review_tutorial,prompt_maintain_project,prompt_understand_project,prompt_write_module}.md` + `CLAUDE.md.template` + `templates/`(8 文件)
- 035 教学线收尾:`/home/charliechen/Cinux/document/book/10-multitasking/{035-power-on-fork-exec,035b-multi-terminal}.md`
- 035 排错笔记(回迁章节范本):`/home/charliechen/Cinux/document/notes/035/`(5 篇)

**CinuxOS**(锚定 v1.0.0 tag):
- 顶层叙事:`/home/charliechen/CinuxOS/document/ai/{PLAN,ROADMAP,A-rewrite-handoff,QUALITY-GATES,CODING-TASTE,DIRECTIVES}.md`(**【v2 改】8 文件**)
- dev notes:`/home/charliechen/CinuxOS/document/notes/`(309 篇 HEAD / 308 篇 v1.0.0,按 `f<N>-m<M>-*` 命名,**【v2 改】仅 ~25 篇详级金矿**)
- todo 立项:`/home/charliechen/CinuxOS/document/todo/{f1-kernel-infra,f2-memory,f3-process,f4-smp,f5-drivers,f6-vfs,f7-network,f8-ipc,f9-security,f10-userspace,f11-platform,f12-developer,f13-gui,f-eco,f-gui-userspace,f-usability,quality}/`
- **【v2 改】submodules**:`/home/charliechen/CinuxOS/.gitmodules`(Cinux-Base 5373 行 @ CinuxOS/Cinux-Base;Cinux-GUI 9090 行 @ Awesome-Embedded-Learning-Studio/Cinux-GUI **同组织**)、`/home/charliechen/CinuxOS/third_party/{Cinux-Base,Cinux-GUI}`
- **【v2 改】ext2 独立库**:`/home/charliechen/CinuxOS/libs/ext2/`(ctor = `Ext2(IBlockDevice*)`,ext2_common.cpp 强依赖 page_cache)
- **【v2 改】options 注册表**:`/home/charliechen/CinuxOS/cmake/options.cmake`(单一开关注册表模式)
- 工具链:`/home/charliechen/CinuxOS/tools/{gcc-toolchain,musl}/`、`/home/charliechen/CinuxOS/rootfs/{buildroot,overlay}/`
- CI:`/home/charliechen/CinuxOS/.github/actions/setup-cinux/action.yml`、`/home/charliechen/CinuxOS/.github/workflows/{ci,build-images}.yml`
- 约束脚本:`/home/charliechen/CinuxOS/scripts/{check_net_decoupling,check_uaccess_boundaries,check_freestanding_headers,generate_kallsyms}.{sh,py}`

---

**总结一句**:回迁是 **10 个 Stage、约 10 卷新增体量(总规模 ~19 卷)、首期用"SMEP 三件套"破冰**的长工程;核心约束不是代码搬运(两仓历史独立),而是**恢复/重建弧驱动的 context-pack 工具链(Stage 0,首选 git 恢复 document/todo/)** + **propose-log 教学化重构(每 Stage +30-50%)**;**最大教学差异化在 Stage 5 SMP 与 Stage 7 网络(Cinux-Book 完全空白)**;**分期最大盲点是 F3↔F9↔F10 三方 sigreturn/vdso 耦合(决策点 2)与 F9-SMAP↔SMP RFLAGS 耦合(Stage 1 单核 + Stage 8 补 SMP)**;CinuxOS 自身 defer 的 ext4 写/epoll/TCP 拥塞/UEFI/NVMe async 一律不回迁;**F10 fork #GP 未根治,Stage 6 等 A-rewrite stable**;F13 GUI 用户态化 defer 到 v1.1(但 Cinux-GUI 同组织是利好,至少补一篇 reference)。