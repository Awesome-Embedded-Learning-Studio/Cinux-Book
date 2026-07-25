---
title: 04 · 调试现场:DS、远跳、译码宽度、#GP
---

# 调试现场:DS、远跳、译码宽度、#GP

进 PM 这一段是 Cinux 踩坑最密集的地方,因为太多东西(寻址模型、译码宽度、CS 刷新)要在几条指令内一起转过来。下面是几个真实调出来的。

**症状一**——`lgdt` 之后莫名其妙崩。 几乎都是 `DS` 没清零。实模式 `lgdt` 按 `DS<<4+偏移` 取 GDTR 地址,`DS` 还是脏的就读到错内存。修复就是 `lgdt` 前那两行 `movw $0,%ax; movw %ax,%ds`。判断:GDB 里在 `lgdt` 前后看 `GDTR`(`info registers` 或 `monitor`),limit 应该是 23、base 应该落在 `0x81xx`;要是 base 一眼不对,就是 `DS` 的问题。

**症状二**——置了 `CR0.PE`,程序原地三重故障重启。 多半是漏了 far jump,或者 far jump 的编码不对。置 PE 之后 CPU 仍在 16 位译码,没有远跳刷新 `CS`,后面那条 `.code32` 编码的指令被当 16 位解码,几条就崩。修复就是老老实实 `ljmp $0x08, $pm_entry`。**别手拼机器码**(`ea <off16> <seg16>`)——GAS 在 `.code16` 下会自动生成正确的 16 位远跳编码,手拼反而容易错。

**症状三**——GDB 报 "Invalid register `ip`",或者反汇编出一堆 `(bad) + rex`。 这是经典的"译码宽度对不上"。要么是 `.code16`/`.code32` 放错了位置,要么是你给 GDB 喂的是 `stage2.bin`(裸二进制,没符号、没段信息)而不是 `stage2`(ELF)。**调试一律用 ELF**(`file build/boot/stage2`),`bin` 是给启动加载用的,两者不能互相替代。`ip`(实模式)变 `eip`(PM)的切换点正好在 far jump,跨过这条线 GDB 的寄存器名会变,这也是判断"是否真的进了 PM"的一个旁证。

**症状四**——GDT 看着填了,但一访问段就 #GP。 `lgdt` 不校验 GDT 内容,错要等到用选择子时才暴露。常见是 access byte 某一位算错(比如把代码段的可执行位弄没了),或者 limit 算成 `gdt_end - gdt`(忘了 `-1`)。对着 Intel SDM 的段描述符位定义再核一遍 base/limit/access/flags 四组字节,别凭感觉。
