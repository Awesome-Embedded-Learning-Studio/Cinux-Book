---
title: 041 · brk 与 Page Cache
---

# 041 · brk 与 Page Cache:用户堆有了,文件映射读到真内容

> 040 立了 VMA 账本和 mmap。这一章往上叠两块:**brk**(传统的连续堆,`malloc` 的底层接口)和 **Page Cache**(让 file-backed mmap 在 page fault 时读到**真文件内容**,而不是 040 那会儿的全零页)。两块都复用 040 的"懒分配 + demand paging"骨架,代价很小。
>
> A 档:用户程序 `malloc` 走的 brk、`mmap` 文件读到真字节,都是端到端可见的。

## 这章咱们要点亮什么

两件事:

1. **`sys_brk`(Linux 12)**:用户态连续堆的边界。懒实现——只挪 `brk_current`,不 map/unmap 页,堆增长靠 demand paging。
2. **Page Cache**:内核级文件内容缓存。file-backed mmap 的 PF 不再映射零页,而是按 `(inode, 页偏移)` 取缓存页(未命中就从盘读进来)。

## brk:懒分配的连续堆

`mmap` 给的是离散映射,适合程序动态要一块新内存。但传统 C 程序的 `malloc` 底层走 `brk`/`sbrk`——它要的是**连续的堆区**,从 `brk_current` 往上长。

`kernel/syscall/sys_brk.hpp`:

```cpp
// sys_brk.hpp:27 —— 6 参(只用第 1 个),匹配 SyscallFn
int64_t sys_brk(uint64_t addr, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
```

关键设计是**懒**:execve 时建一条 **Heap VMA** 覆盖整个堆窗 `[brk_initial, USER_BRK_MAX)`,然后把 `brk_initial/brk_max` 记到 `Task`(`process.hpp:134`)。`sys_brk` 只做一件事——边界检查后挪 `brk_current`:

- `addr == 0` → 返当前 brk;
- 越界(`< brk_initial` 或 `> brk_max`)→ 返当前(忽略,不报错);
- 否则 `brk_current = addr`,返 `addr`。

**不 map、不 unmap 页**。堆真正增长时,访问 `brk_current` 上方的地址触发 PF,走 demand paging 给页——和 mmap 的懒分配是同一条路径,完全复用。

> 两个值得记的点。第一,**`brk_initial` 不是固定的 `USER_BRK_BASE`,而是 ELF 段的末尾**:execve 加载 `PT_LOAD` 段时跟踪 `max_seg_end`,堆紧接在 ELF 镜像后面——这是标准 Linux 行为。第二,**`sys_brk` 返地址,不返 errno**(和 mmap/munmap 返 `-errno` 不同)——这是 Linux ABI,`brk` 失败时返当前值,调用方靠"返值 != 要的值"判断。

## Page Cache:补上文件映射的真内容

040 的 mmap 文件映射有个**洞**:它只记了 `backing` inode,PF 时一律映射**零页**——所以 `mmap` 一个文件再读,读到的是全零,不是文件内容。M4 就是补这个洞的。

`kernel/mm/page_cache.hpp`:

```cpp
// page_cache.hpp:64
class PageCache {
    // page_cache.hpp:78 —— 取一页缓存,未命中就从盘读
    cinux::lib::ErrorOr<CachedPage*> get_page(cinux::fs::Inode* inode, uint64_t offset);
    size_t hit_count() const;   // :84 —— 命中/未命中计数,验证用
    size_t miss_count() const;  // :85
};
// page_cache.hpp:103
extern PageCache g_page_cache;
```

数据结构:256-bucket 的哈希表(侵入式双向链表),键是 `(Inode*, page_offset)`,每个 `CachedPage` 持一对 phys/virt + inode + offset + ref_count。两个设计选择:

- **direct-map 复用**:缓存页的 `virt = phys + KERNEL_VMA`——和 DmaPool 同款(GOTCHA #7),免临时映射槽管理。
- **`get_page` 锁外读 / 锁内 insert**(关键):① 拿锁查,命中就 bump refcount 返;② 未命中,**放锁**,分配页 + 调 `inode->ops->read` 从盘读内容(AHCI 轮询,IF=0 成立)+ EOF 零填;③ 重新拿锁,短临界区 insert(带 race 再查一遍)。这一步是为了**杜绝 IO-under-lock 的重入死锁**——持着 cache 锁去读盘,读盘路径要是再碰 cache 就死锁了。

## handle_pf 变成文件感知

Page Cache 落地后,PF handler(`kernel/arch/x86_64/exception_handlers.cpp`)的文件路径接上:

```cpp
// exception_handlers.cpp:272 / :281 / :282
} else if (vma->backing != nullptr && !anonymous) {
    const uint64_t file_off = vma->file_offset + (virt_page - vma->start);
    auto           gp       = cinux::mm::g_page_cache.get_page(vma->backing, file_off);
    // ... 把缓存页映射进进程页表,PTE 权限按 VmaFlags 翻译
}
```

匿名 VMA / 无 VMA 的路径**字节不变**——还是原来的零页/宽松映射。只有 `backing != nullptr` 的文件 VMA 走新的 cache 路径。这种"加一条分支、不动老路径"的改法,把回归风险压到最低(execve 的 ELF 段是匿名 VMA,boot 期间文件路径 dormant,不会炸 boot)。

## 一个埋着的雷:NX 位(留 F9)

这一弧踩到一个**和未来 F9 强耦合**的坑,记成 GOTCHA #10。Page Cache 的 PF 文件路径本来想给"非可执行"的文件页设 NX 位(PTE bit63)——但**此刻 EFER.NXE 还没启用**(F9 才做),NXE 关着的时候 PTE 里的 NX 位是个**保留位**,设了它触发 reserved-bit #PF(`err=0x8`),无限循环。

诊断靠在 PF handler 里临时打印 `err/cr3/translate`,看到 `err=0x8`(RSVD)+ `translate==phys`(PTE 设对了)才确认是保留位违例。修复:文件路径 + mprotect **暂时都不设 NX**,等 F9 启用 NXE 之后再开。

> 教训:启用一项硬件特性(NXE)之前,所有依赖它的代码(NX 位)都得**先留着、别先上**,否则保留位 fault 比普通 bug 难诊断得多。这种"前向耦合"的坑,记清楚比硬上强。

## 验证

```bash
# brk
grep -rn 'int64_t sys_brk\|brk_current\|brk_initial' kernel/syscall/sys_brk.hpp kernel/proc/process.hpp
# Page Cache
grep -rn 'class PageCache\|get_page\|g_page_cache' kernel/mm/page_cache.hpp
# handle_pf 文件感知
grep -n 'backing != nullptr\|g_page_cache.get_page' kernel/arch/x86_64/exception_handlers.cpp
```

构建 + 内核测试(这一弧 run-kernel-test 从 040 的 721 涨到 730:brk +1、Page Cache +6、file mmap 端到端 +2):

```bash
cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
cmake --build build --target run-kernel-test
```

端到端验证是 file mmap 的 Test A:把 ext2 上某文件的字节和 cache 命中后的字节比对——能对上,就证明 demand-read 真的把盘上内容读进了 cache。`hit_count/miss_count`(`page_cache.hpp:84/85`)也能直观看到缓存命中。

## 小结与下一站

F2 又叠了两层:`brk` 让用户程序有连续堆,Page Cache 让文件映射读到真内容。demand paging 的骨架还是 040 那条,这两块只是往上面挂。

下一站 **042** 收 F2 的 demand paging 重构 + ext2 cache——把 040 故意推迟的"PF 硬门控(真 segfault)"补上,缓存再深一层。
