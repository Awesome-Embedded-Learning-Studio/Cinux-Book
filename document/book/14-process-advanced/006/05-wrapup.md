---
title: 05 · 验证、没做的、小结
---

# 验证、没做的、小结

## 验证

四层,合起来才够。每层验什么、为什么不验,都得讲清。

**第一层:host 单测——讲清有没有、为什么没有。** `ShmRegistry` 没配 host 单测——这层纯逻辑目前只被下面的 ring0 syscall 测顺带覆盖(缺口根因和对照 fifo 的细节 ShmRegistry 那节讲过了,这里不重复)。

**第二层:kernel 测(`test_shm.cpp` 6 例,`run_shm_tests` 在 `main_test.cpp:1208` 无条件调用)。** 这是当前的主力回归网,全走 syscall 层在 ring0 跑。六例分别锁:

1. **round-trip 两地址空间同物理页 + 跨 CR3 写读**(`test_shm.cpp:80-133`,Test1)。机制证明双保险:先断言 `as1.translate(virt1) == as2.translate(virt2)`(两虚拟地址落到同一物理帧),再 `as1.activate()` + `user_write_u64` 写 magic 0x48454c4c4f53484d、`as2.activate()` + `user_read_u64` 读、断言读到的等于写的。这是「真共享」的端到端证据。
2. **IPC_STAT 报 size/nattch**(`test_shm.cpp:143-168`,Test2)。用 3 页段验 `shm_segsz` 和 `shm_nattch`(attach 前 0、attach 后 1)。
3. **RMID 后 shmat 失败 + 未 attach 的 RMID 立即释放**(`test_shm.cpp:178-205`,Test3)。两条都锁:marked 段再 shmat 返负(EINVAL)、nattach=0 的 RMID 立即回收(再 shmat 同 shmid 失败)。
4. **命名 key 的 create/reopen/EXCL/ENOENT 四态**(`test_shm.cpp:215-234`,Test4)。reopen 用 `2*kPageSize` 的 size 但返同 shmid——**size 在复用时被忽略**(段已存在,shmget 直接返原 shmid)。
5. **非法参数全拒**(`test_shm.cpp:244-264`,Test5)。size=0、假 shmid(0xDEAD)、未映射地址 shmdt、假 id IPC_STAT 全返负。
6. **相邻映射 VMA 合并的 detach 回归**(`test_shm.cpp:277-319`,Test6)。就是 shmdt 长度陷阱那一节的真坑(footgun)防复发测。

> **测试框架细节**:`big_kernel_test.h` 的 `RUN_TEST` 宏(`big_kernel_test.h:174-183`)**无 skip 概念**——记下 `_failed_before`、跑完 `fn()`、若 `tests_failed` 没涨就算 PASS(`tests_passed++`),失败时 `TEST_ASSERT` 宏(`big_kernel_test.h:133-141`)做 `tests_failed++`。`ASSERT_OK` 那种硬失败会 `cinux::io::io_outb(0xf4, 1)`(向端口 `0xf4` 写字节 1)让 QEMU `isa-debug-exit` 退出码 3(`big_kernel_test.h:158-168`)。所以「6 例 PASS」是真跑真断言,不是静默跳过。

**SMAP 细节**。ring0 读写用户映射页必须走 stac/clac 窗口——`test_shm.cpp:57-70` 封装了 `user_write_u64`/`user_read_u64`:

```cpp
void user_write_u64(uint64_t addr, uint64_t value) {
    cinux::arch::stac();
    *reinterpret_cast<volatile uint64_t*>(addr) = value;
    cinux::arch::clac();
}
```

([test_shm.cpp](../../../kernel/test/test_shm.cpp#L57-L62),`user_read_u64` 同款。)直接 ring0 读写用户映射页会 #PF(SMAP 挡),stac 临时打开用户访问、clac 关上。每个 AddressSpace 操作前先 `activate()` 换 CR3(`test_shm.cpp:112, 115`)、完事 `write_cr3(AddressSpace::kernel_pml4())` 回内核空间(`test_shm.cpp:119`)——否则栈上 AddressSpace 析构会拆当前 CR3 下的页表,把测试自己的页表拆了。这两个反直觉点 lab-082 让你亲自踩一遍。

**第三层:shell/用户态闭环——讲清目前没有。** musl/glibc 用户态 SHM demo 是 follow-up。当前 `shmid_ds`(`shm.hpp:81-87`)是精简内核内形状,只有 `shm_segsz/cpid/lpid/nattch/mode` 五字段,不是 Linux 全 `ipc_perm`(uid/gid)+ 时间戳(`shm_atime/dtime/ctime`)布局。真要跟 glibc 互操作得先拓宽这个结构体,这是诚实边界。

**第四层:全量 `run-kernel-test-all`。** 两腿(单核 + `-smp 2`)的 passed/failed 数字,**必须在 Book 工作树真跑后填**,不照抄源仓库 dev note 的数(那是源仓库的,Book 侧须独立验证)。`test_shm.cpp` 6 例都进 `big_kernel_test`,真跑后 passed 数应包含这 6 例。用户态真能用 shm 靠「syscall 真注册(`syscall.cpp:224-227`)+ ring0 测端到端通」两腿绿间接证明,不是 big_kernel_test 的直接断言——四层证据合力,任一单独都不够。

> **测试数字怎么填。** 写章节时若没真跑 `run-kernel-test-all`,passed 数字别照抄 dev note 的 1067——那是源仓库 worktree 的数,Book 侧须独立实跑。「教程即验证」不是口号,数字要么标「Book 实测」、要么留空待跑。本教程成稿时这一格留空,等你跑完 `cmake --build build --target run-kernel-test-all` 把两腿数字填进去。

## 这章没做的

- **musl/glibc 用户态 SHM demo**。`shmid_ds` 当前是精简内核内形状(`shm.hpp:81-87` 五字段),不是 Linux 全 `ipc_perm`(uid/gid)+ 时间戳(`shm_atime/dtime/ctime`)布局。真要跟 glibc 互操作得先拓宽这个结构体。这是用户态 demo 推迟的根因。
- **IPC_SET / IPC_INFO / SHM_LOCK 等高级 shmctl 命令**。`do_shmctl_kernel` 对非 STAT/RMID 直接返 `-ENOSYS`(`sys_shm.cpp:284-285`)。SHM_LOCK/UNLOCK(把段页钉在 RAM 不 swap)依赖 swap 子系统,Cinux 没有 swap,这两个命令永远不会做。
- **权限强制**。`IPC_CREAT` 命中既有段、shmat 的 `SHM_RDONLY` 与段 mode 的交互现仅记 mode 不强制检查(`sys_shm.cpp:137-138` 的 readonly 粗判),没有完整 uid/gid 检查。教学内核假设单用户/可信,不做权限强制。
- **SMP 下 `ShmRegistry::segment()` 快照与 detach/mark_removal 之间的 TOCTOU**。单线程测不触发,`shm.hpp:147-150` 注释明说「caller-supplied exclusion is assumed」(调用方假设排他)。真 SMP 多核同时 attach/detach 同一段有竞态,文档化为已知局限。
- **Linux 的 sequence number 防 stale 句柄复用**。教学内核固定表 + index-as-handle 不做,RMID 后槽位被复用时 stale shmid 会命中新段,代价已记(`shm.hpp:24-26`)。
- **真两进程(scheduler 循环)跑**。ring0 测用 `Scheduler::set_current` 装的空壳 Task(`test_shm.cpp:89-92, 95, 100`)、activate/CR3 切换手动(`test_shm.cpp:112, 115, 119`),不走调度器循环。所以「真用户态两进程通过 shm 通信」没被这套测覆盖,是 follow-up。真 SMP 多核 TLB shootdown 这条链 shm 也没走(deferred-free 变体 `pte_count_dec_and_test_no_free` 在 drain kthread 路径别处用)。
- **buddy 尾页的 refcount 残留**。`alloc_pages` 向上取整到 2 的幂(`pmm.cpp:211-217`),整块置 refcount=1(`pmm.cpp:228-231`),映射只碰前 count 页,尾页 refcount 留 stale 1。`free_pages` 只 free head 按 order 回收整块、不查 per-page 计数,所以尾页不会泄漏 buddy(整块回收)。`test_shm_stat` 用 3 页段(实际 alloc 4 页)验证过账不爆,这里提一句不展开。

## 小结

- **shm 的价值不是「比 pipe 快」,是「机制不同」**:pipe 搬数据(字节流过内核 buffer,两次 copy)、shm 搬地址(页表直接共享物理页,零 copy + 无 syscall 可见)。四个 syscall(shmget/shmat/shmdt/shmctl)真注册在 `syscall.cpp:224-227`,号 29/30/31/67 跟 Linux x86_64 ABI 对齐。
- **分层铁律**:`ShmRegistry`(固定 16 槽纯逻辑表,key→segment,只管簿记 + nattach/marked 状态机,零 kernel-only 依赖)vs `sys_shm`(物理页生命周期层,alloc_pages/map/unmap/free_pages)。承 071 的 `FifoRegistry` 模子,跟 081 tmpfs「纯逻辑 vs boot I/O」是同一套切法。
- **mapcount 闭环双计数器真相**:`pte_count`(PTE 映射数,起 0)+ `refcount`(所有权,alloc 给段基线 1)。shmat 同时 inc 两者;teardown 走 `pte_count_dec_and_test` 两级 test(pte_count 先减、归零才 dec refcount)——段页即便所有 attach 退出、pte_count 归零,refcount 仍 > 0 不放,只有 IPC_RMID 显式 free 才回收。源码注释(shm.hpp:18-22、sys_shm.cpp:12-15)把基线说成「pte_count 置 1」是历史措辞漂移,**以 pmm.cpp:229-230 为真相**。
- **shmdt 长度陷阱**:两 SHM 映射会合并成一个 VMA(flags 全等 + 无 backing),shmdt 不能取 VMA 跨度(会拆邻居页),必须用 `translate(addr) → find_by_phys → seg->page_count` 取段自己的页数当权威长度,`remove()` 戳洞处理合并 VMA 切分。`test_shm.cpp:277-319` 专门回归兜底。
- **诚实边界**:ring0 测用栈上 AddressSpace + 空壳 Task 模拟两进程(不是真 libc 跑通);`ShmRegistry` 设计上可链 host 单测但目前没配(对照 fifo 有);shmid_ds 精简五字段(跟 glibc 互操作差一截);IPC_SET/SHM_LOCK 返 ENOSYS;无权限强制、无 sequence number、SMP TOCTOU 文档化、buddy 尾页 refcount 残留是已知非严格性。这些不假装做了,留给后续工程债。
