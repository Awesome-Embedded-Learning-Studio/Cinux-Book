---
title: 04 · 旧城拆掉
description: "直接映射接进 PML4[256]:按 E820 实报的 usable 一窗一窗铺 2MiB 大页,帧缓冲在前一步接上 ioremap 的新门,栈从低位的 0x90000 搬进镜像,然后掏空共享 PDPT、PML4[0] 清零、ReloadCr3 换血。顺序错一步就是黑屏,陈旧镜像差点骗走一次验收。"
chapter: 11
order: 4
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - memory
  - paging
---

# 旧城拆掉

动工本节的活分两处安身。拆城换血的活,咱们收进了一个函数:BringUpAddressSpace。搬栈的活写在装载的剧本里,链接脚本添的段、entry 改的一行,咱们下面见到了再细说。按实走的次序是四件:建直接映射、接帧缓冲、搬栈、拆城换血。咱们一件一件过,顺序本身是本节的正主,每一处为什么不能换,咱们到时候就地给理由。

头一段的活是建直接映射。思路跟门是一脉的:一整张 PDPT 交给 PML4[256],底下的每个 1GiB 窗配一张 PD,PD 里铺的是 2MiB 大页。可它跟 boot 的门有一个本质的分别:门数当年按内核的尾巴算,盖住镜像就收了工。这回咱们按图谱算,内存条的容量报多少,咱们就把窗开到多高——高也高不过名册管得到的顶:16GiB 是名册那一卷立的天花板,种子柜里的粮,也是照着它备的。核心的循环咱们摆出来:

```cpp
unsigned long const kRamTop = highest_usable_top(HandoffArchive::self().record());
for (unsigned long window = 0; window < kRamTop; window += kHugePageSize) {
    if (!window_touches_usable(HandoffArchive::self().record(), window)) {
        continue;
    }
    unsigned long const kDirectoryPhys = SeedPantry::self().take_page();
    if (kDirectoryPhys == 0) {
        return nullptr;
    }
    fill_usable_ram_pages(table_at(kDirectoryPhys), window, HandoffArchive::self().record());
    table_at(kPdptPhys)[SlotIndex(window, WalkLevel::kPdpt)] =
        MakeTableEntry(kDirectoryPhys, kWritable);
}
```

咱们拿 QEMU 眼下真跑的数走一遍。E820 报上来的 usable 有两段:0 到 0x9FC00 的头一段,和 1MiB 到 0x7FE0000 的另一段。咱们把顶折成 MiB 一算,得出的约是 127.9,而离 1GiB 还差老远,所以眼下只开了 0 号一个窗,咱们从种子柜里领一张 PD。这个顶也不是白记的:咱们拿 0x7FE0000 减去 0x100000,算出来的 32480 页,验收的时候咱们还要拿它对一遍。咱们在窗里面再细切:每条 usable 跟窗两头一夹,夹出来的地界按 2MiB 对齐铺大页,一直铺到夹区的顶。您注意,这里的表,没有一张是 boot 留下的——每一项都是咱们照着图谱现场新画的。哪怕某台机器的内存条上有洞,窗碰到洞的地方就少铺几张,图谱说的数是多少,咱们就铺多少。咱们装到这一步,直接映射就活了:PML4[256] 装进了 boot 的 PML4,旧城的房子还没拆,新城的第一条街已经通了车。

第二段的活是给帧缓冲搬家。旧门咱们盘过:屏幕的门开在低半边的 PDPT 槽上,盖住 0xFD000000 的是一张 1GiB 巨页。旧门在拆城的时候必然要断,所以断之前得把新路接好。地图上设备的家在 PML4[259],ioremap 的窗。这一卷窗里住的是一位临时工,咱们的走法是相位一的镜像,物理偏移原样照搬:帧缓冲的物理位置在 0xFD000000,咱们把窗基一加,新门牌就出来了,花的力气就是一步加法。为什么拿这么土的办法顶着?ioremap 的正经差事(登记设备区、发放追踪过的映射)得有分配器伺候,而分配器的消费者还没到。临时工的代码量小得可怜:从种子柜取一张 PDPT 出来,再按帧缓冲横跨的窗数配几张 PD,窗内铺的还是 2MiB 大页,咱们把它装进 PML4[259],就完事了。从这一刻起屏幕控制台取基址走的就是 IoremapVirt,产品内核和测试内核用的都是一个口径。

第三段咱们搬栈,这是拆城前最后一件低位的事。栈的新家安在镜像里:链接脚本添一个 NOLOAD 的 .stack 段,给它 16KiB 的地界(内存里占着地界,盘上倒是一个字节都不占,NOLOAD 说的就是这个),段尾立一个 g_kernel_stack_top 的名字。entry 里硬编码的那句 movabsq,换成 rip 相对的取址:

```cpp
"leaq g_kernel_stack_top(%%rip), %%rsp\n\t"
```

新栈是跟着镜像走的,自然落在镜像窗的门里,咱们拆城也拆不到它。这里有个汇编的小陷阱咱们如实记下:GCC 的扩展汇编里把 % 当保留字,寄存器前面的那个 % 要双写成 %%,咱们头一版写成单的,clangd 当场就把 asm 的转义错指了出来。还有一处顺手的干净:entry 里清 .bss 的循环只认 bss 的起止名,栈段压根不在它的扫区里,落了个两不相扰。这一项搬完了,内核从头到脚没有一处再依赖低位物理——除了脚下踩着的旧城本身。

得交代的还有一处:本节立表改表,大半程用的都是低半边的门牌。vmm.cpp 里有两个取表的帮手,table_at 拿的是物理地址、直接当指针使唤。建直接映射的那一段立起来的每一张表、装进 PML4 的每一项,走的都是它——种子柜圈在低位空地,圈的就是图了个顺手。直接映射通了电之后,同一张表其实有了两条路:旧路走的是低半边,新路走的是直接映射。真到拆城的那一拍,咱们刻意只走新路,拆的就是旧路,一边拆一边踩是要出事的,所以代码里换成了另一个帮手 direct_table_at,取的表一律经直接映射换门牌。您看接管函数里紧跟归档的那一行:

```cpp
Entry* const kPml4 = table_at(cinux::arch::ReadCr3());
```

连根指针自己都是低半边的别名:ReadCr3 读回来的物理地址,眼下还得经由 PML4[0] 的那扇旧门,咱们才摸得到根。所以旧路拆到最后一项的时候,靠它走着的翻译还剩 TLB(CPU 的速查记忆)里的最后一口气,ReloadCr3 的那一声响,才把这口气收了个干净。

第四段的活是拆。这一步咱们把 vmm.cpp 的原话整个摆出来,里面的每一步都是一条命:

```cpp
Entry* const kSharedPdpt = direct_table_at(EntryTarget(kPml4[kKernelPml4]));
for (unsigned int i = 0; i < kEntriesPerTable; ++i) {
    if (i != kKernelImagePdpt) {
        kSharedPdpt[i] = Entry{};
    }
}

direct_table_at(cinux::arch::ReadCr3())[0] = Entry{};
cinux::arch::ReloadCr3();
KernelRoot::self().capture(cinux::arch::ReadCr3());
```

头一行取的是 PML4[511] 指着的 PDPT——咱们记得,它同时还是 PML4[0] 的家,一张表是当两个门牌用的,boot 省了内存,咱们拆的时候就得小心:掏空它,等于同时断了低窗的别名(第 0 槽)和设备的巨页门(帧缓冲那一槽),只有第 510 槽(镜像窗自己的位置)要原样留下。咱们掏完再把 PML4[0] 整项清零,低半边从此没有了入口。为什么顺序错不得,咱们把事故现场摆出来您就明白:要是拆城在前、接帧缓冲在后,掏空 PDPT 的那一拍,屏幕的旧门没了,而新门还没装。下一个写往屏幕的字节就是一次缺页,而缺页处理现在还是转储,转储偏偏还要往屏幕写字。QEMU 的回应干脆利落:黑屏。所以次序是铁的:新门通车在前,旧城的开拆排在后头。最后那行 ReloadCr3 把 CR3 的原值重装一遍,顺手办了一件大事:重装会换来一次整个 TLB 的换血,所有缓存过的旧翻译一并作废——低半边不光从表里死了,也从 CPU 的速查记忆里死了。末了把这一刻的 CR3 存进 KernelRoot,存的就是内核自家的根,它的用处这一卷末尾讲地址空间的时候见。

验收的那天还有一段插曲,咱们照实记下来给后来人省时间。咱们头一次开机,面板上死活不见 vmm 那行新字的影子,咱们盯着串口差点判了接管失败。后来才发现跑的是陈旧镜像:cmake --build build 不带目标名,走的是 all,而 all 里不含 boot_image——镜像没有重打,内核还是旧的。全量验证必须显式地写 --target boot_image,或者干脆走 --target run 的路子,咱们在这儿把话立成纪律。重跑之后面板亮出了新行:

```text
[kern] vmm: address space up, magic 114514
```

这行字的取数有点讲究:它是拿内核的物理地址经 DirectMapVirt 换算成门牌,去读镜像头上的魔数——读出来是 114514,装载那一卷九判检查过的老熟人。一枚魔数走新门读了出来,直接映射是活的,这一行就是它自己给自己的证词。光面板说了还不算,咱们在测试内核里另立了一件案:把活着的根取来,PML4 的 0 到 255 项逐项断言为零。全零的 256 项,就是低半边死亡的法医报告。逐格验过尸的没了,而不是“应该没了”。

旧城拆完了,脚下的路全换成了自己画的。可眼下咱们只会立门,日常的开门还不会:映射要能加、能拆、能查,这就要页表走查了。下一节咱们写走查,写的是一份代码,咱们拉着两个世界一起跑。
