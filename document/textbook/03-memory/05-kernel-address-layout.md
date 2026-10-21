---
title: 05 · 内核地址布局
---

# 05 · 内核地址布局:五扇窗,三条换算,五条约定

借名字的日子,到这一章该结一结了。直接映射窗这个名字,3.1 走查取表页那会儿头一回借,3.2 又借了一路。堆窗,3.4 开张那天借过。天花板与那十二条互锁,3.1 量客人天空时借过。名字全出自同一家:kernel/mm/layout.hpp,一份 168 行的头文件。上一章收卷把话留在明处,layout.hpp 的图咱们借了一路的名字,全本没摊开,今天铺开。摊开以后您会看见,整幅图管的地界,比借过的那几处大得多:五扇窗,一家一项,连没住人的窗都立了名。

这一章的活,咱们数下来三样。头一样,认窗:虚拟地址空间怎么按 PML4 的项分家,五扇窗各住谁,高半的门牌打哪来。再一样,认换算:哪条换算一加一减就成,哪扇窗的门牌减不回物理,分两半的那道线画在哪个数上。末一样,认约定:两扇没开张的窗凭什么留名,用户半边五条约定全本报名,保护页压在哪个数上。

量法照旧三条。码,主角 layout.hpp,168 行全本通读,案身 `test/unit/test_mm_layout.cpp` 也逐枚过,断言一枚一枚对着常量核。屏,长凳一份,宿主测试直跑直抄,机内的证据 3.2 那三案拍过,本章回指不重刊。书,xv6 翻到书页 36,讲内核页表映了些什么的原话,整段借来摆在对照节,您对着书核。还有一句量法的老实话搁在前头:`test/unit/` 里另有一件 test_layout,四案守的是 boot 侧的图,1.8 的领地,本章不进,长凳上只跑 mm_layout 这一件。

> **一份 168 行的头文件,凭什么当得起全内核的地图?**
>
> 凭头注自己报的身份。layout.hpp 的头注,笔者写下的是 `The published virtual address space map of the kernel`,发布出来的内核虚拟地址图。发布两个字有份量,底下那层意思咱们译过来:内存子系统递出去的每一个地址,来路都回溯到头文件里的一枚常量。文件尾巴上的 static_assert 把每家基址跟自家的 PML4 项号互锁,图与硬件的算术谁也别想走岔。公告头这个名字,正文《内存当家:一页一页地精修》那一卷叫开的,咱们接着叫。这一章,咱们挨扇开窗。

## 五扇窗,各占一项

分家的刻度,咱们从一项有多大认起。3.2 劈址那天数过,PML4 的槽号住在址的第 39 位往上,项号底下还压着 39 位,归底下三级表和页内偏移。所以一枚 PML4 项罩住的地,是 2 的 39 次方字节,layout.hpp 的 :31 给它立了名,kPml4EntrySpan,折成平常的单位是 512GiB,:147 的断言原话 `one PML4 entry spans 2^39 bytes`,数对不上,编译都过不去。

地图按项分家,一家一整项,谁也不跟谁借地。五扇窗的项号与门牌,咱们摆在一块看:

```text
PML4[256]  0xFFFF800000000000  直接映射窗
PML4[257]  0xFFFF808000000000  内核堆窗
PML4[258]  0xFFFF810000000000  vmalloc(空窗)
PML4[259]  0xFFFF818000000000  ioremap(空窗,一期镜像顶班)
PML4[511]  0xFFFFFFFF80000000  内核镜像与模块窗
```

四扇连号,256 到 259,一扇比一扇高出一个项的跨度。末一扇 511,孤悬在最顶上。基址全立在 :45 到 :51,kDirectMapBase、kHeapBase、kVmallocBase、kIoremapBase,3.4 亮过的 0xFFFF808000000000 就住在第二扇。次序不是人眼看着齐就算,咱们请四条断言出来说话:

```cpp
static_assert(kHeapBase == kDirectMapBase + kPml4EntrySpan, "heap owns the next entry");
static_assert(kVmallocBase == kHeapBase + kPml4EntrySpan, "vmalloc owns the next entry");
static_assert(kIoremapBase == kVmallocBase + kPml4EntrySpan, "ioremap owns the next entry");
static_assert(kIoremapBase + kPml4EntrySpan <= 0xFFFFFF8000000000UL,
              "reserved entries stop before the kernel image entry");
```

:150 到 :154,堆窗得挨着直接映射,vmalloc 得挨着堆,ioremap 得挨着 vmalloc,四扇连号的窗顶还得停在镜像项的门槛外头。您数窗号就能数出门槛,谁想挪一家,编译期就开口。

511 号项里还分着家。:54 到 :56,kKernelImagePdpt 510,kModulePdpt 511,项门里头 PDPT 两格,镜像住前格,模块住后格。门牌在 :59 与 :61,kKernelImageBase 0xFFFFFFFF80000000,kModuleBase 0xFFFFFFFFC0000000,断言把两格的关系也压了,模块窗紧跟镜像窗,中间隔整 1GiB(:160)。堆窗的注还捎了一句脾气,咱们照注直译,`grows upward inside its entry`,窗里向上长,项就是它的顶,向上长这件事,3.4 开堆那天拍过全本。

镜像窗是老门。正文《第一个内核:把执行流递到它手里》那一卷的《两扇门,和最后一跳》讲过来历:boot 把 PML4 的第 0 项与第 511 项指向同一张 PDPT,高半别名恒等于物理址,内核从 0xFFFFFFFF80200000 取指,走的也是这扇门。2.1 的换算您还记得,g_user_shell_start 报出 0xffffffff8020a000,减掉 kKernelImageBase,物理址 0x20a000,本尊照旧住在镜像里,肉身落点 0x200000 是 1.8 的旧课,不重算。正文《内存当家》那一卷接管那天,第 0 项整项清零,511 留任,门还是那扇,章法从此归地图管,3.2 机内第三案验收过,低半 256 项逐项验零,一项不剩。

## 三条换算,一条分界线

地图认完,轮到地图上办事的手。换算三张脸,问址两枚,全住 layout.hpp 的中段,咱们一张一张请。

咱们把头两张脸摆在一处看,DirectMapVirt 与 DirectMapPhys(:84 到 :86 与 :99 到 :101):

```cpp
constexpr unsigned long DirectMapVirt(unsigned long physical) {
    return kDirectMapBase + physical;
}

constexpr unsigned long DirectMapPhys(unsigned long virtual_address) {
    return virtual_address - kDirectMapBase;
}
```

两张身子各一行,一张管一个方向。物理址进门,直接映射窗里的门牌出门,门牌减掉底数,物理址还原,咱们一去一回各走一趟。一加一减,一对往返,长凳第二案验的就是它:

```cpp
ASSERT_TRUE(DirectMapVirt(0) == kDirectMapBase);
ASSERT_TRUE(DirectMapPhys(DirectMapVirt(0x123456)) == 0x123456);
```

物理 0 递进去,出来正是窗基。挂上门牌再减回来,0x123456 分毫不差。3.2 的走查拿 table() 换表页,KernelTables 供表用的就是头一张脸,那儿咱们当跳板认过,今天才算把头一张脸的正身看全。

这对脸的注里藏着一条章程,值得单独立一段。注写了两层意思,一层是 `no fixed offset`,另一层是 `only the direct map converts by subtraction`,合起来念,堆、vmalloc、镜像的门牌,都没有固定偏移,能一减还原的,全地址空间只有直接映射窗自己。为什么偏偏是它,停一停里咱们推。

问址的两枚,咱们挨枚认。头一枚 IsDirectMap(:114 到 :116),问一枚址落没落进直接映射的项里。注里有半句老实话,`the VMM's question, not the layout's`,它只认项,窗里 RAM 真铺到多高,归 VMM 管,不归地图,铺窗的活是接管那天照 e820 干的,正文那一卷拍过全本。长凳第二案把它探得仔细:kHeapBase 减一在窗里,true。一脚到 kHeapBase,出了项,false。再递一个 0,还是 false。项的边界,一板一眼。

另一枚是分界线本尊,咱们请 IsKernelHalf(:127 到 :129)出来:

```cpp
constexpr bool IsKernelHalf(unsigned long virtual_address) {
    return virtual_address >= kDirectMapBase;
}
```

您把 0xFFFF800000000000 摊开看:3.1 讲过,规范地址得让第 47 位往上的位跟第 47 位一个样,高半的规范门牌打 0xFFFF800000000000 才起头,这道线恰好压在规范高半的头一枚门槛上。线以上,内核的半边。线以下到零,客人的半边。长凳第三案递来五枚址验它,三真两伪,直接映射、ioremap、模块三枚基址都落在高半,kUserTop 减一与 0,都在低半。

换算的末一张脸 IoremapVirt(:143 到 :145),咱们也请出来:

```cpp
constexpr unsigned long IoremapVirt(unsigned long physical) {
    return kIoremapBase + physical;
}
```

咱们看它的身子,还是一行,设备的物理址递进去,ioremap 窗里的门牌出来。加法跟直接映射一个模样,身份是两回事,窗的章法下一节认。

**停一停,给堆窗也配一条减法公式,这活您接得下吗?**

咱们把推演摆开。接得下接不下,不看算式编不编得出来,看的是窗里每一枚门牌守不守同一条约。直接映射窗守:门牌等于底数加物理址,底数全窗只有一枚,挂着 0x200000 的门牌,底下就是物理 0x200000,减回去稳稳还原。堆窗不守:哪枚门牌底下压哪页物理,走查挂页那一刻才定,表上记着。今天 kHeapBase 往上两页底下压的,跟下一回分配之后压的,未必是同一页,固定偏移根本不存在。注里那句 no fixed offset,跟偷懒不相干,它讲的是这扇窗发门牌的路数:门牌归分配器发,底下的物理归表记,想从门牌换回物理,查 translate,老相识了,3.2 请它出来过。3.2 机内头一案您见过同款,unmap 完,translate 当场翻回 0。

## 空着的窗,与用户半边的界碑

五扇窗里,vmalloc 那扇一户没进。ioremap 那扇,正经的登记簿没开张,檐下只有一位顶班的。空窗这个词,咱们给这样的窗立的名:项号与基址都挂在地图上,正经住户还没到任。

vmalloc 的注,咱们念一遍就明白,`no allocator serves it yet`,眼下没有分配器伺候它。常量立了,窗留着,连差事都写明了,vmalloc 与内核线程栈(:37 的注)。分配器哪天造,由用得上它的那天说话,本章不动工。

ioremap 檐下的临时工,您认得的,相位一。这个名字正文《内存当家》那一卷叫开:登记设备区、发放追踪过的映射,正经差事得有分配器伺候,伺候的人没到,拿物理偏移原样照搬的镜像顶班,IoremapVirt 一加,就是临时工的全部手艺。老户是帧缓冲:0xFD000000,接管那天从低半边的旧门搬进 259 号项,屏幕的字从那天起走新门牌。3.1 那一抄把高半整副搬进每个世界,设备的窗也跟着进了客人的世界,正文《下楼开张》那一卷的《客人的世界》里点过这层。长凳第四案当面验它,案身在实况节。

模块窗的住处,注里也写了缘故。kModuleBase 的注是 `the -mcmodel=kernel link keeps code in the last 2 GiB`,内核的代码模型把代码按在地址空间最末 2GiB 里,链接器认的只是这个射程,地图顺着它画:镜像窗 1GiB,模块窗 1GiB,高半的顶恰好用满。编译器那头的细节,注说到哪儿,咱们讲到哪儿,不往下铺。

末了是用户半边。PML4 的 0 到 255 项归客人,地图在这半边只立约定,不盖房。五条约定连注带码,咱们整个请出来(:63 到 :72):

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

五条咱们挨条报名。kUserTop 0x0000800000000000,客人的天花板,3.1 量过,岗上的限与图上的线同数。kUserNullTop 64KiB,空指针禁区,3.1 也认过,客人解引用了没立起来的指针,踩的是这块注定没映射的地。余下三条本章头一回露脸:kUserBrkMmapSplit 0x0000400000000000,程序加 brk 的地界与 mmap 区的分界,恰是天花板折半。kUserMmapTop 0x00007F8000000000,mmap 区的顶,注写明走向,`allocations grow down from here`,门牌从顶往下发。kUserStackTop 定在 kUserTop 减一页,0x00007FFFFFFFF000,默认主栈到这儿为止,上方那一页留在地图外。这儿有一处跟正文对不上,得当面认:正文《内存当家》那一卷把 mmap 区说成从分界线往上长,码上的注写的走向正相反,数与走向,都以码为准。五条的队,断言也排好了,押在文件尾巴上:

```cpp
static_assert(kUserStackTop + cinux::arch::page::kSize == kUserTop,
              "one guard page separates the stack top from user top");
static_assert(kUserNullTop < kUserBrkMmapSplit && kUserBrkMmapSplit < kUserMmapTop &&
                  kUserMmapTop < kUserStackTop,
              "user conventions stay ordered inside the user half");
```

:162 到 :166,两条。头一条您一眼认得出,kUserStackTop 加一页,恰好回到 kUserTop,一页保护页把栈顶跟天花板隔开,越界的那一脚踩空,踩的是没映射的地。这门手艺不是新的,3.4 给堆窗脚下安过一页,署名《页进,块出》,这儿换到客人的栈顶再使一回。第二条把五条约定排成一列,空指针禁区小于分界,分界小于 mmap 顶,mmap 顶小于栈顶,约定与约定,谁也越不过谁。

**停一停,五扇窗加起来占地几何,这道加法咱们当场折?**

咱们折。一项 512GiB,五扇窗,256 到 259 连号四扇,加 511 一扇,五乘 512GiB,2560GiB,折成 TiB,2.5TiB。3.1 量过,高半一共 128TiB,公告头发布出来的只有 2.5TiB。客人的半边同样 128TiB,一项未占,运行期前 256 项全零,3.2 机内第三案逐项验过。您可别小看空着的地:项号本身就是地图的一部分,511 号项里 PDPT 还分着两格,空窗的名与界也是发布出来的内容。数到末了,两笔并在一起:发出来的 2.5TiB,落在 3.1 量过的 128TiB 高半里,余下的全是空地,空得清清楚楚,一笔一笔都在图上。

## Code: Cinux 实况:长凳六案,机内回指

锚还是 r13_userspace,前三章用的也是它。这一章咱们连机器都不用起:地图是常量,长凳直跑就能对实。面板也还是 3.1 抄过的四行,本章一行不动。机内的活证据,3.2 的 ktest 三案拍过了,镜像与直接映射翻得回物理、没人动过的堆基址翻回 0、低半 256 项逐项验零,三案回指,屏不重刊。长凳的构建照 3.4 的口径,默认档备一份目录:

```bash
cmake -B build-dbg
cmake --build build-dbg --target test_mm_layout
build-dbg/test/test_mm_layout
```

屏头尾,咱们全抄:

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

六案咱们逐案认,案名把地图念了一遍。头一案,`kernel regions are disjoint and ordered`,五扇窗互不重叠、次序不乱,案身五枚断言把五家基址从低到高比了一遍,连 ioremap 的顶不得超过镜像项底也压在里头。第二案,`direct map conversion round trips`,停一停里推的往返,加得去减得回,IsDirectMap 的项边界也在案里探了。第三案,`half classification`,分界线的五枚真伪。第四案,`phase one mirrors physical offsets`,临时工的手艺当面验,案身三行:

```cpp
ASSERT_TRUE(IoremapVirt(0xFD000000) == kIoremapBase + 0xFD000000);
ASSERT_TRUE(IoremapVirt(0) == kIoremapBase);
ASSERT_TRUE(!IsDirectMap(IoremapVirt(0xFD000000)));
```

您看帧缓冲的门牌,等于窗基加 0xFD000000,窗基本人递进去,翻回来的还是它自己,末一行再把设备的门牌从直接映射窗里撵出去,三行把临时工的手艺验了个遍。第五案,`image and module windows sit in the last entry`,511 号项里两格的位置,案身挑要紧的几行:

```cpp
ASSERT_TRUE(kModuleBase - kKernelImageBase == 0x40000000UL);
ASSERT_TRUE(kKernelImageBase + 0x200000 == 0xFFFFFFFF80200000UL);
```

0x40000000,1GiB,咱们认得出,这是镜像窗与模块窗中间的隔断。kKernelImageBase 加 2MiB,0xFFFFFFFF80200000,1.8 世界观的原点,注里 `the image itself still links at +2 MiB` 那句,案里对上了数。第六案,`user half conventions`,五条约定逐条对数,案身挑头尾三行:

```cpp
ASSERT_TRUE(kUserTop == 0x0000800000000000UL);
ASSERT_TRUE(kUserStackTop == 0x00007FFFFFFFF000UL);
ASSERT_TRUE(kUserStackTop % 0x1000 == 0);
```

前两行对数,天花板与栈顶,数跟 layout.hpp 分毫不差。您再瞧末一行,栈顶对 0x1000 取余得零,保护页的门槛也得踩在整页上,数与格式,两道岗一起站。

## 对照:同一件事,别的机器怎么做

咱们把 xv6 翻到书页 36,kvmmake 所在的那段,两句原话借来:

> “kvmmake first allocates a page of physical memory to hold the root page-table page. Then it calls kvmmap to install the translations that the kernel needs.”

> “The translations include the kernel’s instructions and data, physical memory up to PHYSTOP, and memory ranges which are actually devices.”

它家内核映了些什么,第二句报了单:自家的指令与数据,PHYSTOP 以下的物理内存,PHYSTOP 是它家物理内存的顶,还有真设备的地界。可单子没有公告头,映什么、映到哪,kvmmake 里一张 kvmmap 清单说了算,调用一回挂一段,图长在通话记录里,纸上没有全张。咱们把 layout.hpp 摆在它旁边:五扇窗、五条约定、十二条互锁,一份头文件全发布,长凳六案背书。同一张内核映射,它家记在通话里,咱们贴在墙上。您想知道咱们的内核映了什么,不用翻调用,翻一份头文件。

没到任的住户,它家那头也没有对得上的条目。设备的地界,它家 kvmmap 现挂现用,表里没有给设备预留的整项。咱们把预留做成了地图上的正式条目,vmalloc 一户没进,名与界全在,ioremap 檐下的临时工也领着公告头发的基址。留名不为好看:项号占下了,后来的住户进门就有号,基址、跨度、跟邻居的次序,全在公告头上写明。

轮到 Linux,这一章咱们不上色。它家的虚拟地址布局散在几份文档里,咱们手边一份能引的都没有,凭印象描图,错一条线比空着还糟,整格留白,材料到齐那天,咱们提笔补色。

自家的窄处咱们也交底两件。一件是问窗的脸只长了一枚:IsDirectMap 只伺候直接映射窗,址落进了堆窗还是 vmalloc 窗,layout.hpp 没给配脸,真要认,拿各家的基址自己比。另一件,两扇窗眼下都没有登记簿:vmalloc 无分配器,ioremap 临时工顶班,窗里哪段被谁占了,地图不记,记是记在将来的章程里的。

C++ 在这一章出手的地方,就是文件尾巴那十二条 static_assert。本章细读了八条:项的跨度一条(:147),次序四条(:150 到 :154),模块紧跟一条(:160),保护页与用户半边次序两条(:162 到 :166),余下四条里,天花板的断言 3.1 逐字点过,另三条本章是头一回照面。互锁的脾气,正文《内存当家》那一卷领教过一回:写镜像与模块那两枚断言时,符号扩展的底子没铺,左移出来的数高 16 位是零,跟高 16 位全 1 的门牌天生不等,两条断言编译当场红,开机都不用等。您今天读到的版本,底子上铺着 0xFFFF000000000000,那回红出来的教训,如今垫在每一枚高半基址的断言底下。

## 这一章留下了什么

收卷,咱们点名。一张图:五扇窗各占一项,256 到 259 连号,镜像与模块同住 511 号项里 PDPT 两格,每家基址都有断言押着项号。一记换算:直接映射一加一减,IoremapVirt 一期镜像,IsKernelHalf 一道线压在规范高半的门槛上,堆窗与镜像窗的门牌,减法不伺候。五条约定:空指针禁区、天花板、brk 分界、mmap 顶、栈顶保护页,次序有断言押队。六案:长凳全过,机内的证据 3.2 在案。

| 术语 | 一句话口径 |
|---|---|
| 公告头 | layout.hpp 的身份:内核虚拟地图全本发布在一份头文件里,正文《内存当家》叫开的名 |
| 项与窗 | 一枚 PML4 项罩 512GiB:窗按项分家,五扇窗占 256、257、258、259、511 |
| 直接映射 | 门牌等于底数加物理址:一加一减,全地址空间只有这扇窗吃减法 |
| 相位一 | ioremap 的一期:物理偏移原样镜像,帧缓冲是檐下的老户 |
| 空窗 | 立了项、发了基址、正经住户没到任的窗:vmalloc 全空,ioremap 临时工顶班 |
| 保护页 | kUserStackTop 上方不映射的一页:栈顶与天花板之间隔一页踩空的地 |

地图铺平了,咱们盘一盘第 3 部的家底,还欠一课:《缺页与按需分页》。地图发门牌,走查记翻译,translate 查无此页翻出的那个 0,3.1 埋过,3.2 接过一回。真有客人一脚踩空,谁来接、接住以后页从哪来,归那一课讲。

## 练一练

咱们留三道题,一道动手,一道猜想,一道读屏。

**题一,动手。** 长凳这一件您自己编自己跑,六案名单与 Results 对屏。抄屏的时候认一认:哪一案守五窗次序,哪一案当面验临时工?

**题二,猜想。** 哪天手滑,把 layout.hpp 里 kHeapBase 的值改成了 kVmallocBase 的数,编译器喊不喊?长凳跑得起来吗?您对着 :150 那枚断言与长凳头一案推一推,再对参考答案。

**题三,读屏。** 第五案那两行断言,头一行 0x40000000,末一行 0xFFFFFFFF80200000,您说说各验的什么?第六案里 kUserStackTop 的两枚断言,枚枚管的哪桩?

### 参考答案

题一:您跑下来,Results 该是 6 passed, 0 failed。守次序的是头一案 `kernel regions are disjoint and ordered`,验临时工的是第四案 `ioremap phase one mirrors physical offsets`,案名跟屏一字不差。

题二:您把 kHeapBase 改成 vmalloc 的基址,:150 那枚断言当场就喊,kHeapBase 得等于 kDirectMapBase 加一项跨度,等式不成立,编译就停,机器根本起不来。长凳头一案也护着一道,kHeapBase 得小于 kVmallocBase,可它轮不到出场,编译期那道岗排在头里。两道岗,一道在编译期,一道在运行期,地图上的数走的是头一道。

题三:您对下来,0x40000000 验的是镜像窗与模块窗的间隔,整 1GiB,两格的界压在断言里。0xFFFFFFFF80200000 验的是镜像的落点,kKernelImageBase 加 2MiB,跟 :58 注里 `the image itself still links at +2 MiB` 对数。第六案头一枚断言对数,kUserStackTop 等于 0x00007FFFFFFFF000。第二枚验对齐,它对 0x1000 取余得零,保护页的门槛踩在整页上。

## 延伸

- xv6 书第 3 章书页 36:kvmmake 与 kvmmap 的两句原话都在这一页,它家内核映什么、由谁说了算,您对原书核。
- 正文《内存当家:一页一页地精修》一卷:《挂在墙上的地图》是地图叙事的全本,公告头、用户半边五约定、互锁首版被抓的旧事都在里面。帧缓冲搬家与相位一,接管那一篇拍的全本。您要现场,卷门开着。
- 正文《第一个内核:把执行流递到它手里》一卷:《两扇门,和最后一跳》讲高半门牌的来历,第 0 项与第 511 项同表、别名恒等,本章把它收进了全图,您两边对读正好。
- 正文《下楼开张:ring3、syscall 与第一位常客》一卷:客人的世界抄整副高半,259 号设备的窗跟着进门,《客人的世界》里点过这层,您翻得到。
