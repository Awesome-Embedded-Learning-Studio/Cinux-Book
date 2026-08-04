---
title: 04 · 调试现场与收尾
---

# 调试现场与收尾

## 调试现场

这一章没有 notes 文件,但 VMM 有三个写错就「极难查」的隐患,值得当成调试现场。

一是 **新页表没清零**,这是 VMM 最经典的坑。`walk_level` 从 PMM 拿到一页做新表,如果没那个清零循环,这页内存里是上一个主人留下的残留数据。那些残留的 64 位整数,被当成页表项后,其中凡是最低位(P 位)恰好是 1 的,都会被 `is_present()` 判成「这条映射在」,walk 就跟着里面的「物理地址」飞到一个随机位置。症状是:映射某个地址后,translate 出来一个莫名其妙的物理地址,或者写一个虚拟地址却改到了毫不相关的物理内存——而且每次重启表现还不一样(取决于那页内存上次的残留)。这种 bug 三分靠代码、七分靠运气,排查极痛苦。根因就一行:新建表后必须把 512 个条目全清零。养成「凡是新建页表,第一件事 memset/循环清零」的肌肉记忆,能省掉这一整类噩梦。

二是 **map/unmap 后忘了 flush_tlb**。CPU 把页表项缓存在 TLB 里,你改了内存里的 PTE,TLB 不会自动更新——CPU 还按旧映射走。于是出现诡异的「明明 unmap 了,访问还不缺页」「明明 map 了,访问还 page fault」,过一会儿又突然好了(TLB 被别的操作刷掉了)。这种「时序相关、忽好忽坏」的症状,九成是忘了刷 TLB。规矩:`map` 设完 PTE、`unmap` 清完 PTE,立刻 `flush_tlb(virt)`(一条 `invlpg`)作废这一页的 TLB 项。这一章的 `map`/`unmap` 末尾都跟了一句 flush,不是装饰。

三是 **物理地址没 mask,污染了 flag 位**。`pt[idx].raw = phys | flags`,如果你传进来的 `phys` 低 12 位不是 0(没按页对齐),或者没用 `ADDR_MASK` 过滤,那些位就会和 flag 位(P、RW、US…)撞车——比如 phys 的某位被当成了 PRESENT,或 flag 被当成了地址位。出来的映射要么权限错、要么地址错。VMM 的写法是 `(phys & ADDR_MASK) | (flags & ~ADDR_MASK)`:物理地址只留 `[12..51]`,flag 只留 `[0..11]` 和高位,两段不重叠,谁也污染不了谁。写 PTE 时永远用 mask 把两段分开拼,这是铁律。

还有一个和 demand paging 相关的:`phys_to_virt` 越界导致**递归缺页**。如果某张页表的物理地址落在了没做高半区映射的范围,VMM 访问它 → 缺页 → demand paging handler 试图 map → 又要 walk 页表 → 又访问那张表 → 又缺页……递归不收敛,最终 double fault、三重错误、机器重启。这种「一碰就重启」且栈里全是 `handle_pf` 的崩溃,先怀疑 `phys_to_virt` 走到了未映射区域。这一章在 boot 期(所有物理内存都高半区映射了)不发作,但要心里有数:demand paging + 自举访问是个可能递归的组合,以后缩小高半区映射范围时这里是雷区。

## 验证

VMM 的逻辑(4 级索引提取、页表 walk、map/unmap/translate、缺表分配)大半能在 host 上镜像测。[test_vmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_vmm.cpp) 把这些逻辑抄了一份,`-O2` 编、`CINUX_HOST_TEST` 门控,测 index 提取对不对、walk 一条已知路径对不对、map 后 translate 是否一致、缺中间表时是否正确分配:

```cpp
// 大致覆盖:
// - PML4/PDPT/PD/PT 索引从虚拟地址正确提取
// - map 后 translate(virt) 返回挂上的 phys + offset
// - unmap 后 translate 返回 0(不在)
// - 中间表缺失时 map 自动建立(计数字数对得上)
```

跑它们:

```bash
ctest --test-dir build -R vmm --output-on-failure
```

但「真页表、真 CR3、demand paging 真触发」只有 QEMU 里验得了真。机内测 [test_vmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_vmm.cpp) 在 QEMU 里:调 `g_vmm.init`(读真 CR3)、`map` 一个地址后 `translate` 验证、主动触发一次未映射地址的访问看 demand paging 是否补上页(补上后那行 `[VMM] Demand-paged %p -> phys %p` 会打出来):

```bash
cmake --build build --target run-big-kernel-test
```

init 时会打印 `[VMM] Initialised, kernel PML4 at phys %p`,看到这行 + demand paging 的补页日志,就说明 VMM 走通了。这一章的验证难点是「页表正确性没法直接看」——你只能通过 translate 是否一致、访问是否成功来间接验证,所以那批 host 单测(焊死 walk 算法)和机内测(真跑一遍)缺一不可。

## 下一站

到这里,内核的内存子系统两块基石都就位了:PMM 管物理页的分配,VMM 管虚拟↔物理的映射,还顺手实现了 demand paging。内核现在有了主动控制地址空间的正经能力——013 那个 `map_mmio` hack 也终于有了它的替代者(这一章 framebuffer 仍走旧路,VMM 是为后面铺的路)。

但你会发现一个粒度上的缺口:PMM 和 VMM 的最小单位都是**一整页 4KB**。如果你想「分配 64 字节」存个结构体,得拿一整页——既浪费,又难管理(谁拥有这页?释放了其中 64 字节怎么办?)。内核需要一个小粒度的分配器,在页的基础上切块、回收,也就是堆(heap)。

下一站就是它:一个堆分配器,让内核能 `kmalloc(64)` / `kfree(...)`,在 VMM 给的页上做细粒度管理。那是内存子系统的第三块拼图,也是后面几乎所有运行时数据结构(动态数组、链表、缓存)的依赖。不过那是下一章的事了,我们先享受一下「内核掌控了虚拟地址空间」这个里程碑。

---

### 参考

- Intel SDM Vol.3(System Programming,4 级分页):PML4→PDPT→PD→PT 四级结构、每级 9 位索引(512 项)、页表项(64 位)的位布局(物理地址位 `[12..51]`、P/RW/U/PS/NX 等 flag)、`CR2`(存缺页地址)、`CR3`(存 PML4 物理基址)、`INVLPG`(作废单页 TLB)。本地 PDF `document/reference/intel/SDM-Vol3A-*.pdf`,可用 `pdf-reader` 搜 "4-Level Paging"/"Page-Fault Error Code" 复核。
- OSDev — [Paging](https://wiki.osdev.org/Paging):4 级页表结构、PTE 位含义、页表 walk 的社区参考实现。
- 015 章 · [给物理内存建账本:bitmap PMM](../001/):VMM 的中间页表和 demand paging 都靠 PMM 的 `alloc_page` 提供物理页,两章是内存子系统的一对基石。
- 本 tag 源码:[vmm.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/vmm.hpp) / [vmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/vmm.cpp)、[paging_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/paging_config.hpp)、[paging.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/paging.hpp)(`PageEntry`/`flush_tlb`/`read_cr3`)、[exception_handlers.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/exception_handlers.cpp)(`handle_pf` demand paging)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(Step 8 `g_vmm.init`);测试 [test_vmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_vmm.cpp)(host 镜像)、[test_vmm.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_vmm.cpp)(QEMU 真 map/demand paging)。
