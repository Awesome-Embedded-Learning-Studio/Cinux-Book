---
title: 调试档案 021 · 大内核入口魔数检查与自旋锁的 IRQ 悬剑
tag: 021_proc_sync
---

# 调试档案 021 · 大内核入口魔数检查与自旋锁的 IRQ 悬剑

> 从 `document/notes/021/001_big_kernel_magic_check.md` 提炼,配套 [021 · 让任务睡得下去、醒得过来:内核同步原语](../book/06-process/021-proc-sync.md)。021 把 020 调度器里铺好的 `block`/`unblock` 接成了真正的 Spinlock/Mutex/Semaphore。这一章真正踩到的坑只有一条被写进了 note——加 `sync.cpp` 让 BSS 长大,`_start` 的 `mov rsp, imm` 换了编码,mini kernel 的入口魔数检查没认出来,big kernel 测试直接进不去。但读完 `sync.cpp` 还有一条当下不发作、却迟早要命的隐形坑值得单独点出来:自旋锁没关中断,而 PIT 的 IRQ0 又能抢占当前任务。两条都值得记成档案。

## 案例一:加了一行 `sync.cpp`,大内核就「不是真内核」了

- **症状**:`make run-kernel-test` 跑出来 mini kernel 这一段全是绿的:

  ```
  === Mini kernel tests completed ===
  === MINI KERNEL TESTS PASSED ===

  === Loaded ELF is not a real kernel, exiting ===
  ```

  mini kernel 的所有测试都过了,但 big kernel 的测试**根本没被执行**——加载器把 big kernel ELF 读进内存、拿到入口点后,做了个「这是不是一个真内核」的字节检查,判定为「不是」,于是直接 `outl` 退 QEMU。注意它不是挂掉,是「静默地没进去」,所以最容易让人怀疑的方向(崩溃、三重错误、寄存器值)全是错的。

- **根因**:mini kernel 跳进 big kernel 之前,会拿入口点的头三个字节做一次「魔数」校验([main_test.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/test/main_test.cpp))。big kernel 的 `_start` 头两条指令是固定的:`cli` + `movq $__kernel_stack_top, %rsp`。`cli` 是 `0xFA`,后一条 `mov` 带了 `REX.W` 前缀 `0x48`,第三个字节本该是 `0xBC`——这是「`REX.W MOV r64, imm64`」的 `48 BC <8字节立即数>` 编码。旧检查就只认这一种:

  ```cpp
  // 旧(只认一种编码)
  bool is_real_kernel = (code[0] == 0xFA) && (code[1] == 0x48) && (code[2] == 0xBC);
  ```

  问题在于 x86-64 的 `mov` 对「把立即数装进 `rsp`」有**两种合法编码**,GNU assembler 会按立即数范围自动挑更短的那种:

  | 编码 | 机器码 | 何时被选中 |
  |------|--------|-----------|
  | `REX.W MOV r64, imm64` | `48 BC <8字节>` | 无条件 64 位 |
  | `REX.W MOV r/m64, imm32` | `48 C7 C4 <4字节>` | 立即数可符号扩展到 64 位时 |

  这一章新加的 `sync.cpp`(Spinlock/Mutex/Semaphore 的实现)带进来一批全局对象(`g_sem_free`、`g_sem_used`、`g_pc_mutex` 等),BSS 段涨了一截,`__kernel_stack_top` 的链接地址跟着挪——从「低 32 位符号扩展后不等于自身」挪成了「低 32 位符号扩展后恰好等于自身」。于是 assembler 给 `mov rsp, imm` 选了更短的 `imm32` 编码,入口字节变成了:

  ```
  FA 48 C7 C4 00 00 02 81 ...
  ^   ^------------------^
  cli  mov rsp, 符号扩展的 imm32
  ```

  第三个字节从 `0xBC` 变成了 `0xC7`。旧检查只认 `0xBC`,看到 `0xC7` 直接判「不是真内核」,big kernel 测试就此被拦在门外。

- **定位**:这坑的定位其实不靠内核调试,靠的是「mini 全绿、big 没进去」这个反差——mini kernel 的测试输出停在 `MINI KERNEL TESTS PASSED` 之后,下一行不是 `Launching big kernel test`,而是 `Loaded ELF is not a real kernel, exiting`。这一行就来自 `main_test.cpp` 里那段魔数检查。顺着这条字符串反查到 `is_real_kernel`,把那一小段反汇编的入口字节拿出来和检查条件一对,立刻能看到第三个字节是 `0xC7` 而不是检查里写的 `0xBC`。换句话说:别去内核里找 bug,问题在加载器那一侧的「字节级判断」上。

- **修复**:把魔数检查放宽,同时接受两种编码:

  ```cpp
  bool is_real_kernel = (code[0] == 0xFA) && (code[1] == 0x48) &&
                        (code[2] == 0xC7 || code[2] == 0xBC);
  ```

  `0xC7`(`imm32` 符号扩展)和 `0xBC`(`imm64`)都算合法的「把栈顶装进 rsp」,认下两种,检查就不再被 assembler 的最短编码选择绊倒。

- **防复发**:任何基于「机器码字节模式」的检查,都必须枚举**所有等价编码**,不能只匹配某一种。x86-64 这种「同一条指令有多种长度不同、都合法」的情况相当常见,assembler 会主动挑最短的,而「最短」这个选择对链接地址敏感——BSS 段大小一变、某个符号地址一挪,编码就可能切换。更稳的做法是不要依赖具体字节,要么解析指令、要么干脆校验「入口是不是一个合法的 ELF 且入口在预期段内」;真要用字节魔数,就得在注释里把「这是两种编码之一」写死,并在加代码后回归跑一遍 `make run-kernel-test`。

## 案例二:自旋锁没关中断,IRQ0 随时能把你切走(当下不炸、上线必爆)

- **症状**:021 的 producer-consumer demo 跑得好好的,`sent: 0..4` / `got: 0..4` 一条不少,测试也全绿。这条坑**当前不发作**。它是定时炸弹:一旦真有任务把一个临界区写大、或者在中断上下文里碰锁,就会出现「持有自旋锁的任务被抢占,另一个任务去抢同一把锁,永远自旋」的死锁,而且因为没有任何崩溃地址给你看,定位起来比案例一痛苦得多。

- **根因**:看 [sync.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/sync.cpp) 的 `Spinlock::acquire`,它就是朴素的 test-and-set 自旋,外面套一层内存序,自旋里插一条 `pause`:

  ```cpp
  void Spinlock::acquire() {
      while (__atomic_test_and_set(&locked_, __ATOMIC_ACQUIRE)) {
          __asm__ volatile("pause");
      }
  }
  ```

  这段代码**没有关中断**。`acquire` 拿到锁之后、`release` 放锁之前,中断一直是开的。问题在于 [main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp) 在初始化末尾 `PIC::unmask(0)` 打开了 IRQ0(PIT 定时器)、`__asm__ volatile("sti")` 放行了中断;而 [pit.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pit/pit.cpp) 的 IRQ0 处理路径是 `pit_irq0_handler → PIT::irq0_handler → Scheduler::tick()`,`tick()` 累计到 `DEFAULT_TIME_SLICE` 就会调 `schedule()`——也就是说,**当前任务在持有自旋锁的任意时刻,都可能被一个时钟中断切走**。

  Mutex/Semaphore 内部那把 `spin_`(只在操作等待队列时短暂持有)目前能侥幸不炸,纯粹是因为它的临界区只有「改 `owner_`/`count_` + 动一下链表」那么几行,且都在「先 release spin 再 block」的铁律下走;但 Spinlock 是直接暴露给用户当互斥锁用的,一旦谁拿它护一个稍微长一点的临界区,或者在中断/信号路径里碰它,IRQ0 就能拍在锁中间,把持有者换下去,新上来的任务要是也想要这把锁,就会一直 `pause` 转圈,持有者却没机会回来放锁——死锁。注意 Mutex 的 `lock()` 里「先 `spin_.release()` 再 `Scheduler::block`」这个顺序也是防同类死锁的关键(反了的话,持锁者会在「自旋锁还攥着」的状态下被 `block` 切走,等同一个队列里其他想拿自旋锁的任务全卡死),这条顺序现在是对的,但 `Spinlock` 本身缺的那道「关中断」还是悬着的。

- **定位**:这类「单核下自旋锁不安全」的坑没法靠崩溃地址定位,得靠读代码提前发现。具体路径:`Spinlock::acquire` → 看有没有 `cli`/`pushfq; popfq; cli` 之类的关中断动作(没有)→ 再追「谁可能在我持锁期间打断我」→ `main.cpp` 末尾的 `sti` + `PIC::unmask(0)` → `irq0_handler → Scheduler::tick → schedule()`。一旦看到「持锁期间中断开 + 时钟中断会调 `schedule`」这两点同时成立,就可以判定这把自旋锁在可抢占场景下不安全。021 把它如实标注成「留给以后」,不是 bug,是阶段性的安全边界。

- **修复**:021 这个 tag **没有**修这条——它属于后续(进入可抢占/多核前)的功课,这里只点出「为什么现在能活」。要真正堵上,做法是在 `Spinlock` 持有期间临时关中断:进临界区前保存 `rflags` 并 `cli`,出临界区时恢复原 `rflags`(恢复时若原本中断是开的,才会重新开)。这样持锁期间时钟中断进不来,也就不会被 `schedule` 切走。Mutex/Semaphore 内部那把 `spin_` 同理,尽管它临界区短,理论上也该走「关中断的自旋锁」才严密。注意:这条路在 021 不能写进正文实现,因为 022 还没到「重新审视自旋锁在中断/用户态下安全性」的那一步。

- **防复发**:「单核自旋锁 = 关中断 + test-and-set」是一条不变的纪律。任何一把只做 `test-and-set` 而不关中断的 Spinlock,都只在「调用它的上下文绝不会被抢占」(协作式、关中断上下文)时才安全;一旦它暴露给会被时钟中断打断的内核线程,就必须升级成「持锁期间关中断」。给 Spinlock 写单测时,除了验证 acquire/release 的基本语义,还应该有一项断言「持锁期间本地中断是否被屏蔽」——这项 021 暂时做不到,但应当作为下一个 tag 的验收点钉下来。

---

### 一句话总结

021 真正踩到的坑只有案例一:加 `sync.cpp` 让 BSS 涨、`__kernel_stack_top` 低 32 位可符号扩展、assembler 给 `mov rsp, imm` 换了更短的 `imm32` 编码(`48 C7 C4`),而 mini kernel 的入口魔数检查只认 `imm64`(`48 BC`),big kernel 测试进不去——修在「魔数同时认两种编码」。案例二是读源码读出来的隐形悬剑:`Spinlock::acquire` 没关中断,而 IRQ0 又会经 `Scheduler::tick` 抢占当前任务,持锁期间被切走就会死锁;当下靠「临界区短 + Mutex 先 release 再 block」侥幸不炸,但它是「进可抢占/多核前」必须补上的那道关中断。
