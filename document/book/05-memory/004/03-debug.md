---
title: 03 · 调试现场与收尾
---

# 调试现场与收尾

## 调试现场

这一章也没有 notes 文件,但 `AddressSpace` 有三个「错一步就全机重启」的隐患,值得当调试现场。

**一是 `init_kernel` 没在构造之前调,或者调早了。** `kernel_pml4_` 启动前是 0。如果第一个 `AddressSpace` 构造时 `kernel_pml4_` 还是 0(忘了在 main 里调 `init_kernel`,或者调的时机早于 VMM 把 CR3 换成真内核页表),构造函数第三步就从地址 0 那里「拷内核半区」——`phys_to_virt(0)` 读到的是物理 0 处的随机内容,拷进新 PML4 的全是垃圾。接着 `activate` 一切,CR3 指向一张内核半区全是垃圾的 PML4,CPU 立刻找不到内核代码,三重错误、重启。症状是「一构造/切换地址空间就重启」,且串口可能在重启前来得及打半行乱码。根因:确保 `init_kernel()` 在 VMM init 之后、首个 AS 之前调用,且 `kernel_pml4()` 读出来非 0。测试里 Test 1(`test_init_kernel_pml4`)就是专门守这条——`TEST_ASSERT_NE(AddressSpace::kernel_pml4(), 0)`。

**二是 `activate` 切了 CR3 之后没切回来,空间就被析构了。** `activate` 把 CR3 换成某个 AS 的 PML4。如果这个 AS 随后析构(比如是个局部变量,出作用域),它的 PML4 及用户半区被 free——可此刻 CPU 还在用这张 PML4 走地址!于是 CPU 走在一张正在被回收的页表上,访问到刚被 free 的页表页 → 内容变垃圾 → 崩。规矩:`activate` 之后、析构之前,必须 `write_cr3(kernel_pml4)` 切回内核。测试里 Test 8、Test 9 都老老实实这么干——`activate` 完,先验完要验的,再 `write_cr3(saved_pml4)` 恢复,然后才让 AS 出作用域析构。真正「切来切去不手动恢复」要等下一章进程切换,那里有调度器管这事。

**三是析构的循环边界写错,扫进了内核半区。** 前面讲过:析构只扫 `PML4[0..255]`。如果误写成扫到 511,`free_subtree` 就会顺着 PML4[256..511] 把内核共享的那套 PDPT/PD/PT 当成「本空间私有」全 free 掉。内核页表没了,下一次任何地址解析(包括中断返回)都炸,而且炸得毫无预兆——因为 free 的时候还好好的,要等 CPU 下次需要走内核映射才暴露。这种「析构完一会儿才崩」的症状,先怀疑是不是回收越界扫进了共享区。守这条的测试是 Test 11(`test_destroy_no_kernel_corruption`):构造一个 AS、映射点东西、析构,然后验 `kernel_pml4()` 没变——确保析构没碰坏内核。

## 验证

地址空间的逻辑(PML4 分配、清零、内核半区拷贝、用户半区隔离、析构回收),核心能在 host 上镜像测。[test_address_space.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_address_space.cpp) 用一个 `MockPMM` + `TestVMM` 把 `AddressSpace` 的行为抄了一份:host 端分配伪造的「物理页」、用简化的 walk 验证隔离。它专门盯几条:构造会分配一张 PML4、构造后用户半区(PML4[0..255])全 0、内核半区(PML4[256..511])从 kernel PML4 拷过来了、两个实例拿到不同的 PML4 物理地址、析构把 PML4 页还回去(无用户映射时只回收 PML4 本身):

```bash
ctest --test-dir build -R address_space --output-on-failure
```

host 侧受限于「没法真改 CR3、没法真缺页」,主要验证页表结构和拷贝/回收的记账;真正的隔离、真正的 CR3 切换,还得 QEMU。

「真 PMM、真 VMM、真 CR3」在 QEMU 里验。[test_address_space.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_address_space.cpp) 在机内跑 11 个场景:init_kernel 存了非 0 的内核 PML4、构造出与内核不同的 PML4、两个实例根不同、单空间 map/translate/unmap、translate 未映射返回 0、**跨空间隔离**(Test 7:在 AS#1 映射,AS#2 `translate` 同一地址返回 0)、**activate 改变 CR3**(Test 8:切完后 `read_cr3() == as.pml4_phys()` 且与切前不同,随后恢复)、activate 后 translate 仍对、同空间映射两页、析构不损坏内核映射:

```bash
cmake --build build --target run-big-kernel-test
```

`init_kernel` 会打 `[AS] Kernel PML4 saved at phys ...`,test section `AddressSpace Tests (018)` 全过、末尾 `ALL TESTS PASSED`,就说明地址空间这套在真硬件语义下成立。尤其 Test 7 的跨空间隔离,是这一章的里程碑要求——两个空间对同一虚拟地址看到不同结果,进程隔离的物理基础就在这一步被焊实。

## 下一站

内存子系统至此收口:PMM 管物理页,VMM 管虚拟↔物理映射(还给了 demand paging),堆在页上做细粒度分配,`AddressSpace` 把「虚拟地址空间」抽象成可创建/切换/销毁的对象。`0xFFFF800000000000` 的谜底解开了——它是 PML4[256],内核半区的入口,所以天然在所有地址空间里可见。016 留的 `pml4` 参数,也终于有了 `AddressSpace` 这个正经调用者。

但你会发现:`AddressSpace` 在生产路径里只做了 `init_kernel` 一件事,一个实例都没造。它是一块铺好、测好、却还没人住的地基。谁来住?——进程。一个进程需要自己的地址空间、自己的执行流、能在多个进程间切换。

下一站(019)就是把它接上:进程结构、调度器、上下文切换。`AddressSpace` 会在那里第一次被真正「用起来」——每个进程挂一个自己的地址空间,调度器在切换进程时 `activate` 对应的空间。不过那是下一章的事,我们先享受一下「内核有了地址空间抽象」这个里程碑。

---

### 参考

- Intel SDM Vol.3(System Programming):4 级分页(PML4→PDPT→PD→PT)、`CR3`(PML4 物理基址)、**规范地址(canonical address)**——bit 47 为低/高半区分界、高位须符号扩展,这正是「PML4[256] 为内核半区入口」的由来。本地 PDF `document/reference/intel/SDM-Vol3A-*.pdf`,可用 `pdf-reader` 搜 "canonical" / "4-Level Paging" 复核。
- 016 章 · [把物理页挂进虚拟地址:VMM](../002/):`AddressSpace` 的 map/unmap/translate 透传的就是 VMM 的 `pml4` 参数,`phys_to_virt` 自举换算也来自这一章。
- 017 章 · [在页上切块:内核堆分配器](../003/):堆基址 `0xFFFF800000000000`(= PML4[256])的来历,这一章给出了它「为什么」的答案。
- 本 tag 源码:[address_space.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/address_space.hpp) / [address_space.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/mm/address_space.cpp)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(Step 9 `AddressSpace::init_kernel()`,生产路径只此一句);测试 [test_address_space.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_address_space.cpp)(host 镜像,MockPMM + TestVMM)、[test_address_space.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_address_space.cpp)(QEMU 11 场景,含跨空间隔离与 CR3 切换)。
