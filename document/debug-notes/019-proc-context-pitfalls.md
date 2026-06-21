---
title: 调试档案 019 · 内核线程切换与 higher-half 扶正的两个坑
tag: 019_proc_context
---

# 调试档案 019 · 内核线程切换与 higher-half 扶正的两个坑

> 从 `document/notes/019_proc_context/001_higher_half_fix.md`、`002_thread_exit_crash.md` 提炼,配套 [019 · 让内核长出第二条执行流:进程上下文](../book/06-process/019-proc-context.md)。019 给内核装上了第一条可切换的内核线程,过程里两个坑最典型:一个是「线程退出时炸成栈底哨兵」,看着像栈溢出、其实是退出路径没接好;一个是「大内核跑在错的地址上」,当下不发作、却会在进程隔离上线时爆雷。两条都值得记成档案。

## 案例一:线程跑完炸成 `RIP=00000000deadc0de`

- **症状**:两个内核线程交替打印若干轮后崩溃,串口吐出 `emulation failure` + `RIP=00000000deadc0de`,然后三重错误重启。其中一个线程的 `done` 都没来得及打全。
- **原因**:**两个 bug 叠加**,单独修哪个都不够。
  - **Bug A —— 线程函数没有返回地址。** `TaskBuilder::build()` 布置新任务初始现场时,只把 `ctx.rsp` 设到栈顶、`ctx.rip` 设到线程入口,栈顶是空的。而 `context_switch` 是用 `jmp *to->rip` 跳进线程入口的(不是 `call`),所以线程函数的栈帧底下没有正常的返回地址。线程函数一 `return`,`ret` 就弹出栈顶那个值当返回地址——栈顶是空的,一路往下弹,最终弹到栈底的溢出哨兵 `0xDEADC0DE`(`build()` 写在那儿防栈溢出的 magic),把它当地址跳过去,CPU 跳到 `0xDEADC0DE`,当场崩。
  - **Bug B —— `exit_current` 先覆盖了 `current_`。** 就算修了 Bug A(在栈顶压一个退场函数地址),`exit_current` 本身还有毛病:它**先**把 `current_` 指向下一个任务,**再**调 `context_switch(&current_->ctx, &next->ctx)`。可这时 `current_` 已经是 `next` 了——`from` 和 `to` 指向同一个任务,`context_switch` 退化成空操作(存自己、恢复自己、换自己的栈、跳自己的 rip,等于什么都没干),执行继续停在已经标记 `Dead` 的线程栈上,该崩还是崩。
- **定位**:看崩溃地址 `RIP=0xDEADC0DE`——这正是 `TaskBuilder::STACK_MAGIC`。一旦看到这个值当 RIP,几乎可以断定是「某个 `ret` 弹栈弹到了栈底哨兵」,即线程/函数的返回地址没接好。再读 `exit_current`,确认它取 `from` 用的指针是不是已经被覆盖。
- **修复**:
  - Bug A:`build()` 里把 `ctx.rsp` 设成 `栈顶 - 8`,并在那个位置写入退场函数(`Scheduler::exit_current`)的地址。线程函数 `return → ret` 弹出的就是这个地址,自动走进退场流程。
  - Bug B:`exit_current` 第一行先 `Task* prev = current_;`,全程用 `prev` 当 `from`,**先 dequeue 再 pick_next 再 `current_ = next`**,最后 `context_switch(&prev->ctx, &next->ctx)`——保证 `from != to`。
- **防复发**:**协作式线程的「干净退出」是一条独立的设计要求**,不是顺便能得到的——任何用 `jmp` 启动的线程,栈底都必须有合法的「返回到哪」落点。`exit_current` 这类「把当前任务换下、切到下一个」的函数,`from` 指针一定要在改 `current_` **之前**取出来。两个原则钉死,线程退出就稳了。

## 案例二:大内核跑在恒等映射,进程隔离形同虚设

- **症状**:019 当下不直接发作(因为演示的两个内核线程共享内核地址空间)。但它是颗定时炸弹:一旦后续真给不同进程造独立地址空间,会出现「A 进程建的页表项莫名出现在 B 进程的地址空间里」——隔离失效。排查笔记 `001_higher_half_fix` 记录了这条症状。
- **原因**:mini-kernel 的 ELF 加载器 `load_elf` 在返回内核入口点时,**剥掉了 higher-half 偏移**:

  ```cpp
  // 旧(错)
  constexpr uint64_t HIGHER_HALF_BASE = 0xFFFFFFFF80000000ULL;
  uint64_t entry = saved_entry;
  if (entry >= HIGHER_HALF_BASE) entry = entry - HIGHER_HALF_BASE;  // 0xFFFFFFFF81000000 → 0x1000000
  return entry;
  ```

  大内核是按 higher-half 地址链接的([linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/linker.ld):`KERNEL_VMA = 0xFFFFFFFF80000000`、`KERNEL_LMA = 0x1000000`,故 `.text` 链接在 `0xFFFFFFFF81000000`)。剥掉偏移后,入口变成 `0x1000000`,mini-kernel 跳过去。这之所以「能跑」,纯粹是因为引导加载程序顺手建了一条**恒等映射**(`PML4[0]` → 物理,盖住 `0x1000000`),让 `0x1000000` 与 `0xFFFFFFFF81000000` 指向同一片物理页。

  但这和 018 的地址空间设计正面冲突:`AddressSpace` 的设计是「内核半区 `PML4[256..511]` 跨所有空间共享,用户半区 `PML4[0..255]` 每个空间私有」。内核理应待在**共享的**高半区;剥偏移却让它跑在 `PML4[0]`——落在**用户半区**、本该每个空间各自私有的那一半。于是内核待在了「错误的一半」,页表子树被多个地址空间错误共享,一个空间里建的项顺着共享的 PDPT 子树泄漏到别的空间。
- **定位**:查 `kernel/mini/elf_loader.cpp` 的 `load_elf` 末尾,看返回的入口有没有被减去 higher-half 基址;再用 `readelf -h build/.../big_kernel` 看 `e_entry`,确认它是 higher-half 地址(`0xFFFFFFFF81000000` 一带)还是被剥成了 `0x1000000`。若加载器返回的是剥过的值,就是这条。
- **修复**:加载器直接返回链接时的 higher-half 入口,不再剥偏移:`return saved_entry;`。内核从此跑在它链接的高半区地址,待在共享的内核半区——这正是 `AddressSpace` 设计期望的位置。(顺带:[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp) 里 `[BIG] Big kernel running @ 0x1000000` 是遗留字符串,打的是物理基址,不代表修复后的运行地址,别被它误导。)
- **防复发**:**「内核映射在所有地址空间共享的高半区」是一条不可破的架构约束**。任何「为了让内核暂时能跑」而把它塞进恒等映射/用户半区的捷径,都会在「上多地址空间」那一刻反噬。ELF 加载器返回的入口,必须与链接脚本里 VMA 一致;内核永远从它链接的高半区地址开始执行。

---

### 一句话总结

019 的两个坑,一个是**退出路径没接好**(线程 `ret` 弹到栈底哨兵 + `exit_current` 的 `from` 被提前覆盖),修在「栈顶压退场函数」和「先存 `prev` 再切」;一个是**内核待错了地址半区**(加载器剥掉 higher-half 偏移),修在「入口原样返回、内核跑在链接的高半区」。前者是「协作式线程干净退出」的必修课,后者是「进程隔离地基」的隐形前提——当下不炸、上线必爆的那种。
