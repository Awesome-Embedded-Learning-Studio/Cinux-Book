---
title: Lab 016 · 把物理页挂进虚拟地址:VMM map/unmap + demand paging
---

# Lab 016 · 把物理页挂进虚拟地址:VMM map/unmap + demand paging

> 配套章节:[016 · 把物理页挂进虚拟地址:虚拟内存管理器](../../book/05-memory/016-mm-vmm.md)。这一关给你目标和约束,不贴 walk 的实现,也不贴 demand paging 的判断。

## 实验目标

让内核真正掌控虚拟地址空间,而不是只跑在 bootloader 写死的页表上。拆成几个能独立验证的子目标:

1. 能 map:把一个 PMM 分配的物理页,挂到任意虚拟地址上,中间缺哪级页表就自动建。
2. 能 unmap / translate:拆掉映射、查一个虚拟地址实际落在哪个物理页。
3. 能 demand paging:在缺页异常里,对「页不存在」的情况现场补一页,让访问继续。

做完这三条,内核就能主动控制虚拟↔物理映射,013 那个 `map_mmio` hack 也该退休了。

## 前置条件

你得先过 Lab 015:PMM 的 `alloc_page` / `free_page` 可用——VMM 建中间页表、demand paging 补页,都靠它提供物理页。这一关所有「新建页表」的需求都转成对 PMM 的 `alloc_page` 调用。

另外要熟悉 4 级页表的结构:虚拟地址被拆成 PML4/PDPT/PD/PT 四段 9 位索引 + 12 位页内偏移,每级表 512 项、每项 8 字节、一张表正好一页。

## 任务分解

**第一步:理清常量。** 把页大小、各级索引移位(9 位一级,PML4 在 [39..47]、PDPT [30..38]、PD [21..29]、PT [12..20])、页表项的地址掩码(物理地址位 `[12..51]`,低 12 位是 flag)、各个 flag 位(P、RW、US、NX 等)集中定义。再写两个从虚拟地址提取各级索引的宏。这步是给后面 walk 打基础,魔法数集中了不容易写错。

**第二步:封装页表项。** 一个 8 字节的联合体,`raw` 当整体读写,配几个访问器:取物理地址(用地址掩码抠 `[12..51]`)、判断 present 位、设物理地址。映射时整体写 `raw = (phys & 掩码) | (flags & ~掩码)`——物理地址和 flag 分开拼,谁也别污染谁。

**第三步:map/unmap/translate,走四级。** `init` 先读 CR3,存住内核的 PML4 物理基址。`map` 从 PML4 开始,按虚拟地址的四段索引逐级往下走,前三级的走法用一个「缺表就建」的辅助函数:这一级的表项若不 present,就从 PMM 要一页(注意失败返回)、**清零**、链接进上一级(PRESENT|WRITABLE)、返回新表的虚拟地址;末级 PT 直接把物理页挂上,再作废这一页的 TLB。`unmap` 同样走四级(但不建表),清掉末级 PT 项、刷 TLB;`translate` 走四级,返回物理页基址 + 页内偏移,不存在则返回 0。走四级时有个自举问题要想清楚:拿到的「下一级表地址」是物理地址,内核只能访问虚拟地址,怎么读那张表——靠一个物理→虚拟的换算(物理地址 + 高半区偏移),前提是那段物理内存已被高半区映射。

**第四步:demand paging。** 在缺页异常 handler 里(它能拿到 CR2 里的缺页地址和栈上的 error code),先判断 error code 的最低位:为 0 表示「页不存在」(可补),那就对齐到页、从 PMM 要一页、用 map 挂上、返回(让那条指令重执);为 1 表示「protection violation」(真错误),走原来的诊断和挂起。把这两种情况分清是这步的关键——补反了要么死循环要么真错误被掩盖。

## 接口约束

你要实现出来的东西,对外长这样(职责和签名,不给 walk 实现):

- `VMM::init()`:读 CR3 存内核 PML4。
- `VMM::map(uint64_t virt, uint64_t phys, uint64_t flags, uint64_t* pml4 = nullptr) -> bool`:走四级,缺表建,挂物理页。返回 false 表示 PMM 没页了。
- `VMM::unmap(uint64_t virt, uint64_t* pml4 = nullptr)`:拆映射(不回收物理页,那是调用者的事)。
- `VMM::translate(uint64_t virt, uint64_t* pml4 = nullptr) -> uint64_t`:返回物理地址,不存在则返回 0。
- 缺页 handler 里那段 demand paging:对 not-present 的缺页,alloc 一页 + map + 返回;protection violation 不处理。

页表项常量、地址掩码、各级移位、flag 位,都得你自己照 4 级分页规范定,这关不提供。

## 验证步骤

纯逻辑(各级索引提取、walk、map/unmap/translate、缺表分配)在 host 上镜像着测。把 walk 和 index 逻辑抄一份到测试里,`-O2` 编、`CINUX_HOST_TEST` 门控:

```bash
ctest --test-dir build -R vmm --output-on-failure
```

建议覆盖:从虚拟地址正确提取四级索引、map 后 translate 返回挂上的物理页 + 偏移、unmap 后 translate 返回 0、中间表缺失时 map 自动建立。

真页表、真 CR3、demand paging 真触发,在 QEMU 里验。机内测调 `g_vmm.init`(读真 CR3)、map 一个地址后 translate 验证、触发一次未映射地址访问看 demand paging 是否补页(会打一行 demand-paged 日志):

```bash
cmake --build build --target run-big-kernel-test
```

init 会打印 `[VMM] Initialised, kernel PML4 at phys ...`,demand paging 补页会打印 `[VMM] Demand-paged ... -> phys ...`。看到这两类日志,就说明 VMM 走通了。

## 常见故障

- **映射后 translate 出来莫名其妙、写一个地址改到了别处**:新建的页表没清零,残留位被当成 present 的映射。新建表后必须把 512 项清零。
- **unmap 了访问还不缺页 / map 了还 page fault,忽好忽坏**:改了 PTE 没刷 TLB,CPU 还用旧映射。map/unmap 末尾立刻 flush 这一页的 TLB。
- **映射的权限或地址错**:物理地址没 mask,低 12 位和 flag 位撞车。永远用 `(phys & ADDR_MASK) | (flags & ~ADDR_MASK)` 分开拼。
- **demand paging 死循环或机器重启**:把 protection violation(error code bit0=1)也当成 not-present 去补页,补了还缺页,递归。只对 bit0==0 补页。
- **一碰页表就重启,栈里全是缺页 handler**:`phys_to_virt` 走到了没做高半区映射的物理地址,递归缺页→double fault。boot 期全物理内存都映射了不发作,但要知道这是雷区。
- **map 返回 false 但你以为成功了**:PMM 没页了 alloc_page 返回 0,walk_level 返回 nullptr。调用方要检查 map 的返回值。

## 通过标准

1. host 单测全绿:四级索引提取、map 后 translate 一致、unmap 后返回 0、缺中间表自动建立。
2. QEMU 机内测通过:init 读到真 CR3、map 后 translate 对、demand paging 对 not-present 缺页成功补页并打印日志。
3. 页表项永远用 mask 分开拼物理地址和 flag;新建表一律清零;map/unmap 后刷 TLB——这三条铁律守住。
4. demand paging 只处理 not-present(bit0==0),protection violation 走原诊断;unmap 不回收物理页(职责分离)。

做到这四条,内核就掌控了虚拟地址空间,PMM + VMM 两块基石就位。但粒度上还差一层:现在最小单位是整页,想要「分配几十字节」还做不到——那是下一关堆分配器的活。
