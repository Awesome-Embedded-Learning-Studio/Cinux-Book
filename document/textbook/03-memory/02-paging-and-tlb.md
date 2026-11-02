---
title: 02 · 分页与 TLB
---

# 02 · 分页与 TLB:五段劈址,两副走法,一簿速记

上一章末了,咱们把一套立好的世界交到了机器手里。表是软件画的,取指访存的却从来不是软件:机器自己拿一枚虚拟门牌照表下楼,一拍一趟的步子天天如此,由不得咱们。这一章咱们跟着机器走一遍它脚下的路,顺带把上一章收卷时留下的两问一起答了:走岔了找谁,表改了机器多久知道。

1.4 手铺三张表的时候留过一句期票,真正的页表体系是第 3 部的正戏,那儿咱们不铺高速公路。期票到这一章兑清:三张表怎么填,1.4 教过了,机器拿到表之后自己怎么走,今天从头走给大伙看。

这一章的活,咱们数下来三样。头一样认词汇:四级表各吃地址里的哪九位,一枚表项八个字节怎么摆,词汇认了,机器脚下的路咱们就看得见了。再一样认走查的两副走法,一副只读的,一路带建的,3.1 留的那句缺表谁补谁报零,谜底就落在这对分家的身上。末一样答后一问:表改了,机器凭什么不是当场就知道,又凭什么能当场知道,登场的名字叫 TLB,机器手头的翻译速查缓存,本章的第二位主角。

量法还是咱们熟悉的三条。码的主角是 kernel/mm/page_walk.hpp,一份 351 行的文件,咱们通读。位段的语法住在 page_entry.hpp,作废的两张脸住在 registers.hpp,两件都在 kernel/arch/x86_64/ 的地界里,用到哪家咱们就请哪家。屏的量法是两份。长凳上的一份:宿主测试不出机器,在咱们开发它的电脑上直跑直抄,长凳这个叫法是正文叫开的。机器里的另一份:内核自测三案起机即跑,您连键都不用喂。书还是 xv6 的第 3 章,书页 36 与 37 两页都逐字核过了,整段的原话誊在章里,大头落在对照节的名下,险情那句赶在 TLB 正戏里露了面。

> **表是软件画的,机器下楼凭什么认它?表改了,机器手里的旧翻译几时换新?**
>
> 两问其实一个出处。page_walk.hpp 的头注,笔者放在末尾的那句是 `TLB maintenance is deliberately outside: the walk edits tables, callers own translations`,走查管的只是改表,而翻译归调用方作主。后一问的谜底压在后半句上,本章中段专门开了一节来接。前一问的谜底铺满整份文件:机器认表,认的是一套定死的格式,位的所在、段的所在,一级都不由咱们自作主张,下一节咱们就把这套格式认全。

## 词汇:址劈五段,项读八字节

头一笔家底是表的个头,咱们数一数。一张页表拢共装下了 512 项,一项的分量是八字节,512 乘 8 恰好凑成了 4096,一张表自己就住满了一页,新表上岗前的清零,清的就是这一页的尺寸。page_walk.hpp 的 :49 把 512 立成 kEntriesPerTable,这个数咱们全文只立一处,别处用的都是名字。

接着咱们认劈址。WalkLevel 四档住在 page_walk.hpp 的 :59 到 :64,四档的名字是 kPml4、kPdpt、kPageDirectory、kPageTable,一名认的是一档。每档从地址里咬九位当槽号,咬在哪一段的事由 IndexRange 按档报出,四档的起头依次落在第 39、30、21、12 位,宽度一律定成了 9。四档的九位合起来是三十六位,加上页内偏移占的低 12 位,就凑成了 48 位,一枚址就这么劈成了五段。48 往上的 16 位,3.1 量天花板时交了底,得仿着第 47 位填满才叫规范地址,这层格式是机器自己守着的,咱们不用再操心。取槽的手续也立了名叫 `SlotIndex(地址, 档位)`,:107 起的身子只有一行,拿同一副位段词汇去读地址自己:读表项怎么抠位,读地址就怎么劈段,一副词汇咱们两头使。

址会劈了,轮到的是项。八字节的一枚表项,低位的那几枚开关,位是 3.1 见过的,名字今天立全了:第 0 位 kPresent 管在不在,第 1 位的 kWritable 管可写不可写,第 2 位 kUser 管客人过不过,page_entry.hpp 的 :35 到 :42 一位一行。今天添的是第 7 位,住在 :45 的 kLarge,而它一翻,这项就不再是路标了,自己就成了一片地,这样的叶,本章给它立的名字叫大叶,底细咱们放到本章后面专门开讲。开关之上的部分是物理字段,三档各守各的一段。kTablePhys 守的段从第 12 位起宽 40,表指针项用的是它。kLargePagePhys 守的段从第 21 位起宽 31,2MiB 的大叶用。kHugePagePhys 守的段从第 30 位起宽 22,1GiB 的大叶用。三个字段的头顶齐刷刷收在第 51 位,52 往上那 12 位是机器格式里的另一些差事,咱们这套词汇表没用到,这里就不展开了。物理字段偏偏从第 12 位起头,缘由摆在页的个头上:页表只认页框,低 12 位的零头进不了字段,MakeTableEntry(:85 起)把递进来的物理址右移 12 位再装,装完顺手把 kPresent 与递来的 flags 并作一把,一枚表项就出厂了。低 12 位被扔掉的这一层注释写得直白,`low bits below 12 are dropped`,页没对齐的址递进来,零头无声无息地没了,这类话头等用着它的章节再展开。

**咱们停一停,一枚址递到您手里,五段您自己劈**。

长凳测试里拼址的小帮手叫 compose,test/unit/test_vmm.cpp 的 :46 到 :54:它把 5 填进 PML4 段、7 填进 PDPT 段,PD 与页表两段的槽号当参数递进来。头一案递的是 `compose(9, 11, 0xABC)`。您拿 IndexRange 自己折一遍:这枚址写开是多少,五段各是几?

咱们对一遍。5 落到了第 39 位上,7 落到了第 30 位上,9 落到了第 21 位上,11 落到了第 12 位上,尾巴坠着的是 0xABC,写开就是 5 个 2 的 39 次方,加 7 个 2 的 30 次方,加 9 个 2 的 21 次方,加 11 个 2 的 12 次方,末了再加 0xABC,合出来的就是 0x281C120BABC。MapPage 把 0xABC000 挂上了这个门牌,TranslatePage 走了一趟回来,交的是 0xABCABC:物理页 0xABC000,页内的偏移是 0xABC,分毫不差地对上了。案里还断了一笔,world.taken 等于 4:根一张的底子,路上缺的 PDPT、PD、页表补了三张,缺的是谁就补谁,一遍就数清了。

## 走查:一副只读,一路带建

四级怎么走,咱们看码。走查的每张脸,头一个参数的名字都叫 world,3.1 里 AddressSpace::map 造的那枚 KernelTables,递的就是它。world 这个名字是有正经名分的:正文《一份走查,两个世界》那一篇给它立的名叫 TableWorld,page_walk.hpp 的 :44 到 :47 立成 concept,一个世界只要把两张脸摆了出来就够格,table() 把物理址换成能下手的表页,take_page() 交的是一张全零新页、回 0 表示拒发。够不够格的事,编译期就判了下来,喂一个不称职的对象进来,编译器就开了口,机器根本出不了事。世界怎么供表的事走查一概不问,所以同一份下楼码,内核的世界能跑,长凳上的假世界也能跑,这一层的实况,咱们到实况节现场见。

只读的那副叫 FindSlot,:173 到 :188 的那段,咱们整个看:

```cpp
template <TableWorld World>
SlotAt FindSlot(World& world, unsigned long root, unsigned long virtual_address) {
    Entry* table = world.table(root);
    for (const WalkLevel kLevel : kUpperLevels) {
        Entry& slot = table[SlotIndex(virtual_address, kLevel)];
        if (!slot.has(cinux::arch::page::kPresent)) {
            return SlotAt{.entry = nullptr, .level = kLevel};
        }
        if (slot.has(cinux::arch::page::kLarge)) {
            return SlotAt{.entry = &slot, .level = kLevel};
        }
        table = FollowTable(world, slot);
    }
    return SlotAt{.entry = &table[SlotIndex(virtual_address, WalkLevel::kPageTable)],
                  .level = WalkLevel::kPageTable};
}
```

咱们从根进表,看的是它一级一级怎么认:kUpperLevels 把 PML4、PDPT、PD 三级上级表排好队,一级取的是一枚槽号,SlotIndex 把它劈了出来。路上的分岔有三处。槽上没翻起 kPresent 的牌,下楼的路断了,回的是一个空手,level 里记下了断在哪一级,translate 报零的来路就在这儿。槽上翻起了 kLarge,半路就到大叶了,只读的走法不评判,把这枚项和它住的档位如实报给调用方,怎么解读是后头的事。两项都不占的话,FollowTable 顺着项里的物理字段摸到下一张表,就接着往下走了。三级走完了,落点是页表里属于这枚址的那一项,原样交了出去,在不在的事,咱们交给调用方自己看 kPresent。整副走法其实一个字没写,注里也写明了 read-only。

带建的那副叫 EnsureLeafTable,:204 到 :225 的那段,咱们再整个看:

```cpp
template <TableWorld World>
Entry* EnsureLeafTable(World& world, unsigned long root, unsigned long virtual_address,
                       Entry leaf_flags) {
    Entry* table = world.table(root);
    for (const WalkLevel kLevel : kUpperLevels) {
        Entry& slot = table[SlotIndex(virtual_address, kLevel)];
        if (!slot.has(cinux::arch::page::kPresent)) {
            unsigned long const kFresh = world.take_page();
            if (kFresh == 0) {
                return nullptr;
            }
            slot  = cinux::arch::page::MakeTableEntry(kFresh,
                                                      cinux::arch::page::kWritable | leaf_flags);
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

同一副楼梯的底子,岔口上各有各的主意,咱们一级一级对。空槽不报断了,现场的补法是:take_page 领一张新表,MakeTableEntry 把新页挂进了槽里,脚踩上去就接着走了。领页吃了闭门羹,回的是 nullptr,建不起来就是建不起来了,没有半点糊弄的意思。拦路的照旧是 kLarge,这回的处置也是回 nullptr:大叶底下没有页表可到,把一片 2MiB 细成一堆 4KiB 是调用方的策略,所以走查不替人拿这个主意。比只读那副多出的一个参数是 leaf_flags,路上每张新出生的表,挂进槽里的项除了 kWritable 还要带上它。它管什么,看 map 的两张脸就明白。

MapPage 与 MapUserPage 咱们把 :246 到 :256 与 :276 到 :286 摆在一处看,身子是一个模子里出来的:都是 EnsureLeafTable 开路,末行往页表里写的是一枚 MakeTableEntry。差的就是客人的位一路带到底:递给 EnsureLeafTable 的 leaf_flags,map 递的是空的,新表出生带着 kWritable 就收了工。map_user 递的是 kUser,路上每张新表出生就带着客人的位,末行的那枚叶项也要并上 kUser。kUser 的脾气 3.1 教过了:权限每过一级验一遍,而任何一级写着仅内核,ring 3 就迈不过那一级,所以 kUser 得四级全带,咱们让新表出生就带上,事后补都补不回来了。两张脸之所以分家,分的就是这个:画谁的页就叫谁的名字,消费面从此不必接触 flags 的词汇。

末一副的名字是 UnmapPage,:303 到 :312 的一段,守门的一判,抄、清、还的三句,咱们挨句认。头一句 FindSlot 走到了槽前。第二句守门:半路断了,或者停在了大叶上,回一枚零项交了差,大叶的事不归它管,跟 map 那边走的是同一条章程,细不细的主意由调用方拿。到得了槽,抄下旧项当了回执,槽清成了零项,末一句把旧项原样交还了。注里又写了半句,`flushing the translation is the caller's duty`,翻译的作废是调用方的差事,凭什么是调用方的,这半句欠下了,过两节就是它的正戏。

## 大叶:停在半路的叶

大叶的正戏开讲,咱们把名字立稳。3.1 认叶子的时候,它住在页表的末级,末级的一枚项盖 4KiB 一页。可中间两级的项不肯总当路标:kLarge 一翻,PDPT 上的项自己就是一片 1GiB 的地,PD 上的项就是一片 2MiB 的地,项自己当起了叶,往下的路就免了。它图什么,摊开就明白:拿 4KiB 的叶盖 1GiB,中间的三级得一张一张把表铺满。而大叶一翻,一枚项顶一片地,底下的表一张都不用。

大叶其实不是新客。咱们接管那天铺下的直接映射窗,用的就是它:1GiB 一窗,窗里的叶是 2MiB 一片,正文那一卷讲直接映射铺窗与旧表退役的那一篇里一窗一窗数过。boot 当年的恒等映射,表只铺到了 PD 为止,落的也是 2MiB 大叶,1.4 只铺 8MB 的缘由今天也见了分晓:盖 boot 自己那点地界,到 PD 级就足够了。至于 1GiB 的这一档,眼下机器里没有现成的住户,长凳上有一案专给它留了位置,实况节里咱们见。

翻译的脸 TranslatePage,:329 到 :349 的那段,咱们整个看:

```cpp
template <TableWorld World>
unsigned long TranslatePage(World& world, unsigned long root, unsigned long virtual_address) {
    SlotAt const kSlot = FindSlot(world, root, virtual_address);
    if (kSlot.entry == nullptr || !kSlot.entry->has(cinux::arch::page::kPresent)) {
        return 0;
    }
    if (kSlot.level != WalkLevel::kPageTable) {
        if (kSlot.level == WalkLevel::kPml4) {
            return 0;
        }
        auto const kField = kSlot.level == WalkLevel::kPdpt ? cinux::arch::page::kHugePagePhys
                                                            : cinux::arch::page::kLargePagePhys;
        Entry      physical{};
        physical.deposit(kField, kSlot.entry->extract(kField));
        return physical.raw | LowBits(virtual_address, IndexRange(kSlot.level).low);
    }
    Entry physical{};
    physical.deposit(cinux::arch::page::kTablePhys,
                     kSlot.entry->extract(cinux::arch::page::kTablePhys));
    return physical.raw | LowBits(virtual_address, IndexRange(WalkLevel::kPageTable).low);
}
```

进门照例是 FindSlot 的那副,咱们分两路看。落点空了手,或者到了槽上却没带 kPresent,回的是 0,3.1 埋的那个 0 来路就是头一道判。落点是页表档的话,那就是一枚 4KiB 的叶:物理字段抠出页框,低 12 位拼上了页内偏移,就完事了。而落点不是页表档,说明半路停在了大叶上,这头有一重门槛:PML4 档的大叶直接回 0。机器的格式里,大叶的落脚只许在 PDPT 与 PD 两档,PDPT 档翻牌盖的是 1GiB,PD 档翻牌盖的是 2MiB,各档翻牌盖多宽的事,章程里白纸黑字写明白了。PML4 上的 kLarge,盖多宽咱们在格式里找不着,FindSlot 只报而不评,这一重替它把不合法的挡下。过了门槛,停在哪档就用哪档的物理字段:PDPT 档用 kHugePagePhys,PD 档用的是 kLargePagePhys,基址抠了出来,低位拼上了偏移。这儿有个自洽值得单独看一眼:拼偏移用的宽度,恰好是 IndexRange(档位) 的 low,1GiB 的叶停在 30 位上,叶内的偏移就是低 30 位,2MiB 的叶停在 21 位上,偏移取的就是低 21 位,4KiB 的叶,偏移占的是 12 位。一个公式就通吃了三档,档位自己把自己的偏移宽度报了出来,IndexRange 的注里那半句说的就是它,`doubles as the leaf's offset width`。

## 表改了,机器多久知道

按前两节的读法,表改了,机器下一拍走的就该是新图,毕竟每回下楼都是照表走的。机器偏不这么憨:四级下楼一趟脚程不便宜,每条访存指令都来一趟的话,脚程全折在了楼道里,机器就快不起来了。所以它把查过的翻译记进一本速记的小本本,同一枚址走了第二遍,翻小本本就够了,查表的那道工序全省。这本小本本的行话叫 TLB,翻译的速查缓存,名字是正文《unmap 欠的那一下》那一篇叫开的,咱们接着叫,底下这段症状的讲法也是那一篇的,“最玄的症状出台了”这个话头咱们也一并借来,照单全收了。

快是真的快,险也是真的险,您把两边分开看:表是软件改的,小本本是机器记的,两下里是不通气的。表里的项清了,小本本里的那枚旧翻译还揣着,机器又走到了那个门牌,一翻小本本就中了,照旧落到了旧页上。于是最玄的症状出台了。明明 unmap 了,访问偏偏不缺页。明明 map 了,访问偏偏报缺页。过一阵子别的操作把小本本挤一挤,旧翻译让了位,病又自己好了。时好时坏、看运气吃饭的毛病,多半是小本本上有一条旧翻译忘了作废。

作废的两张脸住在 registers.hpp。单页的叫 InvalidatePage,:89 到 :91 的一段,一行 invlpg 就是它的全部身子,咱们递一枚址进去,只作废那一页的翻译。头注把差事写成了英文,`Required after unmapping or tightening permissions`,清了映射、收了权限,都少不了一发,漏了会怎么样,注里也把话直说了,`skipping it leaves stale translations in play`,说的就是旧翻译照常在场上跑。全空间的叫 ReloadCr3,:55 到 :57 的一段,把 CR3 的原值重装一遍,重装的那一拍,机器放掉的是不带全局牌的翻译,全局牌翻了的它拦不下,头注给它定的位是 `the blunt instrument`,糙家伙的意思,单页的活犯不上惊动它。咱们词汇表里压根没立这枚全局牌,机器里也没有一处翻过它的牌,所以这一挥就是一件不剩的清场。

凭什么走查不代劳,头注那半句已经给了答案,咱们把它展开:作废废的是哪套表的翻译?根是入参,走查改的可以是一个不在场的世界,给不在场的世界发作废,发到谁的头上,其实谁也说不上来。哪套表在跑、跑表的人是谁,说得清的只有调用方,义务就归了调用方,UnmapPage 注里悬着的那半句到这儿落地。老户其实也有:接管旧表那一拍,vmm.cpp 的 :229 到 :231,旧门清零、ReloadCr3、KernelRoot capture 的三行连着走了下来,3.1 说的定格就落在末一行。那一响是 ReloadCr3 在咱们机器上的头一回出场。

所以后一问的答案,咱们摊开说:表改了,咱们能让机器当场知道,前提是调用方把作废的这一枪补上。补上的话,下一拍走的就是新图。不补的话,机器揣着旧翻译继续跑它的,跑到哪天小本本被挤掉了才算完。前一问的答案也顺带交了:机器走岔了,14 号当场报了出来,接报的机构 2.2 立起来了,机器停在了报告上。把走岔没走岔变成一问就答的事,靠的是 translate 这几张脸,本章已经把它们挨个请了出来。

**咱们停一停,作废的那一枪要是不补,您说最坏能坏到哪一步?**

咱们架空推一推。一枚页 map 进堆的基址 kHeapBase(layout 立的名)往上一页,咱们写读都对,unmap 完了,页还回了名册,invlpg 的那行当它不存在。表上这项已经没了,可机器脚下这一世界的小本本里,那枚门牌的旧翻译还端端正正躺着。咱们下一回拿旧门牌读写,机器翻它的小本本,一翻就中了,照旧够到了那枚物理页。而名册早把它发给了别人:下一个领页的,不管是内核的表还是谁家的地,写进旧门牌的每个字节,一字不差全落在别人新领的地上。吃亏的人还蒙在鼓里,他自己的那页一个字没写错。

xv6 把这个险情写在了书页 37,前半句躺在 36 的末行,咱们从句腰接着誊,前后两句的原文整段如下,扎眼的末一个词是 scribble,在别人的纸上乱涂:

> 咱们从句腰把原话整段誊来:`…when xv6 changes a page table, it must tell the CPU to invalidate corresponding cached TLB entries. If it didn't, then at some point later the TLB might use an old cached mapping, pointing to a physical page that in the meantime has been allocated to another process, and as a result, a process might be able to scribble on some other process's memory.`

## Code: Cinux 实况:长凳六案,机内三案

这一章的实况是两份,咱们让两个世界把同一份走查各跑一遍。锚用的还是 r13_userspace,上一章用的也是它。长凳那边的世界叫 HostWorld,test/unit/test_vmm.cpp 的 :27 到 :44:一块 64KiB 的池子当物理内存,table() 拿池子的基址一加就当表页使,take_page() 从 0x1000 起一发一发地发,发的每一页照例清零,发空了回 0。清零的缘故 3.1 交过底,残留上岗的病不再重叙。机器那边的世界是 KernelTables,vmm.cpp 里的两件差事,:150 的 table() 走直接映射换门牌,:154 的 take_page() 向名册领页、清零、交货。TableWorld 的说法是世界只要两张脸,两个世界就这么各把两张脸摆了出来,同一份的走查,踩着各自的供给,两边都真的跑了。

长凳的活是咱们编出 test_vmm 直跑,`cmake --build build --target test_vmm`,屏头尾咱们全抄:

```text

=== Cinux Test Runner ===
Running 6 case(s)...

[PASS] vmm walk: map creates three tables and translates round trip
[PASS] vmm walk: unmapped addresses translate to zero
[PASS] vmm walk: unmap reports the entry and clears the slot
[PASS] vmm walk: remapping overwrites the previous target
[PASS] vmm walk: 2MiB and 1GiB leaves translate with offsets
[PASS] vmm walk: map refuses to split a large leaf

=== Results: 6 passed, 0 failed ===
```

六案咱们逐案认。头一案就是停一停里劈过的那枚址,5、7、9、11 拼上 0xABC 的那枚,挂的是 0xABC000,翻回的是 0xABCABC,顺带断的还有 world.taken 等于 4。到了第二案,没映射过的址翻出来必须是 0,FindSlot 半路报的是空,TranslatePage 照单收的是零。到了第三案,unmap 抄回的旧项必须带着 kPresent,槽清成了零,translate 当场归了零,同一枚址再 unmap 了一回,回来的还是零项,空槽没什么可报。到了第四案,重映射考的是覆盖:同一门牌挂 0x111000 再改挂 0x222000,翻出来的必须是新页,旧项无条件让了位。第五案专伺候的是大叶,案身挑要紧的几行:

```cpp
kPd[3]   = MakeLargePageEntry(6_MiB, kWritable);
kPdpt[2] = MakeHugePageEntry(2_GiB, kWritable);
ASSERT_TRUE(TranslatePage(world, kRoot, 6_MiB + 0x1234) == 0x601234);
ASSERT_TRUE(TranslatePage(world, kRoot, 2_GiB + 0x4567) == 2_GiB + 0x4567);
```

PD 槽 3 上咱们摆一片 2MiB 大叶,基址给的是 6MiB。PDPT 槽 2 上再摆的是一片 1GiB 大叶,基址给的是 2GiB。翻一枚 6MiB 加 0x1234 的址,走的是 PD 档大叶,低 21 位拼的是偏移,回的是 0x601234。翻一枚 2GiB 加 0x4567 的址,走的是 PDPT 档,低 30 位拼的是偏移,翻回的还是它自己。读穿三档叶的那段码,这两扇门当的就是考官。到了第六案,拒劈的考法:同一副摆设原样再摆一回,拿 MapPage 往 6MiB 与 2GiB 两片大叶肚子里各递一枚 4KiB 的 map,回的都是 false。EnsureLeafTable 半路遇上了 kLarge 就回 nullptr,大叶细不细的事,走查做不了主。

机器里那三案考的全是活根,活根说的就是 CR3 里现役的那枚根。咱们起 ktest 镜像,量法里说的内核自测,烧的就是它:`build/boot/cinux_ktest_vmm.img`,TCG 起了机,串口接上了文件,起了机自己就跑,您连键都不用喂,镜像显式重打的老话上一章念过,照旧给咱们作数。机内的屏没有 [kern] 面板,stage2 起机那一截的老话前两部念过多回,就不重印了,咱们从 ktest 报到的那行往下全抄:

```text
[ktest] kernel test world alive, 7 e820 entries

=== Cinux Kernel Test Runner ===
Running 3 case(s)...

[RUN] vmm: a page lives in the heap window only while mapped
[PASS] vmm: a page lives in the heap window only while mapped
[RUN] vmm: the live root translates image and direct map
[PASS] vmm: the live root translates image and direct map
[RUN] vmm: the user half of the live root is dead
[PASS] vmm: the user half of the live root is dead

=== Results: 3 passed, 0 failed ===
```

三案咱们逐案认。屏序的头一案,`a page lives in the heap window only while mapped`,就是停一停里架空推演过的一趟,机器里真跑了一遍:向名册领一枚页,MapPage 挂到 kHeapBase 往上一页的门牌上,拿指针往里写了 0x114514、再读回来,translate 对了一遍表,翻出来的必须是那枚页号。跟着 UnmapPage 抄回了旧项,清槽的下一行就是 InvalidatePage,作废的那一枪当场补上,translate 再问了一回,回的是 0。末了把页还回名册。链上每一步都是前几节认过的脸,串起来就是一枚页的一生。到了第二案,`the live root translates image and direct map`,拿活根真翻三枚址:镜像的住址 kKernelImageBase 加 2MiB,翻回的是物理 2MiB,直接映射窗里的 2MiB,翻回的还是 2MiB,没人动过的堆基址 kHeapBase,翻回的是 0。头两枚是接管那天铺下的老翻译,第三枚留的是空白,三枚一起把现役世界脚下的路验了一遍。到了第三案,`the user half of the live root is dead`,案名自己把话说了,活根的低半边是死的:ReadCr3 解出活根,PML4 的前 256 项逐项验零。旧门清零的手续,正文的那一卷讲过来历,这一案是它的验收单,低半边不光没了入口,连一项都不剩了。

## 对照:同一件事,别的机器怎么做

咱们把 xv6 翻到 walk 函数所在的书页 36,原话咱们整段搬来摆在下面:

> 第一句咱们请来:`walk (1497) mimics the RISC-V paging hardware as it looks up the PTE for a virtual address (see Figure 3.2).`
>
> 第二句咱们也照誊:`walk descends the page table tree one level at a time, using each level's 9 bits of virtual address to index into the relevant page directory page.`

头一句把它自家的立场交了底:walk 模仿硬件。而模仿,说的是软件走查照硬件下楼的格式走,劈址的协议两家共用,咱们 page_walk.hpp 走的也是这个路子。第二句说的逐级九位、九位当索引,跟 IndexRange 的低三档一段一段对得上,30、21、12 一档不差地对齐了。39 那一档它家没有:咱们这套四级,是 x86_64 比 RISC-V 多出的顶层,它家三级就到了顶,3.1 比顶时点过的三极表,说的就是这副个头。括号里的 1497 是它家书里标到源码行的注记,3.1 引 MAXVA 的那回也带着一枚,照原样抄了下来。

咱们在同一页还找到一句,看它把补表的事怎么交代:

> 原话咱们整段请来:`If a PTE in a first or second level page directory page isn't valid, then the required directory page hasn't yet been allocated; if the alloc argument is set, walk allocates a new page-table page and puts its physical address in the PTE.`

它把找与建捏在了同一张脸上,alloc 的开关管补不补。咱们分了两副,FindSlot 管的只是找,EnsureLeafTable 带的是建:translate 与 unmap 走只读那副,心里不必背着要不要补表的念头。map 的两张脸走带建那副,要建的话就一路建齐。分家的道理跟 map 与 map_user 那对一样,各管各的一摊,读码的人看脸就知道心思。

它家 walk 的脚手架也有一句,咱们搬来看:

> 这句咱们也搬来:`The above code depends on physical memory being direct-mapped into the kernel virtual address space.`

取下一级表靠的是把物理内存直接映射进内核虚拟地址,咱们 KernelTables::table 的 :150 同款,DirectMapVirt 一加就得了,两家的走查踩在同一块跳板上。

TLB 的这头,书页 37 的上面也有一句,咱们抄来:

> 这句咱们原样复制过来:`The RISC-V has an instruction sfence.vma that flushes the current CPU's TLB.`

sfence.vma 的一条指令管作废,咱们摊成两张脸,单页的差事交 invlpg,全空间的差事交 ReloadCr3。险情它家也写明了,说的就是停一停里那句 scribble,小本本拿旧翻译够到已经易主的页。指令集是各家各叫的,作废的这门差事哪家都不敢欠。

Linux 的这一头,今天照样是不开口的。它家的 TLB 怎么养、几时刷、刷多宽,咱们翻不着能引的材料,这里就不强编了,这笔空白记在了纸面上。

轮到自家的债,2.3 欠的那笔到日子了,3.1 停一停里把这活指了名,说接到岗上去是本章的正戏,这儿咱们认下这茬。当时试的是低半区,递一枚没映射的址,岗拦得了高却拦不了空,原话是逐字验的细活,等第 3 部页表齐了再补。页表到这一章齐了,逐字验的器械也齐了,验空就是 translate 的本职:递一枚址下去,走查把表逐级查了下来,在的话,翻出的是物理址,不在的话,回的是 0,FindSlot 连断在了哪一级都肯报。岗上的代码今天还是老样子,还是拦得了高拦不了空,咱们一行都没动。器械认过了亲,换岗的手续归后头的章节办,这儿只把器械指给大伙看,日子就不预告了。

窄处咱们也交底三件。unmap 只认页表档的槽,大叶它是碰不得的,细不细大叶的事由调用方自己拿主意,眼下代码里还没有把大叶细成 4KiB 的手续。MapPage 遇上了大叶就回 false,一片 2MiB 要不要细的决断,得等策略出现了再定。PML4 档的大叶一律判非法回 0,凭的是机器的格式,门是 TranslatePage 替 FindSlot 把的。

C++ 在这一章出手的地方,值得单独看一眼的是 TableWorld。concept 把够格当世界的标准写在类型上,两张脸各自的返回型别都由 concept 看住,喂一个少脸的世界进来,编译期就翻了脸,轮不到等跑起来了才见分晓。档位与劈址也全立成了名字,WalkLevel 四档立了名,IndexRange 按档报的是段,头注里的那句 `no raw shifts, no bare level numbers`,说的就是这层家法,来历咱们在正文《一份走查,两个世界》里讲过了,咱们受用就好。

## 这一章留下了什么

散卷之前咱们把这一章添的家当挨件过一遍手。一副词汇:一枚址劈成了五段,39、30、21、12 各占九位的槽号,低 12 位的页内偏移,一张表的 512 项,一枚表项占的还是八字节,kPresent、kWritable、kUser、kLarge 的四枚开关,三档物理字段各守各的段。两副走法:FindSlot 走的是只读,断在了哪儿就报哪儿,EnsureLeafTable 带的是建:新表出生带 leaf_flags,大叶门口一律止了步。一记翻译:TranslatePage 读穿的是三档叶,偏移的宽度由档位自报,PML4 档的大叶判非法。一本小本本:表改了机器不自知,作废是调用方的差事,管单页的是 invlpg,管全空间的是 ReloadCr3。九案全数过了,长凳的六案,机器的三案。

| 术语 | 一句话口径 |
|---|---|
| 劈址 | 一枚 48 位址劈五段:39、30、21、12 各九位槽号,低 12 位页内偏移 |
| 大叶 | 中间两级翻起 kLarge 的项:项即叶,PDPT 档盖 1GiB,PD 档盖 2MiB |
| 两副走法 | FindSlot 只读,断在哪儿报哪儿,EnsureLeafTable 带建,大叶门口止步 |
| TLB | CPU 的翻译速查缓存,行话叫小本本:表改了它不自知 |
| 作废 | 调用方补的那一枪:invlpg 单页,ReloadCr3 全空间 |

走查会走了,翻译会翻了,作废那一枪也认了,收尾还压着两桩没答的:头一桩问的是,真有客人一脚踩空的话,接的人是谁,接住以后页的来路又在哪。另一桩问的是,这一章向名册领走的每一张表、每一枚页,来路的事还悬着,名册的一位管一页,管到了 4KiB 为止,内核里几十个字节的小件、堆里连绵的住户,页表伺候不了那么细。咱们把两桩都记到《缺页与按需分页》名下,那一章开了门来接。

## 练一练

咱们留三道题:一道动手的,一道猜想的,一道读屏的。

**题一请您动手。** 您自己编 test_vmm 跑一遍,六案的名单与 Results 对屏。再改一处重跑:compose 里填进去的 5 与 7,换成的是 3 与 4,重编了再跑,六案还都过吗?变的是什么,不变的又是什么?

**题二请您猜一猜。** TranslatePage 见到 PML4 档的大叶,回的是 0 而不是基址。您拿大叶只许停在 PDPT 与 PD 两档的章程推一推,机器为什么不把 PML4 档的 kLarge 也读成一片窗?

**题三请您读一读屏。** 机内三案的屏,您认认哪一案是停一停里推演的全程?0x114514 出现在案身哪一步?InvalidatePage 补在哪一行的下一行?第二案里翻回 0 的那枚址,凭什么必须是 0?

### 参考答案

题一:您换了数再跑,六案照旧都过了。要害在断言盯的是哪一头:0xABC000 与 0xABCABC 全在物理那头,槽号 5 与 7 变的是虚拟那一侧,而 map 挂页、translate 翻址,吃的恰是 compose 拼出的同一枚虚拟址,来回的两头一咬合,断言就跟着对上了。案五与案六的表是手摆的,压根是绕开 compose 摆的,数换了它们也不理。

题二:您翻翻各档的章程,翻牌盖多宽写明白了,PDPT 档的宽度是 1GiB,PD 档的宽度是 2MiB。PML4 上的牌一翻,盖多宽的说法没有,按一片窗的样子去读,宽度就只能靠猜了。走查的路数是宁可不翻也不猜,TranslatePage 把不合法的挡下回 0。

题三:您对着屏认,屏序的头一案,`a page lives in the heap window only while mapped`,就是推演的全程。0x114514 是 map 之后写进那枚页的值,读回的断言用它。InvalidatePage 补在 UnmapPage 的下一行,作废是当场补的。第二案翻回 0 的是堆基址 kHeapBase,咱们没给它挂过门牌,FindSlot 报的是空,translate 回的是 0。

## 延伸

- xv6 书第 3 章书页 36 与 37:walk 的模仿句、九位句、alloc 开关句、直接映射依赖句,前四句在 36。sfence 句与 scribble 险情句翻到 37,scribble 那段的前半句还压在 36 的末行,您对原书核。
- 正文《内存当家:一页一页地精修》一卷:走查降形的全本在《一份走查,两个世界》,作废的两档与两处自埋的失误在《unmap 欠的那一下》,直接映射铺窗与旧表退役的那一篇有 ReloadCr3 的头一回出场。教科书层把它们收拢成一副读法,您要现场,那一卷的门开着。
- 正文《长模式:在 32 位的世界里搭一座 64 位的桥》一卷:boot 三张表怎么填,《把三张表填起来》一篇有全本,2MiB 大叶当年就上岗,1.4 教过填法,您两边对读正好。
