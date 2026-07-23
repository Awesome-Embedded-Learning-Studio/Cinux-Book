---
title: Lab 061 · SMAP-SMP + exception table 验证
---

# Lab 061 · SMAP-SMP + exception table 验证

> 对应 `document/book/16-security/061-smap-smp-extable.md`。验证档 **C 档**:这一章交付的是两件内部机制——「撤全局 stac、改 accessor」和「exception table 容错」——没有用户可见的新能力。所以验证靠「看见机制真的在」:context_switch 确实不存 RFLAGS(根因)、全局 stac 确实注释掉了、accessor 确实是 stac-rep-movsb-clac 小窗、`__ex_table` 在二进制里真有内容、accessor fault 真的返 false 而不是把内核炸了。最后一项是这一章的 punchline,亲手触发一次看效果。

## 目标

确认五件事:

1. **`CpuContext` 不存 RFLAGS**——这是「全局 stac 在 SMP 下崩」的物理根基,在汇编头注释里白纸黑字;
2. **入口级全局 stac 真撤了**(`syscall.S` / `interrupts.S` 注释掉);
3. **accessor 是一扇 `stac` → `rep movsb` → `clac` 的小窗**,且窗口挂了 `_ASM_EXTABLE` 注解;
4. **`__ex_table` 在内核二进制里真有内容**——不是空表,是几十条 `{fault_rip, fixup_rip}`;
5. **accessor fault 真走 exception table 返 false**(负测试 PASS),反汇编里 fixup 路径确实 `clac`。

## 步骤

### 1. 根因:CpuContext 不存 RFLAGS

```bash
sed -n '9,21p' kernel/arch/x86_64/context_switch.S
```

应看到 `CpuContext` 的布局:r15/r14/r13/r12/rbp/rbx/rsp/rip,加上 RESERVED 的 gs_base/kgs_base(标 per-CPU)、per-thread 的 fs_base。**没有 RFLAGS 这一格**。再看一行:

```bash
grep -n 'Clobbers: flags' kernel/arch/x86_64/context_switch.S
```

函数头明说 `Clobbers: flags`——context_switch 切任务时根本不保存标志寄存器。两个事实叠一起(RFLAGS.AC 是 per-CPU 位 + 切任务不存它),任务跨核迁移就丢 AC——这就是上一章那个「入口级全局 stac」在 SMP 下必崩的物理原因。不是某个 case 没覆盖,是模型本身跟 SMP 冲突。

### 2. 全局 stac 撤了

```bash
sed -n '58,62p' kernel/arch/x86_64/syscall.S
```

应看到那条 `stac` 被**注释掉了**,旁边写着「P3: global STAC removed -- SMAP real; user mem only via accessor stac」。再确认中断入口:

```bash
grep -n 'global STAC removed' kernel/arch/x86_64/interrupts.S
```

应命中三处(三个 ISR 宏:NOERRCODE / ERRCODE / IRQ,`:75` / `:173` / `:271`),都是从用户态分支的 `stac` 被注释。撤完之后,内核默认 AC=0,合法访问用户内存只能走 accessor 那扇小窗——这才是「SMAP 真生效」(真机/TCG 下)。

### 3. accessor 小窗 + extable 注解

```bash
sed -n '103,114p' kernel/arch/x86_64/user_access.hpp
```

应看到 `copy_to_user` 的内核态内联汇编:`stac` → `1: rep movsb` → `clac` → `jmp 3f`(成功路径),`2:` 是 fixup(`clac` + `xorl %k[ok]` 清失败),末尾 `_ASM_EXTABLE(1b, 2b)` 给那条 `rep movsb` 挂注解。注意这扇窗有多窄:开门、一条指令拷完整段、关门,中间没有任何会阻塞的东西——这是「accessor 窗口绝不跨 schedule」铁律的落点。

### 4. exception table 的 linker section

```bash
sed -n '70,80p' kernel/linker.ld
```

应看到 `__ex_table` section:`AT(ADDR - KERNEL_VMA)`、`ALIGN(8)`、`KEEP(*(__ex_table))` 防 gc,两头是 `__start___ex_table` / `__stop___ex_table` 符号。这一段就是收 `_ASM_EXTABLE` 那些注解的地方。

### 5. 二进制里的 `__ex_table` 有几条

这是这章最直观的一眼——亲手数表里有多少条记录:

```bash
nm build/kernel/big/big_kernel | grep '__ex_table'
```

应看到两个地址:`__start___ex_table` 和 `__stop___ex_table`(标 `R`,read-only section)。两个地址之差除以 16(每条记录两个 quad)就是条数。当下构建是 21 条——也就是有 21 个 accessor 实例(`copy_to/from_user` 内联到各 syscall 边界的地方)挂了注解。这个数不是固定的:加一个用 accessor 的 syscall,表就多一条。你自己构建出来数当下的数,对得上「每个 accessor 实例一条」就对了。

### 6. PF handler 接线

```bash
sed -n '198,213p' kernel/arch/x86_64/exception_handlers.cpp
```

应看到 `handle_pf` 读 CR2 之后、demand-page 之前,先判「内核态 fault(`cs & 3 == 0`) + `search_exception_tables(frame->rip)` 命中」,命中就把 `frame->rip` 改成 `fixup_rip` 直接 return。注意两个边界:用户态 fault(`cs & 3 != 0`)跳过这张表(走 demand-page,F2 lazy-allocation 不动);正常内核 fault 的 RIP 不是 accessor 指令,查表 miss,原逻辑不变。

### 7. punchline:accessor fault 真返 false

这一章的明星验证。先看负测试长什么样:

```bash
sed -n '151,165p' kernel/test/test_user_ptr.cpp
```

应看到 `test_copy_from_unmapped_returns_false`:一个 `uint8_t buf[8]`、一个 `unmapped_user = reinterpret_cast<void*>(0x7000000000ULL)`,`TEST_ASSERT_FALSE(copy_from_user(buf, unmapped_user, sizeof(buf)))`。地址不是随便挑的——`0x7000000000` 是刻意挑的高位用户地址,避开 identity/direct-map 覆盖区(物理 RAM 映射到 1 GB 往上),不然碰巧有映射,`rep movsb` 直接读成功不 fault,exception table 就没机会拦。

跑一下,看它 PASS:

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -iE 'unmapped|copy_from_unmapped' | head
```

应看到这一项 **PASS**。要是没有 exception table,这一下 `rep movsb` 第一字节就 #PF,内核 panic,测试根本到不了断言——所以这一项能 PASS,本身就证明 exception table 真的把 accessor fault 拦下来、改成返 false 了。

### 8.(加分)反汇编,亲眼看见两路 clac

想看得更实,反汇编一个 accessor 内联点:

```bash
objdump -d build/kernel/big/big_kernel | grep -B1 -A6 'rep movsb' | head -16
```

应看到(在 `sys_read` / `sys_write` / `sys_getdents` 等边界里)这么一段:

```
stac                       # 0f 01 cb   开窗
rep movsb (%rsi),(%rdi)    # f3 a4      单 fault 点(1:)
clac                       # 0f 01 ca   成功路径关窗
jmp +N                     # eb 06      jmp 3f
clac                       # 0f 01 ca   fixup 路径关窗(2:)
xor %r8d,%r8d              # 45 31 c0   清 ok = false
```

两条 `clac`:成功路径一条、fixup 路径一条。fixup 那条尤其要紧——fault 发生时 AC 还是 1,fixup 不 `clac` 就会 AC 泄漏成 SMAP 旁路。反汇编里它在,说明 fixup 真的把窗关上了。

## 验收清单

- [ ] `context_switch.S:9` 的 `CpuContext` 布局无 RFLAGS,`:39` 标 `Clobbers: flags`——AC 跨核丢失的物理根基坐实。
- [ ] `syscall.S:61` + `interrupts.S` 两处全局 `stac` 注释掉;accessor (`user_access.hpp:103`) 是 `stac`→`rep movsb`→`clac` 小窗。
- [ ] `linker.ld:76` 有 `__ex_table` section;`nm` 看到 `__start/stop___ex_table`,算出非零条数(当下 21)。
- [ ] `handle_pf`(`exception_handlers.cpp:198`)在内核态门查 extable,命中改 `frame->rip`;用户态 fault 跳过。
- [ ] `test_copy_from_unmapped_returns_false` PASS;反汇编 accessor 见两路 `clac`,fixup 路径有 `clac` + 清 ok。
- [ ] 知道 exception table **只拦内核态 accessor RIP**;用户态缺页照常 demand-page,这一章没碰它。

## 别做这些

- **别**指望在本机看到「SMAP 真拦了一次内核访问用户页」——WSL2 嵌套 KVM 不透传 SMAP,CR4.SMAP 在开发机上没设。这一章能本机验的是上面五件事,**都不依赖 SMAP 真生效**(extable 负测试走内核态 accessor 指令的 fault,跟 SMAP 开没开无关)。SMAP 真生效要真机或 TCG。
- **别**以为 `__ex_table` 的条数是写死的——它是 accessor 内联点的个数,改了 syscall 数量就变。lab 里让你自己 `nm` 算当下的数,对得上「每个 accessor 实例一条」就行,别纠结具体是不是 21。
- **别**把 exception table 跟用户态 demand-page 搞混。exception table 只管「内核代用户访问、用户却传了坏地址」这种**内核态** fault;用户态自己访问缺页(栈、heap 第一次碰)是 F2 lazy-allocation 的正常 demand-page,这一章一行没动它。范围栅栏是故意的——撤宽松 demand-page 是另一个高风险里程碑的事。
- **别**以为「撤了全局 stac」单独成立。光撤 stac,那些既碰用户内存又会 block 的 syscall 就违反 accessor 窗口铁律了。撤 stac 必须配 accessor + syscall 分层(`do_*_kernel` 可 block 不碰用户 / `sys_*` accessor 边界不 block)——三件事是一套,缺哪个都是 AC 跨核丢失的重演。
