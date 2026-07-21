---
title: 042 · demand paging 硬门控 + ext2 缓存
---

# 042 · demand paging 硬门控 + ext2 缓存:野指针终于会被杀,read() 也走缓存了

> 040 立了 VMA 账本但只做"诊断",041 补了 brk + Page Cache。这一章把 F2 收一收:**demand paging 升级成硬门控**(无 VMA 命中的用户态访问终于真 segfault,不再是静默容错),外加 **ext2 的 `read()` 也走 Page Cache**(读路径统一到一个缓存层,重复读免再读盘)。
>
> 主要是 B 档:硬门控是"撤掉容错"的行为变化(野指针/栈溢出现在杀进程),ext2 cache 是性能优化。验证靠构建 + 测试 + 实机不崩。

## 这章咱们要点亮什么

1. **PF 硬门控**:用户态 not-present PF,命不中 VMA → 真 segfault(终止进程),不再映射零页静默放过。
2. **ext2 `read()` 走 Page Cache**:`sys_read` 读磁盘文件改走 `PageCache::read_bytes`,和 demand paging 共用一份 `(inode, 页偏移)` 缓存,重复读命中免读盘。

## demand paging:从诊断到硬门控

040 给 PF handler 加了 VMA `find()`,但没命中只 `klog_warn` 然后**继续映射零页**——NULL 解引用、野指针、栈溢出全被静默容错,VMA 账本沦为死数据。042 把它拧成硬门控:

```cpp
// kernel/arch/x86_64/exception_handlers.cpp:273 / :280
const bool user_fault = (err & 0x04) != 0;   // 0x04 = user-mode 触发
if (vma == nullptr && user_fault) {
    cinux::lib::klog_error(...);
    cinux::proc::Scheduler::exit_current();   // 真 segfault:终止进程,不返回
}
```

关键在 **`err & 0x04`** 这一位——它区分**真实用户态 fault**(ring3 触发,`err&0x04=1`)和**内核态访问用户地址**(ring0,比如 kernel-test 注入 PF、或 `copy_to_from_user`,`err&0x04=0`)。硬门控**只杀前者**;后者保持零页容错。这不是偏袒,是给测试留活路:内核测试是 ring0 访问无 VMA 的用户地址,要是也杀,测试就 hang 了。

### segfault 怎么"杀"

用的是 `Scheduler::exit_current()`——`context_switch` 切到下一个任务,**不返回** PF handler 的栈帧。被杀进程的中断帧整体抛弃,等价 SIGSEGV-killed。比"标记 Dead + 延迟退出"简单(后者要伪造中断帧,否则 iret 重执行 faulting 指令 → PF 死循环)。

> 真正的 SIGSEGV **信号**交付是 F3 的事;现在只是"杀"(等价 SIGKILL),没有 handler、没有 core dump、没有 waitpid status。先把地址合法性门控做对,信号外壳后套。

### 栈必须配套扩到 1MB

硬门控上了,但栈 VMA 还是 040 的 16KB 的话,**会自爆**——深调用栈的用户程序(init/gui/shell)栈 PF 落到 VMA 外 → segfault。所以这一弧把栈增长窗从 16KB 扩到 1MB:

```cpp
// kernel/arch/x86_64/usermode.hpp:45
constexpr uint64_t USER_STACK_GROWTH = 0x100000ULL;  // 1 MB
// 栈 VMA: [USER_STACK_TOP - 1MB, TOP),顶 16KB 预分配,余 demand-page 向下长
```

VMA 底(TOP-1MB)以下没有 VMA → segfault,这就是隐式的栈溢出 guard。批1(门控)+ 批2(栈扩)**必须配套**——单上批1,实机会崩(run-kernel-test 不跑真实深栈,730/0 的绿会掩盖这个依赖)。

> 教训:run-kernel-test 有盲区——它全是 kernel-mode fault(`err&0x04=0`),**不覆盖** user-mode segfault 路径。user-mode 路径靠实机冒烟(启动到 GUI 桌面不崩)间接覆盖。绿测试 ≠ 用户路径没问题。

## ext2 `read()` 走 Page Cache

041 的 Page Cache 只服务 file-backed mmap 的 demand paging。而 `sys_read` 读普通文件直走 `Ext2FileOps::read`——**每次按 ext2 块读盘,无缓存**。于是同一文件"mmap 读"和"read() 读"各走各的,read() 重复读反复 I/O。042 把 read() 也接进 Page Cache:

```cpp
// kernel/mm/page_cache.hpp:87
cinux::lib::ErrorOr<int64_t> read_bytes(Inode* inode, uint64_t file_off, void* buf, uint64_t count);
```

`read_bytes` 按页切片,每页调既有的 `get_page`(命中 bump ref / 未命中锁外读盘填充 / EOF 零填),再 memcpy 切片到用户 buf。**复用 041 的 `get_page`,不另起缓存逻辑**——读路径从此只有一层缓存。

### 怎么判别"该走缓存的磁盘文件" vs "不该走缓存的 pipe"

坑点:pipe 的 inode `type` 也是 `Regular`(和 ext2 文件一样),靠 `type` 字段判会把 pipe 误路由进 page_cache(pipe 内容不在盘上,缓存就错)。又禁 RTTI,不能 `dynamic_cast`。解法是给 `InodeOps` 加一个带默认实现的 virtual:

```cpp
// kernel/fs/inode.hpp:81
virtual bool is_page_cacheable() const;   // 默认 false
// Ext2FileOps override 返 true;pipe/ramdisk/mock 继承默认 false,行为不变
```

`sys_read` 分流(`kernel/syscall/sys_read.cpp:54`):

```cpp
file->inode->ops->is_page_cacheable()
    ? cinux::mm::g_page_cache.read_bytes(file->inode, file->offset, buf, count)
    : file->inode->ops->read(file->inode, file->offset, buf, count);
```

virtual 带默认实现是**源码兼容**的——所有现有子类(PipeReadOps/RamdiskFileOps/测试 mock)继承默认 false,**不用改一行**;只有 Ext2FileOps override 成 true。这比加个 type 字段或开 RTTI 都干净。

> 一个必须避开的环:`read_bytes → get_page → Ext2FileOps::read`(读盘原语)。`Ext2FileOps::read` **绝不调** page_cache,否则 read→get_page→read 死循环。读盘原语保持"只读盘、不碰缓存"。

## 验证

```bash
# PF 硬门控
grep -nE 'err & 0x04|exit_current' kernel/arch/x86_64/exception_handlers.cpp
grep -n 'USER_STACK_GROWTH' kernel/arch/x86_64/usermode.hpp
# ext2 read 经 PageCache
grep -rn 'read_bytes\|is_page_cacheable' kernel/mm/page_cache.hpp kernel/fs/inode.hpp kernel/syscall/sys_read.cpp
```

构建 + 内核测试(这一弧 run-kernel-test 730→734:read_bytes 基本+EOF + 二读命中 + 跨页 EOF 裁剪 + 真 ext2 端到端):

```bash
cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

端到端铁证是真 ext2 测试:读 `/hello.txt` 两遍,第二遍 `g_page_cache.hit_count()` 上升、且读盘次数不增、字节一致——等于 sys_read→read_bytes→get_page 全链接通。

## 小结与下一站

F2 的 demand paging 拧紧了(真 segfault),read 路径统一到 Page Cache。到这儿 VMA/brk/PageCache/demand-paging 这条线算闭环;剩下 F2 的 Buddy/Slab(分配器升级)是下一章 043 的事——那两块换的是 PMM 和堆的底层分配策略,和这一章的虚拟内存层是两个层面。
