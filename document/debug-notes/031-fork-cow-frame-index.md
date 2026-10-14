---
title: 调试档案 031 · fork teardown 恒漏一页与 COW 安全审查
---

# fork teardown 恒漏一页与 COW 安全审查

本文针对 2026-10-12 工作区的 Cinux v2 实现。页号索引已修复；下文列出的其他审查发现仍待修复。旧版 `document/notes/035/` 的安全结论不适用于这份实现。

## 根因：页号又除了一次页大小

`base/include/cinux/math.hpp` 的 `Floor(value, unit)` 返回 `value / unit`，不是向下对齐后的字节地址。原 `kernel/mm/pmm.cpp` 写成：

```cpp
return math::Floor(phys.raw, page::kSize) / page::kSize;
```

因此真正使用的索引是 `phys / 4096 / 4096`。同一段 16 MiB 内的 4096 个物理帧落到同一个 `shares_[]` 槽。修复为：

```cpp
return math::Floor(phys.raw, page::kSize);
```

三条虚拟映射对应两个物理帧：A 映射两次，B 映射一次。clone 后 A 应计 4、B 应计 2；原代码却把它们合并成 6。按 child teardown、parent teardown 的顺序遍历：

| 释放的映射 | 错误的共用计数 | 是否 free 当前帧 |
|---|---:|---|
| child A 第一处 | 5 | 否 |
| child A 第二处 | 4 | 否 |
| child B | 3 | 否 |
| parent A 第一处 | 2 | 否 |
| parent A 第二处 | 1 | 否 |
| parent B | 0 | 是，释放 B |

A 的最后一处映射已经退场，却因 B 的份额仍在同一个槽里而没有释放。后来共用计数归零时，调用者只知道眼前的 B，不会回头释放 A。于是总 retain/release 对称，PMM 位图仍少一个空闲页。

单页世界只有一个物理帧，所有释放都指向它，即使索引错了也会在最后释放它。决定性差别是两个物理帧撞计数槽；页表链数本身不是根因。同一条 PT 链中的两个不同物理帧也能触发，两个 PML4 分支共用一个物理帧则不会因这处错误漏页。

`test/unit/test_pmm.cpp` 新增相邻物理帧的独立计数回归，模拟 A 四份、B 两份引用，并按 `A,A,B,A,A,B` 释放、断言各帧的剩余引用。原代码在第一条剩余计数断言失败，修复后通过。

## 其他安全审查发现

### 高：缺页处理破坏 SIMD 寄存器

`kernel/mm/cow.cpp:117` 的 `CopyBytes` 在当前普通构建中生成 `movdqa (...), %xmm0`、`movups %xmm0, (...)`。异常 stub 和 `ReportFault` 使用 `-mgeneral-regs-only`，但 `cow.cpp` 不在 `kernel/CMakeLists.txt` 的 `CINUX_KERNEL_NO_SSE_ISLAND` 中，异常路径也没有保存/恢复 FPU/SIMD 状态。因此真实 COW 写故障能破坏被中断代码的 XMM0。

需要将可返回的缺页处理及其可达调用链完整纳入禁止 SIMD 的编译范围，或在异常边界保存/恢复完整状态。只给 exception.cpp 加编译选项无法约束间接调用到的 resolver。

### 高：可抢占 syscall 使用 per-CPU 槽保存任务快照

`kernel/arch/x86_64/syscall.S:21` 将 frame 指针和 r12..r15 写入 GS 槽，随后 `sti`。`kernel/syscall/sys_fork.cpp:12` 从槽中取 frame，`kernel/proc/fork.cpp:60` 在克隆地址空间和分配之后再取 r12..r15。调度切换不会恢复这些槽。

任务 A 进入 fork 后被抢占，任务 B 执行任意 syscall 就会覆盖这些值；A 恢复后可将 B 的 frame 或寄存器交给自己的 child。即便给 BuildForkChild 加关中断，也不能修复进入 HandleFork 之前发生的覆盖。快照应保存在任务自己的 syscall 栈帧中，并沿调用链传递，或在重新调度时完整恢复任务的入口状态。

另外，普通 syscall 返回段 `syscall.S:32` 至 `sysretq` 没有关中断。共享 `gs:16` 返回值能被另一任务覆盖，切回用户 RSP 后仍可在 CPL0 接受 IRQ。返回收尾需要在仍使用内核栈时关闭中断；SYSRET 再恢复用户 RFLAGS。

### 高：重复 fork 对同一父映射重复 retain

`kernel/mm/cow.cpp:84` 无条件 retain。首次克隆后父、子合计 2；父再次克隆时又 retain 父和新子，合计 4，但真实映射只有 3。全部 teardown 后剩 1，导致新的确定性泄漏；不断 fork 最终还会撞到 0xFF 检查。

同一已登记的只读 COW 映射再次被标记时不应增加引用。当前 resolver 又会留下 writable+kCow、但引用为 0 的私有页，所以修复不能简单看到 kCow 就跳过；需要区分已登记的只读共享状态与写故障后的私有状态，或统一改为每个映射都登记引用。

### 高：clone 将原权限统一改成 user+COW

`share_leaf` 不检查原叶的 kUser、kWritable；child.map_user 创建 user+writable 映射，MarkPageShared 再用固定 flags 覆盖父子叶。结果包括：

- 用户半区的 supervisor 4 KiB 叶可变成用户可访问映射；若上层权限也允许，用户能访问原本不允许的帧。
- 原本真正只读的页变成可以在写故障后获得写权限的 COW 页。
- 原叶的其他属性被丢弃，包括已有的缓存或执行限制位。

当前 map_user 创建的普通可写叶没有这些权限差异，现有测试因此不能覆盖该问题。应保留原权限，仅对原本可写或已经是 COW 的映射授予 COW 写恢复资格；纯只读页的共享引用仍需要被正确登记。

### 高：fork 和 COW 的 OOM 路径会停机

`cow.cpp:45` 的页表分配失败触发 Check，没有销毁部分 child 后返回 nullptr。`cow.cpp:105` 在申请副本之前减少旧帧引用，随后分配失败同样触发 Check。若以后改成返回失败而不修顺序，旧映射仍在却少了引用，会留下提前释放风险。

`kernel/mm/heap_runtime.cpp` 的 operator new 在耗尽时也触发 Check，所以 CloneSharedSpace 和 BuildForkChild 中对 new 结果的 nullptr 检查不能覆盖堆耗尽。`Scheduler::seat` 的 ready 队列满也触发 Check。

fork 应完整销毁未发布的 child 并返回 ENOMEM。COW 应先取得可用副本，再提交 PTE 和旧引用变化；资源不足应通过明确的任务故障策略处理，而不是停掉整机。

### 高：单页 PMM 分配没有 IRQ 保护

`kernel/mm/pmm.hpp:65` 的 allocate_page 直接进入 allocate_run；allocate_pages 则有 IrqGuard。allocate_run 的 find_clear_run 与 set_range 不是一个不可中断操作。可抢占 syscall 中的 clone 使用单页分配，另一任务若在这个窗口分配，可以拿到同一帧。

应统一保护所有 PMM 分配入口。单核关中断只能解决当前调度模型；今后 SMP 仍需真正的并发保护和 TLB shootdown。

### 中：文件描述符与 FPU 继承不完整

`kernel/proc/fork.cpp:85` 用最低空闲槽 alloc 来继承 fd。若父 fd 为 3、5，子得到 3、4，原描述符编号发生变化。offset 按值复制，父子也没有共享同一个打开文件的游标。需要支持按原槽继承，并用共享的打开文件对象管理 offset 和生命周期。

`fork.cpp:58` 复制的是父任务 ctx 中上次切换保存的 FPU image，不是本次 syscall 入口的状态；新任务首次运行以来、或上次恢复后更新的用户 FPU 状态不会被正确继承。当前 syscall C++ 本身也使用 SIMD，因此临近 fork 时才读取硬件 FPU 状态仍可能已经太晚。应在进入可使用 SIMD 的内核代码之前保存用户态状态。

## 本次验证

- 普通构建的 43 项宿主 CTest 全通过；普通测试镜像及 boot_image 构建成功。
- ASan+UBSan+lockdep 的 43 项宿主 CTest 全通过。首次在沙箱内运行被 LeakSanitizer 的 ptrace 限制阻断，随后在沙箱外以 detect_leaks=1、halt_on_error=1 重跑通过。
- TSan+lockdep 的 43 项宿主 CTest 全通过。
- lockdep+内核 UBSan 配置的 43 项宿主 CTest 全通过；cow、sched 测试镜像及 boot_image 构建成功。
- `scripts/checks/acceptance.sh` 在普通 KVM 测试阶段因缺少 `/dev/kvm` 中止；另行运行 checks 配置的 cow/sched KVM 测试也因同一原因无法启动。没有切换为 TCG，未声称本次机内测试通过。
- 上述其他问题来自代码和反汇编审查，尚未用 KVM 实弹验证；本次代码修复仅包含 PMM 页号索引和对应回归测试。

具备 KVM 的环境仍须运行完整 acceptance，并补上重复/嵌套 clone、父先退出、双方真实写入、OOM 回滚、带 fd 空洞继承及多任务 syscall 快照压力测试。
