---
title: 041 · brk 与 Page Cache
---

# 041 · 连续的堆,和文件映射背后的真内容:brk 与 Page Cache

> 040 给了 `mmap`(离散映射)和 VMA 账本。这一章往上叠两块,都复用 040 那套"懒分配 + 按需分页"的骨架,所以代价很小。
>
> 一块是 **`brk`**——传统 C 程序 `malloc` 底层要的**连续堆区**,和 `mmap` 的离散映射是两种用法。另一块是 **Page Cache**——040 的文件映射有个洞:映射了文件,page fault 时却映射**零页**,于是读出来全是零,不是文件内容。Page Cache 补上这个洞,让文件映射在 fault 时读到真内容。

## brk:像 mmap 一样懒

`mmap` 给的是"随便一块新内存",适合动态映射。但传统 C 程序的 `malloc` 底层走 `brk`/`sbrk`——它要的是**从 `brk_current` 往上连续长的堆区**。

`sys_brk` 的实现和 mmap 一样**懒**:execve 时建一条 **Heap VMA** 覆盖整个堆窗 `[brk_initial, USER_BRK_MAX)`,然后 `sys_brk` 只做一件事——边界检查后挪 `brk_current`,**不映射、不取消映射任何页**。堆真正增长时,程序访问 `brk_current` 上方的地址触发 fault,按需分页给页——和 mmap 同一条路径。

```cpp
// kernel/syscall/sys_brk.hpp —— 6 参(只用第 1 个)
int64_t sys_brk(uint64_t addr, ...);
```

两个细节:`brk_initial` 不是写死的,而是 **ELF 段的末尾**(execve 加载段时跟踪最远段尾,堆紧接在程序镜像后面——标准 Linux 行为);还有 `sys_brk` **返地址、不返 errno**(和 mmap 返 `-errno` 不同)——失败时返当前值,调用方靠"返值 ≠ 要的值"判断,这是 Linux ABI。

## Page Cache:文件映射读到真内容

040 的文件映射有个洞:它只记了 `backing` inode,fault 时一律映射**零页**——所以 `mmap` 一个文件再读,读到全零。Page Cache 补这个洞:缓存文件的内容页,fault 时按 `(inode, 页偏移)` 取缓存页(没缓存就从盘读进来)。

`kernel/mm/page_cache.hpp`:

```cpp
class PageCache {
    // 取一页缓存;没命中就从盘读进来填上
    ErrorOr<CachedPage*> get_page(Inode* inode, uint64_t offset);
    size_t hit_count() const;    // 命中/未命中计数,验证用
    size_t miss_count() const;
};
extern PageCache g_page_cache;
```

数据结构是个哈希表,键是 `(Inode*, 页偏移)`,每条 `CachedPage` 持一对物理/虚拟地址。两个设计点:

**复用 direct-map。** 缓存页的虚拟地址直接取 `phys + KERNEL_VMA`,和 037 的 `DmaPool` 同款——物理地址唯一决定虚拟地址,免单独分配。那条"direct-map 的页表项绝不 unmap"的纪律这里也适用。

**`get_page` 锁外读、锁内插。** 命中的话,拿锁查到、bump 引用计数、返;**没命中**的话,**放锁**,分配一页 + 调 `inode->ops->read` 从盘读内容,再**重新拿锁**插进缓存。这一步是为了**杜绝持锁读盘的重入死锁**——拿着缓存锁去读盘,读盘路径要是再碰缓存就死锁了。

## page fault 变成文件感知

Page Cache 落地后,fault handler(`kernel/arch/x86_64/exception_handlers.cpp`)多了一条文件分支:

```cpp
} else if (vma->backing != nullptr && !anonymous) {
    uint64_t file_off = vma->file_offset + (fault_addr - vma->start);
    auto gp = g_page_cache.get_page(vma->backing, file_off);
    // 把缓存页映射进进程页表
}
```

匿名 VMA / 没 VMA 的路径**一字不改**——还是原来的零页/宽松映射。只有文件 VMA 走新的缓存路径。这种"加一条分支、不动老路径"的改法,把回归风险压到最低。

## 一个埋着的雷:NX 位(留到安全弧)

文件 fault 路径本来想给"不可执行"的页设 NX 位(页表里的不可执行标志)——但**此刻 NXE(不可执行使能)还没开**(安全弧才做)。NXE 没开时,页表里的 NX 位是个**保留位**,设了它触发保留位异常,无限循环。所以文件路径**暂时不设 NX**,等安全弧开了 NXE 再补。**启用一项硬件特性之前,所有依赖它的代码都得先留着别上,否则保留位异常比普通 bug 难诊断。**

## 验证

```bash
grep -n 'int64_t sys_brk\|brk_current\|brk_initial' kernel/syscall/sys_brk.hpp kernel/proc/process.hpp
grep -rn 'class PageCache\|get_page\|g_page_cache\|hit_count' kernel/mm/page_cache.hpp
grep -n 'backing != nullptr\|g_page_cache.get_page' kernel/arch/x86_64/exception_handlers.cpp
```

构建:

```bash
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
```

端到端证据是文件映射的测试:把 ext2 上某文件的字节,和缓存命中后读到的字节比对——对得上,就证明按需读取真把盘上内容读进了缓存。`hit_count`/`miss_count` 也能直观看到:第一次读是 miss(读盘),第二次同偏移读是 hit(命中缓存,不读盘)。
