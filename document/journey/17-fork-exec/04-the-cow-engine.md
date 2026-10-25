---
title: "04 · 写时复制的引擎"
description: "kCow 坐 bit 9,硬件无视的软件标记。CR0.WP 全程为 0 的真发现:ring0 写穿只读页不响铃,首版测试值对帧数对不上就是这么来的,KernelStart 从此拉起 WP。缺页裁决者坐进异常报告员门前的单座,有伴复制、末位复权。一帧两映射的测试重写,构造性测试的绿要能被探针证伪才算数。"
chapter: 17
order: 4
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - memory
  - cow
---

# 写时复制的引擎

这一回咱们不碰 fork,引擎单独立、单独地考——任何映射都可以标成共享,写缺页也一视同仁地递给裁决者,和 fork 谁也不欠谁的。

标记用的位只有一个:kCow,坐在页表项的 bit 9。页表项的 bit 9 是硬件留给软件的 AVL 格子,CPU 走页表的时候看都不看它一眼,咱们拿它记一件事:这页在跟人共享,只读是装的,动手写之前免不了要分家。配套的动作叫 MarkPageShared,三拍:把页降成只读、置上 kCow、给物理帧的计数加一。同一帧在两个地址各标一次,计数就是二——fork 的克隆循环将来要的正是这个原语。这里咱们埋了一处幂等:已经是只读加 kCow 的页,再标一遍的话直接回真,不做重复的计数。不这么写,同一个世界反复地 fork,计数变成了只增不减,帧永远回不了池子,这是一条确定性的泄漏,咱们当场立修。

引擎立好了还没跑,咱们就挨了当头一棒,这一棒是这一批最大的真发现。首版的复制测试,值倒是全对,帧数对不上——写共享页的那一行执行完,而计数纹丝不动。查到了最后,病根在 CR0 的 WP 位:它管的就是 ring0 写只读页响不响铃。而 x86 复位之后,WP 的缺省值就是 0,咱们自己写的代码里,没有谁碰过它——保护模式切换那一卷摸的是 PE 位,SSE 开禁那一卷清的是 EM 与 MP,WP 就这么一直躺平。WP=0 的意思是,ring0 的写,穿只读页就跟穿平地似的,写缺页根本发生不了,字节直直地落进共享帧,值当然对。也就是说咱们的机器从加电起,只读页对内核是没有约束力的。咱们在 KernelStart 里把 WP 拉起来:今天无人受害(内核映像的页全可写,直接映射的那片也全可写),可写时复制要的就是那声铃,铃偏偏不响,引擎转了个空。机内测试走的不是产品入口,咱们自己在用例里拉。

铃响了,咱们得有人接。缺页走的是 14 号异常,异常报告员面前咱们安了一把单座:ReportFault 在转储、停机之前,把 CR2 和错误码递给坐在座上的裁决者,裁决者说修得了,咱们就返回继续跑,说修不了,照旧走转储的老路,一个字的诊断都不少。错误码也给立了三个词:bit 0 说的是错在哪一层,为零的话整页没映,为一的话页在、错在权限。bit 1 说的是写,bit 2 说的是来自用户——从此裸位从两个文件里退役。产品的那一侧,启动阶梯里多了新的一步:page fault resolver,排在地址空间之后、控制台之前。

裁决者本人的名字是 TryResolvePageFault,InstallResolver 把它安进了那把单座。判定链咱们整段端出来,真码面前一环一环地过:

```cpp
bool TryResolvePageFault(unsigned long long address, unsigned long long error_code) {
    if ((error_code & cinux::arch::isr::kFaultWasWrite) == 0) {
        return false;
    }
    cinux::mm::KernelTables world;
    const SlotAt            kSlot = FindSlot(world, cinux::arch::ReadCr3(), address);
    if (kSlot.entry == nullptr || kSlot.level != walk::WalkLevel::kPageTable) {
        return false;
    }
    const Entry kLeaf = *kSlot.entry;
    if (!kLeaf.has(cinux::arch::page::kPresent) || kLeaf.has(cinux::arch::page::kWritable) ||
        !kLeaf.has(cinux::arch::page::kCow)) {
        return false;
    }
    const unsigned long long kTarget  = walk::EntryTarget(kLeaf);
    const unsigned long long kCompany = release_share(kTarget);
    if (kCompany == 0) {
        *kSlot.entry = cinux::arch::page::MakeTableEntry(kTarget, leaf_flags(true));
        cinux::arch::InvalidatePage(address);
        return true;
    }
    const cinux::base::PhysAddr kCopy = cinux::mm::Pmm::self().allocate_page();
    cinux::base::safety::Check(kCopy != cinux::base::PhysAddr{}, "cow copy out of frames");
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* const       kDestination = reinterpret_cast<void*>(cinux::mm::DirectMapVirt(kCopy.raw));
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const auto* const kSource = reinterpret_cast<const void*>(cinux::mm::DirectMapVirt(kTarget));
    cinux::base::CopyBytes(kDestination, kSource, cinux::arch::page::kSize);
    *kSlot.entry = cinux::arch::page::MakeTableEntry(kCopy.raw, leaf_flags(true));
    cinux::arch::InvalidatePage(address);
    return true;
}
```

您看头一问,问的是写吗?读缺页不归咱们管,那是按需分页的案子,轮不到咱们这一卷。第二问拿着 CR3 和出错的地址,KernelTables 是走表的拐杖,FindSlot 从活根一路走到叶上的表项,走表落不到叶上的,不是咱们的案子。第三问在叶上验三个词:在吗、只读吗、带 kCow 吗,三个条件里缺了一个,咱们都不受理。过了三问,数伴:release_share 的返回值,就是这一帧还剩下的伴。伴还在的话,咱们领一帧新页,字节整个地拷过去,本方的表项改指新帧,计数减的是一,各走各的路。数下来的伴是零,末位了,省去了拷这一步,把这一页的写权限还回去就算完——kCow 位留着不摘,可写与可共享的并存是无害的,往后这一页再要标共享的话,走的还是同一条降级的路。这里有个门道咱们特意想过:裁决者不问缺页来自内核还是用户。您看着像漏,其实是必需——往后 wait4 要替父进程把退出码写进用户栈,内核替客人动手的写,同样是合法的写,同样免不了分家。至于根本没映射的指针,叶上没有 kCow 的标记,天然地不受理,落进转储里响亮地死,不会变成悄悄的错。

纪律还立了一条,咱们写在引擎的门口,裁决者跑在异常深度——它的代码是在缺页异常的处理程序里跑的,栈上还叠着故障的现场——不睡、不拿锁、除了一帧新页不分配。它是机器报丧时刻的急救员,急救员自己是不能再病倒的。

测试这边咱们交两桩手艺。头一桩的活是重写。首版的复制用例,咱们自以为造了一帧两映射,实际上是领了两帧、各自映射——两帧互不相干,怎么写都绿,引擎空转也照样是绿的。探针打出了一行:company=0,当场就穿了帮。重写后的用例才是对的形状:一帧,两个地址,各标了共享,然后写 A 地址,断言 A 变了、B 没变,世界的帧数收支相抵。咱们把它升格成一句话:构造性测试的绿,要禁得起探针的证伪,才算作数的。第二桩还是基线的事,这一卷的第二回,一口气来了两次:一回取在 init 之后,差了根表那一页,另一回取在测试把自己的共享帧都标好之后,差了五页。咱们把对错两边摆开一比,取基线的时机跟着要数的东西定——数一个世界的全部家底,取在它的建立之前。数一段操作的净变化,取在布置完了之后。

引擎立完了,四条测试线全都过了,产品镜像在 WP=1 下照常跑到了 shell 提示符,咱们听见的铃都是该响的。下一节咱们请 fork 本体进场,把引擎挂上真正的钩子。
