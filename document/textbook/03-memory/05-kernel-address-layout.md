---
title: 05 · 内核地址布局
---

# 05 · 内核地址布局:五扇窗,三条换算,五条约定

借名字的日子,到这一章该结一结了。直接映射窗这个名字是 3.1 走查取表页那会儿头一回借的,3.2 又借了一路。堆窗的名字,咱们 3.4 开张那天借的。天花板与那十二条互锁的叫法,3.1 量客人天空的时候借过。这些名字其实全出自同一家:kernel/mm/layout.hpp,一份 168 行的头文件。上一章收卷的时候,咱们把话留在了明处:layout.hpp 的图,名字是借了一路的,全本是留到今天才铺开的。摊开以后您会看见,整幅图管的地界,比借过的那几处大得多:五扇窗一家一项,连没住人的窗都立了名。

这一章的活,咱们数下来三样。头一样认的是窗:虚拟地址空间怎么按 PML4 的项分家,五扇窗各住的是谁,高半的门牌打哪来。再一样认的是换算:哪条换算是一加一减就成的,哪扇窗的门牌减不回物理,分两半的那道线画在哪个数上。末一样认的是约定:两扇没开张的窗凭什么留名,用户半边的五条约定全本报名,保护页压住的是哪个数。

量法还是照旧的三条。码的事,主角咱们请的是 layout.hpp,168 行是全本通读下来的,案身 `test/unit/test_mm_layout.cpp` 也逐枚过了一遍,断言是一枚一枚对着常量核的。屏的事,长凳备的是一份,宿主测试是直跑直抄的,机内的证据 3.2 那三案拍过,咱们只回指不重刊。书的事,xv6 翻到的是书页 36,讲内核页表映了些什么的原话,整段咱们借来摆在对照节,您对着书核。还有一句量法的老实话搁在前头:`test/unit/` 里另有一件 test_layout,四案守的是 boot 侧的图,那是 1.8 的领地,本章就不进了,长凳上跑的只有 mm_layout 这一件。

> **一份 168 行的头文件,凭什么当得起全内核的地图?**
>
> 凭头注自己报的身份。layout.hpp 的头注,笔者写下的是 `The published virtual address space map of the kernel`,发布出来的内核虚拟地址图。发布两个字是有份量的,底下那层意思咱们译过来:内存子系统递出去的每一个地址,来路都回溯到头文件里的一枚常量。文件尾巴上的 static_assert 把每家基址跟自家的 PML4 项号互锁,图与硬件的算术谁也别想走岔。公告头这个名字是正文《内存当家:一页一页地精修》那一卷叫开的,咱们接着叫。这一章咱们挨扇开窗。

## 五扇窗,各占一项

分家的刻度,咱们从一项有多大认起。3.2 劈址的那天数过,PML4 的槽号住在址的第 39 位往上,剩下的 39 位压在项号底下,归的是底下三级表和页内偏移。所以一枚 PML4 项罩住的地,是 2 的 39 次方字节,layout.hpp 的 :31 给它立下的名就是 kPml4EntrySpan,折成平常的单位是 512GiB,:147 的断言原话 `one PML4 entry spans 2^39 bytes`,数对不上的话,编译都过不去了。

地图按项分家,一家占的是一整项,谁也不跟谁借地。五扇窗的项号与门牌,咱们摆在一块看:

```text
PML4[256]  0xFFFF800000000000  直接映射窗
PML4[257]  0xFFFF808000000000  内核堆窗
PML4[258]  0xFFFF810000000000  vmalloc(空窗)
PML4[259]  0xFFFF818000000000  ioremap(空窗,一期镜像顶班)
PML4[511]  0xFFFFFFFF80000000  内核镜像与模块窗
```

四扇是连号的:256 到 259,一扇比一扇高出一个项的跨度。末一扇 511 孤悬在整张地图的最顶上。立在 :45 到 :51 的基址有四枚:kDirectMapBase、kHeapBase、kVmallocBase、kIoremapBase,3.4 亮过的 0xFFFF808000000000 就住在第二扇。次序不是人眼看着齐就算的,咱们请四条断言出来说话:

```cpp
static_assert(kHeapBase == kDirectMapBase + kPml4EntrySpan, "heap owns the next entry");
static_assert(kVmallocBase == kHeapBase + kPml4EntrySpan, "vmalloc owns the next entry");
static_assert(kIoremapBase == kVmallocBase + kPml4EntrySpan, "ioremap owns the next entry");
static_assert(kIoremapBase + kPml4EntrySpan <= 0xFFFFFF8000000000UL,
              "reserved entries stop before the kernel image entry");
```

:150 到 :154 的四条断言,管的就是次序:堆窗的邻居是直接映射,vmalloc 的邻居是堆,ioremap 的邻居是 vmalloc,四扇连号的窗顶还得停在镜像项的门槛外头。您数窗号就能数出门槛,谁想挪一家的话,那编译期就开口了。

511 号项的里头还分着家::54 到 :56 写的是 kKernelImagePdpt 510 与 kModulePdpt 511,项门里头装的是 PDPT 两格,镜像住的是前格,模块住的是后格。门牌立在 :59 与 :61:kKernelImageBase 的 0xFFFFFFFF80000000,kModuleBase 的 0xFFFFFFFFC0000000,断言把两格的关系也压了,模块窗紧跟的是镜像窗,中间隔的是整 1GiB(:160)。堆窗的注还捎了一句脾气,咱们照注直译,`grows upward inside its entry`,说的就是窗里向上长,项就是它的顶,向上长的这件事,3.4 开堆的那天拍过全本。

镜像窗是老资格的门。正文《第一个内核:把执行流递到它手里》那一卷的《两扇门,和最后一跳》讲过来历:boot 把 PML4 的第 0 项与第 511 项指向同一张 PDPT,高半的别名恒等于物理址,内核取指用的正是 0xFFFFFFFF80200000,走的也是镜像窗。2.1 的换算您还记得,g_user_shell_start 报出的是 0xffffffff8020a000,咱们减掉 kKernelImageBase,得的是物理址 0x20a000,本尊住的地方照旧在镜像里,肉身落点 0x200000 是 1.8 的旧课,这里就不重算了。正文《内存当家》那一卷接管的那天,第 0 项整项清了零,留任的是 511,门还是原来的门,章法从此归地图管,3.2 机内的第三案验收过,低半的 256 项逐项验零,剩下的一项都没有。

## 三条换算,一条分界线

地图认完,轮到地图上办事的手。换算的是三张脸,问址的是两枚,全住 layout.hpp 的中段,咱们一张一张请。

咱们把头两张脸摆在一处看,请的是 DirectMapVirt 与 DirectMapPhys(:84 到 :86 与 :99 到 :101):

```cpp
constexpr unsigned long DirectMapVirt(unsigned long physical) {
    return kDirectMapBase + physical;
}

constexpr unsigned long DirectMapPhys(unsigned long virtual_address) {
    return virtual_address - kDirectMapBase;
}
```

两张身子各占的是一行,一张管的是一个方向。物理址进了门,直接映射窗里的门牌出门,门牌减掉了底数,物理址就还原了,咱们一去一回各走一趟。一加一减凑成的一对往返,长凳第二案验的就是它:

```cpp
ASSERT_TRUE(DirectMapVirt(0) == kDirectMapBase);
ASSERT_TRUE(DirectMapPhys(DirectMapVirt(0x123456)) == 0x123456);
```

物理 0 递了进去,出来的正是窗基。挂上了门牌再减回来,0x123456 是分毫不差的。3.2 的走查拿 table() 换表页,KernelTables 供表用的就是头一张脸,那儿咱们当跳板认过,今天才算把头一张脸的正身看全。

这对脸的注里藏着一条章程,值得咱们单独立一段。注写了两层意思,头一层写的是 `no fixed offset`,另一层写的是 `only the direct map converts by subtraction`,您把两层合起来念:堆、vmalloc、镜像的门牌,都是没有固定偏移的,能一减还原的,全地址空间只有直接映射窗自己。为什么偏偏是它,停一停里咱们推。

问址的两枚,咱们挨枚认。头一枚咱们认 IsDirectMap(:114 到 :116),问一枚址落没落进直接映射的项里。注里的半句老实话,`the VMM's question, not the layout's`,说的是它认的只是项,窗里的 RAM 真铺到多高,管它的是 VMM,不归地图,铺窗的活是接管那天照 e820 干的,正文的那一卷拍过全本。长凳的第二案把它探得仔细:kHeapBase 减一在窗里,答的是 true。一脚跨到 kHeapBase 的时候就出了项,答的是 false。咱们再递一个 0,答的还是 false。项的边界是一板一眼的。

另一枚就是分界线的本尊,咱们请 IsKernelHalf(:127 到 :129)出来:

```cpp
constexpr bool IsKernelHalf(unsigned long virtual_address) {
    return virtual_address >= kDirectMapBase;
}
```

您把 0xFFFF800000000000 摊开看:3.1 讲过,规范地址得让第 47 位往上的位跟第 47 位一个样,高半的规范门牌打 0xFFFF800000000000 才起头,这道线恰好压在规范高半的头一枚门槛上。线以上的半边归内核,线以下到零的半边归客人。长凳的第三案递来五枚址验它,配的是三真两伪,直接映射、ioremap、模块的三枚基址都落在高半,kUserTop 减一与 0 落的都是低半。

换算的末一张脸 IoremapVirt(:143 到 :145),咱们也请出来:

```cpp
constexpr unsigned long IoremapVirt(unsigned long physical) {
    return kIoremapBase + physical;
}
```

咱们看它的身子,还是一行的,设备的物理址递进去,ioremap 窗里的门牌出来。加法走的是直接映射的同款,身份算的是另一回事,窗的章法下一节认。

**咱们停一停,给堆窗也配一条减法公式的话,这活您接得下吗?**

咱们把推演摆开。接得下接不下:咱们不看算式编不编得出来,看的是窗里每一枚门牌守不守同一条约。直接映射窗守的是一条约:门牌等于底数加物理址,全窗共用的底数只有一枚,挂着 0x200000 的门牌,底下压的就是物理 0x200000,减回去就稳稳还原了。堆窗守的是另一条章程:哪枚门牌底下压哪页物理,走查挂页的那一刻才定,这笔记在了表上。今天 kHeapBase 往上两页底下压的,跟下一回分配之后压的,就未必是同一页了,固定偏移根本就是不存在的。注里的那句 no fixed offset,跟偷懒是不相干的,它讲的是这扇窗发门牌的路数:门牌归分配器发,底下的物理归表记,想从门牌换回物理的话,查的就是 translate,老相识了,咱们 3.2 就请它出来过。3.2 机内头一案您见过同款,unmap 完了,translate 当场翻回的就是 0。

## 空着的窗,与用户半边的界碑

咱们看五扇窗里,没进住户的正是 vmalloc 那扇。ioremap 那扇的正经登记簿没开张,檐下只有一位顶班的。空窗这个词是咱们给这样的窗立的名:项号与基址都挂在地图上,正经的住户还没到任。

vmalloc 的注,咱们念一遍就明白,`no allocator serves it yet`,眼下是没有分配器伺候它的。常量立了,窗是留着的,连差事都写明了,vmalloc 与内核线程栈(:37 的注)。造分配器的事,由用得上它的那天说话,本章就不动工了。

ioremap 檐下的临时工,您认得的,说的就是相位一。这个名字正文《内存当家》那一卷叫开:登记设备区、发放追踪过的映射,正经的差事得有分配器伺候,伺候的人没到,拿物理偏移原样照搬的镜像顶班,IoremapVirt 一加就是临时工的全部手艺。帧缓冲是这里的老户:0xFD000000,接管那天从低半边的旧门搬进 259 号项,屏幕的字从那天起走新门牌。3.1 那一抄把高半整副搬进了每个世界,设备的窗也跟着进了客人的世界,正文《下楼开张》那一卷的《客人的世界》里点过这层。长凳的第四案当面验它,案身放在了实况节。

模块窗的住处,注里也写了缘故。kModuleBase 的注是 `the -mcmodel=kernel link keeps code in the last 2 GiB`,内核的代码模型把代码按在地址空间最末 2GiB 里,链接器认的只是这个射程,地图顺着它画:镜像窗占的是 1GiB,模块窗占的是 1GiB,高半的顶恰好用满。编译器那头的细节,注说到了哪儿,咱们就讲到哪儿,再多的就不往下铺了。

末了是用户半边。PML4 的 0 到 255 项归客人,地图在这半边只立约定,是不盖房的。五条约定咱们连注带码整个请出来(:63 到 :72):

```cpp
/** @brief Highest user-visible address plus one; PML4 entries 0..255 end here. */
inline constexpr unsigned long kUserTop          = 0x0000800000000000UL;
/** @brief Null-trap guard: user mappings never start below this address. */
inline constexpr unsigned long kUserNullTop      = 64_KiB;
/** @brief Default boundary between the program-plus-brk area and the mmap area. */
inline constexpr unsigned long kUserBrkMmapSplit = 0x0000400000000000UL;
/** @brief Default top of the mmap area; allocations grow down from here. */
inline constexpr unsigned long kUserMmapTop      = 0x00007F8000000000UL;
/** @brief Upper edge of the default main stack; the page above stays unmapped. */
inline constexpr unsigned long kUserStackTop     = kUserTop - cinux::arch::page::kSize;
```

五条咱们挨条报名。kUserTop 报的是 0x0000800000000000,客人的天花板,3.1 是量过的,岗上的限与图上的线同数。kUserNullTop 定的是 64KiB,空指针的禁区,3.1 也是认过的,客人解引用了没立起来的指针,踩的是这块注定没映射的地。余下的三条,分界线在缺页那一课的 brk 一节点过名,这回补全的是队伍:kUserBrkMmapSplit 0x0000400000000000,程序加 brk 的地界与 mmap 区的分界,恰是天花板的折半。kUserMmapTop 报的是 0x00007F8000000000,mmap 区的顶,注里写明了走向,`allocations grow down from here`,门牌是从顶往下发的。kUserStackTop 定在了 kUserTop 减一页,算出来的是 0x00007FFFFFFFF000,默认的主栈到这儿为止,上方那一页留在地图外。这儿有一处跟正文对不上的地方,咱们得当面认:正文《内存当家》那一卷把 mmap 区说成从分界线往上长,码上的注写的走向正相反,数与走向的事,咱们都以码为准。五条的队,断言也排好了,押在了文件尾巴上:

```cpp
static_assert(kUserStackTop + cinux::arch::page::kSize == kUserTop,
              "one guard page separates the stack top from user top");
static_assert(kUserNullTop < kUserBrkMmapSplit && kUserBrkMmapSplit < kUserMmapTop &&
                  kUserMmapTop < kUserStackTop,
              "user conventions stay ordered inside the user half");
```

咱们再看 :162 到 :166 的两条。头一条您一眼认得出,kUserStackTop 加上一页的话,恰好回到了 kUserTop,一页保护页隔开的是栈顶跟天花板,越界的那一脚踩空,踩的是没映射的地。这门手艺不是新的,3.4 给堆窗的脚下安过一页,署的是《页进,块出》的名,这儿换到客人的栈顶再使一回。第二条把五条的约定排成一列,空指针的禁区小于分界,分界小于 mmap 的顶,mmap 的顶小于栈顶,约定与约定的次序,谁也越不过谁。

**咱们停一停,五扇窗加起来占地几何,这道加法当场折?**

咱们折:一项 512GiB,连号的四扇在 256 到 259,511 的一扇孤悬在顶,咱们拿五乘 512GiB,得出来的是 2560GiB,折成 TiB 的话,得的是 2.5TiB。3.1 量过的整个高半拢共 128TiB,公告头发布出来的只有 2.5TiB。客人的半边同样 128TiB,占住的项一项也没有,运行期前的 256 项全零,3.2 机内的第三案逐项验过。您可别小看空着的地:项号本身就是地图的一部分,511 号项的里头 PDPT 还分着两格,空窗的名与界也是发布出来的内容。数到末了,两笔并在一起:发出来的 2.5TiB,落在 3.1 量过的 128TiB 高半里,余下的全是空地,空得清清楚楚的,一笔一笔咱们都翻得到。

## Code: Cinux 实况:长凳六案,机内回指

锚还是用的 r13_userspace,缺页那一课换了 r19_lazy_alloc_page,前头其余各章用的也是它。这一章咱们连机器都不用起:地图是常量,咱们用长凳直跑就能对实。面板也还是 3.1 抄过的四行,咱们本章一行不动。机内的活证据,3.2 的 ktest 三案拍过了,镜像与直接映射翻得回物理、没人动过的堆基址翻回 0、低半 256 项逐项验零,三案咱们回指,屏就不重刊了。长凳的构建照 3.4 的口径,默认档备的是一份目录:

```bash
cmake -B build-dbg
cmake --build build-dbg --target test_mm_layout
build-dbg/test/test_mm_layout
```

屏的头尾,咱们全抄:

```text

=== Cinux Test Runner ===
Running 6 case(s)...

[PASS] mm layout: kernel regions are disjoint and ordered
[PASS] mm layout: direct map conversion round trips
[PASS] mm layout: half classification
[PASS] mm layout: ioremap phase one mirrors physical offsets
[PASS] mm layout: image and module windows sit in the last entry
[PASS] mm layout: user half conventions

=== Results: 6 passed, 0 failed ===
```

六案咱们逐案认,案名把地图念了一遍。头一案讲的是 `kernel regions are disjoint and ordered`,验的是五扇窗互不重叠、次序不乱,案身五枚断言把五家基址从低到高比了一遍,连 ioremap 的顶不得超过镜像项底也压在里头。第二案讲的是 `direct map conversion round trips`,停一停里推的往返,加得去就减得回了,IsDirectMap 的项边界也在案里探了。第三案讲的是 `half classification`,分界线的五枚真伪。第四案讲的是 `phase one mirrors physical offsets`,临时工的手艺当面验,案身挑的是三行:

```cpp
ASSERT_TRUE(IoremapVirt(0xFD000000) == kIoremapBase + 0xFD000000);
ASSERT_TRUE(IoremapVirt(0) == kIoremapBase);
ASSERT_TRUE(!IsDirectMap(IoremapVirt(0xFD000000)));
```

您看帧缓冲的门牌,算的是窗基加 0xFD000000,窗基本人递了进去,翻回来的还是它自己,末一行再把设备的门牌从直接映射窗里撵出去,三行把临时工的手艺验了个遍。第五案讲的是 `image and module windows sit in the last entry`,511 号项里两格的位置,案身挑要紧的几行:

```cpp
ASSERT_TRUE(kModuleBase - kKernelImageBase == 0x40000000UL);
ASSERT_TRUE(kKernelImageBase + 0x200000 == 0xFFFFFFFF80200000UL);
```

0x40000000 折成的是 1GiB,咱们认得出,这是镜像窗与模块窗中间的隔断。kKernelImageBase 加上 2MiB 的话,得的是 0xFFFFFFFF80200000,1.8 世界观的原点,注里的 `the image itself still links at +2 MiB` 那句,案里对上了数。第六案讲的是 `user half conventions`,验的是五条约定逐条对数,案身挑的是头尾三行:

```cpp
ASSERT_TRUE(kUserTop == 0x0000800000000000UL);
ASSERT_TRUE(kUserStackTop == 0x00007FFFFFFFF000UL);
ASSERT_TRUE(kUserStackTop % 0x1000 == 0);
```

头两行对的是天花板与栈顶两枚数,跟 layout.hpp 是分毫不差的。您再瞧末一行,栈顶对 0x1000 取余得的是零,保护页的门槛也得踩在整页上,数与格式的事,两道岗是一起站的。

## 对照:同一件事,别的机器怎么做

咱们把 xv6 翻到书页 36,kvmmake 所在的那段,两句原话咱们各摆一行:

- “kvmmake first allocates a page of physical memory to hold the root page-table page. Then it calls kvmmap to install the translations that the kernel needs.”
- “The translations include the kernel’s instructions and data, physical memory up to PHYSTOP, and memory ranges which are actually devices.”

它家内核映了些什么,第二句报了单:自家的指令与数据,PHYSTOP 以下的物理内存,PHYSTOP 是它家物理内存的顶,还有真设备的地界。可单子是没有公告头的,映什么、映到哪,kvmmake 里一张 kvmmap 清单说了算,调用一回就挂了一段,图全长在了通话记录里,纸上是没有全张的。咱们把 layout.hpp 摆在它旁边:五扇窗、五条约定、十二条互锁,一份头文件发布的是全本,长凳的六案背书。同一张内核映射:它家记在了通话里,咱们贴在墙上。您想知道咱们的内核映了什么,咱们不用翻调用,翻的是一份头文件。

没到任的住户,它家那头也没有对得上的条目。设备的地界,它家的 kvmmap 现挂现用,表里没有给设备预留的整项。咱们把预留做成了地图上的正式条目:vmalloc 一户没进,名与界是全在的,ioremap 檐下的临时工也领着公告头发的基址。留名不为好看:项号占下了,后来的住户进门就有号,基址、跨度、跟邻居的次序,全在公告头上写明白了。

轮到 Linux:这一章咱们不上色。它家的虚拟地址布局散在几份文档里,咱们手边一份能引的都没有,凭印象描图的话,错一条线是比空着还糟的,所以整格留白,材料到齐的那天,咱们提笔补色。

自家的窄处咱们也交底两件。一件是问窗的脸只长了一枚:IsDirectMap 只伺候直接映射窗,址落进了堆窗还是 vmalloc 窗,layout.hpp 是没给配脸的,真要认的话,拿各家的基址自己比。另一件咱们看两扇窗眼下的登记簿:vmalloc 无分配器,ioremap 是临时工顶的班,窗里哪段被谁占了,地图不记,记是记在将来的章程里的。

C++ 在这一章出手的地方,就是文件尾巴上的那十二条 static_assert。本章细读了八条:项的跨度一条(:147),管次序的四条(:150 到 :154),管模块紧跟的一条(:160),管保护页与用户半边次序的两条(:162 到 :166),余下的四条里,天花板的断言 3.1 逐字点过,另三条是本章的头一回照面。互锁的脾气,正文《内存当家》的那一卷领教过一回:写镜像与模块那两枚断言的时候,符号扩展的底子没铺,左移出来的数高 16 位是零,跟高 16 位全 1 的门牌天生不等,两条断言编译当场就红了,开机都不用等了。您今天读到的版本,底子上铺的是 0xFFFF000000000000,那回红出来的教训,如今垫在每一枚高半基址的断言底下。

## 这一章留下了什么

收卷的时候,咱们把留下的家当数一遍。头一件数出的是一张图:五扇窗各占一项,256 到 259 是连号的,镜像与模块同住的是 511 号项里 PDPT 两格,每家的基址都有断言押着项号。第二件认的是一记换算:直接映射一加一减,IoremapVirt 干的是一期镜像,IsKernelHalf 一道线压在规范高半的门槛上,堆窗与镜像窗的门牌,减法是不伺候的。第三件记的是五条约定:空指针禁区、天花板、brk 分界、mmap 顶、栈顶保护页,次序也是有断言押队的。末一件跑的是六案:长凳全过,机内的证据 3.2 在案。

| 术语 | 一句话口径 |
|---|---|
| 公告头 | layout.hpp 的身份:内核虚拟地图全本发布在一份头文件里,正文《内存当家》叫开的名 |
| 项与窗 | 一枚 PML4 项罩 512GiB:窗按项分家,五扇窗占 256、257、258、259、511 |
| 直接映射 | 门牌等于底数加物理址:一加一减,全地址空间只有这扇窗吃减法 |
| 相位一 | ioremap 的一期:物理偏移原样镜像,帧缓冲是檐下的老户 |
| 空窗 | 立了项、发了基址、正经住户没到任的窗:vmalloc 全空,ioremap 临时工顶班 |
| 保护页 | kUserStackTop 上方不映射的一页:栈顶与天花板之间隔一页踩空的地 |

地图铺平了,咱们盘一盘第 3 部的家底,还欠着的是最后一课:《Page Cache 与写回》。地图管的是发门牌,走查记的是翻译,缺页那一课把没映的页补上,文件页要用到的那一拍,字节是从一座柜子里递出来的,柜子当时是没摊开的。柜子几格、凭什么敢把写扣在内存里、还的时辰定在哪,咱们到那一课里讲。

## 练一练

咱们留三道题:动手的一道,猜想的一道,读屏的一道。

**题一咱们动手。** 长凳这一件您自己编自己跑:六案名单与 Results 对屏。抄屏的时候认一认:哪一案守五窗次序,哪一案当面验临时工?

**题二咱们猜一猜。** 哪天手滑的话,把 layout.hpp 里 kHeapBase 的值改成了 kVmallocBase 的数,编译器喊不喊?长凳跑得起来吗?您对着 :150 那枚断言与长凳头一案推一推,咱们再对参考答案。

**题三咱们读屏。** 第五案的那两行断言,头一行报的是 0x40000000,末一行报的是 0xFFFFFFFF80200000,您说说各验的什么?第六案里 kUserStackTop 的两枚断言,枚枚管的哪桩?

### 参考答案

题一:您跑下来,Results 该写的是 `6 passed, 0 failed`。守次序的是头一案 `kernel regions are disjoint and ordered`,验临时工的是第四案 `ioremap phase one mirrors physical offsets`,案名跟屏是一字不差的。

题二:您把 kHeapBase 改成 vmalloc 的基址,:150 的那枚断言当场就喊,喊的是 kHeapBase 得等于 kDirectMapBase 加一项跨度,等式是不成立的,编译就停了,机器是根本起不来的。长凳的头一案也护着一道,护的也是 kHeapBase 得小于 kVmallocBase,可它是轮不到出场的,编译期的那道岗排在头里。咱们算下来是两道岗:编译期的一道,运行期的一道,地图上的数走的是头一道。

题三:您对下来,0x40000000 验的是镜像窗与模块窗的间隔,整的是 1GiB,两格的界压在断言里。0xFFFFFFFF80200000 验的是镜像的落点,kKernelImageBase 加的是 2MiB,跟 :58 注里的 `the image itself still links at +2 MiB` 对得上数。第六案的头一枚断言对的是数,kUserStackTop 对的数,落在了 0x00007FFFFFFFF000 上。第二枚验的是对齐,它对 0x1000 取余得的是零,保护页的门槛踩在整页上。

## 延伸

- xv6 书第 3 章书页 36:kvmmake 与 kvmmap 的两句原话都在这一页,它家内核映什么、由谁说了算,您对原书核。
- 正文《内存当家:一页一页地精修》一卷:《挂在墙上的地图》是地图叙事的全本,公告头、用户半边五约定、互锁首版被抓的旧事都在里面。帧缓冲搬家与相位一,接管那一篇拍的全本。您要现场,卷门开着。
- 正文《第一个内核:把执行流递到它手里》一卷:《两扇门,和最后一跳》讲高半门牌的来历,第 0 项与第 511 项同表、别名恒等,本章把它收进了全图,您两边对读正好。
- 正文《下楼开张:ring3、syscall 与第一位常客》一卷:客人的世界抄整副高半,259 号设备的窗跟着进门,《客人的世界》里点过这层,您翻得到。
