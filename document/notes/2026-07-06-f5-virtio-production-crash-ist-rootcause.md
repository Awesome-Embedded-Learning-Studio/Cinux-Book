# F5-M2/M7 production gcc 崩 = IST IRQ 栈缺失(非 VirtIO 的因)

> 2026-07-06。VirtIO 弧收官(见 [finale](2026-07-06-f5-virtio-finale.md))后,production
> GUI/console 跑 `gcc`/`g++` 偶发崩,一度归因 init_msi_x。深查后真凶 = **硬件 IRQ 没用 IST 栈**
> (F13-B Bug② 同族,见 [f13-b-bug2-irq-ist](2026-07-06-f13-b-bug2-irq-ist.md))。并 origin/main
> 带入 IST2 修后稳定。本 note 记调试链 + 教训,免下个 AI 重追 VirtIO。

## 症状(崩点漂移)

production `run`(挂 virtio-blk-pci + init_msi_x unmask 0x42)+ gcc/g++ 重负载 → **崩点漂移**:
- `#GP` in `BuddyAllocator::clear_bit`(RDX 非规范地址)
- `#DF` RSP=user(内核在用户栈上执行)
- 用户态 segfault(kernel_init exec /bin/sh、cc1 NULL 写)
- `EXT2 failed to read inode block` + cc1 加载坏 ELF

四跑四样 → 内存损坏签名(不是确定性逻辑 bug)。

## bisect 误导 → sub-bisect 还原清白

`run` 当时**没挂 virtio 设备**(只 test target 有),前一个 AI 的 `#if 0` bisect
无效(Step 21a2/21a3 本就是 no-op)→ 错误归因 VirtIO。

把 virtio-blk 加进 `run` 后重做 + console 自跑(build-console,GUI=OFF)sub-bisect:
- **B**:只 map table(+0x84000)→ clean。
- **C**:map table+PBA(直接 `g_vmm.map`)→ clean。canary:+0x84000/+0x85000 map 前 = 0x0,
  map 后邻居 +0x90000/+0x70000 PTE 不变 → **map 干净,无 PT 踩踏/别名**。
- **D**:`msix_.init`(maps+指针+cap 赋值)→ clean。
- **E**:**full init_msi_x(msix_.init + mask_all + program_vector + enable)+ IST 修** → clean。

⭐ init_msi_x 的每个动作(map/mask/program/enable + PTE 校验映到设备 MMIO 0xfebf5000 非 RAM、
0x42 中断没 fire 无 I/O)**全清白**。bisect 干净指向触发器,不是根因。

## 真根因:IRQ 无 IST 栈(F13-B Bug② 原文)

CinuxOS 硬件 IRQ **没用 IST**:`interrupts.S` 的 `ISR_IRQ` stub 的 `sub $512 + fxsave(%rsp)`
+ ISR frame 落在**被中断 task 的栈**上。PIT 100Hz / 各 IRQ 在 gcc/g++ 深栈中段抢进,
fxsave 512B + frame **物理覆盖**调用链沿途的栈对象 → corrupt 指针 → 踩 .text/数据 → 崩点漂移。

> 之前因「栈不溢出」排除过 IST —— 错。**IST 不是防溢出,是防 IRQ frame 物理覆盖 task 栈对象**。
> watermark 测的是 render/调用自身栈(浅),漏了 IRQ entry 在同栈上的 fxsave 区。

init_msi_x 的角色:arm virtio-blk 的 MSI(unmask entry 0)+ 挪二进制布局 → 触发/暴露了
这个**潜在**的 IST 溢出(PIT tick 在 gcc 深栈上 fxsave 覆盖)。与 init_msi_x 无因果。

## 修:并 origin/main 带 IST2 栈

`merge eab66a0`(并 origin/main)带入 F13-B 的 IST IRQ 栈(`54f6392` fix(gui) F13-B 稳定化):
- `gdt.hpp/cpp`:`alignas(16) uint8_t irq_stack_[4KB]` + `tss_.ist[1] = &irq_stack_[...]`(照抄 IST1 #DF 模板)
- IDT gate 的 `ist` 字段 = 2 → CPU 进 ISR 自动把 RSP 换成 TSS.ist[1],fxsave + frame 落 IST2,**完全不碰 task 栈**。

## 验证

- boot 日志:`[BIG] GDT loaded (TSS with IST1 #DF + IST2 IRQ stacks)`。
- build-console(GUI=OFF)自跑 3+ 次完整 usability(`/etc/cinux-usability-test.sh` 跑 gcc hello.c **+ g++ hello.cpp**)全 PASS,0 segfault。
- `run-kernel-test-all` 两 leg 889/0 + SMP ALL PASSED。

⭐ console 模式(build-console)Claude 可**自跑**迭代(`timeout N cmake --build build-console --target run < /dev/null`;run 无 isa-debug-exit 跑到 timeout,auto-script 自动跑 gcc+g++)—— 不用烧用户 GUI run,调试快。GUI QEMU 仍用户启动。

## 教训

1. **bisect 干净 ≠ 因果**:init_msi_x vs diagnose 干净 bisect,但 init_msi_x 各动作全 clean —— 它只是挪布局/时序触发了潜在 IST 溢出。bisect 指向触发器,不一定是根因。
2. **崩点漂移 = IRQ frame 覆盖栈对象**(非栈溢出):watermark 测栈深会漏(fxsave 区在同栈上)。
3. **console gate Claude 自跑** + canary(translate 前/后对比 PTE)是快速缩范围利器。
4. 别信「bisect 干净就结案」—— 当 manifestation 漂移(内存损坏签名),要怀疑潜在时序/布局 bug,bisect 找的是触发器。

## follow-up(独立线,不挡 VirtIO)

- **GUI 桌面**被 F13-B host adapter 重写搞挂(切新 core 未完成)—— 下一弧。
- blk↔net BAR 撞(都 self-assign 0xfeb60000)—— 加 net 进 run 前必修(self_assign_bar/map_bar 给每实例独立 slot)。
- NetStack attach virtio-net + SLIRP ping + perf(virtio-blk vs NVMe vs AHCI)。

接 finale note + memory [[f5-m2-m7-virtio-progress]] + [[f13-b-host-adapter-handoff]](IST2 根治原文)。
