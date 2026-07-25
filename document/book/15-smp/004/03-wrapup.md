---
title: 03 · 收尾:验证、已知局限、下一站
---

# 收尾:验证、已知局限、下一站

## 验证

```bash
# 加固产物都在
grep -rn 'RefCount\|UserPtr\|mapcount\|irq_guard\|Spinlock' kernel/ third_party/Cinux-Base/include/cinux/ | grep -vE '\.o:' | head
grep -n 'Werror\|-Wimplicit-fallthrough\|CINUX_HOST_ASAN' kernel/CMakeLists.txt test/CMakeLists.txt | head
```

构建 + 内核测试:

```bash
cmake -B build -S . -DCINUX_BUILD_TESTS=ON && cmake --build build --target big_kernel_test -j$(nproc)
cmake --build build --target run-kernel-test
```

B 档:`big_kernel_test` 构建零错误 + 零警告(门禁生效),`run-kernel-test` 全绿(含本章相关的内核测试 `test_user_ptr`(用户指针标记)、`test_pmm_pte_count`(覆盖 refcount + pte_count 拆分后的 CoW 计数契约)——后者取代了拆分前的 `test_pmm_mapcount`)。饱和引用计数(`RefCount` 类型本身)则在 host 单测 `test/unit/test_refcount.cpp` 里验证,不是内核测试。

## 已知局限

- **`UserPtr` 暂无消费者**:类型标记铺好了,但要等 `access_ok` + `copy_to/from_user`(后续)用它,才真正隔离用户/内核指针。现在它是脚手架。
- **`-Wframe-larger-than` 暂缓**:几个 syscall handler 在 16 KB 栈上放 `char[PATH_MAX]` 大缓冲(集中的路径工具在 `kernel/syscall/path_util.cpp`),现在开会破坏零警告;得先把那些缓冲挪出栈。
- **host 测试的既有债**:某个 host 单测(`test_ext2_inode_ops`)的 mock 还停在旧接口(返回 `int64_t`,滞后于内核 `InodeOps` 的 `ErrorOr`),host 测试整体编不过。这是 Book 既有的测试维护债(不属本弧),内核测试不受影响、全绿。

## 小结与下一站

两个核真跑之后,这一弧把并发债(分配器/注册表/CoW 页/退出路径)连同类型安全(UserPtr)和可观测性(零警告门禁 + UBSan)一起往上抬了一档。饱和引用计数堵住了 wrap 出来的 UAF,irq-safe 自旋锁护住了全局结构,CoW mapcount 让 fork/exec 的页共享有了正确性依据。

下一站回到时间序:GUI 解耦、xHCI、再往后是 SMP 迁移竞态的最终修复(把上一章那个 `-smp 2` AHCI heisenbug 收掉)。
