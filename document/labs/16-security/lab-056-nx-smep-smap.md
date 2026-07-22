---
title: Lab 056 · NX / SMEP / SMAP 机制回读验证
---

# Lab 056 · NX / SMEP / SMAP 机制回读验证

> 对应 `document/book/16-security/056-nx-smep-smap.md`。验证档 **A 档偏 B**(机制本身无「用户可见现象」,靠**机制回读**——直接读 EFER/CR4 看那几个位设上没——加上跑真程序确认开完三位不炸)。本章拨开 x86_64 的三个硬件保护位:NX(用户数据页不可执行)、SMEP(内核不可执行用户页)、SMAP(内核不可读写用户页)。
>
> ⚠️ **WSL2 环境限制**:嵌套 KVM 的 `-cpu max` 不透传 CPUID.07H:EBX,SMEP/SMAP 在开发机上**代码正确但跳过**(看不到 CR4[20]/[21] 设上)。NX 不受影响(EFER.NXE 是 baseline,透传→真生效)。真机/完整 KVM/TCG 才能看到 SMEP/SMAP 活。lab 里会区分「在 WSL2 看到什么」vs「在真机看到什么」。

## 目标

确认四件事:

1. EFER.NXE(bit 11)**设上了**(x86_64 baseline,任何环境都该有);
2. SMEP/SMAP 的 enable 逻辑是 **CPUID-gated**——读 CPUID.07H:EBX[7]/[20],支持才设 CR4[20]/[21](WSL2 跳过,真机设上);
3. `stac`/`clac` 挂在所有从用户态进来的入口(SYSCALL + 三个 ISR 宏),且只在「从用户态来」的分支置/清 AC;
4. 三个位都开完,跑真程序(`run-kernel-test`、`make run` 起 shell/GUI)**零 #PF / 零 panic**——证明内核路径没踩 SMAP/SMEP 的雷。

## 步骤

### 1. NXE 设上了(EFER bit 11)

最直接的看法是跑那个机制回读测试:

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -iE "f9|nxe|smep|smap" | head
```

应看到 `test_f9_nxe_smep_smap_enabled` 这一项 **PASS**。它读 EFER 断言 bit 11(NXE)= 1——这是无条件断言(baseline),任何环境都该过。

想自己核 EFER,看测试源码的读法:

```bash
sed -n '122,140p' kernel/test/test_usermode.cpp
```

应看到读 EFER(走 `rdmsr` on `MSR_EFER=0xC0000080`)、`(efer >> 11) & 1` 断言 NXE。注意那个「移位再 `& 1`」——别写成 `efer & 0x800`(结果是 `0x800` 不是 0/1,某些断言宏按 `==1` 判会假阴性)。

### 2. SMEP/SMAP 是 CPUID-gated(关键)

去看 enable 逻辑本体:

```bash
sed -n '41,59p' kernel/arch/x86_64/paging.cpp
```

应看到 `enable_smep_smap()` 先查 CPUID.07H:EBX(SMEP=bit7、SMAP=bit20),**支持才** `cr4 |= (1ULL<<20)` / `(1ULL<<21)`,最后写回 CR4。注释会说明「写不支持的 CR4 位会 #GP」——这就是为什么不能像 NX 那样无条件设。

再确认它 per-CPU 调够:BSP 一次 + 每个 AP 一次:

```bash
grep -n 'enable_smep_smap' kernel/main.cpp kernel/arch/x86_64/ap_main.cpp
```

应各命中一次(`main.cpp:119` BSP、`ap_main.cpp:141` 每个 AP)。CR4 每核独立,漏一个核那个核就没保护。

> **在 WSL2 上你会看到什么**:跑 `test_f9_nxe_smep_smap_enabled` 仍 PASS——因为它对 SMEP/SMAP 是 **CPUID-跟随** 断言(CPUID 不报支持就不断言,也不断错)。但若你手写一段读 CR4 看 bit20/21,在 WSL2 上会是 0(CPUID.07H:EBX=0 → enable 逻辑跳过)。这不是 bug,是 `-cpu max` 在嵌套 KVM 下藏了 CPUID leaf 7。换 `-cpu host`(非嵌套)或 TCG,bit20/21 就设上。

### 3. stac/clac 挂满所有用户态入口

SMAP 要生效,所有「从用户态进来、handler 要碰用户内存」的入口都得 `stac`(放行)/`clac`(关上)。核这两处:

```bash
# SYSCALL 入口(必从用户来,无条件 stac/clac)
grep -n 'stac\|clac' kernel/arch/x86_64/syscall.S
# 中断入口(三个 ISR 宏,挂在「从用户态来」的分支,同 swapgs 条件)
grep -n 'stac\|clac' kernel/arch/x86_64/interrupts.S
```

`syscall.S` 应有 entry `stac`(`:57`)+ exit `clac`。`interrupts.S` 应在**每个** ISR 宏(NOERRCODE/ERRCODE/IRQ)里各看到一对 `stac`(`:75`/`:173` 起)+ `clac`(`:98` 起)。关键是确认它们挂在 `testb $3,%al` 的**用户态分支**(和 `swapgs` 同条件)——不是无条件。内核态中断不该动 AC,否则打断 `copy_from_user` 时把嵌套的 AC 清了。

### 4. 三个位都开,跑真程序不炸

这是「开完不破坏正常路径」的硬验证——SMAP 尤其,漏一个 `stac` 就 #PF:

```bash
cmake --build build --target run-kernel-test 2>&1 | tail -4
```

应是 `942 passed, 0 failed` + `ALL TESTS PASSED`(含本章新增的 `test_f9_nxe_smep_smap_enabled`)。任何 `#PF` / `panic` 都说明某个访用户入口漏了 `stac`。

再起一次真 GUI/shell 冒烟(默认配置 USB+GUI 开):

```bash
cmake --build build --target run 2>&1 | tail -20
```

应看到 shell / GUI Desktop / xHCI keyboard 正常启动,**零 panic / #PF / SIGSEGV**。SYSCALL 入口的 `stac` 护住了「shell 读用户内存」这条最高频路径——这条不炸,SMAP 覆盖基本就到位了。

> **在 WSL2 上**:SMAP 没真生效(CR4[21] 没设),`stac`/`clac` 是 NOP,所以这一步在 WSL2 上验的是「asm 写对了、没把别的东西弄崩」,不是「SMAP 真拦了什么」。真机/TCG 上 SMAP 活了,这一步才是「SMAP 覆盖全、真程序还能跑」。

## 验收清单

- [ ] `test_f9_nxe_smep_smap_enabled` PASS:EFER.NXE(bit 11)设上(无条件,baseline)。
- [ ] `enable_smep_smap()` 是 CPUID.07H:EBX-gated(SMEP[7]/SMAP[20] 支持才设 CR4[20]/[21]);BSP + 每 AP 各调一次。
- [ ] `stac`/`clac` 挂满 SYSCALL 入口 + 三个 ISR 宏,且在「从用户态来」分支(同 swapgs 条件),不是无条件。
- [ ] `run-kernel-test` 942 passed / 0 failed + `make run` 起 shell/GUI 零 panic/#PF(三个位开完不炸)。
- [ ] 知道 WSL2 上 SMEP/SMAP 是「代码对但跳过」,NX 是「真生效」——别把环境限制误判成 bug。

## 别做这些

- **别**在本机(WSL2)指望看到 SMEP/SMAP「拦了一次攻击」——CPUID.07H 不透传,enable 逻辑正确跳过,CR4[20]/[21] 是 0。看不到不是没做对,是环境没给条件。验 SMEP/SMAP 真生效得上真机 / 完整 KVM(`-cpu host`)/ TCG。
- **别**把 NX 和 SMEP/SMAP 的 gate 策略搞混:NX 无条件设(x86_64 baseline),SMEP/SMAP 必须 CPUID-gate(写不支持位 #GP)。这是三个位待遇不同的根因。
- **别**给 `stac`/`clac` 用无条件写法——必须挂在 `testb $3,%al` 的用户态分支。无条件 `clac` 会清掉内核态嵌套中断继承的 AC,破坏 `copy_from_user` 期间的状态。
- **别**以为 SMAP 开了 `copy_from_user` 就完整了——现在还是 `validate_user_ptr`(查 canonical)+ 直接解引用,靠 syscall `stac` 放行;带 exception table 的容错 `copy_from_user` 是后面的事(F10/extable)。
