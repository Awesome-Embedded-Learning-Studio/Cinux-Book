---
title: 05 · 收尾:验证与下一站
---

# 收尾:验证与下一站

## 验证

008 的验证分三层,从「机制对不对」到「真并发扛不扛得住」,一层比一层狠。少了哪一层都有漏网的可能。

**第一层:host 单测(std::thread 真线程压测)**

```bash
cmake --build build
ctest --test-dir build -R 'sync_concurrent|^sync$|pmm|heap|scheduler' --output-on-failure
```

`sync_concurrent` 这个可执行文件在 `test/CMakeLists.txt` 里链了 `-pthread`,也就是说它在宿主机上**真的起多个 `std::thread`** 去压自旋锁、互斥、分配器,然后看计数和分配结果对不对。pmm / heap 的 host 测试也在 008 新增了并发用例——比如 pmm 起多个 `std::thread` 同时 `alloc_page`,断言不会把同一页分给两个线程(正是正文讲的 find-then-set 竞争)。要注意:scheduler 的 host 测试仍是**单线程白盒**(只验运行队列的结构和状态机),它的并发正确性不靠这一层,而是靠下面第二、第三层来压。这一层跑得快、能反复回归,是日常开发的第一道闸。

**第二层:kernel 端 RAII 机制测试(QEMU,验正确性)**

```bash
cmake --build build --target run-kernel-test
```

`big_kernel_test` 启动后,`main_test.cpp` 会调到 `run_sync_concurrent_tests()`(注释标着「Sync concurrent tests (008): InterruptGuard, IrqSpinlockGuard」),输出里会有 `Sync Concurrent Tests (008)` 这一段。它逐条断言:

- `InterruptGuard`:进 guard 后 IF 确实被清(`flags & 0x200` 为 0),析构后恢复原值;嵌套场景下内层进出 IF 一直是 0、最外层才恢复。
- `Spinlock::IrqGuard`:持锁期间 IF=0,析构后锁释放、中断恢复;不同锁可嵌套;进入时 IF 本来就是 0 的,析构后仍是 0(不误开)。
- Spinlock / IrqGuard 的互斥:三个 Task 协作式地各自自增若干次,总和精确。
- Scheduler 并发:add/remove 一批 Task 后状态正确,block/unblock 状态转换正确。

这一层是 DONE.md 说的「协作式」多任务测试——它手动在几个 Task 之间切 `g_per_cpu.current`、顺序执行,并不制造真正的并行,但它把关中断、加锁、释放的**每一步**都用断言钉死,保证 RAII 的机制行为正确。

**第三层:生产 stress(真·抢占 + 时钟中断,验并发)**

```bash
cmake --build build --target run-stress-test
```

这是最狠的一层。`stress_test.cpp` 在**生产内核**启动时(shell 之前)起 4 个内核线程,开着抢占和 100 Hz 时钟中断,每个线程做:

- 200 次 `alloc_page` / `free_page`;
- 200 次 `alloc(64)` / `free`;
- 1000 次原子计数器自增。

然后 `boot_continuation` 线程 spin 等全部完成,断言**精确**操作数。这些数字来自 `stress_test.cpp` 里的 `kprintf`,是真实输出格式:

```text
[STRESS] ===== 028d Concurrent Stress Test =====
[STRESS] Threads=4  PMM_ops/thread=200  Heap_ops/thread=200
...
[STRESS] PMM alloc/free ops: expected=800 actual=800 PASS
[STRESS] Heap alloc/free ops: expected=800 actual=800 PASS
[STRESS] Atomic counter:       expected=4000 actual=4000 PASS
[STRESS] ALL PASSED -- launching shell
```

`expected=actual` 看着理所当然,但它的潜台词是:在真抢占 + 真时钟中断的环境下,PMM 没有把同一页分给两个人、堆的 `free_list` 没有被改坏、计数器没有丢更新。如果锁加错了,`actual` 会小于 `expected`(操作在竞争里丢失),或者干脆 triple fault。这层过了,008 的「并发安全」才算真正站住。

> 三层的分工:host 单测覆盖广、跑得快、天天回归;kernel 机制测试证明「我们的 RAII 在真硬件上行为分毫不差」;生产 stress 证明「这些原语部署之后,在最恶劣的并发形态下也不崩」。机制正确 ≠ 并发安全,必须分开验证。

## 下一站

008 把内核的共享数据都锁好了,但你会发现一个尴尬的事实:**启动流程本身还是「一根筋」的**。`kernel_main` 从头跑到尾——AHCI 读盘、ext2 mount、起 shell——全是同步的、跑在一个不可调度的上下文里。哪天哪一步需要「等一会」(等磁盘就绪、等一个事件),现在没有干净的地方让它 sleep,因为启动代码压根不在任何可调度的线程上。

009 要解决的就是这个:把「启动到 shell」这条路径本身,变成一个**可以被调度、可以阻塞、可以让出**的内核线程——init 线程。这么一来,启动逻辑就能复用我们这一章刚造好的 `Mutex`/`Semaphore`、能 `block`、能被时钟中断打断——本章那些「造好但还没上岗」的阻塞原语,终于等到真正的用武之地。

(顺带一提:那个临时的 `stress_test.cpp` 会在 009 被移除——它是 008 的阶段性验证脚手架,使命完成后就退场,把启动的主舞台让给 init 线程。)

怎么把启动路径线程化、又怎么避免它和新接进来的驱动撞 MMIO 地址,那是 009 的故事。

---
