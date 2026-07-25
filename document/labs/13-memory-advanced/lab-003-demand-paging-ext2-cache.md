---
title: Lab 003 · demand paging 硬门控 + ext2 缓存 验证
---

# Lab 003 · demand paging 硬门控 + ext2 缓存 验证

> 对应 `document/book/13-memory-advanced/003/`。验证档 **B 档**(行为变化 + 性能优化)。验证靠构建 + 测试 + grep + 命中计数 + 实机不崩。

## 目标

确认四件事:

1. PF 硬门控:用户态 not-present PF 命不中 VMA → `exit_current` 真 segfault,靠 `err & 0x04` 区分 user/kernel;
2. 栈增长窗扩到 1MB(`USER_STACK_GROWTH`),配套硬门控;
3. `sys_read` 对磁盘文件走 `PageCache::read_bytes`,靠 `is_page_cacheable()` virtual 判别;
4. run-kernel-test 从 002 的 730 涨到 734。

## 步骤

### 1. 构建 + 测试

```bash
git checkout 003_demand_paging_ext2_cache 2>/dev/null || git checkout 003_*
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

**期望**:`build=0`;run-kernel-test ~734。

### 2. PF 硬门控

```bash
grep -nE 'err & 0x04|exit_current' kernel/arch/x86_64/exception_handlers.cpp
grep -n 'USER_STACK_GROWTH' kernel/arch/x86_64/usermode.hpp
# 期望:PF handler 有 user_fault=(err&0x04) 判定 + exit_current;USER_STACK_GROWTH=0x100000(1MB)
```

**思考**:为什么硬门控靠 `err & 0x04` 区分?——见章节:kernel-test 是 ring0 访问用户地址(`err&0x04=0`),真实用户程序是 ring3 fault(`err&0x04=1`)。只杀后者,前者保持零页容错给测试留活路。**为什么栈 VMA 必须配套扩到 1MB?**——硬门控上了但栈还 16KB,深调用栈用户程序栈 PF 落 VMA 外 → segfault 自爆。门控 + 栈扩必须配套。

### 3. ext2 read 经 PageCache

```bash
grep -rn 'read_bytes\|is_page_cacheable' kernel/mm/page_cache.hpp kernel/fs/inode.hpp kernel/syscall/sys_read.cpp
# 期望:page_cache.hpp:87 read_bytes;inode.hpp:81 is_page_cacheable(默认 false);sys_read.cpp:54 三元分流
grep -n 'is_page_cacheable' kernel/fs/ext2_common.hpp
# 期望:Ext2FileOps override 返 true
```

**思考**:为什么用 `is_page_cacheable()` virtual 而不是 `inode->type` 字段?——见章节:pipe 的 type 也是 Regular,靠 type 会误把 pipe 路由进缓存。virtual 带默认实现源码兼容,现有子类不用改。

### 4.(端到端)命中计数

内核测试里真 ext2 读 `/hello.txt` 两遍:第一遍 miss(读盘),第二遍 `hit_count()` 上升、读盘次数不增、字节一致——这就是 sys_read→read_bytes→get_page 全链通的铁证。

## 验收清单

- [ ] 构建 `build=0`,run-kernel-test ~734。
- [ ] PF 硬门控(`err&0x04` + exit_current)在,栈 `USER_STACK_GROWTH=1MB`。
- [ ] `read_bytes` + `is_page_cacheable()` 在,sys_read 三元分流。
- [ ] 能说清「为什么靠 err&0x04 区分」「为什么 read_bytes 不能让 Ext2FileOps::read 反向调 cache(会成环)」。

## 别做这些

- **别**在硬门控里不加 `err&0x04` 判定就杀——kernel-test(ring0)会被误杀 hang。
- **别**让 `Ext2FileOps::read` 调 page_cache——read→get_page→read 死循环。
- **别**上硬门控不扩栈 VMA——深栈用户程序自爆(测试盲区掩盖,实机才暴露)。
