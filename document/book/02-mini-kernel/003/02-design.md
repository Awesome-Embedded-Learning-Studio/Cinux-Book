---
title: 02 · 设计图:GDT/IDT/ISR 协作
---

# 设计图:GDT/IDT/ISR 协作

先看这三张表怎么协作。一次 `int $3` 的完整旅程:

```text
int $3 指令
   │  CPU 查 IDT[3] → 得到 isr_bp_stub 地址 + 代码段选择子
   ▼
isr_bp_stub (汇编)
   ├─ push $0              ← 补一个伪错误码(#BP 没有硬件错误码,补 0 保持栈帧统一)
   ├─ push rax, rbx, ... r15   ← 保存全部通用寄存器(构造 InterruptFrame)
   ├─ mov %rsp, %rdi       ← InterruptFrame* 作为第一参数
   ├─ call handle_bp       ← 进 C
   │     └─ dump 寄存器到串口,返回
   ├─ pop r15..rax         ← 恢复寄存器
   ├─ add $8, %rsp         ← 跳过那个伪错误码
   └─ iretq                ← 返回被中断的代码,继续往下跑
```

再看 IDT 里一个"门"长什么样(16 字节),关键字段是处理程序地址(拆三段存)和 `type_attr`:

```text
IdtEntry (16 字节)
  offset_low [0:15] ┐
  offset_mid [16:31]├─ 处理程序地址(64 位,拆三段)
  offset_high[32:63]┘
  selector          ← 代码段选择子(指 GDT 的 code64 = 0x08)
  ist               ← IST 偏移(本章用 0)
  type_attr         ← P|DPL|0|GateType
  reserved
```

`type_attr` 这一个字节决定门的关键行为,本章用两个值:#BP 用 `0x8F`(陷阱门),#PF 用 `0x8E`(中断门),差别下面讲。
