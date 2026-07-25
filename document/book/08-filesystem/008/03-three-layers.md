---
title: 03 · 三层加固落地:谁加什么、PMM 怎么改、调用链
---

# 三层加固落地:谁加什么、PMM 怎么改、调用链

造完原语,就是大面积套用。DONE.md 把这次加固按风险分了三层,我们也按这个来看。

## 谁加了什么、为什么是这种原语

| 子系统 | 保护方式 | 为什么选它 |
|---|---|---|
| PMM `alloc/free` | Spinlock 普通 `guard()` | 只有内核线程碰,中断不碰 |
| Heap `alloc/free` | Spinlock 普通 `guard()` | 同上 |
| **Scheduler 运行队列** | Spinlock **`irq_guard()`** | 被 PIT IRQ0 的 `tick()` 路径碰,**必须关中断** |
| FDTable `alloc/close/get` | Spinlock 普通 `guard()` | 线程间;防 double-close / use-after-free |
| `PIT::tick_count_` | `std::atomic` | 单字段、超高频,加锁太贵 |
| Scheduler `tick_count_`/`current_slice_`、`next_tid`、`next_stack_vaddr` | `std::atomic` | 同上,热路径上的单字段 |
| `File::offset` | Spinlock 普通 `guard()`(`offset_lock_`) | sys_read/write/getdents 改偏移 |
| VMM `map/unmap` | Spinlock 普通 `guard()` | 线程间改页表 |
| `g_mount_table` | static Spinlock 普通 `guard()` | 线程间改挂载表 |
| Keyboard 环形缓冲 | `InterruptGuard` | 与 IRQ1 ISR 共享,单生产者单消费者 |

整张表的判断标准其实只有一句话:**这块数据,中断处理路径会不会碰它?**

- **会** → 走「关中断」那一侧:`irq_guard()`(既要挡别的线程、又要挡中断)、`InterruptGuard`(只挡那个 ISR、不需要多线程互斥),或者原子(单字段、高频)。
- **不会** → 普通 `guard()` 就够,少一次 `cli`/`popfq` 的开销。

## PMM 怎么改的:把 find 和 set 圈进同一把锁

以 PMM 为例看「plain guard 怎么治 find-then-set 的竞争」:

```cpp
uint64_t PMM::alloc_page() {
    auto g = lock_.guard();        // ← 整个「找 + 置位」被圈成一个临界区
    (void)g;
    return alloc_page_locked();    //   find_first_free + bm_set + free_pages--
}
```

007 之前,「找空闲 bit」和「置位」是分开的两步、且无锁,于是两个线程能各自 find 到同一个 bit。现在 `guard()` 把它们包成一个不可分割的临界区:第二个线程会在 `acquire` 处 spin,等第一个 `release` 之后再去 find——这时 bit 已经被置上了,它自然会 find 到**下一个**空闲位。竞争就这么根治了。

顺带一个小拆分:你会看到 `alloc_page_locked` / `free_page_locked` 这种「不拿锁」的内层函数。原因是 `alloc_pages`(分配连续多页)需要**在已经持锁的状态下**复用单页逻辑——它不能去调会再次 `guard()` 的 `alloc_page`,因为我们的 Spinlock 不支持重入(同一个线程第二次 `acquire` 会把自己 spin 死)。所以把「真正干活但不碰锁」的逻辑抽成 `_locked` 版本,由外层加好锁再调。这是个很常见的锁代码组织手法。

## 调用链:一次 sys_read(fd>0) 穿过哪些锁

把这些锁串起来看,一次读文件实际穿了两个临界区:

```text
sys_read(fd)
  └─ g_global_fd_table().get(fd)          ── 持 FDTable::lock_  (guard)
        └─ 拿到 File*
             └─ { auto g = file->offset_lock_.guard();   ── 持 File::offset_lock_ (guard)
                  inode->ops->read(inode, offset, buf, count);
                  file->offset += result;
                }
```

两次都是普通 `guard()`,因为读路径上没有中断碰这些数据。注意 `offset_lock_` 的粒度——它锁的是**偏移量的「读 + 改」**,不是整个 inode。两个进程通过各自的 fd 读同一个文件,各自有独立的 `offset`、各自的 `offset_lock_`,互不阻塞。这正是 `offset_lock_` 挂在 `File`(打开文件描述)上、而不是挂在 `Inode` 上的原因:每个打开实例有自己的读写位置,不该因为一个进程在读,就把整个文件锁死。
