---
title: 参考 · 中断与异常:IDT、8259A PIC、8254 PIT 与 ISR 栈帧
---

# 参考 · 中断与异常:IDT、8259A PIC、8254 PIT 与 ISR 栈帧

> 查阅层。这一页是 Cinux 中断子系统的速查表,不按 tag 组织,给后续每一章(键盘 014、鼠标 030、调度器 020、系统调用 023、CoW page fault 035……)查向量号、门描述符布局、EOI 规则、ISR 栈账用。实现以最终 tag `035_multi_terminal` 的源码为准;某个特性是哪一 tag 引入的,在行内点出。
>
> 范围:CPU 异常(0–31)+ 8259A PIC 重映射后的硬件 IRQ(0x20–0x2F)+ 8254 PIT 节拍。**不含 APIC/IOAPIC、不含 MSI、不含中断虚拟化**——Cinux 全程用经典 8259A。

## 子系统地图

```text
   CPU 异常 (#DE..#PF, vector 0..31)        硬件设备 (键盘/鼠标/RTC/IDE…)
        │  CPU 自动压栈 + 查 IDT                    │  拉低 8259A IRQ 线
        ▼                                            ▼
   ┌─────────────────────────┐            ┌──────────────────────────┐
   │  IDT (256 × 16B 门描述符) │            │  8259A PIC (主 0x20/从 0xA0) │
   │  vector → stub + selector│            │  重映射:IRQ0-7→0x20-0x27    │
   │               + type_attr│            │         IRQ8-15→0x28-0x2F   │
   └────────────┬────────────┘            └──────────────┬───────────┘
                │  jmp stub                              │  投递 INT vector
                ▼                                        ▼
   ┌─────────────────────────────────────────────────────────────────┐
   │  ISR stub (interrupts.S):压假错误码 + 存 15 GPR + 对齐 padding      │
   │     leaq 8(%rsp),%rdi  →  call C handler(InterruptFrame*)        │
   └─────────────────────────────────┬───────────────────────────────┘
                                     ▼
              C handler(exception_handlers.cpp / irq_handlers.cpp / 各驱动)
                                     │  末尾必须 PIC::send_eoi(irq)(IRQ 才需要)
                                     ▼
              stub 恢复 GPR、弹错误码、iretq
```

调用方依次:`GDT::init` → `IDT::init`(装异常门)→ `PIC::init`(重映射,默认全 mask)→ 注册 IRQ stub → `PIC::unmask(irq)` → `sti`。PIT/键盘/鼠标各自 `init` 后再 `unmask` 对应 IRQ。

## 异常向量表

`idt.hpp` 的 `ExceptionVector` 枚举 + `idt.cpp` 的路由表(`IDT::init` 里那张 data-driven 表):

| 向量 | 助记 | 名称 | 错误码 | 门类型 | 特权级 | IST |
|---|---|---|---|---|---|---|
| 0 | #DE | Divide Error | 否 | Interrupt | Kernel | 0 |
| 1 | #DB | Debug | 否 | **Trap** | Kernel | 0 |
| 2 | — | NMI | 否 | Interrupt | Kernel | 0 |
| 3 | #BP | Breakpoint (INT3) | 否 | **Trap** | **User (DPL3)** | 0 |
| 4 | #OF | Overflow | 否 | Interrupt | Kernel | 0 |
| 5 | #BR | BOUND Range | 否 | Interrupt | Kernel | 0 |
| 6 | #UD | Invalid Opcode | 否 | Interrupt | Kernel | 0 |
| 7 | #NM | Device Not Available | 否 | Interrupt | Kernel | 0 |
| 8 | #DF | Double Fault | **是** | Interrupt | Kernel | **1** |
| 10 | #TS | Invalid TSS | 是 | Interrupt | Kernel | 0 |
| 11 | #NP | Segment Not Present | 是 | Interrupt | Kernel | 0 |
| 12 | #SS | Stack-Segment Fault | 是 | Interrupt | Kernel | 0 |
| 13 | #GP | General Protection | 是 | Interrupt | Kernel | 0 |
| 14 | #PF | Page Fault | 是 | Interrupt | Kernel | 0 |

门类型策略(设计决定):**#BP(3) 与 #DB(1) 用 Trap 门(进入门时 IF 保持)**;其余异常一律 Interrupt 门(进入即 `cli`,IF 清零)。向量 9(协处理器段越界)在 64 位下已废弃,路由表不注册。`#BP` 是唯一一个 DPL=3 的异常——这样才能让 ring-3 的 `int3` 陷进来。

> **诚实点:只有 `#DF` 用了 IST 1(独立栈)。** `#PF`(14)在 tag 035 仍是 IST 0、走当前栈。`document/notes/030/` 里设想的「`#PF` 用 IST2 + guard page」修法**在最终 tag 仍未落地**——`split_2mb_page`/unmap 无调用点。引用 guard page 机制前,先 `git show <tag>:kernel/arch/x86_64/idt.cpp` 核对 `#PF` 那行的 `ist` 字段。

## IDT 门描述符(每项 16 字节)

`IDT::Entry`(`[[gnu::packed]]`,`static_assert(sizeof(Entry)==16)`):

| 字段 | 位宽 | 说明 |
|---|---|---|
| offset_low | 16 | handler 地址低 16 位 |
| selector | 16 | 段选择子(Cinux 用 `GDT_KERNEL_CODE`) |
| ist | 8 | IST 索引(0 = 不切换栈) |
| type_attr | 8 | P/DPL/Type,见下 |
| offset_mid | 16 | handler 地址中 16 位 |
| offset_high | 32 | handler 地址高 32 位 |
| reserved | 32 | 恒 0 |

`type_attr` 由 `make_idt_attr(priv, gate)` 拼:`0x80 | priv | gate`,其中 `0x80` 是 present 位。常见组合:

| 组合 | type_attr | 含义 |
|---|---|---|
| Kernel + Interrupt | `0x8E` | ring0,中断门,清 IF |
| Kernel + Trap | `0x8F` | ring0,陷阱门,保 IF(#DB) |
| User + Trap | `0xEF` | ring3,陷阱门(#BP,允许用户态 `int3`) |
| User + Interrupt | `0xEE` | ring3,中断门(系统调用若用 `int` 指令会走这个) |

IDT 加载:`IDT::load()` 执行 `lidt`(64 位 `idtr` = 16 位 limit + 64 位 base)。`IDT::kMaxEntries = 256`。

## 8259A PIC

端口(`PicPort`):

| 端口 | 用途 |
|---|---|
| `0x20` / `0xA0` | 主 / 从 PIC 命令口(也是 EOI 口) |
| `0x21` / `0xA1` | 主 / 从 PIC 数据口(IMR 中断屏蔽寄存器 / ICW2-4) |

`PIC::init(master_offset=0x20, slave_offset=0x28)` 发 ICW1-ICW4 把两片 PIC 重映射:

- 主片 IRQ0-7 → INT `0x20-0x27`
- 从片 IRQ8-15 → INT `0x28-0x2F`(从片级联在主片 IRQ2)
- 8086 模式、**手动 EOI**(不用 auto-EOI)

常用 IRQ 号(重映射后):

| IRQ | INT | 设备 | 谁用 |
|---|---|---|---|
| 0 | 0x20 | PIT channel 0 | PIT 节拍 / GUI tick |
| 1 | 0x21 | 键盘 PS/2 | Keyboard (014) |
| 2 | 0x22 | 级联(从片) | — |
| 8 | 0x28 | RTC | — |
| 12 | 0x2C | 鼠标 PS/2 (AUX) | Mouse (030) |
| 14 | 0x2E | 主 IDE | — |

EOI 规则:`PIC::send_eoi(irq)`——**传的是硬件 IRQ 号(0-15),不是 INT 向量**。从片 IRQ(8-15)要同时给从片和主片发 EOI;主片 IRQ(0-7)只给主片。**每个 IRQ handler 末尾必须发 EOI,否则下一次中断永远不再投递**——这是 Cinux 里反复踩的坑(键盘、鼠标、PIT 都中过)。

`PIC::mask(irq)` / `unmask(irq)` 改 IMR 对应位;`disable_all()` 写 `0xFF` 到两片数据口。`init` 后默认全 mask,用到哪个再 `unmask`。

## 8254 PIT(channel 0)

端口(`PitHW`):

| 端口 | 用途 |
|---|---|
| `0x40` | channel 0 数据口(→ IRQ0) |
| `0x41` | channel 1(内存刷新,别碰) |
| `0x42` | channel 2(PC speaker) |
| `0x43` | 命令/模式寄存器 |

`PIT::init(freq_hz=100)` 发命令 `0x36`(channel 0 | LSB-then-MSB | 方波 mode 3 | 二进制)到 `0x43`,再把 16 位 divisor(=`1193182 / freq_hz`)按低字节、高字节顺序写进 `0x40`。100 Hz 对应 divisor ≈ 11931,即每 10 ms 一次 IRQ0。频率范围 ~19 Hz(divisor=65535)到 1193182 Hz(divisor=1)。

`PIT::irq0_handler` 递增全局 `tick_count_`(`std::atomic`),每 `freq_hz` 次打印一次 uptime,末尾 `send_eoi(0)`。GUI 构建下还能 `set_tick_callback(cb, ctx)` 注册每 tick 回调(`029` 用来 flip 画布、`030` 用来排空 GUI 事件队列 + composite)。注意:回调跑在 IRQ0 中断上下文里,不能阻塞、不能长拷贝。

## 中断标志(IF)控制

`irq.hpp` 是 `cli/sti/pushfq/popfq` 的薄封装,全部带 `"memory"` clobber(既是编译屏障):

| 函数 | 作用 |
|---|---|
| `irq_disable()` | `cli` |
| `irq_enable()` | `sti` |
| `irq_save()` | `pushfq; popq; cli`——返回旧 RFLAGS 并关中断 |
| `irq_restore(f)` | `pushq f; popfq`——恢复(含 IF) |
| `irq_enabled()` | 读 RFLAGS,查 bit `0x200`(IF 位) |
| `hlt()` | 暂停到下一个中断(调用前必须 `sti`,否则永远卡住) |

`irq_save()` / `irq_restore()` 是临界区标配:`{ auto f = irq_save(); ...; irq_restore(f); }`。host 测试(`CINUX_HOST_TEST`)下这些全是 no-op。

## ISR stub 与栈帧(`interrupts.S`)

两个宏:`ISR_NOERRCODE`(CPU 不压错误码,stub 自己压个 dummy 0)与 `ISR_ERRCODE`(CPU 已压错误码)。两者都:

1. 存 15 个 GPR(push 顺序 rax,rbx,rcx,rdx,rbp,rsi,rdi,r8-r15);
2. `push $0` 垫 8 字节对齐 padding;
3. `leaq 8(%rsp), %rdi`——`InterruptFrame*` 跳过 padding,指向最后压的 `r15` 字段;
4. `call` C handler;
5. `addq $8,%rsp` 弹 padding,`pop` 还原 15 GPR,`addq $8,%rsp` 弹错误码,`iretq`。

`InterruptFrame`(`[[gnu::packed]]`,字段从低地址到高地址):`r15,r14,r13,r12,r11,r10,r9,r8,rdi,rsi,rbp,rdx,rcx,rbx,rax,error_code,rip,cs,rflags,rsp,ss`。注意 struct 顺序与 push 顺序**相反**——最先 push 的 `rax` 在最高地址、排在 struct 末尾;`leaq 8(%rsp)` 指向最后 push 的 `r15`(最低地址、struct 开头)。

**栈对齐账(为什么要那 8 字节 padding):** System V AMD64 ABI 要求进入函数瞬间 `RSP ≡ 8 (mod 16)`。无错误码异常:CPU 压 5×8=40,stub 压 dummy 8 + 15 GPR 120 + padding 8 = 136,合计 176,`call` 再压 8 = **184**,`184 ≡ 8 (mod 16)` ✓。有错误码异常:CPU 压 6×8=48,stub 压 15 GPR 120 + padding 8 = 128,合计 176,`call` +8 = 184,同样 ✓。**没有这 8 字节 padding,handler 入口会落在 `RSP ≡ 0`,编译器一旦生成 `movaps` 等 16 字节对齐指令就 `#GP`**——这正是 tag 030 开机即 `#GP` 的根因(详见 [030-gp-stack-alignment.md](../debug-notes/030-gp-stack-alignment.md))。

## 约束与边界(本子系统的真实限制)

- **手动 EOI,不用 auto-EOI。** 忘了 `send_eoi` → 该 IRQ 不再来;给没发生的 IRQ 发 EOI → 假中断(spurious IRQ7 / IRQ15)。
- **只有 `#DF` 用 IST 1。** 其余异常(含 `#PF`)IST 0、走当前栈。内核栈溢出会直接 triple fault,没有 guard page 兜底(修法见 notes,未落地)。
- **PIC 是 8259A,不是 APIC。** 只有 15 个可用 IRQ(IRQ2 被级联占)、无优先级动态分发、无多核投递。要 SMP 必须迁 APIC/IOAPIC。
- **PIT 回调在中断上下文。** `set_tick_callback` 注册的函数跑在 IRQ0 里,不能睡、不能 `new`、不能长拷贝(029 flip 的整帧 memcopy 其实是个隐患,真实系统该用下半部)。
- **异常打印用 `fatal_halt`。** 除 `#PF` 被 035 接进 CoW 处理外,大多数异常 handler 直接打印 `InterruptFrame` 后 `hlt` 死循环,不做恢复。
- `sti` 后立即 `hlt` 是节拍等待的常用模式;`cli` 后 `hlt` 会永久卡死。

## 验证入口

- host 单测:`ctest --test-dir build -R "gdt_idt|pic|pit" --output-on-failure`(`test/unit/test_gdt_idt.cpp`、`test_pic.cpp`、`test_pit.cpp`)。
- QEMU 机内测:`cmake --build build --target run-big-kernel-test`(`kernel/test/test_gdt_idt.cpp`、`test_pic_pit.cpp` 走真 IDT/PIC)。
- 可视化:`cmake --build build --target run`,看 `[TICK] uptime: Ns` 节拍输出、键盘/鼠标响应。

## 源码索引

- IDT:[idt.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/idt.hpp) / [idt.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/idt.cpp)(异常向量、门类型、IST 路由表)。
- ISR stub:[interrupts.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/interrupts.S)(`ISR_NOERRCODE`/`ISR_ERRCODE` 宏、对齐 padding)。
- 异常 handler:[exception_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/exception_handlers.cpp)。
- PIC:[pic.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/pic.hpp) / [pic.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/pic.cpp)。
- IRQ flag:[irq.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/irq.hpp)(`cli/sti/irq_save`)。
- IRQ handler 注册:[irq_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/irq_handlers.cpp)。
- PIT:[pit.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pit/pit.hpp) / [pit.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pit/pit.cpp)。

## 权威依据

- Intel SDM Vol 3,Ch 6(IDT、异常与中断、门描述符、IST)、Ch 8(双核异常 / `#DF` 与 TSS):门描述符 16 字节布局、P/DPL/Type 编码、IST 切栈规则、向量 0–31 分配。
- 8259A datasheet:ICW1-4 初始化序列、OCW2 EOI(`0x20`)、IMR 读写、级联。
- Intel 8254 datasheet:channel 0-2、命令字 `0x36`(channel 0 | LSB-MSB | mode 3)、基准时钟 1.193182 MHz。
- OSDev — [8259 PIC](https://wiki.osdev.org/8259_PIC)、[PIT](https://wiki.osdev.org/Programmable_Interval_Timer)、[Interrupt Descriptor Table](https://wiki.osdev.org/Interrupt_Descriptor_Table)。
- System V AMD64 ABI §3.2.2(The Stack Frame,handler 入口 `RSP ≡ 8 mod 16`):<https://gitlab.com/x86-psABIs/x86-64-ABI>。
