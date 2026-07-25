---
title: 06 · 收尾:范围边界与四块地基拧成一根绳
---

# 收尾:范围边界与四块地基拧成一根绳

## 范围与边界(诚实说)

### 三个 deferred commit:依赖 ext2 重构地基,划给 080/081

三个 `ext2` 盘元数据 race 没在这一章修,因为它们都依赖 `KmBuf` RAII 和 `ext2_dirops` 拆分这块地基:

- **`block_buf_[4096]` 共享 buffer 无锁 race** —— 两 CPU 同时 `read_block` 互相覆盖 `block_buf_`,`resolve_disk_block_` 拿到别人的 indirect 数据 → wild block 号 → NVMe 读超范围 failed → demand page 失败 → segfault。修法是堆 `KmBuf` RAII 替共享 buffer(当前 `ext2.hpp` 里仍是裸 `uint8_t block_buf_[4096]`,无 `KmBuf`);
- **盘位图 alloc/free RMW 无锁** —— 两 CPU 同时 alloc 都读同 bit free → 都 mark+write → 同块分给两文件、一覆盖另一。修法是加 `block_alloc_lock_` 串行位图 RMW;
- **样式整理** —— 与 `ext2_dirops` 拆分绑定。

这几笔债还有一层「检测器盲区」的教学价值:`RACE_TOUCH` 能抓 `inode_cache_`(单一访问点),但 `block_buf_` 是被几十处 `read_block` 共享的 scratch,`RACE_TOUCH` 标不过来——这类「共享 scratch 互踩」要等 host TSAN(host-build 直接观察内存访问)才能秒级定位,是 081 的内容。本章只作检测器互补的对照引用,不展开。

### deferred-CoW 基建与接线

整条 deferred-CoW 修复在当前 Book 工作树已端到端接通,逐条交代:

- **`handle_cow_fault` 走 deferred 路径** —— `process_new.cpp` 的 `handle_cow_fault` 已调 `pte_count_dec_and_test_no_free(old_phys)`,命中(计数归零)后再 `enqueue_pending_shootdown(old_phys, fault_vaddr)`,**不再立即 free**([process_new.cpp](../../../kernel/proc/process_new.cpp#L114-L126),注释 L119 自承「B3 defect C: defer the free」);
- **`enqueue_pending_shootdown` 已有调用方** —— 上一条就是它的调用点;
- **`CINUX_TLB_DRAIN` 这个 CMake option 已声明** —— `cmake/options.cmake` `option(CINUX_TLB_DRAIN "Spawn the TLB shootdown drain kthread (deferred CoW free)" ON)`,默认 ON;`kernel/arch/CMakeLists.txt` 的 `if(CINUX_TLB_DRAIN)` 据此决定链 `tlb_drain.cpp` 真实现还是 `tlb_drain_stub.cpp` 空实现;
- **`start_tlb_drain_thread()` 已有调用方** —— `proc/init.cpp` 在初始化阶段调用它起 drain kthread(init.cpp L19 include、L165 调用);
- **`shootdown_ipi_stub`(0xE1)已注册进 IDT** —— `irq_init()` 在 reschedule 0xE0 之后 `set_handler(kShootdownIpiVector, shootdown_ipi_stub, ...)` 注册 0xE1([irq_handlers.cpp](../../../kernel/arch/x86_64/irq_handlers.cpp#L172-L177)),`shootdown_ipi_stub` 声明在 [irq_handlers.cpp:67](../../../kernel/arch/x86_64/irq_handlers.cpp#L67),`interrupts.S` 的 `ISR_IRQ shootdown_ipi_stub, shootdown_ipi_handler, 0` 定义在 [interrupts.S:455](../../../kernel/arch/x86_64/interrupts.S#L455)。

连带机制测试也端到端跑通:机制测试里的 `tlb_shootdown_page(0xDEADB000)`([main_test.cpp](../../../kernel/test/main_test.cpp#L1044-L1048))在 `-smp 2` 下会真发 0xE1 IPI 给 AP、AP ack 回来、BSP 的 spin 等到 acks==0 退出,打出 `[F-VERIFY] shootdown IPI test: PASS (all APs acked)`——0xE1 收发通路在当前工作树已可验证。

### race-detect 门控链在本机工作树的状态

主线一讲过 opt-in 是一条链,本机工作树三环都已接通:

- **`option(CINUX_RACE_DETECT ...)` 已声明** —— [cmake/options.cmake](../../../cmake/options.cmake#L63) `option(CINUX_RACE_DETECT "Enable SMP data-race watchpoint detector (debug)" OFF)`,默认 OFF(opt-in);
- **编译宏通过 foreach 自动传** —— `cmake/options.cmake` 的 `CINUX_COMPILE_DEF_OPTS` 列表含 `RACE_DETECT`([cmake/options.cmake:148](../../../cmake/options.cmake#L148)),`kernel/CMakeLists.txt` 的 `foreach(_opt IN LISTS CINUX_COMPILE_DEF_OPTS)`([kernel/CMakeLists.txt#L130-L135](../../../kernel/CMakeLists.txt#L130-L135))把它自动 map 成 `target_compile_definitions`,加新开关无需改 kernel/CMakeLists.txt;
- **AP 侧机制测试 touch 已落地** —— 上面主线二讲过,`ap_test_selfcheck` 已有 `race_check_access_probe(g_race_test_wp)`。

所以 `-DCINUX_RACE_DETECT=ON -DCINUX_LOCKDEP=ON` 之后,三件套齐、机制自测端到端通、能真报 PASS。lab 会带你亲手开这两个 option 并验证 PASS 亮起。

### VFS offset_lock 分流锁

主线三 rider ③ 已详述:CinuxOS 上游修的 `is_page_cacheable` 分流锁已回迁到 Book 工作树,`sys_read.cpp:48` / `sys_write.cpp:53` 都按 cacheable 与否分流持锁,schedule-while-held 病灶态已根治。

### WSL2 上的验证边界

本机 WSL2 + KVM 能跑 `-smp 2`(qemu-system-x86_64 11.0.2 在 `/usr/sbin/`、`/dev/kvm` 存在且 `crw-rw-rw-`),`run-kernel-test-smp` target 在 `cmake/qemu.cmake` 里定义(QEMU `-smp 2` + test kernel + 自动退出)。race-detect 本身不依赖 SMAP/CPUID 透传,所以 WSL2 的 SMAP 限制对 race-detect 验证无影响。但真机上的 heisenbug(`-smp 2` 偶发踩时序)是环境相关的——race-detect 抓的是「无锁交错」这类**结构性**缺陷,不是时序抖动;真机 heisenbug 要靠 race-detect 开着多跑、看它报不报,单次没报不能下结论(海森堡悖论)。

## 收尾:四块地基拧成一根绳

回看这一章的四个阶段,race-detect 是那根红线:发现期用它证实「`inode_cache_` 根本没锁」;验证期拿它当靶子证明检测器真能抓;根除期病灶上锁后它让位给 `lockdep_assert_held` 做回归护栏;纵深期推导出的「IF=0 不能 spin 等跨核 ack」逼出了 deferred CoW——这条推导的兑现端(`_no_free` + drain kthread)正是 044 章那两本账(`pte_count` / `refcount`)的延展。

四个阶段不是孤立的修法清单,是同一件事的四步:**把 SMP 上「靠人 audit + 靠崩发现」换成「靠机制报警 + 靠结构保证」**。报警器抓结构性缺陷(无锁),结构保证消除时序侥幸(deferred 把「可能互锁」降到「结构上不可能」)。这根绳从 044 的两本账起头,经过 078 的 race-detect 和 deferred CoW,绳结越收越紧——下一章(080/081)会接着把 `ext2` 盘元数据的 race(`block_buf_` / 位图 RMW)和 host TSAN 那套「共享 scratch 互踩」的检测能力补上。

> 诚实边界再压一句:race-detect 那条机制自测(`[F-DYN-COV] race-detect test:`)当前是「测试在跑、断言在执行」的状态——它的检测逻辑端到端通(见主线二的 exchange + 比较),三件套(option + 编译宏 foreach + §14 文件门)齐了、AP 侧 touch 也已落地,`-DCINUX_RACE_DETECT=ON -DCINUX_LOCKDEP=ON` 下 `-smp 2` 跑会真报 PASS。shootdown IPI 那段 0xE1 已注册进 IDT、机制测试已端到端跑通打出 `[F-VERIFY] shootdown IPI test: PASS`;deferred-CoW 的接线(`_no_free` + `enqueue_pending_shootdown` + drain kthread + `CINUX_TLB_DRAIN` option)和 rider ③ 分流锁都已落地——这些「已接通」的事实按上面边界逐条交代,当前树就是「已生效」的现场。
