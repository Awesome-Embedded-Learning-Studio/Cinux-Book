---
title: 02 · AP boot trampoline:把第二个核从实模式拉到长模式
---

# AP boot trampoline:把第二个核从实模式拉到长模式

## AP boot trampoline:把第二个核从实模式拉到长模式

x86 多核启动的规矩是:第二个核(AP,Application Processor)上电后停在 halt,等第一个核(BSP)给它发 SIPI;SIPI 里带一个向量,AP 就从「向量 × 4KB」那个物理地址开始执行,而且**是从 16 位实模式开始**。所以这个入口地址上,必须放着一段能把 AP 从实模式一路带到长模式的代码——这就是 trampoline(跳板)。

这段跳板在 `ap_trampoline.S`,BSP 把它拷到物理 `0x8000`,然后发 SIPI 向量 `0x08`(0x08 << 12 = 0x8000)。AP 醒来后经历三个阶段,每个阶段往 debugcon(端口 0xE9,落到 `build/debug.log`)打一个标记(`1`/`2`/`3`),triple fault 时就能看出走到哪步:

- **16 位实模式**:清段寄存器、开 A20(端口 0x92,保险)、加载一个临时 GDT、置 CR0.PE,然后 `ljmp` 到 32 位。
- **32 位保护模式**:置 CR4.PAE、把 BSP 准备的临时 PML4 物理地址写进 CR3、置 EFER.LME、重载 GDT(这次带上 64 位代码段)、置 CR0.PG,`ljmp` 到 64 位。
- **64 位长模式**:把 BSP 塞进来的四个参数(AP 栈、内核 PML4、cpu_id、入口地址)读进寄存器,然后 `jmp` 到内核镜像里的 `ap_entry_long`。

跳板有个汇编技巧值得专门讲:**它被链接在更高半的内核镜像里,却在物理 `0x8000` 执行**。所以一个标号运行时的物理地址是 `(标号 - ap_trampoline_start + 0x8000)`。GAS 会把「标号减 start」这部分(两个符号都在同一个 section 里)折叠成一个常量,于是每个这样的表达式都是一个绝对立即数——不用运行时修址、不用改 linker script。访存时把这个常量装进寄存器,再用寄存器间接寻址(`lgdt`/`mov` 没法直接吃这个立即数)。

为什么要先在 `0x8000` 跑一段、再跳到 `ap_entry_long`?因为 CR3 切换有讲究。临时页表既 identity-map 了低内存(让 `0x8000` 这段跳板能跑),又镜像了更高半内核镜像(让 `ap_entry_long` 也能跑)。但 AP 一旦切到真正的内核 PML4,`0x8000` 就不再映射了。所以切 CR3 这一步必须在「两边都映射」的 `ap_entry_long` 里做,不能在 `0x8000` 里做:

```asm
ap_entry_long:
    movq    %r12, %cr3     # 切到真正的内核页表
    movq    %rbx, %rsp     # 设好 AP 的内核栈
    call    ap_main        # 不返回
```

## BSP 侧:怎么把 AP 喊起来

BSP 的活儿在 `boot_aps()`(`ap_main.cpp`)。它先准备两份页表和一份跳板,再逐个把 AP 喊起来。

**临时页表**(`build_ap_temp_page_tables`):分配三个页(PML4/PDPT/PD),identity-map 物理 0..64 MB(2 MB 大页),再在更高半(`P4[511]`/`PPT[510]`)镜像同一份 PD——让 AP 在 `0x8000` 跑跳板时、以及跳到更高半 `ap_entry_long` 时,地址都通。这份临时页表用完就扔,AP 切到内核 PML4 后再也不碰它。

**塞参数**:`copy_trampoline_to_lowmem` 把跳板拷到 `0x8000`,`inject_param` 往跳板的数据区里写四个值——AP 栈顶、临时 CR3、内核 CR3、`ap_entry_long` 地址、cpu_id。BSP 知道每个 AP 该用第几号 per-CPU 块(`percpu_blocks[cpu]`),就把这个索引塞进去。

**INIT-SIPI-SIPI**:`send_init` → 忙等 → `send_sipi(vec=0x08)` → 忙等。第二条 SIPI 是兜底:只有当第一条没把 AP 带起来(看 `g_aps_online` 计数没涨)才补发,免得 AP 已经 online 并 halt 之后,再来一条 SIPI 让它把跳板重跑一遍、炸掉。然后 BSP 在一个有上限的循环里 `pause` 自旋,等这个 AP 原子地给自己 `++g_aps_online` 报到。

## AP 侧:ap_main 怎么接入内核

AP 进了长模式、切好 CR3 和栈,就 `call ap_main(cpu_id)`。这是 AP 的 C 入口,干七件事(`ap_main.cpp`):

1. **锚定 GS**:`write_msr(GS_BASE, &percpu_blocks[cpu_id])`、`write_msr(KERNEL_GS_BASE, 0)`。从此这个核的 `percpu()` 读 `MSR_GS_BASE` 拿到自己的块。这一步的顺序是后面迁移 GP 的关键,先记着。
2. **装本核 GDT + 共享 IDT**:`gdt_blocks[cpu_id].init()`(每核独立 TSS/IST)、`g_idt.load()`(IDT 全局共享)。
3. **开 LAPIC**:读自己的 apic_id 存进 per-CPU 块,`enable(0xFF)`(伪中断向量跟 BSP 一致)。LAPIC 的 MMIO 窗口是「谁访问就解码到谁的本地 APIC」,所以同一份 `g_lapic` 驱动能驱动每个核自己的 LAPIC。
4. **向 BSP 报到**:`__atomic_add_fetch(&g_aps_online, 1, SEQ_CST)`。BSP 那边正在自旋等这个计数。
5. **等调度器就绪**:`cli;pause` 自旋等 `Scheduler::is_initialized()`。注意是 `cli;pause` 不是 `sti;hlt`——因为 `boot_aps` 跑在 `Scheduler::init` **之前**,init 不发 IPI,`hlt` 会一直睡死。init 几微秒就好,所以这个自旋极短。
6. **建本核 idle**:`setup_ap_idle(cpu_id)` 给这个 AP 造一个专属 idle 任务(入口 `ap_idle_entry`),`current()` 设成它。必须先有个合法的 current,第一次 `context_switch` 才不会被 `schedule()` 里「current()==nullptr 直接返回」的早出口挡掉。
7. **切过去**:`context_switch(&dummy, &ap_idle->ctx)`,跳到 `ap_idle_entry` 自己的栈上,AP 再也不回 `ap_main`。
