---
title: 05 · 一份走查,两个世界
description: "MapPage、UnmapPage、TranslatePage 降形成 TableWorld 概念之上的模板:表怎么取、新页从哪来,各世界自带。host 拿一个 64KiB 缓冲池当物理内存,内核拿直接映射加 Pmm,同一份走查逻辑两边真跑。档位立名 WalkLevel,零初始化是页表的出厂纪律。"
chapter: 11
order: 5
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - memory
  - paging
  - testing
---

# 一份走查,两个世界

旧城拆完了,页表归了咱们,可咱们手里还没有工具:想给堆添一页映射,想拆掉现有的一页,想问某个虚拟门牌落到哪块物理——这些日常的活,其实件件都是同一件事:顺着四级表往下走。这件事的正名叫走查,本节咱们给它安家。安家之前咱们得定形状,这里有一段决策的来路值得摆出来:当年第一遍的走查,住的是一个带全局实例的 VMM 类。咱们复盘它的家底,发现这个类的成员拢共一个——内核 PML4 的物理地址。一个字段就撑起了一个类,对象身份其实是空的。而它真正的参数早晚不止一个:本卷末尾的地址空间,每个世界都有一套自己的根,走查的根就该是入参,而不该是成员。所以这一遍咱们把走查降了形:做成纯函数模板,根成了传进来的参数,对象身份整个让给后头的 AddressSpace。

形状定下来了,跟着来的第二道题是:走查代码凭什么信它对?按咱们立下的双轨路子(武器库那一卷在长凳上立了框架,会听那一卷把机器里的一轨补齐),该干的就是在长凳上真跑。可走查摸的是页表,长凳上偏偏没有 CR3,怎么跑?咱们把走查对世界提的要求降到最少,提炼成了一个概念,起的名字就叫 TableWorld:

```cpp
template <typename World>
concept TableWorld = requires(World& world, unsigned long physical) {
    { world.table(physical) } -> std::same_as<Entry*>;
    { world.take_page() } -> std::convertible_to<unsigned long>;
};
```

咱们要世界摆出来的脸,拢共两张:table() 把一个物理地址变成可写的表页——怎么变,是每个世界自己的事。take_page() 交出一张全零的页当新表,物理地址 0 表示拒绝。走查对世界的要求,到两张脸就打住了。所以同一个模板伺候两个世界:内核这边 KernelTables,table() 走的是直接映射换门牌,take_page() 从 Pmm 取页、亲手清了零再交货。长凳那边的 HostWorld,拿一个 64KiB 的缓冲池当作物理内存,发页从池内的 0x1000 起,发的每一页也照样清零。您看设计的分量:走查逻辑全仓只有一份,测对了一回,收获的就是两边同对——长凳上跑过的每一趟,都是内核里即将发生的事。

走查的本体,咱们从下楼梯的台阶说起。四级表每级从地址里咬九位当槽号。九位各住在哪一层?咱们给它们立了名:

```cpp
enum class WalkLevel : unsigned char {
    kPml4          = 0,
    kPdpt          = 1,
    kPageDirectory = 2,
    kPageTable     = 3,
};
```

这一段有个来路:头一版的循环里,档位是 0、1、2 的裸数字,比较用的是 level < 3,算槽号靠的是数字移位。写是写通了,可您每读一行,都得在脑子里维护一张编号对照表——这跟长模式那一卷立下的家法正好顶牛,那卷的家法把话立成了:位置只写在定义点里,而数值只许出现在断言里。档位的 0 到 3 散在定义点之外,头一个不答应的就是家法。所以档位立名,槽号的九位用 IndexRange 按档位给出,取槽一律走的是 SlotIndex(地址、档位),代码里的裸移位一处都不留。往后的循环里只有 kUpperLevels 这样念得出名字的东西,0 到 3 的数字回到了它们该在的地方——枚举的定义点。

下楼的主函数是 EnsureLeafTable:咱们从根出发,过了三层上级表一路走到页表,表里缺谁咱们就补谁:

```cpp
template <TableWorld World>
Entry* EnsureLeafTable(World& world, unsigned long root, unsigned long virtual_address) {
    Entry* table = world.table(root);
    for (const WalkLevel kLevel : kUpperLevels) {
        Entry& slot = table[SlotIndex(virtual_address, kLevel)];
        if (!slot.has(cinux::arch::page::kPresent)) {
            unsigned long const kFresh = world.take_page();
            if (kFresh == 0) {
                return nullptr;
            }
            slot  = cinux::arch::page::MakeTableEntry(kFresh, cinux::arch::page::kWritable);
            table = world.table(kFresh);
        } else if (slot.has(cinux::arch::page::kLarge)) {
            return nullptr;
        } else {
            table = FollowTable(world, slot);
        }
    }
    return table;
}
```

三个分支各管一种路上的情况。槽是空的:现场补页——take_page 交货,MakeTableEntry 随手把新页挂进了槽里,下楼梯的脚就踩上了新台阶。槽是大页的:直接回来报拒绝——大页底下没有页表可到,把人家一张 2MiB 拆成 4KiB 不是走查能替人做的主,那是调用方的策略问题,咱们后面翻译大页的时候再细说这个拒绝。槽是正常的表指针:FollowTable 取物理地址,咱们踩下去接着走。MapPage 本身薄得就剩了三行:EnsureLeafTable 拿到页表,写进去的目标项就是一条 MakeTableEntry。参数表上有一个刻意的缺席:标志位。MapPage 眼下写的只有 present 加 writable:消费方没有提出第三种需求之前,咱们不涨这个参数,需求来了再加。接口一次作对的意思是不多做,而不是做全。

三个分支里最值得停下来看的是补页,而补页的要害在 take_page 的一个形容词上:零页。零字咱们把它立成了出厂纪律,当年的调试现场翻出来,您就知道这纪律的分量。页表页要是带着上一任主人的残留数据上岗,残留里那些最低位恰好是 1 的项,CPU 一查就是映射在——映射到了哪个物理地址?按残留里剩下的位走,得到的就是一个随机数。症状玄得没边:translate 出来一个莫名其妙的地址,或者写这个虚拟地址,改的却是毫不相干的物理内存,等咱们重启一次,那页内存换了残留,症状跟着换了一副面孔。三分靠的是代码,而七分靠运气,病起来是要命的。所以纪律只有一条:凡是新页上岗当表,交接的前提是清成全零。种子柜发的是清过零的页,Pmm 侧的 KernelTables 交货前清零,HostWorld 发页也清了零。三处发页走的是同一道工序,咱们一道都不省。

长凳上的验收,mm 家族这回添的是 vmm 一件六案。咱们点两案说:头一案,一个没映射过的地址走 MapPage,断言 world.taken 涨了四——根一张、PDPT 一张、PD 一张、页表一张,四级下楼缺谁补谁的事,一遍就数清了。咱们再看一案,验的是重映射覆盖:同一个门牌挂过 0x111000 再改挂 0x222000,translate 出来的必须是新页——旧项无条件被顶掉。引用计数的那一套是用户态库的活计,内核的页表不用它,谁家活着谁说了算。

走查会下楼了,也该会查数了。下一节咱们讲另外两个动词:拆除,和只读的翻译——顺便把一张走查改完表之后,CPU 那头还蒙在鼓里的大事说了。
