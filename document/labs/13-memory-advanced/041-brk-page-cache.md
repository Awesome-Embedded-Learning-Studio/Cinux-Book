---
title: Lab 041 · brk 与 Page Cache 验证
---

# Lab 041 · brk 与 Page Cache 验证

> 对应 `document/book/13-memory-advanced/041-brk-page-cache.md`。验证档 **A 档**(brk/文件 mmap 端到端可见)。验证靠构建 + 测试 + grep + 命中计数。

## 目标

确认四件事:

1. `sys_brk` 在(6 参),懒实现——只挪 `brk_current`,Heap VMA 覆盖整个堆窗;
2. Page Cache(`PageCache`/`CachedPage`/`g_page_cache`)在,`get_page` 锁外读/锁内 insert;
3. `handle_pf` 文件感知——`backing != nullptr` 时走 `get_page` 取真内容;
4. run-kernel-test 从 040 的 721 涨到 730。

## 步骤

### 1. 构建 + 测试

```bash
git checkout 041_brk_page_cache 2>/dev/null || git checkout 041_*
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

**期望**:`build=0`;run-kernel-test ~730。

### 2. brk 懒实现

```bash
grep -rn 'int64_t sys_brk' kernel/syscall/sys_brk.hpp
grep -n 'brk_current\|brk_initial\|brk_max' kernel/proc/process.hpp
# 期望:sys_brk.hpp:27 签名(6 参);process.hpp 有 brk 三字段
```

**思考**:`sys_brk` 为什么不 map/unmap 页?——见章节:Heap VMA 覆盖 `[brk_initial, USER_BRK_MAX)` 整个窗,brk 只挪 `brk_current`,堆增长靠 PF demand paging。和 mmap 懒分配统一。**brk 返地址不返 errno**(Linux ABI),和 mmap 不同——为什么?

### 3. Page Cache

```bash
grep -rn 'class PageCache\|get_page\|g_page_cache\|hit_count\|miss_count' kernel/mm/page_cache.hpp
# 期望:page_cache.hpp:64 PageCache、:78 get_page、:103 g_page_cache、:84/85 hit/miss_count
```

**思考**:`get_page` 为什么"锁外读、锁内 insert"?——见章节:持着 cache 锁去 `inode->ops->read` 读盘,读盘路径若再碰 cache 就重入死锁。所以未命中时**放锁**读盘、再拿锁短临界区 insert。

### 4. handle_pf 文件感知

```bash
grep -n 'backing != nullptr\|g_page_cache.get_page\|file_off' kernel/arch/x86_64/exception_handlers.cpp
# 期望:PF handler 在匿名路径之外,加了 backing!=nullptr → get_page → map 的分支
```

去看匿名/无 VMA 路径**没改**——这种"加分支不动老路径"的改法把回归风险压到最低(execve ELF 段是匿名,boot 期间文件路径 dormant)。

### 5.(A 档)端到端 + 命中计数

内核测试里 file mmap 的 Test A:把 ext2 上某文件的字节和 cache 命中后读到的字节比对——对上 = demand-read 真读到了盘内容。跑完看 `g_page_cache.hit_count()` / `miss_count()`:第一次读是 miss(从盘读),第二次同偏移读是 hit。

## 验收清单

- [ ] 构建 `build=0`,run-kernel-test ~730。
- [ ] `sys_brk`(6 参)+ Task 的 brk 三字段在。
- [ ] `PageCache`/`get_page`/`g_page_cache` 在,`get_page` 锁外读锁内 insert。
- [ ] `handle_pf` 的 `backing != nullptr` 文件分支在。
- [ ] 能说清「为什么 brk 懒」「为什么 get_page 锁外读」「为什么 NX 位暂不设(GOTCHA #10)」。

## 别做这些

- **别**在 PF 文件路径给非 exec 页设 NX 位——EFER.NXE 未启用(F9 才做),NX 是保留位,设了 reserved-bit #PF 无限循环(GOTCHA #10)。
- **别**在 `get_page` 持锁的状态下调 `inode->ops->read`——重入死锁。
