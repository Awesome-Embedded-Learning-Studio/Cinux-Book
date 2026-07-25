---
title: 03 · 调试现场:三连 bug 互相掩盖 + QEMU 不持久化 SFMASK
---

# 调试现场:三连 bug 互相掩盖 + QEMU 不持久化 SFMASK

这一章的两份笔记,一个是「三个 bug 互相掩盖」的连环雷,一个是「模拟器把你的测试骗了」的经典,都值得拆开讲。

## 案例一:进 Ring 3 卡在三连 bug

现象是 `launch_first_user()` 跑完,串口没有预期的 `#GP ... protection works`,反而吐出一串乱码:`[[[[[[[[[[[VMM] Demand-paged 0x... -> phys 0x...`。排查发现三个独立 bug 叠在一起,必须全修才进得了 Ring 3。

根因有仨。第一个,framebuffer 的 identity-mapping 在用户地址空间丢失——`AddressSpace` 只复制高半区,低半区的 1 GB 大页没了,`kprintf` 一写 console 就 `#PF`,需求分页又映个普通 RAM 页,fault handler 内部再 `kprintf` 形成重入,串口被搅烂。第二个,STAR 写入用 `shlq $32`——`wrmsr` 只认 32 位 `EDX`,`RDX` 高半区被丢,`STAR[63:48]=0`,SYSRET 推出 `CS=0x13`(数据段 + RPL3),CPU 在数据段上取指。第三个,`walk_level` 分配中间页表漏了 `FLAG_USER`,四级页表某级缺 user 位,Ring 3 访问被拒 `#PF(0x05)`。

定位过程的关键,是意识到这三个 bug **互相掩盖**。bug 一和 bug 三的 `#PF` 先发作,把 bug 二(STAR 错位)的症状盖住了——你以为修到一半能跑了,其实还差。只有把三个都修:framebuffer identity-mapping 复制进用户 PDPT、`shlq $32` 改成 `shlq $16`、`walk_level` 加 `user_flag` 逐级传,串口才干净地吐出 `Jumping to Ring 3` → `#GP CS=0x001b` → `protection works`。

防复发有三条。一是地址空间切换前,要把所有「内核隐式依赖但不在共享高半区」的映射(像 identity-mapped 的 MMIO)显式继承过去——这是「用物理地址直当虚拟地址」这种 identity mapping 方案在切 CR3 时的固有脆弱点。二是写 64 位 MSR 时心里始终记着 `wrmsr` 只认 `EDX:EAX`,对 `RDX` 做超过 32 位的移位是无效操作。三是 x86-64 权限检查遍历全部四级页表,任何分配新中间页表的代码路径都必须把 user 位传下去——别只在最终叶子节点上带。

## 案例二:QEMU 不持久化 SFMASK 写入

现象是机内测试 `test_sfmask_if_bit` 挂了,断言 `(sfmask & 0x200) == true` 失败:`wrmsr` 写 `0x200` 进 `IA32_FMASK` 后,`rdmsr` 读回是 0。同一函数里 STAR、EFER 的读写都正常。

定位链走得很扎实。先反汇编确认 `usermode_init_asm` 指令序列没错;再把 SFMASK 的写入挪到 EFER 之后,排除「EFER 的 `wrmsr` 覆盖了 SFMASK」;再换 C++ inline asm 直接写,排除汇编链接问题;再在测试函数体内写完立刻读,排除时序。全都失败。最后一步是关键证据:写全 `1`(`0xFFFFFFFF:0xFFFFFFFF`)进 SFMASK,正常触发 **#GP**。这说明 QEMU 认得这只 MSR、也做了合法性检查,只是对**合法值**(如 `0x200`)的写入静默丢弃——`wrmsr` 不报错,值却不落盘。KVM 和 TCG 两种后端一致,确认是 QEMU 本身的模拟行为。

根因是 QEMU 对 `IA32_FMASK` 的模拟不完整。修复不是去改 QEMU,而是改测试的断言口径:从「硬断言读回 `0x200`」改成「写 `0x200` 不触发 `#GP` 就算通过」——能走到 `wrmsr` 之后这一行,就证明指令编码正确。注释里写明:真硬件上 `rdmsr` 应读回 `0x200`。

提炼成教训有两条。一是测试要会区分「模拟器限制」和「真 bug」——当测试结果和代码正确性明显矛盾时,先在模拟器层面排除干扰(这里就是用「写全 1 触发 #GP」反证编码没错)。二是回到设计本身:`SFMASK` 只影响 SYSCALL 方向,而本 milestone 只走 SYSRET、SYSRET 从 `R11` 恢复 `RFLAGS`,所以这只 MSR 写不写得进去,对功能没有任何影响——这个「白写」的事实,恰恰让我们能放心地把测试断言放宽。
