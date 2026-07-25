---
title: Lab 004 · F-QA 质量加固验证
---

# Lab 004 · F-QA 质量加固验证

> 对应 `document/book/15-smp/004/`。验证档 **B 档**(构建零警告 + 内核测试绿)。本章是 SMP 真跑后的并发债清算 + 类型安全/可观测加固,验证靠构建门禁 + 内核测试(含新增 refcount/user_ptr/mapcount 用例)+ grep。

## 目标

确认六件事:

1. `RefCount`(饱和引用计数,Cinux-Base)在 + `UserPtr<T>`(用户指针标记)在;
2. PidAllocator / 任务注册表加了 irq-safe Spinlock;
3. CoW per-page mapcount 在(mapcount_inc/dec_and_test);
4. AddressSpace 有 RefCount + clone CLONEVM acquire;
5. 零警告门禁(kernel/CMakeLists 的 -Werror)+ UBSan 桩;
6. 构建零错误零警告 + `run-kernel-test` 全绿(含新测试)。

## 步骤

### 1. 构建(开测试,host 测试跳过——见已知债)

```bash
cmake -B build -S . -DCINUX_BUILD_TESTS=ON && cmake --build build --target big_kernel_test -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
```

> 只构建 `big_kernel_test`(内核测试镜像)。**别** `cmake --build build`(ALL):host 单测有个既有债(`test_ext2_inode_ops` 的 mock 滞后于内核 `ErrorOr` 接口)会让 ALL 失败,跟本章无关。

### 2. 加固产物 grep

```bash
grep -rn 'RefCount\|UserPtr' kernel/ third_party/Cinux-Base/include/cinux/ | grep -vE '\.o:' | head
grep -rn 'mapcount_inc\|mapcount_dec\|irq_guard\|Spinlock' kernel/mm/ kernel/proc/ | grep -vE '\.o:' | head
grep -n 'Werror\|-Wimplicit-fallthrough\|CINUX_HOST_ASAN' kernel/CMakeLists.txt test/CMakeLists.txt | head
```

去看:`RefCount` 的饱和逻辑(acquire/release 到饱和值不动)、`UserPtr` 的类型标记(零开销、可持 nullptr)、`mapcount` 在 fork/exec/handle_cow_fault 的配对、PidAllocator 的 `irq_guard`。

### 3. 内核测试

```bash
cmake --build build --target run-kernel-test 2>&1 | tail -5
```

应全绿,含本章新增的 `test_refcount`(饱和计数不 wrap)、`test_user_ptr`(类型标记)、`test_pmm_mapcount`(CoW 页计数)。

**思考**:为什么普通原子计数器做引用计数会 wrap 成 UAF?——见章节:release 多调一次减过 0,wrap 成大正数(假活)或回 0(double free);饱和计数卡在"已死"状态,bug 表现成卡死/报错而非隐蔽 UAF。**为什么用 `__atomic_*` 不用 `std::atomic`?**——`std::atomic` 拖 `__glibcxx_assert_fail` 符号,内核 `-ffreestanding -nostdlib` 链不过;`__atomic_*` 是 x86-64 lock-free 内联,零外部符号。

## 验收清单

- [ ] `big_kernel_test` 构建 `build=0`,**零警告**(门禁生效)。
- [ ] `RefCount`(Cinux-Base)+ `UserPtr<T>`(kernel/lib)在。
- [ ] PidAllocator / 任务注册表有 irq-safe Spinlock;CoW mapcount 配对;AddressSpace RefCount + clone acquire。
- [ ] 零警告门禁(kernel/CMakeLists -Werror)+ UBSan 桩(报具体 type)。
- [ ] `run-kernel-test` 全绿(含 test_refcount/test_user_ptr/test_pmm_mapcount)。

## 别做这些

- **别**用 plain `std::atomic<int>` 做引用计数——会 wrap 成 UAF;用饱和 `RefCount`。
- **别**给分配器/注册表留"单核串行"假设——SMP 真跑后双核并发,必须 irq-safe Spinlock。
- **别**fork/exec 不维护 CoW mapcount——共享页会被误释放,UAF。
- **别**指望 `UserPtr` 自动保证安全——它是类型标记脚手架,运行时校验仍在 `validate_user_ptr`;直接解引用和裸指针一样危险。
- **别** `cmake --build build`(ALL)验证本章——host 单测既有债(test_ext2_inode_ops)会失败;用 `big_kernel_test` + `run-kernel-test`。
