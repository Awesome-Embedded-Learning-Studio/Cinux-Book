---
title: 01 · 野指针终于会被杀,read() 也走缓存了
---

# 野指针终于会被杀,read() 也走缓存了

> 两件事,都兑现前面留的债。第一,040 立 VMA 账本时只做"诊断"——访问没登记的区域,内核只是 `klog_warn` 一声然后**照常给页**(野指针、栈溢出都被静默放过)。这一章把按需分页拧成**硬门控**:用户态访问没 VMA 的区域,**真 segfault**,终止进程。第二,041 的 Page Cache 只服务文件映射的按需分页,而 `read()` 读普通文件每次直读盘、不缓存;这一章让 `read()` 也走 Page Cache,读路径统一到一个缓存层。

## 把"诊断"拧成"硬门控"

page fault 时,如果命不中任何 VMA,该怎么处理?Linux 的答案是:**这是野指针 / 非法访问,杀进程**(SIGSEGV 的内核侧)。040 因为风险(要重构整个按需分配)只做了诊断;现在补上真 segfault。

关键在**区分谁触发的 fault**——靠错误码里的一位 `err & 0x04`(用户态触发):

```cpp
// kernel/arch/x86_64/page_fault.cpp
const bool user_fault = (err & 0x04) != 0;
if (vma == nullptr && user_fault) {
    klog_error("segfault ...");
    signal_force_send(task, Signal::kSigsegv);   // 排进 SIGSEGV,ISR 返回时投递
}
```

为什么只杀用户态?因为内核测试是**内核态**访问用户地址(`err&0x04=0`,比如测试注入 fault、或内核的 `copy_to_from_user`)——这些要保持原来的零页容错,否则测试就挂了。只有**真实用户程序**(ring 3)的非法访问才杀。这一位天然就是"测试豁免"的分界。

### 栈必须配套扩到 1MB

硬门控上了,但有个**必须配套**的改动:栈的增长窗。040 的栈 VMA 只有 16KB,硬门控一上,**深调用栈的程序(init、gui、shell)栈 fault 就会落到 VMA 之外 → segfault,自己把自己杀了**。所以这一章把栈增长窗从 16KB 扩到 1MB:

```cpp
// kernel/arch/x86_64/usermode.hpp
constexpr uint64_t USER_STACK_GROWTH = 0x100000;   // 1 MB
// 栈 VMA: [USER_STACK_TOP - 1MB, TOP),顶 16KB 预分配,余下按需向下长
```

VMA 的底(顶 - 1MB)以下没有 VMA → segfault,这就是隐式的栈溢出 guard。**门控和栈扩必须配套上**——单上门控、栈还是 16KB,实机会自爆(而内核测试因为不跑真实深栈,绿得把这个依赖掩盖了)。

### segfault 怎么"杀"

用的是 `signal_force_send(task, kSigsegv)`——把 SIGSEGV 排进目标任务,ISR stub 在 `handle_pf` 返回后调 `signal_check_deliver_isr` 投递:**有自定义 handler 走 handler,否则默认 Terminate**(终止进程)。所以终止机制本身就是信号外壳,不再"裸杀"。比"标记 Dead + 延迟退出"简单(后者要伪造中断帧,否则返回会重执行出错的指令 → fault 死循环)。

## read() 也走 Page Cache

041 的 Page Cache 只服务文件映射的按需分页。而 `read()` 读普通文件,直走 `Ext2FileOps::read`(wholesale 后位于 `libs/ext2/ext2_common.cpp`,080 章 ext2 独立成库那步挪过去的)——每次按 ext2 块读盘,**没缓存**。于是同一份文件,"mmap 读"和"read() 读"各走各的,read() 重复读反复 I/O。这一章让 read() 也接进 Page Cache:

```cpp
// kernel/mm/page_cache.hpp —— 给 read() 用的按字节读,内部按页切片复用 get_page
ErrorOr<int64_t> read_bytes(Inode* inode, uint64_t file_off, void* buf, uint64_t count);
```

`read_bytes` 按页切片,每页调 041 的 `get_page`(命中 bump 引用 / 没命中锁外读盘填充),再把切片 memcpy 到用户缓冲。**复用 041 的 `get_page`,不另起缓存逻辑**——读路径从此只有一层缓存。

### 怎么判别"该走缓存的磁盘文件"vs"不该走缓存的管道"

坑点:管道的 inode 类型也是 `Regular`(和 ext2 文件一样),靠类型字段判会把管道误路由进缓存(管道内容不在盘上,缓存就错)。又禁用 RTTI,不能 `dynamic_cast`。解法是给 `InodeOps` 加一个**带默认实现的虚函数**:

```cpp
// kernel/fs/inode.hpp
virtual bool is_page_cacheable() const;   // 默认 false
// Ext2FileOps override 返 true;管道/ramdisk/测试桩继承默认 false,行为不变
```

`sys_read` 按它分流(`kernel/syscall/sys_read.cpp`):

```cpp
inode->ops->is_page_cacheable()
    ? g_page_cache.read_bytes(inode, ...)    // 磁盘文件走缓存
    : inode->ops->read(inode, ...);          // 管道等走原来的直读
```

虚函数带默认实现是**源码兼容**的——所有现有子类(管道、ramdisk、测试桩)继承默认 false,**一行不用改**;只有 ext2 override 成 true。比加类型字段或开 RTTI 都干净。

> 有个必须避开的环:`read_bytes` → `get_page` → `Ext2FileOps::read`(读盘原语)。`Ext2FileOps::read` **绝不调** page cache,否则 read→get_page→read 死循环。读盘原语保持"只读盘、不碰缓存"。

## 验证

```bash
# PF 硬门控(err&0x04 + signal_force_send 投 SIGSEGV)+ 栈 1MB
grep -nE 'err & 0x04|signal_force_send|kSigsegv' kernel/arch/x86_64/page_fault.cpp
grep -n 'USER_STACK_GROWTH' kernel/arch/x86_64/usermode.hpp
# read() 走 PageCache
grep -rn 'read_bytes\|is_page_cacheable' kernel/mm/page_cache.hpp kernel/fs/inode.hpp kernel/syscall/sys_read.cpp
```

构建:

```bash
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
```

端到端铁证是真 ext2 测试:读某个文件两遍,第二遍 `g_page_cache.hit_count()` 上升、读盘次数不增、字节一致——等于 `sys_read → read_bytes → get_page` 全链接通。PF 硬门控则靠实机冒烟:正常程序不崩,而一个故意访问野地址的程序会被杀(不再静默放过)。
