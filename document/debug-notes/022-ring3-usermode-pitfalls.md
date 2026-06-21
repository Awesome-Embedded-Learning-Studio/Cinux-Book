---
title: 调试档案 022 · 第一次跳进 Ring 3 的两个坑
tag: 022_ring3_usermode
---

# 调试档案 022 · 第一次跳进 Ring 3 的两个坑

> 从 `document/notes/022/001_usermode_three_bugs.md`、`002_sfmask_qemu_msr.md` 提炼,配套 [022 · 第一次跳进 Ring 3:用户态与特权隔离](../book/07-userland/022-ring3-usermode.md)。022 把内核从「只有 Ring 0」推到「能真正进 Ring 3、并被特权隔离弹回来」。过程里两个坑最典型:一个是「进不去 Ring 3,串口炸成乱码」——其实是三个 bug 叠在一起互相掩盖,缺一不可地全修才进得去;一个是「SFMASK 写进去读回来是 0」——看着像 bug,其实是 QEMU 对这只 MSR 模拟不完整,得在测试层把"模拟器限制"和"真 bug"分清。两条都值得记成档案。

## 案例一:SYSRET 之后串口吐一串 `[`,根本进不了 Ring 3

- **症状**:`launch_first_user()` 执行后,串口没打出预期的 `#GP from user mode`,反而吐出一坨乱码,预期应该是 SYSRET 进入 Ring 3 → 用户代码第一条 `cli` 触发 #GP → 打寄存器转储并 halt:

  ```text
  [USER] Setting up first user-mode program...
  [[[[[[[[[[[[[[[[[VMM] Demand-paged 0x00000000FD08F000 -> phys 0x0000000001089000
  V[[[[[[[[[[[[[
  ```

  注意两个细节:一是满屏 `[` 重复——这正好是 `[USER]`、`[VMM]` 那个方括号被反复打印;二是中间夹了 `Demand-paged ...` ——这是 `handle_pf` 里的 `kprintf`。两个细节合起来指向同一个方向:console 的写路径本身在崩。
- **根因**:**三个独立 bug 叠加,任何一个单独修都不够**。下面逐个拆。
  - **Bug A —— framebuffer 的 identity mapping 在用户地址空间里消失了。** 022 之前,内核的 framebuffer/MMIO 靠 `map_mmio()` 用 1 GB 大页做 identity mapping(物理地址直接当虚拟地址,挂在 `PDPT[3]`)。而 `AddressSpace` 构造时只复制 PML4 的高半区(`PML4[256..511]`,内核共享部分),低半区全清零。于是 `launch_first_user` 里 `user_space.activate()` 一切 CR3,`PDPT[3]` 那个 1 GB 大页就没了。后果是连环的:`kprintf` 往 console 写 → 写到那个虚拟地址触发 #PF → `handle_pf` 走 demand-page,给它**随便分配了一个普通 RAM 页**并 map 上去 → 写进的是个无关地址,console 当然不亮;更要命的是 `handle_pf` 内部又调 `kprintf` 打 `Demand-paged ...` 这条日志,而 console 还是坏的,于是 #PF handler 自己又 #PF,**重入**。串口那串 `[` 就是这么来的:`kprintf` 想打 `[USER]`,每次写到坏地址就 #PF,demand-page 又想打 `[VMM]`,如此循环,每条日志的第一个 `[` 被吐出来,后面就崩了。根子不在 console,在「切 CR3 把 identity mapping 切丢了」。
  - **Bug B —— 写 STAR MSR 用了 `shlq $32` 而不是 `shlq $16`。** `usermode_init_asm` 要把内核 CS 选择子 `0x08` 塞进 STAR 的 `[63:48]`(SYSRET 取这个字段算 user CS)。代码先 `movq $0x08, %rdx` 再 `shlq $32, %rdx`,想把 `0x08` 移到 RDX 的高 32 位。问题在于:`wrmsr` 只读 `EDX:EAX`——也就是只读 RDX 的**低 32 位**,高 32 位被直接丢弃。`shlq $32` 把 `0x08` 移到了 RDX 的 bit 32 以上,`wrmsr` 根本看不到:

    ```text
    预期:EDX = 0x00080008  →  STAR[63:48] = 0x08  →  SYSRET 后 CS = 0x1B(用户代码段)
    实际:EDX = 0x00000008  →  STAR[63:48] = 0x00  →  SYSRET 后 CS = 0x13(内核数据段 + RPL3)
    ```

    `0x13` 是数据段选择子,SYSRET 之后 CPU 拿着数据段当代码段去取指,必然出事。为什么一开始没发现?因为 Bug A 和 Bug C 先爆了 #PF,把 Bug B 盖住了——等到 A、C 都修好,SYSRET 真的能跳了,Bug B 才以 `CS=0x13 的 #PF` 的形式暴露。
  - **Bug C —— 四级页表中间层缺 `FLAG_USER`。** `VMM::map` 会在用户页上带 `FLAG_USER`,但 `walk_level` 内部分配**新的 PDPT/PD/PT 页**时,只写了 `FLAG_PRESENT | FLAG_WRITABLE`,没带 user 位。x86-64 的权限检查要**遍历全部四级页表**:哪怕最终的 PT 项带了 `FLAG_USER`,只要 PML4/PDPT/PD 里有一级的中间项缺 user 位,Ring 3 访问就会被拒。表现是用户代码页刚一取指就 #PF,错误码 `0x05`(P=1、W/R=0 即读、U/S=1 即用户态——页在,但权限不足)。

      ```cpp
      // 旧(错):分配中间页表时不带 user 位
      entry.raw = new_page | FLAG_PRESENT | FLAG_WRITABLE;
      ```

- **定位**:这案的关键是「别被乱码带偏」。串口乱码第一眼像 console/serial 驱动坏了,但 `[` 这个字符反复出现、且夹着 `Demand-paged` 日志,指向的是「console 写路径触发 #PF,而 #PF handler 自己又打日志又 #PF」的重入。顺这条线查 `handle_pf` 的 demand-page 分支,确认它在用户 CR3 下把一个**普通 RAM 页**映到了 framebuffer 的虚拟地址——这就把矛头指向 identity mapping 在切 CR3 后丢失。Bug C 的 `error_code = 0x05` 是另一条独立线索:`U/S=1` 说明是用户态访问被拒,且 `P=1` 说明页存在,那就是权限位问题,直接查 `walk_level` 有没有把 user 位传到每一级。Bug B 最隐蔽,要等 A、C 修好、SYSRET 真的跳了,看到 `CS=0x13` 才能抓住——所以这三 bug 的定位顺序通常是 A→C→B。
- **修复**:三处分别对症下药,合起来才进得了 Ring 3。
  - Bug A:在 `launch_first_user` 的 `activate()` **之前**,把内核 PDPT 里已经存在的 identity-mapping 条目复制到用户 PDPT,只复制用户 PDPT 里**还缺**的那些项(避免覆盖已经建好的用户映射):

    ```cpp
    auto* kern_pdpt = reinterpret_cast<const uint64_t*>(
        (kern_pml4[0] & ADDR_MASK) + KERNEL_VMA);
    auto* user_pdpt = reinterpret_cast<uint64_t*>(
        (user_pml4[0] & ADDR_MASK) + KERNEL_VMA);
    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        if ((kern_pdpt[i] & FLAG_PRESENT) && !(user_pdpt[i] & FLAG_PRESENT))
            user_pdpt[i] = kern_pdpt[i];
    }
    ```

    这样切了 CR3 之后,console 用的 1 GB 大页还在,`kprintf` 不再 #PF,重入循环断掉。
  - Bug B:`shlq $32` 改成 `shlq $16`,让 `0x08` 落在 `EDX[31:16]`(也就是 STAR 的 `[63:48]`),`wrmsr` 这次真能读到。当前 tag 的 `usermode.S` 里已经是 `shlq $16, %rdx`。
  - Bug C:给 `walk_level` 加一个 `user_flag` 参数(默认 0),`VMM::map` 从传入的 flags 里抠出 `FLAG_USER`,一路传到 PDPT/PD/PT 的分配:

    ```cpp
    uint64_t user_flag = flags & FLAG_USER;
    auto* pdpt = walk_level(pml4_table, PML4_INDEX(virt), true, user_flag);
    auto* pd   = walk_level(pdpt,     PDPT_INDEX(virt), true, user_flag);
    auto* pt   = walk_level(pd,       PD_INDEX(virt),   true, user_flag);
    ```

    `walk_level` 内部新建页表项时一律 `| user_flag`。
- **防复发**:三条经验,都该钉进 muscle memory。一是 **`wrmsr` 只认 32 位的 EDX:EAX**——对 64 位寄存器做移位再喂给 `wrmsr`,一定想清楚目标到底是 RDX 还是 EDX;往 STAR 这种 `[63:48]`/`[47:32]` 位域塞值,要么用 `shlq $16` 配 `orq` 在 EDX 内拼,要么干脆用 C++ 算好 64 位常量再拆高低写。二是 **x86-64 的 user 位是逐级检查的**,四级页表任何一级缺 user 位,Ring 3 都进不去——所以页表分配的内部函数必须把 user 标志当参数传下去,不能只在最末级 PT 项上设。三是 **identity mapping 和地址空间切换天然不合**:「物理地址直当虚拟地址」的方案,在 CR3 一换的瞬间就会断裂。凡是切了地址空间还要继续用的 MMIO/console 映射,要么在激活前显式继承,要么一开始就别用 identity mapping、改用高半区映射。这三条任意一条漏了,都会以「乱码」「#PF(0x05)」「CS 不对」这类看似不相干的症状出现,排查时先从这三类根因入手。

## 案例二:SFMASK 写进去读回来是 0,但 `wrmsr` 又不报 #GP

- **症状**:QEMU 机内测试 `test_sfmask_if_bit` 一上来就挂:

  ```text
  [RUN] test_msr::test_sfmask_if_bit
  [FAIL] (sfmask & 0x200) == true at kernel/test/test_usermode.cpp:125
  ```

  `usermode_init_asm` 里明明用 `wrmsr` 给 `IA32_FMASK`(0xC0000084)写了 `0x200`(屏蔽 IF 位),`rdmsr` 读回来却是 `0`。而同一段汇编里 STAR(0xC0000081)和 EFER(0xC0000080)的写/读都正常。也就是说,**唯独 SFMASK 这一只写丢了,且不报错**。
- **根因**:这不是代码 bug,是 **QEMU 对 `IA32_FMASK` 这只 MSR 的写入持久化模拟不完整**。验证下来,QEMU 既不是不认识这只 MSR、也不是乱报错,而是「认得、会做合法性检查、但把合法写入静默丢弃」:

  - `wrmsr` 写合法值(如 `0x200`):不触发 #GP,但值被丢,`rdmsr` 读回 0;
  - `wrmsr` 写非法值(如全 `1`):照常触发 #GP;
  - STAR、EFER 等其他 SYSCALL/SYSRET 相关 MSR 写入正常;
  - KVM 后端和 TCG 软件模拟后端**行为一致**,所以不是 KVM 的锅,是 QEMU 模拟层本身。

  好在它对本 milestone 无功能影响:`IA32_FMASK` 只在执行 **SYSCALL** 时用来决定清掉 RFLAGS 的哪些位;022 只用 **SYSRET** 单向进 Ring 3,SYSRET 是从 R11 恢复 RFLAGS、不读 SFMASK,所以这只 MSR 写没写进去,都不影响进 Ring 3 和特权隔离的验证。
- **定位**:这案的排查链很典型,值得记下顺序,因为它示范了「怎么把模拟器限制和真 bug 区分开」。
  1. 先反汇编 `usermode_init_asm`,确认 `movabs $0xc0000084,%rcx / xor %rdx,%rdx / mov $0x200,%rax / wrmsr` 这段指令序列本身没写错——指令是对的。
  2. 把 SFMASK 的写入挪到 EFER 之后(最后执行),排除「EFER 的 `wrmsr` 覆盖了 SFMASK」——挪完仍然失败,排除顺序依赖。
  3. 改用 C++ inline asm 在 `usermode_init()` 里直接写 SFMASK,排除「.S 汇编文件链接/符号问题」——仍然失败。
  4. 在测试函数体内「写完立即读回」,排除时序——读回仍是 0。
  5. **关键一步**:写全 `1`(非法值)进去,结果正常触发 #GP。这一步把案子翻过来了:非法值会 #GP、合法值不 #GP 但不持久,说明 QEMU **认识**这只 MSR 并且做了合法性检查,只是**丢掉了合法写入**。如果指令编码错了,合法值也应该 #GP;现在合法值不 #GP,就反证了「指令没错,锅在模拟器」。
  6. 最后用 `-accel tcg -cpu max -vga std` 跑一遍,排除 KVM 虚拟化层——TCG 也复现,坐实是 QEMU 本身的模拟行为。
- **修复**:既然是模拟器限制而不是代码错,修法不是改内核,而是**把测试从「硬断言读回值」改成「验证写合法值不触发 #GP」**。`test_sfmask_if_bit` 现在只做 `wrmsr 0x200`,只要没 #GP 就算通过——能走到测试函数末尾,就说明指令编码正确、CPU 接受了这个合法值:

  ```cpp
  void test_sfmask_if_bit() {
      // 写 0x200 不应触发 #GP;非法值(如 0xFFFFFFFF)才会。
      __asm__ volatile(
          "movl $0xC0000084, %%ecx\n\t"
          "xorl %%edx, %%edx\n\t"
          "movl $0x200, %%eax\n\t"
          "wrmsr\n\t"
          ::: "rax", "rcx", "rdx"
      );
      // 走到这儿说明 wrmsr 接受了 0x200,指令编码无误。
  }
  ```

  在真实硬件上,这只 MSR 的写入会正常持久化,`rdmsr` 应读回 `0x200`;真要在真机上加严校验,可以套一层 `#ifndef __QEMU__` 守卫做读回断言,但本 tag 不做。
- **防复发**:两条。一是 **测试结果和代码正确性矛盾时,先在模拟器层面排除干扰**——不是所有 `wrmsr` 不报错就代表写入生效,QEMU 对部分 MSR(尤其是冷门的 `IA32_FMASK`)的模拟并不完整。判断「指令对不对」的一个好招是写一个**明知非法的值**看会不会 #GP:非法值 #GP、合法值不 #GP,就反证了指令编码正确,锅在模拟器。二是 **测试断言要对齐这只 MSR 在当前设计里的真实语义**:`IA32_FMASK` 只管 SYSCALL 方向,SYSRET 从 R11 恢复 RFLAGS 不读它;022 是 SYSRET-only 的演示,SFMASK 写没写进去对功能没影响,所以测试断「不 #GP」是恰当的,既覆盖了「指令编码正确」这个唯一需要验证的点,又不被模拟器的已知缺陷卡住。

## 附:几个当下不发作、迟早要踩的隐形坑

下面这几条在本 tag 的 demo 里不会爆(因为用户程序就四字节 `cli;hlt;jmp .-2`,触发 #GP 后直接 `fatal_halt`),但一旦后续真上系统调用、真跑多任务、真做异常返回,就会要命,先记在这里。

- **`launch_first_user` 把 `TSS.RSP0` 设成了当前 `rsp`。** 022 里 `GDT::tss_set_rsp0(kernel_rsp0)` 用的 `kernel_rsp0` 就是 `movq %rsp, ...` 取到的当前内核栈。这只在「用户态触发异常后直接 halt、不返回 Ring 3」的 demo 里够用。等 023 接上系统调用、需要从 Ring 3 进内核再**回** Ring 3 时,RSP0 必须指向一个稳定、专属、栈顶干净的内核栈,不能再随手拿当前 rsp——否则异常/中断在烂栈上再炸一次就是 #DF。
- **`launch_first_user` 不返回,`main.cpp` 里它后面的键盘 poll loop 不可达。** 用户代码 `cli` 一触发 #GP,`handle_gp` 走 `fatal_halt()` 永久 `cli;hlt`。所以 `main.cpp` 里 `Returned from user mode launch (unexpected)` 和后面的键盘轮询循环,在本 tag 是死代码,别以为「用户态跑完会回到 main 继续」。(顺带:`main.cpp` 头注释里还写着 `17. Scheduler init, create tasks`,但 Step 17 的 Scheduler init 在本 tag 的 diff 里**已经被删了**,实际只剩 `usermode_init` + `launch_first_user`;那条注释是没擦干净的遗留,别当成"调度器和用户态并存"的证据。)
- **`#PF` 的 demand-page 在用户态访问内核地址时会乱映。** Bug A 暴露的是一个更深的问题:`handle_pf` 的 demand-page 分支对任何「页不存在」的 #PF 都一视同仁地 `g_vmm.map` 一页普通 RAM 上去,不区分这个地址该不该被映、是用户态访问还是内核态访问。在本 demo 里它只是把 console 写歪了;等真正跑用户程序时,用户态访问一个本不该存在的地址,demand-page 却默默给它建了个映射,等于**隔离被偷偷打穿**。这条在后续做系统调用/真用户程序时必须收紧(demand-page 只该服务合法的、用户地址空间内的缺页)。

---

### 一句话总结

022 的两个坑,一个是「三个 bug 互相掩盖、得全修才进得了 Ring 3」——framebuffer identity mapping 切 CR3 后丢失引发 #PF 重入、STAR 用 `shlq $32` 写错位导致 `CS=0x13`、`walk_level` 中间页表缺 user 位导致 `#PF(0x05)`,分别修在「激活前复制 identity mapping」「`shlq $16`」「user 位逐级下传」;一个是「QEMU 对 SFMASK 写入静默丢弃」的模拟器限制——用「写全 1 触发 #GP」反证指令没错,再把测试从硬断言读回值改成「不 #GP 即通过」。前者是「x86-64 权限/地址空间切换」的必修课,后者是「测试要分清模拟器限制和真 bug」的典型样本。
