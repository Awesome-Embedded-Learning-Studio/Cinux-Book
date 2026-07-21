---
title: Lab 040 · VMA 与 mmap 验证
---

# Lab 040 · VMA 与 mmap 验证

> 对应 `document/book/13-memory-advanced/040-vma-mmap.md`。验证档 **A 档**(用户态 mmap 可见)+ B(VMA 记账内部)。验证靠构建 + 测试 + grep + mmap round-trip。

## 目标

确认四件事:

1. `AddressSpace` 有 VMA 账本(`LinkedListVMAStore`),`vmas()` 可访问;
2. `mmap`/`munmap`/`mprotect` 三个 syscall 在(6 参,注册到 syscall 表);
3. execve 段 + 用户栈都注册了 VMA(`init.cpp` 的 `kStackVma`);
4. run-kernel-test 从 039 的 705 涨到 721。

## 步骤

### 1. 构建 + 测试(真退出码)

```bash
git checkout 040_vma_mmap 2>/dev/null || git checkout 040_*
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

**期望**:`build=0`;run-kernel-test 全绿 ~721(VMA store +7、mmap/munmap/mprotect +9)。

### 2. VMA 账本

```bash
grep -rn 'class LinkedListVMAStore\|class IVMAStore\|enum class VmaFlags' kernel/mm/vma.hpp
grep -n 'vma_store_\|vmas()' kernel/mm/address_space.hpp
# 期望:vma.hpp:142 LinkedListVMAStore、:105 IVMAStore、:51 VmaFlags;address_space.hpp:124 vmas()、:150 vma_store_
```

**思考**:为什么 VMA 账本用链表(`LinkedListVMAStore`)而不是红黑树?——见章节:区域少时链表实现简单够快;`IVMAStore` 留了抽象,将来区域多了换红黑树只换实现。**抽象的时机是"有第二个实现的需求出现",不是"看起来能抽象"。**

### 3. mmap 三 syscall

```bash
grep -rn 'int64_t sys_mmap\|int64_t sys_munmap\|int64_t sys_mprotect' kernel/syscall/
# 期望:三个都在 sys_mmap.hpp,都是 6 参(匹配 SyscallFn)
```

**思考**:`sys_munmap` 只用前两个参数,为什么也声明 6 参?——见章节:syscall 表统一 `SyscallFn` 类型,不匹配就注册不上,后面的参数是哑的。

### 4. 栈 VMA 注册 + PF 诊断

```bash
grep -n 'VmaFlags::Stack\|vmas().insert' kernel/proc/init.cpp
# 期望:init.cpp 建栈后 insert 一条带 Stack flag 的 VMA
```

去看 demand paging 的 PF handler:它查 VMA 时没命中应该 `klog_warn`(诊断),但**仍然 demand page**(没真 segfault)。**思考**:为什么不直接 segfault?——见章节「关键决策」:真 segfault 要重构 demand paging(VMA 硬门控 + 栈下扩 + guard),漏判即 shell halt,高风险;留 M5。**这是分阶段重构的判断。**

### 5.(A 档)mmap round-trip

内核测试里 `sys_mmap` 的几条(匿名映射、写、读回、`munmap` 释放)就是端到端验证。想自己跑:测试里 `Scheduler::set_current(&tmp)` 装一个临时 Task(`tmp.addr_space=&as`),然后 `sys_mmap(...)` → 写字节 → 读回 → `sys_munmap`。

## 验收清单

- [ ] 构建 `build=0`,run-kernel-test ~721。
- [ ] `LinkedListVMAStore` + `IVMAStore` + `VmaFlags` 在;`AddressSpace::vmas()` 可访问。
- [ ] mmap/munmap/mprotect 三 syscall 在(6 参)。
- [ ] init.cpp 栈 VMA 注册(带 `Stack` flag)。
- [ ] 能说清「为什么 PF 只诊断不门控」「为什么 VMA 用链表」。

## 别做这些

- **别**写 `cinux::lib::klog_warn(...)`——它是宏,带命名空间前缀会语法错。用裸 `klog_warn(...)`。
- **别**在 demand paging 里立刻上"无 VMA → segfault"硬门控——会 shell halt,留 M5 重构。
