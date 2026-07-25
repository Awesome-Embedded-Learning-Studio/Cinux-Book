---
title: Lab 006 · SysV 共享内存:两地址空间映射同一物理页
---

# Lab 006 · SysV 共享内存:两地址空间映射同一物理页

> 对应 `document/book/14-process-advanced/006/`。验证档 **A 档**:这一章交付的是 SysV 共享内存的端到端映射——两个地址空间把同一物理页映射通、写者写一个字节读者无需 syscall 就看到。punchline 是 **ring0 模拟两进程用 shm round-trip**:shmget 拿 shmid → 两个栈上 AddressSpace 各自 shmat → translate 验证两虚拟地址落到同一物理帧 → 跨 CR3 写 magic、跨 CR3 读回来相等。验证靠 kernel harness 的 ring0 测(test_shm.cpp 6 例),host 无 ShmRegistry 单测(对照 fifo 有,这是缺口)。

## 目标

确认六件事:

1. **shmget 拿 shmid**:`IPC_PRIVATE` + `IPC_CREAT` 建匿名段,返回表索引(0..15);
2. **两 AS 各自 shmat**:同一 shmid 在两个 AddressSpace 里映射,拿到的虚拟地址**可以不同**;
3. **机制证明双保险**:`translate(virt1) == translate(virt2)`(同一物理帧)+ 跨 CR3 写读(写者写、读者读到);
4. **SMAP stac/clac 窗**:ring0 读写用户映射页必须走窗口,直接读写会 #PF;
5. **CR3 还原铁律**:`write_cr3(kernel_pml4())` 必须在 AddressSpace 析构前——否则栈上 AS 析构拆当前 CR3 的页表;
6. **shmdt 长度陷阱**(可选进阶):相邻段 detach 用段的 `page_count` 不取 VMA 跨度。

## 脚手架

这一档**不给你答案堆**,只给脚手架。断言逻辑你自己填——照着 `test_shm.cpp:80-133` 的模子,把空格填满。

### 1. 头部 + 常量 + 助手

```cpp
#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging.hpp"       // write_cr3
#include "kernel/arch/x86_64/user_access.hpp"  // stac / clac
#include "kernel/ipc/shm.hpp"
#include "kernel/mm/address_space.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/sys_shm.hpp"

using cinux::arch::USER_MMAP_BASE;
using cinux::ipc::kIpcCreat;
using cinux::ipc::kIpcPrivate;
using cinux::ipc::kIpcRmid;
using cinux::mm::AddressSpace;
using cinux::proc::Scheduler;
using cinux::proc::Task;
using cinux::syscall::sys_shmat;
using cinux::syscall::sys_shmctl;
using cinux::syscall::sys_shmdt;
using cinux::syscall::sys_shmget;

namespace {
constexpr uint64_t kPageSize = 4096;

// RAII: 退出作用域前还原原 current task。测试里用 Scheduler::set_current
// 直接换 current,不走调度器循环。
struct CurrentTaskSave {
    Task* prev;
    CurrentTaskSave() : prev(Scheduler::current()) {}
    ~CurrentTaskSave() { Scheduler::set_current(prev); }
};

// SMAP 窗口写一个 8 字节小端值到用户映射页。
void user_write_u64(uint64_t addr, uint64_t value) {
    cinux::arch::stac();
    *reinterpret_cast<volatile uint64_t*>(addr) = value;
    cinux::arch::clac();
}

// SMAP 窗口读一个 8 字节小端值。
uint64_t user_read_u64(uint64_t addr) {
    cinux::arch::stac();
    uint64_t v = *reinterpret_cast<volatile uint64_t*>(addr);
    cinux::arch::clac();
    return v;
}
}  // namespace
```

(`test_shm.cpp:16-72`,基本逐字。)`CurrentTaskSave` 是 RAII——记下进测试前的 current、出作用域还原。`user_write_u64`/`user_read_u64` 是 SMAP 窗口助手——ring0 直接读写用户映射页会 #PF,必须 stac/clac 夹一下。

### 2. 主测函数——你填断言

```cpp
namespace my_shm_lab {

void test_two_spaces_share_page() {
    CurrentTaskSave save;

    // (1) shmget 建一个 1 页匿名段。shmid 应 >= 0。
    int64_t shmid = sys_shmget(kIpcPrivate, kPageSize, kIpcCreat | 0666, 0, 0, 0);
    // TODO: 断言 shmid >= 0

    // (2) 两个栈上 AddressSpace + 两个空壳 Task。
    AddressSpace as1;
    AddressSpace as2;
    Task         t1{};
    Task         t2{};
    t1.addr_space = &as1;
    t2.addr_space = &as2;

    // (3) t1 attach,拿 virt1。断言 virt1 > 0 且 >= USER_MMAP_BASE。
    Scheduler::set_current(&t1);
    int64_t virt1 = sys_shmat(shmid, 0, 0, 0, 0, 0);
    // TODO: 断言 virt1 > 0 且 >= USER_MMAP_BASE

    // (4) t2 attach,拿 virt2。断言 virt2 > 0。
    Scheduler::set_current(&t2);
    int64_t virt2 = sys_shmat(shmid, 0, 0, 0, 0, 0);
    // TODO: 断言 virt2 > 0

    // (5) 机制证明 #1:两虚拟地址落到同一物理帧。
    const uint64_t phys1 = as1.translate(static_cast<uint64_t>(virt1));
    const uint64_t phys2 = as2.translate(static_cast<uint64_t>(virt2));
    // TODO: 断言 phys1 != 0 且 phys1 == phys2

    // (6) 机制证明 #2:跨 CR3 写读。
    const uint64_t kMagic = 0x48454c4c4f53484dULL;  // "MSHELLO" 小端
    as1.activate();
    user_write_u64(static_cast<uint64_t>(virt1), kMagic);

    as2.activate();
    uint64_t seen = user_read_u64(static_cast<uint64_t>(virt2));

    // (7) CR3 还原铁律:必须在 AS 析构前回内核空间。
    cinux::arch::write_cr3(AddressSpace::kernel_pml4());

    // TODO: 断言 seen == kMagic

    // (8) 清理:两 AS 各自 detach,然后 IPC_RMID 回收段页。
    Scheduler::set_current(&t1);
    // TODO: 断言 sys_shmdt(virt1, ...) == 0
    Scheduler::set_current(&t2);
    // TODO: 断言 sys_shmdt(virt2, ...) == 0
    // TODO: 断言 sys_shmctl(shmid, kIpcRmid, 0, ...) == 0
}

}  // namespace my_shm_lab
```

填断言用 `TEST_ASSERT_TRUE(...)` / `TEST_ASSERT_EQ(a, b)`(`big_kernel_test.h:142, 150`)。

## 你会踩的两个反直觉点

这一档的真价值在这两个坑——lab-006 让你亲身体验,不是看教程就懂。

> **坑 1:直接读写用户映射页会 #PF。** `as1.activate()` 换了 CR3,现在页表是 as1 的用户空间。你想 `*((uint64_t*)virt1) = magic` 直接写——#PF。因为测试内核 **SMAP 开着**,ring0 不能直接访问用户页。必须 `stac()` 临时打开用户访问、写完 `clac()` 关上。`user_write_u64`/`user_read_u64` 就是干这个的封装。同理 066 PTY 那卷的 stac/clac 同款。

> **坑 2:不回内核空间会让 AS 析构拆自己的页表。** `as2.activate()` 之后,CR3 指向 as2 的 PML4。函数返回时,栈上的 `as1`/`as2` 析构——`~AddressSpace()` 调 `free_subtree` 把这个地址空间的页表全拆了、页表页还回 buddy。**如果当前 CR3 还指向 as2**,析构拆的就是 as2 自己的页表(包括正在跑的内核代码依赖的页表)→ 炸。所以必须在析构前 `write_cr3(AddressSpace::kernel_pml4())` 切回内核 PML4。这一步**不能漏**——漏了就是薛定谔的崩溃,看你运气。

## 可选进阶:相邻段 detach 回归

照 `test_shm.cpp:277-319` 写一个相邻段用例,亲身体验「shmdt 取段 page_count 而非 VMA 跨度」这条真坑(footgun)。模子:

```cpp
namespace my_shm_adjacent {

void test_adjacent_detach_preserves_peer() {
    CurrentTaskSave save;

    int64_t s1 = sys_shmget(kIpcPrivate, kPageSize, kIpcCreat | 0666, 0, 0, 0);
    int64_t s2 = sys_shmget(kIpcPrivate, kPageSize, kIpcCreat | 0666, 0, 0, 0);
    // TODO: 断言 s1 >= 0、s2 >= 0

    AddressSpace as;
    Task         t{};
    t.addr_space = &as;
    Scheduler::set_current(&t);

    // 用固定地址贴邻映射:a1=base、a2=base+4096。
    const uint64_t base = USER_MMAP_BASE;
    const uint64_t a1   = base;
    const uint64_t a2   = base + kPageSize;
    // TODO: 断言 sys_shmat(s1, a1, 0, ...) == a1
    // TODO: 断言 sys_shmat(s2, a2, 0, ...) == a2

    // 两段写不同 magic。
    const uint64_t kOne = 0x1111111111111111ULL;
    const uint64_t kTwo = 0x2222222222222222ULL;
    as.activate();
    user_write_u64(a1, kOne);
    user_write_u64(a2, kTwo);
    cinux::arch::write_cr3(AddressSpace::kernel_pml4());

    // 只 detach 第一段。
    // TODO: 断言 sys_shmdt(a1, ...) == 0
    // TODO: 断言 as.translate(a1) == 0  (第一页已拆)
    // TODO: 断言 as.translate(a2) != 0  (第二页还在)

    // 邻居内容未被踩。
    as.activate();
    uint64_t got = user_read_u64(a2);
    cinux::arch::write_cr3(AddressSpace::kernel_pml4());
    // TODO: 断言 got == kTwo

    // TODO: 清理 sys_shmdt(a2) + sys_shmctl(s1/s2, kIpcRmid)
}

}  // namespace my_shm_adjacent
```

如果 shmdt 错按 VMA 跨度算长度(合并后 VMA 跨度 = 8192),`sys_shmdt(a1)` 会把 a2 的页也拆了——`translate(a2)` 返 0、读 a2 会读到 0 或脏值而非 kTwo。改回取段 `page_count`(每段 1 页)就对了。**这条真坑亲自踩一遍,比看十遍教程记得牢。**

## 跑测

### 1. 把你的函数挂进 big_kernel_test

在 `kernel/test/main_test.cpp` 找 `run_shm_tests` 的调用处(`main_test.cpp:1208` 附近),或者加自己的入口调用 `my_shm_lab::test_two_spaces_share_page()`。

### 2. 编 + 跑 kernel test

```bash
cmake --build build --target run-kernel-test 2>&1 | grep -iE "shm|my_shm" | head
```

应看到你的测 `[PASS]`。如果挂在 SMAP #PF 或 AS 析构崩溃,回头看上面那两个坑。

### 3. 对照官方 6 例

你的最小用例应该跟官方 `test_shm.cpp:80-133` 的 Test1 走的是同一条路径——这是验证你理解对了模子。其他 5 例(STAT、RMID、命名 key、非法参数、相邻段)你也可以照着扩,但 lab 只要 Test1 + 可选相邻段就够验证机制。

### 4. 全量两腿

```bash
cmake --build build --target run-kernel-test-all 2>&1 | grep -E "Tests: [0-9]+ passed" | tail -1
```

单核 + `-smp 2` 两腿都应 `passed, 0 failed`(基线 + 你的测,具体数字看你工作树基线)。**别照抄源仓库 dev note 的 1067**——那是源仓库的数,Book 侧须独立实跑。

## 验收清单

- [ ] `shmget(IPC_PRIVATE, 4096, IPC_CREAT|0666)` 拿到 shmid >= 0。
- [ ] 两个栈上 AddressSpace 各自 `sys_shmat`,virt1/virt2 都 > 0 且 >= USER_MMAP_BASE。
- [ ] `as1.translate(virt1) == as2.translate(virt2)`(同一物理帧,机制证明 #1)。
- [ ] `as1.activate()` + `user_write_u64`(SMAP 窗)写 magic、`as2.activate()` + `user_read_u64` 读、`seen == kMagic`(机制证明 #2)。
- [ ] `write_cr3(AddressSpace::kernel_pml4())` 在 AS 析构前调用(CR3 还原铁律,坑 2)。
- [ ] 两 AS 各自 `sys_shmdt` 返 0,`sys_shmctl(shmid, kIpcRmid)` 返 0。
- [ ] (可选进阶)相邻段 detach 后 `translate(a2) != 0`、读 a2 仍得 kTwo(shmdt 取 page_count 不取 VMA 跨度)。
- [ ] `run-kernel-test-all` 两腿 0 failed,passed 数字 Book 实测填。

## 别做这些

- **别**在 ring0 直接 `*((uint64_t*)virt) = ...` 读写用户映射页——SMAP 挡,#PF。必须 stac/clac 窗(`user_write_u64`/`user_read_u64`)。同 066 PTY 那套 stac/clac。
- **别**漏 `write_cr3(AddressSpace::kernel_pml4())`——栈上 AddressSpace 析构会拆当前 CR3 的页表,不切回内核 PML4 会拆自己(薛定谔崩溃)。
- **别**用 `vma->end - vma->start` 算 shmdt 长度——两 SHM 映射会合并成一个 VMA,按 VMA 算会拆邻居页。用 `translate(addr) → find_by_phys → seg->page_count` 取段自己的页数。
- **别**拿 `pte_count = 1` 当 alloc 基线去 grep——batch 3 拆成双计数器后,基线在 refcount 上(alloc 设 refcount=1、pte_count=0)。源码注释的旧措辞跟 pmm.cpp 真值对不上,以 pmm.cpp:229-230 为真相。
- **别**以为这套测覆盖了「真用户态两进程跑」——ring0 测用栈上 AS + 空壳 Task 模拟两进程,`Scheduler::set_current` 直接换 current,不走调度器循环。真 libc 跑通是 follow-up(要先把 shmid_ds 拓宽到 Linux 全 ipc_perm + 时间戳布局)。
- **别**照抄源仓库 dev note 的 passed 数字——那是源仓库 worktree 的,Book 侧须独立实跑 run-kernel-test-all。
- **别**指望 ShmRegistry 有 host 单测——对照 FIFO 有 `test_fifo.cpp`,shm 没配,纯逻辑层目前只被 ring0 syscall 测顺带覆盖(诚实缺口)。
