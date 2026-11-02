---
title: 04 · 物理内存管理
---

# 04 · 物理内存管理:页有阶,堆有窗,块有头尾

上一章收了卷,把铺子和堆的来路一并记到了本章名下,门是今天开的。来路的这件事,欠下的日子比上一章早。2.1 头一回摊开那本日志的时候,就把堆的来路交代了,说它住在正文后面的几卷。2.3 的屏上,那一行 `heap: new/delete live (probe 114514)` 亮了出来。2.5 收了卷,又把 2.1 照过面没讲的堆记进《内存》名下。3.1 与 3.2 的两章,世界立起来了,走查也走通了,可走查铺的每张表、挂的每枚页,向名册领走的都是整页,页表伺候不了的零碎小件、连绵的住户,去处一直是悬着的。这一章咱们把悬着的都交全:整页按什么秩序发出来,发出来的页怎么住进堆窗,堆里的页又怎么细成 new 与 delete 天天花着的零钱。

名册的本体是老相识,1.7 整整的三拍:全掩的起手,开格的放行,闭格的登记,一位一页的名册连 1 比 32768 的杠杆也念过。本章咱们一页不重铺,讲的是名册之后的秩序。开卷咱们把两个版本的读数对一对:1.7 手里那版报 32350,今天的这版报 32316,内核的个头从 810C0 长到 90A20,个头长了一截,别的开销也添了些,差的构成不复算,咱们数认到差值为止,1.7 当年认 1.6 的 3 页差,走的也是这个旧例。

这一章的活,咱们数下来三样。头一样认的是名册的取用:allocate_pages 按阶发页,一发就是 2 的 order 次幂那么一套,出不了货的请求,咱们拿零址打发。再一样认的是堆的开张:堆窗脚下让出一页不映射的保护页,走查把底货一页一页铺了进去,Heap 把整段领成了一块。末一样认的是铺子的算术:32 字节的头,16 字节的尾巴,一条按地址排队的空闲链,挑块的是 first-fit,够大的就分裂,还回来的就两头合并,new 与 delete 的四件在门口接单。

量法的三条,这一章还是照旧的。码的主役是自家 kernel/mm/ 底下的七件,pmm_config.hpp、pmm.hpp、pmm.cpp 管的是名册,heap.hpp、heap.cpp、heap_config.hpp 管的是铺子,heap_runtime.cpp 管页与铺子的接头,咱们逐段读。屏的话,咱们自家起机抄面板,长凳的两台测试当场跑当场抄,`test_pmm` 跑的是七案,`test_heap` 跑的是六案,机内的一台自测跑的是三案。书要翻的是 xv6 第 3 章讲物理内存分配器的那两页,书页 37 与 38 是逐字核过的,原话是原样搬进对照节的,您对着书核。

> **名册的本体 1.7 交过底了,这一章的纲,凭什么压在一句头注上?**
>
> 凭头注把名册的营生报得齐全。pmm.hpp 的头注,笔者写下的是 `Pmm turns the E820 map into page accounts — what boot stood on stays spent, what firmware keeps stays closed, everything else is allocatable in single pages or power-of-two runs.`。boot 踩过的地记作花销,固件圈的地记作封存,余下的一切,单页的也发,成套的连段也发。前半句是 1.7 的旧课,后半句的 `single pages or power-of-two runs`,是这一章的正文,货发出去以后接住它的是谁,也是这一章要交代的。

## 按阶发页:一发一套的秩序

头一样说的是名册怎么把页发出去。allocate_page 与 free_page 挂的是两个单页名,allocate_pages 挂的是一个成套名,三个名字 1.7 都点过了,free_pages 是随成套那一对儿来的,在本章头一回露了脸。单页的两个是便利面,pmm.hpp 里的身子各一行:`allocate_page()` 就是 `allocate_run(1)`,`free_page(phys)` 对应的就是 `free_pages(phys, 0)`。正主是后面的那一对,咱们把身子整个请出来:

```cpp
PhysAddr Pmm::allocate_pages(int order) {
    if (order < 0 || order > kMaxOrder) {
        return PhysAddr{};
    }
    return allocate_run(1UL << order);
}

void Pmm::free_pages(PhysAddr phys, int order) {
    if (phys == PhysAddr{}) {
        return;
    }
    bitmap_.clear_range(cinux::base::math::Floor(phys.raw, cinux::arch::page::kSize),
                        1UL << order);
}
```

pmm.cpp 的 :46 到 :58,咱们逐句对。进门设的是一道岗,order 出了 0 到 9 的圈,递回的是零址,机器是不炸的,多的话一句不说,1.7 练一练里您试过的那个 order 10,拒它的就是头两行。圈内的:`1UL << order` 一移,一套的斤两就算了出来:order 0 一页,order 9 的一套是 512 页,合的是 2MiB,正好一片大叶的地界,顶是 pmm_config.hpp 的 kMaxOrder 拍下的,1.7 是报过数的,今天读到它站岗的地方。还页的那半更直白,址折回了页号,`1UL << order` 那么多位拉灭,完事了。有一条约定得单独念:当初拿的是几阶,还的就得是几阶,pmm.hpp 的注写得明白:`freeing with a wrong order corrupts the ledger`,还错了阶,名册就坏了,守约的差事在调用方,名册是不替人兜底的。

真正的找货在 allocate_run 的 :68 到 :75,拢共的三句。头一句的 find_clear_run 从名册头上往下数,数出头一段够长的灭位,咱们给这个找法起个名,叫的是首空连段,头一处空得出这么一串的地方。数的手法 1.7 交过底,整词全亮的整词跳,词里混着空位的才进去一位一位数,地界越大扫得越久。数着了,整段点亮了,首址乘上了页大小,交了货。数不着的话,零址出了门,分配失败的口径从名册上岗那天起就没变过:低 1MiB 整块圈着,发出去的页永远不会落在零上,零在这儿当的是“没有”,咱们用得一点不含糊。

名册的身上还有两本数,咱们顺手认了。free_page_count(:60 到 :62)报的是 clear_count,灭着的位有几根它报几,面板上那个 32316 就是它数的。managed_page_count(:64 到 :66)报的是另一回事,init 开格那天记下的总数,数的是名册一共管着多少页。两数一减的话,亮位自己现了形,4194304 减亮位得的是 free 数,1.7 是当场演过一遍的,咱们不重演。至于门槛 1MiB、天花板 16GiB、顶 9 阶这三件的话,全住在 pmm_config.hpp 的一页纸上,1.7 是都认过脸的,咱们这里指个住处就算见过。

## 页进堆窗:一页让路,一窗底货

第二样说的是页怎么住进堆。堆在图上的住处,layout.hpp 立的名,3.2 的机内案里照过面:kHeapBase,高半里自家的一扇窗,窗占 PML4 的 257 号槽。门牌的数本章亮出来:0xFFFF808000000000,layout.hpp 的 :47 立着。BringUpHeap 的身子在 heap_runtime.cpp 的 :41 到 :47,拢共的三行,咱们整个看:

```cpp
bool BringUpHeap() {
    unsigned long const kBase = kHeapBase + kHeapLowerGuardBytes;
    if (!map_heap_pages(kBase, kHeapInitialBytes)) {
        return false;
    }
    return Heap::self().init(kBase, kHeapInitialBytes);
}
```

头一行的活,咱们把门牌定下。堆窗的基址上头让出一页再开工,让出来的这一页特意不映射,名字跟的是正文《页进,块出》的那一篇:保护页,kHeapLowerGuardBytes 立的就是一页宽。铺子里的代码哪天越了界往下多写一步,头一脚踩进的就是这片没映射的地,报到的就是 14 号,病灶是留在自家门槛上的,顺着号一挖就是越界的那行。在 3.2 的机内第二案里,没人动过的堆基址翻回 0,守的正是这一页。

第二行干的是进货,活咱们一页一页地干:map_heap_pages 在同一份文件里的 :23 到 :37,向名册领的一枚,MapPage 挂的一枚,门牌是从 kBase 起挨着排的。EnsureLeafTable 一路带建的走法 3.2 刚教过,路上缺的表现场补,领出来的页直接踩上去,领空了就回 false,开张是开不成的。开张的底货是 heap_config.hpp 定下的 64KiB:十六枚页。

第三行把整段交给了 Heap,init 的身子在 heap.cpp 的 :100 到 :111,咱们整个看:

```cpp
bool Heap::init(unsigned long base, unsigned long bytes) {
    if (base % kPayloadAlign != 0 || bytes < span_bytes(kMinPayload)) {
        return false;
    }
    base_                        = base;
    top_                         = base + bytes;
    unsigned long const kPayload = bytes - kHeaderBytes - kFooterBytes;
    make_free_block(base, kPayload);
    free_head_  = base;
    free_bytes_ = kPayload;
    return true;
}
```

进门立的是两道岗,您一眼认得出:基址得踩在 16 的倍数上,地界至少够立一个最小块,歪址和小地界当场拒。过了岗,base_ 与 top_ 把整段圈了下来,余下的字节整个当成一块空闲块立起来,头尾的一装一挂,free_head_ 指向了它,free_bytes_ 记下了全部家底。开张的那一刻,铺子里立着的是一件整货,一整段的空,家底有多大的事,实况的测试里它自己报数。

## 页切成零钱:头、尾、一条排队的链

第三样进的是铺子。零钱、铺子、进货:叫开这些词的是正文《把页切成零钱》《页进,块出》的那两篇,咱们接着叫。铺子的家底刻在每一块上,咱们把常量和头一起看:

```cpp
constexpr unsigned long kHeaderBytes  = 32;
constexpr unsigned long kFooterBytes  = 16;
constexpr unsigned long kPayloadAlign = 16;
constexpr unsigned long kMinPayload   = 16;

enum class BlockState : unsigned char {
    kFree,
    kLive
};

struct Header {
    unsigned long payload;
    unsigned long state;
    unsigned long next;
    unsigned long spare;
};
static_assert(sizeof(Header) == kHeaderBytes);
```

heap.cpp 的 :12 到 :28,咱们把家底念一遍。一块的身家是 32 字节的头,四个 8 字节的格:payload 记负载多大,state 记的是闲忙,闲忙的两样,拿在枚举 BlockState 的手里,next 是空闲链上的后继,spare 留作的是余量。头是顺走的,往后摸链全凭的是它。尾巴是在另一头的,:38 到 :44 露的是两张小脸,write_footer 把本块的跨度:头加负载加尾一共多少字节,写进了块尾,read_prev_span 到本块跟前念上一格:念出前邻的跨度。

这儿有一处出入得跟您当面认:正文那一卷记的尾巴是 8 字节,那是它更早的个头,如今码上宽了一倍,数咱们以码为准。宽出来的 8 个字节不为别的,为对齐:负载一律取整到 16 的倍数,头 32 也是 16 的倍数,尾巴占了 16,三样加出来的跨度才还踩在 16 的倍数上,下一块的开头接着 16 对齐。您把尾巴收成 8 试试,跨度成了 8 的倍数,第二块往后的门牌全要挪窝,allocate 许出去的 16 对齐就破了功。

对齐之外咱们还认一条下限,管它的是 round_payload(:46 到 :50),咱们递一个 5 字节的请求试试:按 16 取整,抬到的是 16,不足 16 的都照这个数办。取整用的 Ceil 数的是单位个数,乘回单位宽出来的才是字节,这层换算正文那卷吃过语义的亏,如今写在了明面上。

空闲链按地址排队,link_sorted(:61 到 :73)管的是挂链,咱们看新块怎么落座:从头扫到它该在的座位,地址比它大的都请到它后头。挑块的脸 pick_free_block(:89 到 :96)顺着链走,头一个装得下请求的就成交,行话叫的是 first-fit,链既然按地址排,成交的永远是最低的合身洞。1.7 的 find_clear_run 也是从头找,铺子的挑法跟它一个脾气,图的是便宜,咱们不挑食。

**您停一停,链要是不排队的话,哪件事头一个失了灵,您推推看?**

咱们把链打散了推。合并是不靠链的,后邻看的是跨度,前邻看的是尾巴,物理上的邻居凭地址相认,链乱成什么样都拦不住它俩会合。挑块就不同了,first-fit 认的是链上头一个合身的块,链一乱的话,成交的就不再是最低的洞,一样的分配序列今天落这块明天落那块,布局跟着链的脾气漂。咱们长凳上有一案,断的恰恰是拿回来的块必须是原来那个最低的洞,链要是乱了,那道断言就赌不赢了。

成交之后的第二幕是分裂,allocate 的 :123 到 :130,咱们凑近看:挑中的块比请求大出一截,余下的部分够立一个最小块,头尾开销 48 字节带 16 字节的负载,咱们就从中间切一刀:前半成交,后半立成新的空闲块挂回链上。切不出来就整块给了,零头算进买家的负载。free_bytes 的那本数跟着每一刀走,切走了多少、还回了多少,它都记下了。

还钱是最有戏的一笔,free 的 :138 到 :179。头一道工序验身:址得落在铺子的地界里,头的 state 得是忙,跨度得还在顶的里头,不占理的,原样退回了 false。双重释放与野指针在这一关就地拦下,长凳上有一案递的就是野指针,您到实况里看它挨拒。验完身看两头的邻居:后邻的门牌就在本块跨度的末尾,闲的,从链上摘了下来并进来。前邻的门牌没人直接记着,靠的是尾巴回走,本块跟前念出前邻的跨度:一减,前邻的家门就找到了,闲的,也并了进来。三块要是都闲了,就并成了一块。并完了往链上归座,走的分两路:前邻要是闲,合并块指的就是它,它本来就在链上的,改了跨度就归位,谈不上挂回的事。前邻没闲的话,新立的那块这才挂链。被吞掉的邻居,头也标成了闲:免得谁攥着旧指针再来一回,验身的关还放它进门。

grow 是铺子长个头的一笔:身子在 :181 到 :203,咱们看它怎么收货。顶上来了新地界:新顶得高过旧顶,还得踩在 16 的倍数上,新增的至少够一个最小块,过了这三道,新地界整段立块。顶上原本那块要是闲的,直接并了进去,链都省得重挂了。没闲的话,新块自己挂了链。那新地界从哪来?HeapAllocate(:49 到 :60)立的章程是试分在前:allocate 分不出,咱们别急着回空,按页取整的时候还多带一页,免得整页装得下、加个头就装不下的请求白跑一趟,走查把窗铺好了,grow 收了货,咱们再试一回 allocate。铺子的进货单,到这儿跟名册接上了头。

## Code: Cinux 实况:面板两行,长凳十三案,机内三案

锚用的还是 r13_userspace,3.1 与 3.2 用的也是它,切版、重打镜像、喂键的老话前几章念全了,这儿不重印了。咱们起机,等的是面板长齐,认的是四行里的两行:

```text
[kern] pmm: 32316 pages free (probe 113000 ok)
[kern] heap: new/delete live (probe 114514)
```

pmm 的这行,1.7 是读过同款的,数字换了一版,咱们逐段认:free 数的是名册上的灭位,clear_count 现数的,括号里是 probe 的回执:分一页、还一页、报个平安,数怎么来的:照屏抄,咱们不解释。32350 对 32316 的差值,构成开篇认过了。heap 的这行,2.1 照了面、2.3 认了名,今天交底:new 与 delete 在开机的路上真跑过一趟,回执打在了这一行,那枚数的来历,正文的那一篇《页进,块出》里交代过的,不重叙了。

长凳的活是两台测试,咱们把它们编出来。编之前有一条环境的老实话搁在前头:test_heap 里有一案,故意把指针往界外递了一字,验的就是拒收,优化档的编译器看不得这个,把警号升成了错误,编译过不去了。长凳的构建目录用默认档备一份,咱们让两台都从它出:

```bash
cmake -B build-dbg
cmake --build build-dbg --target test_pmm test_heap
build-dbg/test/test_pmm && build-dbg/test/test_heap
```

咱们把两台的屏头尾全抄:

```text

=== Cinux Test Runner ===
Running 7 case(s)...

[PASS] pmm: constants are pinned
[PASS] pmm: low megabyte stays out of the ledger
[PASS] pmm: order runs respect kernel holes
[PASS] pmm: order bounds are refused
[PASS] pmm: every free page is reachable exactly once
[PASS] pmm: ram above 4G joins the ledger up to the cap
[PASS] pmm: a map with no usable ram allocates nothing

=== Results: 7 passed, 0 failed ===
```

```text

=== Cinux Test Runner ===
Running 6 case(s)...

[PASS] heap: init claims the span as one free block
[PASS] heap: allocate returns aligned payloads and spends free bytes
[PASS] heap: free restores the span through coalescing
[PASS] heap: first fit reuses the lowest fitting hole
[PASS] heap: refused pointers stay refused
[PASS] heap: grow appends usable space

=== Results: 6 passed, 0 failed ===
```

十三案逐案认:头一台 pmm 的七案,咱们一案一案来。头一案把案身五道断言当面点了一遍,页守的是 4KiB,门槛守的是 1MiB,天花板守的是 16GiB,顶守的是 9 阶,外带 type=1 的那枚暗号,取的也得是 1。第二案守门槛:单子上一段 128KiB 的 type=1 整段趴在 1MiB 底下,抬完是空的,另一段 4MiB 的从 1MiB 起,管下了 1024 页,内核占去了 2 页,free 的是 1022,头一发分到的正是 1MiB 起头那页。第三案是 order 的正戏,发的是一套 512 页,落点得在内核洞的后头,洞里的地一页不占,还了回去,free 数原样复了原。第四案把 -1 与 10 一齐拒在了门外。第五案下的是狠手:1022 页一发到底,咱们数下来一页不少,每页恰好到手了一回。第六案当面验天花板:4GiB 起的一大段 type=1,管到的只有 16GiB 顶以下那截,头一发分的是 4GiB 那页。末一案单子上写的一个 type=1 都没有,名册管的是 0 页,free 的是 0 页,分配回的是零址:一句怨言没有。

heap 的六案,您跟着认:头一案里 16KiB 的池子整段领成了一块,free_bytes 报的是 16384 减 48,头尾的开销当面扣掉,基址递一个歪的,init 当场拒了。第二案递的是 64 字节的请求,拿回的指针 16 对齐,free_bytes 少的恰好是 64 带 48,花数是全对得上的。第三案是最有戏的:各 64 字节的三块,还了中间、还了头一块、还了末一块,三回并完了,free_bytes 回到开张的原数,同一块还第二回的话,回的是 false,双重释放是进不了门的。第四案就是停一停里推的那案,还掉了头一块再要 32 字节,拿回的必须是原来那块,最低的洞头一个填。第五案把拒收的章程验了个遍,nullptr、界外一字、没立过的址,回的一概是 false,1GiB 的请求也只配回空。第六案里 12KiB 占了下去,8KiB 是分不出的,grow 把顶推到了 32KiB,8KiB 当场成了交,顶的读数分毫不差,同一处 grow 再来一回的话,回的是 false,地界其实没长,虚报是不许的。

heap 的自测机,机内也留了一台,咱们起 `build/boot/cinux_ktest_heap.img` 这枚镜像。法门与 3.2 的机内三案同款:TCG 起机,串口接了文件,起了机就跑,喂键的活都省了。stage2 起机的那一截老话前两部是念过多回的,这回不重印了,从 ktest 报到的那行往下全抄:

```text
[ktest] kernel test world alive, 7 e820 entries

=== Cinux Kernel Test Runner ===
Running 3 case(s)...

[RUN] heap: freed blocks are reused before the span grows again
[ktest] heap reuse: first FFFF808000001020 third FFFF808000001020
[PASS] heap: freed blocks are reused before the span grows again
[RUN] heap: a large allocation grows the span and stays writable
[ktest] heap grew: top FFFF808000011000 -> FFFF80800002B000, block FFFF808000001020
[PASS] heap: a large allocation grows the span and stays writable
[RUN] heap: new and delete round trip through operator new
[PASS] heap: new and delete round trip through operator new

=== Results: 3 passed, 0 failed ===
```

机内的三案,您接着认。头一案的 `freed blocks are reused before the span grows again`,案名把章程念了一遍,还掉的洞回头就有人住,顶是不为它涨的。证据行里的 first 与 third 两个址一字不差,停一停里推的最低洞头一个填,机器里验的就是它。第二案里大笔的分配把顶顶了上去,证据行把前后两个 top 原样打了出来,FFFF808000011000 涨到了 FFFF80800002B000,块是照常可写的,起手的那个 0x11000,正是 64KiB 底货让完一页保护页的落点。第三案的 `new and delete round trip through operator new`,new 与 delete 经全局那四件真跑了一个来回:名册、窗、铺子,一条链上过了一遍。

**您停一停,一案断言绿了,您说,那一回分配就真发生过吗?**

这一问咱们手头这部机器刚刚答过一半。机内第二案的那行 heap grew,断言前后各记了一回 top,原样打了出来,当的差事就是防折叠。当年正文那卷立这案的头一版,断言是光断不打印的,跑下来红得没了道理,铺子的算术刚在长凳验过,进货的代码二十来行,顶怎么推都不该不涨。反汇编开了,谜底是在光天化日之下的,测试里 operator new[] 的那通 call 压根不存在,编译器把结果没逃出测试的分配整个删了,戏是没演的,顶自然也就不涨了。咱们把 top 打出编译单元,编译器是删不动它的,这行的证据从那天起常驻了案身,咱们屏上这一行,就是那套工序当场打的。案的三层爬梯与修法,正文《消失的那通 call》里逐层摆着,这里不重讲了,判别法值得带在身上:断言世界状态的案子,绿的不算数,call 在了场才算数。

## 对照:同一件事,别的机器怎么做

xv6 的第 3 章里,物理内存的分配器自成一节,咱们从书页 37 起头,把三段原文搬了来,一段一段地对。头一段看它的库存长什么样:

```text
“The allocator resides in kalloc.c (2950). The allocator's data structure is a free list of physical memory pages that are available for allocation.”

“The allocator stores each free page's run structure in the free page itself, since there's nothing else stored there while the page is free.”

“It allocates and frees whole 4096-byte pages at a time.”
```

它家是没有名册的,咱们把两家的库存摆在一处比:空闲的页一页挂一个 run 结点,结点就睡在页自己的肚子里,反正空着的时候那页什么都没存。页也是它家最小的面额,方才那句 4096 交了底,想要更小的零钱,它那头是没有铺子的。咱们的名册一位一页,查连段靠的是算术,find_clear_run 整词整词地跳。它家想找一段连续的空页,得顺着链一段一段地数。位图对的是链表,花销与腿脚各让一头:咱们的名册 512KiB 起楼,连段靠的是算,它家是分文不花的,连段靠的是走。

第二段咱们看它把内存的家底怎么问:

```text
“Xv6 ought to determine how much physical memory is available by parsing configuration information provided by the hardware. Instead xv6 assumes that the machine has 128 megabytes of RAM.”
```

书自己说的话很直白:本该问硬件要配置,它却一口咬定了机器有 128MB。清单其实就在 e820 里躺着,1.5 咱们验签接货,1.7 照单开了格,七行里可用的是两行,一页一页都是固件亲口报的。教学内核是犯不上逐条问的:128MB 的一刀切,虚拟机给的多少都当没看见。其实清单一直在那儿,读不读的事,是各家的福气。

第三段咱们看它还页的脾气,对应的书页是 38:

```text
“The function kfree (3005) begins by setting every byte in the memory being freed to the value 1.”

“Then kfree prepends the page to the free list: it casts pa to a pointer to struct run, records the old start of the free list in r->next, and sets the free list equal to r.”
```

头一句是它还页的头一道工序,整页涂成了 1,底下紧跟的一句讲缘故:free 完还想接着用的悬空引用,涂完读到的就是垃圾,不再是原来的有效内容,这样的代码坏得早:好查,书里用的词是 hopefully,盼的就是它早坏。链是头插的,还回来的页排到链头,kalloc 拿的永远是头一个。咱们这边,名册上的还页是位一翻,残留的字节一个不动,页表那头有出厂清零的工序候着,3.2 认过 take_page 的脾气,领出来的必是干干净净一页。堆上的还块两头认邻、按地址归座,跟头插的又是两副走法。同一份的小心,它家涂在了页上,咱们安在了清零的工序里,各有各的器官。

Linux 的那头:白卷还是白卷。物理内存它怎么管,咱们书架上至今没有一份能引的材料,什么名目、什么机关,咱们一个都编不出来。这一章引得动的书都在延伸里,架上还欠一本讲它的。白卷就摊在了那页上,书架进了新书,咱们回来补。

自家的窄处也交底两件。first-fit 的脾气 1.7 就交过,低处的洞永远头一个被填,零碎的小块攒在低处,日子久了铺子里免不了几件碎料,合并兜得住的是邻居,兜不住散落四处的碎。order 封在了 9 阶,600 页的大件一发办不成:得分几趟,1.7 的练一练里您亲手拼过四趟。

C++ 在这一章出手的地方,咱们挑两处。一处是 heap.cpp 里的那行 static_assert,sizeof(Header) 的值要是不等于 32,编译当场过不了,头的个头从此不靠人眼看着。另一处说的是接管本身,operator new、new[]、delete、delete[] 的四件,语言许咱们把全程序的分配改道,heap_runtime.cpp 的 :68 起。new 的两件是薄转发,进门递的是 HeapAllocate,递回来的空指针过 Check,“kernel heap exhausted” 的一句遗言,机器当场停了下来。内核里分配失败没有体面的退路,没有可展开的栈,也没有接得住的处理程序,停在病发的地方好过揣着空指针跑到最后认不出病因。delete 两件递的是 HeapFree:nullptr 放行,这是 C++ 的老约定。真还回来的指针要是 free 拒了,Check 换的是那句 “invalid heap free”,同样的急停,处刑的活归 operator delete,报真话的活归 HeapFree,测试要的是后者。

## 这一章留下了什么

收了卷,咱们把这一章立下的家当数一数。名册立的是一副秩序:allocate_pages 按阶发页,出了圈就拒,找到的首空连段整段点亮,free_pages 几阶拿的几阶还,两本数各报各的。堆窗开的是一扇窗:保护页让在了脚下,走查一页一页地铺货,开张的是 64KiB,Heap 把整段领成了一块。一座铺子也立起来了:32 字节的头、16 字节的尾巴,对齐的是 16,起发的也是 16,按地址排队的空闲链,first-fit 挑最低的合身洞,够大的就分裂,还回来的就两头合并,grow 进的货从顶上来。new 与 delete 的四件转发在门口接单,失败的话就急停。长凳的七加六,机内的三案,十六案全过了。

| 术语 | 一句话口径 |
|---|---|
| 阶 | order:一发 2 的 order 次幂页,0 到 9,顶上 512 页,2MiB 一片大叶的地界 |
| 首空连段 | find_clear_run 的找法:从头数出头一段够长的空位,整段点亮发出 |
| 保护页 | 堆窗脚下不映射的一页:越界往下写,缺页在自家门槛上报到 |
| 头与尾 | 每块 32 字节头记身家,16 字节尾巴记跨度,尾巴是回走的路 |
| 空闲链 | 按地址排队的空闲块序列:first-fit 从链头挑,成交的永远最低 |
| 合并 | 还块时两头认邻,闲的并进来,前邻靠尾巴回走找门 |

页和零钱都有了来路,2.1 照过面没讲的堆交了底,3.2 悬着的来路落了地。可名册的发页、堆窗的安家,凭的都是高半地图上那几枚门牌,堆窗挨着谁、直接映射窗铺到多高、内核镜像住在哪一格的答案,layout.hpp 的图咱们借了一路的名字,全本咱们还没摊开。下一章《内核地址布局》办的就是这件事:把整张图铺在桌上。

## 练一练

咱们留三道题,一道动手的,一道猜想的,一道读屏的。

**题一练的是动手。** 长凳两台您自己编自己跑,七案六案对的是 Results。起了机抄面板,今天认的两行照实况抄。抄下的 pmm 数,您拿 free_page_count 的来路核一遍,数对上了就算成。

**题二练的是猜想。** 尾巴要是从块上删干净了,free 的哪一步当场走不动?长凳十三案里哪一案跟着红?您拿 read_prev_span 的来路推,推完了对参考答案。

**题三练的是读屏。** pmm 七案的名单里,您认认哪一案守的是低 1MiB 的门槛,哪一案当面验了 16GiB 的顶?heap 六案里,哪一案是链要按地址排队那道推演的验收?

### 参考答案

题一:您跑下来,Results 的两行该是 `7 passed, 0 failed` 与 `6 passed, 0 failed`。面板的两行,pmm 报的是 32316,heap 报的是 new/delete live,数对上了就算成。

题二:您推到的是回走断了。read_prev_span 念的是前邻的尾巴:尾巴一删,念处就没了,前邻的闲不闲无从知道,两头的合并瞎了前半边,后邻认的是跨度,靠的不是尾巴:还能走。第三案里 free 末一块要并的是前邻:并不成,free_bytes 回不到开张的原数,断言跟着红了。

题三:您对出来,第二案的 `low megabyte` 守的是门槛,趴在 1MiB 底下的段一页不收。第六案的 `ram above 4G` 验的是顶,16GiB 往上的那截一页不管。heap 的那边,第四案的 `first fit reuses the lowest fitting hole`,链按地址排队的验收单。

## 延伸

- xv6 书第 3 章书页 37 与 38:本章引的三段都在这两页上,讲分配器的自成一节,从库存、家底问到还页的脾气,您对原书核。
- 正文《内存当家:一页一页地精修》一卷:《把页切成零钱》有铺子算术的全本,还有三个真 bug 的现场。《页进,块出》有保护页与四件转发的来历。《消失的那通 call》把绿断言的陷阱讲成三层的爬梯,您要现场,三篇都在这一卷里候着。
- 正文《内核接管》一卷:《一页一 bit 的名册》是名册的全本,《接口一次作对》讲 order 为什么第一天就进签名,您要追根,两篇都在卷里。
