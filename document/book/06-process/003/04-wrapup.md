---
title: 04 · 收尾:验证 + 下一站 + 参考
---

# 收尾:验证 + 下一站 + 参考

## 验证

这套同步原语有两层测试,一层在 host、一层在 QEMU 机内,互为补充。

host 侧,[test_sync.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_sync.cpp) 把 `Spinlock`/`Mutex`/`Semaphore`/等待队列的逻辑**纯重实现**了一份(不链内核代码,用 `std::atomic` 代替 `__atomic`,mock 掉 `Scheduler::block`/`unblock` 和 `g_per_cpu`),`CINUX_HOST_TEST` 门控。它盯的全是「逻辑对不对」:`Spinlock` 初态/acquire-release/guard/double-release 良性、等待队列 enqueue/dequeue/FIFO/空队/清 `wait_next`、`Mutex` 设 owner/清 owner/`try_lock` 成功失败/竞争 block+入队/所有权交接/三等待者 FIFO/RAII、`Semaphore` 初值/默认 0/`post++`/`wait` 正数不阻塞/`wait` 到负阻塞/`try_wait` 成功失败/边界(大初值、repeated post)/lock-unlock 复用、`Task::wait_next` 零初始化为 null:

```bash
ctest --test-dir build -R sync --output-on-failure
```

真正的原子操作(`__atomic_test_and_set`)和真正的 `Scheduler::block`/`unblock`(真调度器、真状态流转)只能在 QEMU 里验。[test_sync.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_sync.cpp) 跑在真 PMM/VMM/Heap 之上,节名 `Sync Tests (021)`,入口 `run_sync_tests()`:

```bash
cmake --build build --target run-kernel-test
```

机内会跑九组:`Spinlock` acquire+guard;`Mutex` lock/`try_lock` free/held、竞争 block+入队 / unlock 交接 / 三等待者 FIFO、RAII;`Semaphore` 初值/默认 0/`post++`/`wait` 正数/`wait` 到 0 阻塞(`count`→-1)、`try_wait` 成功/0 失败/耗尽、`post` 唤醒 / 三等待者 FIFO、生产者-消费者计数模型(无阻塞,验证计数对得上)、`Task::wait_next` build 后为 null。这组测试是阻塞语义的真正保证——demo 不一定触发的阻塞路径,在这里被构造性覆盖。

最后是生产 demo,跑大内核看端到端:

```bash
cmake --build build --target run
```

串口应该看到 producer / consumer 交替输出 `sent: 0` ... `sent: 4` 与 `got: 0` ... `got: 4`,不死锁、不丢数。如前所述,这个规模不保证每次都真走阻塞,它验证的是「三件套拼对了、计数对得上」,不是「阻塞路径每次都触发」——后者归 QEMU 机内测试管。

## 下一站

021 让任务「睡得下去、醒得过来」,内核线程第一次有了真正的同步原语。可这套原语的所有前提都还停在内核态、单核、不关中断:`Spinlock` 的 `acquire` 没有关中断,正确性靠「自旋锁绝不跨阻塞持有」加单核抢占时机;`Mutex`/`Semaphore` 的 `block` 走的是 020 那条软件调度路径,和中断使能无关;`Task::addr_space` 在 demo 里压根没填,所有线程共享内核地址空间。

下一站(022)要跨出这一步:进 ring3,造用户态进程、系统调用,于是 SFMASK、MSR、中断门改 IF 这些会被重新审视——那一章会反过来拷问本章的同步原语:「在用户态可被打断、在中断里可能重入的世界里,这把自旋锁还安全吗?」本章留下的多核/IRQ 安全缺口,要往真正的可抢占方向推,就得先把 ring0 内核线程和 ring3 用户进程的边界划清楚。021 的 Spinlock/Mutex/Semaphore 是那条边界上一旦跨过去就要重新加固的地基——原语先立住,边界后划清。

---

### 参考

- **GCC `__atomic` Builtins**(`https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html`,在线 200):`__atomic_test_and_set`(原子置 1 并返回旧值)、`__atomic_clear`(原子清 0)、`__ATOMIC_ACQUIRE`/`__ATOMIC_RELEASE` 的 happens-before 语义。支撑本章 `Spinlock::acquire`/`release` 的实现与内存序配对,延续 020 章已核引用。
- **POSIX `sem_post(3)`**(`https://man7.org/linux/man-pages/man3/sem_post.3.html`,man-pages 6.18,已读正文):「increments (unlocks) the semaphore;若结果 > 0 则唤醒一个 `sem_wait` 阻塞者」、`MT-Safe`、`async-signal-safe`。支撑 `Semaphore::post()` 的设计锚点,以及正文「`SEM_VALUE_MAX`/async-signal-safe 本章未实现」的边界对比。
- **Intel SDM Vol.2B `PAUSE` 条目**(本地 `document/reference/intel/SDM-Vol2B-Instruction-Reference-M-U.pdf`):`pause` 作为 Spin-Wait Hint 的概念性依据;手册内具体页未在本地 PDF 定位到,故正文仅作概念描述、不引页码。
- **OSDev Wiki "Spinlock" / "Semaphore"**(`https://wiki.osdev.org/Spinlock`、`https://wiki.osdev.org/Semaphore`):test-and-set + `PAUSE` 朴素自旋锁、计数信号量 + 有界缓冲生产者-消费者的社区路径,概念性对照(域名 403 反爬,无法抓正文,仅作方向引用)。
- **002 章 · [时钟到点,该换人了:抢占式调度](../002/)**:`Scheduler::block(Task*, const char*)` / `unblock(Task*)`、`g_per_cpu.current`、`Spinlock` 原语与「自旋锁只定义、没人用」的现状——本章直接接续并落地。
- 本 tag 源码:[sync.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/sync.hpp) / [sync.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/sync.cpp)、[process.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/process.hpp)(`Task::wait_next`)、[scheduler.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.hpp) / [scheduler.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/scheduler.cpp)(`block`/`unblock`)、[per_cpu.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/per_cpu.hpp)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(生产者-消费者 demo)、[main_test.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mini/test/main_test.cpp)(魔数检查双编码);测试 [test_sync.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_sync.cpp)(host 镜像)、[test_sync.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_sync.cpp)(QEMU 机内,节名 `Sync Tests (021)`)。
