---
title: 01 · 地址空间
---

# 01 · 地址空间:一套表,一记抄,一条回家路

首跳拍完到今天,客人一直还在机器里当值。上一部收工的时候,机器停在 cinux> 后头等您敲字,您不敲,他就不走:echo 替您回话,yield 替他让路,跟柜台开张那天一个样子。可他在楼下踩着的那片地,咱们到今天没看过全貌。首跳那一幕拍得仔细,cli、activate、跳板五条,一拍没漏,拍的只是动作的次序。他脚下的地界是谁画的、画了多大、凭什么内核在他当值的每一拍里还看得见自己,那一幕只给了半句,全本没摊开。这一章把全本交出来。

2.5 收卷时把话留在了那儿:地址空间排在《内存》名下,内存自己的正戏,下一部开锣。锣是今天开的。1.4 还有一句更早的期票,boot 只手铺三张表、恒等映射 8MB,那儿不铺高速公路,真正的页表体系是第 3 部的正戏。期票到这儿算到了日子,兑现的大头还在后头,今天这一章,咱们把整套机构在地基里立起来,立到客人能搬进去住的程度。

这一章的活,咱们数下来有三样。头一样,认 AddressSpace,行话叫地址空间:一个世界一套页表,世界这个词是正文《内存当家》那一卷叫开的,咱们接着叫,叫顺它。再一样,看客人的世界怎么画:0x400000 一枚代码页,0x500000 一枚栈页,画页的 map_user 凭什么一位都不能省。末一样,走一遍搬家的手续:activate 怎么把机器搬进新世界,搬进去了,回家的路在哪,顺带回答开头那问,客人当值的时候,内核凭什么还看得见自己。

量法照旧三条。码,自家 kernel/mm/ 底下四件,layout.hpp 管图,address_space.hpp 与 address_space.cpp 管世界,vmm.hpp 管内核自家的表页从哪来,咱们逐段读。屏,自家起机抄,机器还是首跳那一版。书,xv6 翻到了第 3 章,书页 33、38 与 39 三页逐字核过,原话整段请进来当面比。Linux 那头这一章交白卷,缘故在对照节交代。

> **客人下楼那张底,咱们只数过步数:他脚下的地界凭什么画得出来?他当值的每一拍,内核又凭什么看得见自己?**
>
> 两问共用同一个答案。address_space.hpp 的头注,笔者落笔的头一句是 `One world of translations: a PML4 the kernel can enter.`,一个世界,一整套翻译,一张内核进得去的 PML4。世界怎么立起来,客人的地怎么画进去,机器怎么搬进去、又怎么回得来,全在这句话的两半里,这一章就是给它作注。

## 一套表,立起一个世界

AddressSpace 这个类,咱们从脸认起。address_space.hpp 里拢共六张脸:init 立起一套页表,root 报出这套表的根,map 与 map_user 往世界里挂页,一个伺候内核自己的页,一个伺候客人的页,translate 拿虚拟门牌换出物理门牌,activate 把机器整个搬进这个世界。类注上有一句交代,`Place instances in static storage`,实例请搁在静态的住处,注里把缘故也写了,内核的栈浅,深不到能糟蹋一份家当的地步,这样的对象天生该住静态的地。六张脸各管一摊,谁也不越界,本节认头三样:世界怎么立,表页从哪来,谜底藏在哪一抄里。

世界不是咱们新造的词。正文《内存当家:一页一页地精修》那一卷里,AddressSpace 的全本叙事住在《换世界》一节,那卷把话说得直白,一个 AddressSpace 就是一个世界:它自己的一套页表,自己的用户半边,想进就装它的根,想走就换回来。今天教科书层把这套话接过来,对着自家的码一块块落到实处。空户口这个说法,2.1 实况里叫过一回,户口空在哪、怎么就不空了,咱们今天看码。

init 的身子在 address_space.cpp 的 :28 到 :40,咱们整个看:

```cpp
bool AddressSpace::init() {
    base::PhysAddr const kFrame = Pmm::self().allocate_page();
    if (kFrame == base::PhysAddr{}) {
        return false;
    }
    auto* const kFresh = cinux::base::PtrAt<Entry>(DirectMapVirt(kFrame.raw));
    cinux::base::SetBytes(kFresh, 0, cinux::arch::page::kSize);
    auto* const kKernel = cinux::base::PtrAt<Entry>(DirectMapVirt(KernelPageRoot()));
    cinux::base::CopyBytes(kFresh + kKernelHalfSlots, kKernel + kKernelHalfSlots,
                           kKernelHalfSlots * kSlotBytes);
    root_ = kFrame.raw;
    return true;
}
```

四步,咱们一步一步对。头一步,:29,向名册领一页,Pmm 发出来的这枚物理页,就是新世界的 PML4。下一行把物理址换成能下手的门牌,法子是加一枚写死的基数,这桩换算的行话叫直接映射,口径几段之后再细认。名册的脾气 1.7 认过,一位管一页,领页本身的手续,《物理内存管理》名下。领失败了,init 回 false,世界立不起来,调用方自己看着办。第二步,:34,SetBytes 把整页清成零。清零是页表的出厂纪律,正文那一卷立过它的分量:表页带着上一任的残留上岗,残留里最低位恰好是 1 的项,CPU 一查就当真有映射,translate 翻回莫名其妙的址,写一枚门牌改的却是别家的地,重启一回,残留换了,症状跟着换一副面孔。所以凡是新页上岗当表,交接的前提是全零,这儿一道工序都不省。

第三步是全场最要紧的一抄。:36 到 :37,把现役根的高半,256 到 511 项,整段原样抄进新页,kKernelHalfSlots 这个名字记的就是 256。为什么抄的是这半边、抄的为什么恰好够?咱们想一件事:换世界的那一拍,正在跑的代码是谁,是内核。它脚下的路、手里的栈、串口的驱动、屏幕的字库,全住在高半的门牌里。新世界要是只有客人的半边,LoadCr3 装进去那一拍,取指的路当场就断,机器连一声都来不及出。所以每个世界生下来,内核的半边必须整副都在。您要问凭什么客人当值的时候内核还看得见自己,谜底就是这一抄:高半是抄来的,谁上场都在。正文那一卷给这一抄留了名字,就叫那一抄,还点破了一层妙处:抄的只是 PML4 这一层,底下的 PDPT、PD、PT 一张没动,PT 是末一级,行话叫叶子,各个世界共享的还是那几张表,多花的内存一分没有。

末一步,:38,root_ 把领来的页记下,这就是这个世界的根,root 报的也是它。世界的家底从它起头,咱们往里挂页,挂的就是它。顺带交代一重保险:init 没跑成的世界,root_ 躺在零上,map、map_user 进门头一句就问根在不在,是零就回 false,translate 问同样的话,是零就回 0。没立起来的世界,挂不上页,也翻不出址,三张脸谁都不含糊。

世界立起来只是开始,往里挂页的日子还长。map、map_user、translate 三张脸,身后是同一条走查:顺着四级表一级一级往下走。缺了表,map 两张脸就地补,translate 只报零,共用的正是同一条往下的道。补表的页从哪来,归 KernelTables 管,vmm.hpp 里它的两张差事写得明白:table() 拿表页的物理址换出可写的门牌,换的法子是加一枚写死的基数,物理址加多少,门牌就是多少,这片窗行话叫直接映射窗,全本在正文《挂在墙上的地图》里,咱们借个名就行。take_page() 向名册领一枚新页,领出来照例清零再交货。三张脸的身子,咱们把 map 整个请出来,余下两张您对着看:

```cpp
bool AddressSpace::map(unsigned long virtual_address, unsigned long physical) const {
    if (root_ == 0) {
        return false;
    }
    KernelTables tables;
    return MapPage(tables, root_, virtual_address, physical);
}
```

进门头一句都问根在不在,没立起来的世界,咱们挂页挂不进,翻译翻不出。跟着造一枚 KernelTables,把表页的来路递给走查,末一行,三处各请各的脸:map 请 MapPage,map_user 请 MapUserPage,translate 请 TranslatePage。至于走查逐级怎么走、缺表时谁补谁报零,本章只借一句话,三张脸共用同一条往下的道,根是递进去的参数,机器脚下的 CR3 动都不动,给不在场的世界画图,凭的就是递进去的那枚根。细目是下一章的正戏,这儿一行都不展开。

## 客人的世界,两枚门牌起家

新世界的低半边起手是纯零,客人的家当全得一笔一笔画。眼下机器里就一位客人,shell,他家的画法在 kernel.cpp 的 :159 到 :169,首跳里咱们只认了次序,今天连图带纸一起看。头一枚门牌是代码的,kUserCodeBase 定在 0x400000,页面本体不在别处,就编在内核镜像里:链接符号 g_user_shell_start 报出它的高半住址,减掉 kKernelImageBase,物理址 0x20a000,map_user(0x400000, 0x20a000) 一发,同一页字节,从此在客人的世界里也认得门牌了,本尊 0x4dd 个字节,一枚页装它绰绰有余。栈那一页另请,:166,Pmm 分一枚物理页,:169 映到 kUserStackBase 定下的 0x500000 上,跳板接的栈顶是这枚页的页顶。两枚门牌,两页地,客人的世界开张。

地界上还有一条小约定,咱们顺手认了。客人的门牌从零起数,可最低的 64KiB,layout.hpp 给它立了常量 kUserNullTop,永不映射。C 的空指针是 0,客人哪天解引用了一个没立起来的指针,踩的就是这块注定没映射的地,14 号报得干净利落,不会拿着 0 号址真去够到什么要紧的东西。约定的全本在正文《挂在墙上的地图》里,用户半边拢共五条,本章用到的就它与天花板,余下的等用着它们的章节再认。

map_user 与 map 差在哪儿,差的那枚开关得单独摆上桌。页表项的低位里躺着几枚开关:第 0 位管在不在,第 1 位管可写不可写,第 2 位,page_entry.hpp 的注释把差事写成了英文,`ring 3 may walk through this entry`,这一级许不许客人路过,kUser 就是它。脾气是硬件给的,注释下一句也写了,一路下到叶子,每一级都得带,哪一级没带,整条路对 ring 3 就地封死。道理摆在 CPU 那头:查表每过一级,机器都拿当下的特权级验一遍权限,任何一级写着仅内核,ring 3 的脚就迈不过那一级。AddressSpace::map_user 的注释把利害交了底,`without it the page is kernel-only even in the low half`,位不带,页就归内核独享,哪怕它住在低半。当年第一遍在这儿摔过一跤,咱们把现场复述一遍:叶子把位带足了,中间一级漏了,客人在门口就吃了 14 号。这一遍把病根治在走查里,路上新起的每张中间表,出生就带着位,正文《客人的世界》里逐级验过。map_user 挂出去的每一页,四级全带位,客人才真正下得来。

画出去的地也得查得回来。translate 拿一枚虚拟门牌走一遍表,把物理址翻出来,查无此页,回 0。这个 0 眼下安安静静,咱们等会儿在停一停里跟它打交道。

## 换世界,与回家的路

世界立好了,客人的地画好了,轮到机器搬家。activate 的身子在 address_space.cpp 的 :70 到 :74,咱们整个看:

```cpp
void AddressSpace::activate() const {
    if (root_ != 0) {
        cinux::arch::LoadCr3(root_);
    }
}
```

实活就一行,root_ 不为零,LoadCr3(root_)。CR3 是控制寄存器里记根的那一枚,1.4 换长模式的时候它在场,那时里头装的是 boot 手铺那套页表的根。现役这个词,2.1 头一回用,LoadCr3 把新户口换成现役,这一章咱们请它正式当主角:CR3 里装的是谁的根,机器就住在谁的世界里,activate 换的就是这一枚。装它之前有一道纪律,中断得关着,kernel.cpp 的 :172 到 :174,cli、activate、JumpToRing3 三步连着走。担子都在高半,桩也好,桩后面的服务也好,全住在内核的门牌里,这一层镜像替咱们保着,搬进哪个世界都不慌。真怕的只有一件事:切到一半钟响,处理手续把世界又切走了,机器落在一个谁也没画全的半路上。所以 cli 排在 activate 头里,搬家那几拍,一个中断都不许插进来。跳板落地,客人旗里的 IF 自己立着,楼下照常听钟。

回家的路也得备好。vmm.hpp 的 :96 到 :108,KernelPageRoot,头注里笔者写的是 `this stays the road home`。接管落成那天,内核把自家的根存进了定格,正文那一卷给它起过这个名字。从 activate 出现起,CR3 成了变量,谁上场它记谁,KernelPageRoot() 读的永远是那枚定格。哪天想回家,LoadCr3(KernelPageRoot()),机器回到内核自己的世界。

那首跳之后的机器,现役是谁?是客人。:171 把客人的根记进任务的 user_root,记档不是白记的,proc/task_runtime.cpp 里 arm_task_entry 读它:换任务装栈那一拍,谁的 user_root 非零就装谁的根,是零,装回家的定格,内核自己人的格子里躺的就是零。装根之前它还要比对一眼:ReadCr3() 读出来跟要装的根一样,它就不重装了。所以 yield 那单的一个来回,屏上只多两行,底下连世界都没换:内核的差事,串口、键盘、调度,全借高半的镜像,在客人的世界里照常办。您回头看实况里 echo 与 yield 的来回,客人当值的整段时间,机器就停在他的世界里,一步没挪。一台机器,两个世界,这在装根前的那一眼比对里看得见,arm_task_entry 的 ReadCr3 就是那双眼。真到多位客人轮着上场的那天,换任务就是换世界,机构已经长在道上了。

**停一停,把客人的天空量一量。** 客人的地界从零往上,可它有顶。您把顶画在哪个数上、凭什么画在那儿,自己推一推?

顶画在 0x0000800000000000。layout.hpp 的 :161 立着断言,kUserTop 等于 1 左移 47 位,消息写着 `user half ends at the canonical boundary`。这个数您自己折一折,2 的 47 次方,折成平常的单位是 128TiB。x86_64 的地址名义上 64 位,规定却是 48 位真能用,高 16 位必须仿照第 47 位填满,填出来才叫规范地址,行话 canonical。第 47 位是 0,址落在低半,是 1,落在高半,0x0000800000000000 恰好骑在两半的分界上。客人住低半,顶就画在这道分界线上。还有同值两名的一对得点一句:syscall.hpp 里管岗的叫 kUserAddressLimit,layout.hpp 里管图的叫 kUserTop,两个名字,同一个数,岗上的天花板,就是地图上那道线,名字各随各家,数没有第二枚。顺带把两半的宽度也量了:中线以下 128TiB,以上到顶也是 128TiB,客人与内核一家一半,谁也不多占。

**停一停,替没有的那一页想一想。** translate 查无此页回 0,那客人真的一脚踩在没映射的址上,您说谁来接他?

咱们把现场摆开。2.3 试探那回,递给柜台的是一枚低半区里没映射的址,天花板拦不住它,岗拦高不拦空,内核自己伸手去够,挨了一记 14 号。客人自己踩空也是同一号:报告的机构 2.2 立起来了,14 号缺页会来报到,可把报告接住往下修的手续,机器眼下没安,报告到了也只能停在报告上。至于岗为什么拦得住高拦不住空,器械其实已经在这章的桌上躺着,translate 与走查,查的正是有没有。把逐字验空的活接到岗上去,是下一章的正戏。

## Code: Cinux 实况:面板四行,认两行,四单会话

机器还是首跳那一版,实况锚 r13_userspace,2.1 首跳用的也是它,切版、构建、起机的老话那边念全了,不重印。有一条老话值得再念一遍:镜像得显式重打,拿陈旧镜像开机,开出来的还是上一版的机器,面板的数对不上号,您会白疑一圈。喂键的法门,2.3 交过底,monitor 接 stdio,字母原样,空格 sendkey spc,回车 sendkey ret。起机,等面板长齐,咱们从 kernel 那行往下抄:

```text
[kern] kernel at 200000 size 90A20 entry 200040
[kern] pmm: 32316 pages free (probe 113000 ok)
[kern] vmm: address space up, magic 114514
[kern] heap: new/delete live (probe 114514)
```

四行,逐行认。kernel 那行是 1.8 拿户口本认过的交接单,个头随版本长,这一版念 90A20。pmm 报 32316,2.3 的屏上就是它,同一版机器,数不换。2.4 的 32336 与 2.5 的 32309 是别的版本各自的读数,别到这一章来找对照。vmm 与 heap 两行,2.3 那回只认过名,堆的去处当时就记在了第 3 部名下。今天开讲的是 vmm 这行,heap 那行只指个住处。vmm 行是本章的主角:address space up,内核自家的世界立起来了。boot 交接的门牌,在这一拍换成公布的地图,layout.hpp 画的图从纸面落到了机器上。magic 后头那枚 114514,屏上是什么就是什么,来历咱们没考证过,不编。heap 那行,来路就是 2.1 说的后面几卷,住处在《物理内存管理》名下:整页怎么切成零钱,new 和 delete 怎么开的张,本章不动工。

面板往下是六位任务的三场拉练,2.4 逐场读过的排场,咱们翻到收尾。从 kernel.cpp 的 :155 那声报幕起,末段屏咱们整块抄到 exit 落定,档里长什么样,纸上长什么样:

```text
[kern] launching shell in ring 3
cinux shell - type 'help'
cinux> help
commands: echo <text...>, help, yield, exit
cinux> echo two worlds one machine
two worlds one machine
cinux> yield
yielded and back
cinux> exit
```

报幕那一拍,咱们读过的手续正一件件跑:init 立起新世界,两发 map_user 画好客人的家,user_root 记档,cli、activate、跳板五条,执行流下楼。跟着的两行横幅与提示符,全出自 ring 3 里的 shell,递法 2.1 拍过,横幅一整串,一回 write 递完。

四单逐单过。help 报家当,单字命令当场应答,2.5 那版一声不吭吞掉单字的脾气是后来的事,两版别混。echo 把咱们递的话原话吐回来,two worlds one machine,一台机器,两个世界,这句话落在这一章的屏上,正好替咱们把题目喊了一遍:递话的是客人的世界,回话的手是内核的,一递一回,门上过的是同一台机器。yield 让了一圈回来,exit 敲下去,串口再无一行,这一版的壳,exit 是真退场。机器自己不会断电,收场交给 timeout,2.1 交代过的老话,不重念。

## 对照:同一件事,别的机器怎么做

咱们把 xv6 翻到第 3 章。头一段在书页 33,讲的是权限位:

> “PTE_U controls whether instructions in user mode are allowed to access the page; if PTE_U is not set, the PTE can be used only in supervisor mode.”

PTE_U 与咱们的 kUser 是同一枚开关的两家叫法:名字里的 PTE 就是页表项的缩写,位长在项上,不跟随页走,ring 3 想过,级级都得带,咱们把 kUser 带在四级上。差事同一桩,位号倒是各家排各家的,您哪天对着位号比两家的表,记得各按各家的项格式来。

第二段在书页 38,咱们看一人一表:

> “Each process has its own page table, and when xv6 switches between processes, it also changes page tables.”

换表的手势,咱们把书页 33 上紧挨着权限位的那一句请出来,它写得最直白:

> “To tell a CPU to use a page table, the kernel must write the physical address of the root page-table page into the satp register.”

把根的物理址装进 satp,机器就换了世界,咱们拿 activate 的 LoadCr3 跟它一字不差地对上,连装的必须是物理址这层细节都一样。内核在客人的世界里怎么安置,两家就分了岔,书页 39 的原话把它的安置说了明白:

> “Third, the kernel maps a page with trampoline code at the top of the user address space (without PTE_U), thus a single page of physical memory shows up in all address spaces, but can be used only by the kernel.”

它家的办法,是往客人的表里只画一页跳板,还不带 PTE_U。客人当值的每一拍里,内核的本尊不在他的表里。咱们把高半整副都画了:那一抄让每个世界出生就整副带着内核,谁上场都在。

还有一段在书页 38,咱们看它守边的姿势:

> “To detect a user stack overflowing the allocated stack memory, xv6 places an inaccessible guard page right below the stack by clearing the PTE_U flag.”

它把栈底下那一页的 PTE_U 清掉,踩上去就是缺页,守的就是栈溢出这件事。咱们在地图上走的是另一条路,栈顶上方那一页压根不画,layout.hpp 的断言原话是 `one guard page separates the stack top from user top`。一个清位,一个留空,立意是同一句:给边界配一页进不去的地。

顶上的数也值得一比,咱们把两家的顶摆在一处。它家的顶,书页 38 的原话里带着数:

> “A process's user address space starts at zero and in principle ends at MAXVA (0x4000000000) (0896), though in practice only a small fraction of this is mapped to physical memory.”

MAXVA 是 0x4000000000,256GiB,RISC-V 那套三极表的界,行话叫 Sv39。括号里那个 0896,是它家书里标到源码行的注记,咱们照原样带着。轮到咱们自家,kUserTop 是 0x0000800000000000,128TiB,x86_64 规范地址的中线,差出好几百倍。原话里的 in principle 与 though in practice 一前一后,顶是原则上的界,一位客人真用满用不满,是各家自己的事。

Linux 那头,这一章咱们交白卷。它家的内存文档没进过咱们的书架,高半从哪个数起、直接映射留多宽,咱们报不出带出处的数,宁可空着,不拿印象顶数,手册进门那天再填。

自家的窄处咱们也交底。世界退场的手续没造:销毁要把用户半边的表一层一层还回去,漏一层,那几页物理内存就永远挂在没人用的表上,正文那一卷把话说在明处,这套手续得跟进程的生老病死一起立才有意义,图纸留着等进程来落笔。一人一世界的多客人排场也一样,机制在,第一位客人在,第二位还没进门,真正的多客人戏,等客人真多起来再唱。heap 那行与拦空的岗,两笔各记在各章名下,前文都指过住处了。

C++ 在这一章出手的地方,咱们放在 layout.hpp 的尾巴上看。十二条 static_assert 把每家的基址跟自家的 PML4 项号互锁,谁想单方面挪一步,编译期就开口,人不用等机器黑屏。停一停里读过的断言就在其中,kUserTop 对着 1 左移 47 对表,消息原文就是 canonical boundary 那句。图、岗、断言,三处同数,谁也错不成谁。

## 这一章留下了什么

动身之前,咱们把手里添的家当点一遍名。一个 AddressSpace,六张脸,init 四步,名册领页、清零、高半 256 项整段抄来,末了 root 记根,那一抄是任何世界都看得见内核的谜底。一条走查,三张脸共用,根递进去,CR3 不动。kUser 一位,四级全带,0x400000 与 0x500000 两枚门牌就是它挂出来的。activate 一行 LoadCr3,CR3 记现役,KernelPageRoot 存定格,回家就装它。天花板一道,0x0000800000000000,岗上的限与图上的线同数。屏上这一章没添新行,vmm 那行 address space up,如今您知道它报的是哪一件事了。

| 术语 | 一句话口径 |
|---|---|
| 地址空间 | 一个世界的一整套翻译:自己的 PML4,高半整副抄自内核的根 |
| 世界 | AddressSpace 的共同语:想进就装它的根,想走就换回来 |
| kUser 位 | 页表项第 2 位:四级一路都带,缺一级 ring 3 就迈不过 |
| 现役 | CR3 里装着的根:谁上场记谁,activate 换的就是它 |
| 回家路 | KernelPageRoot 存下的定格:LoadCr3 它,机器回到内核的世界 |
| 天花板 | 0x0000800000000000:客人地界的顶,岗上的限与图上的线同数 |

世界立起来了,眼下在咱们手里还是一套靠软件走查伺候的图。机器自己取指访存,那四级翻译是硬件一拍一拍在走的,走错了找谁、表改了机器多久知道,下一章《分页与 TLB》接过去讲。

## 练一练

咱们留三道题,一道动手,一道猜想,一道读屏。

**题一,动手。** 您自己起一回机,面板四行照实况抄,会话四单照实况喂。抄的时候认一认:哪一行今天把报的事讲开了,哪一行只指了住处?echo 那单的回话是哪几个词?

**题二,猜想。** kernel.cpp 里给代码页挂门牌的那发 map_user,哪天手滑写成了 map,客人还下得了楼吗?落地那一拍会发生什么?您拿 kUser 的脾气推一推,再对参考答案。

**题三,读屏。** 会话四单里,您认认哪几单真穿过了柜台,哪一单进了门就再没回来?echo 那单的一递一回,两头各是谁,各在哪个世界里动的手?

### 参考答案

题一:您抄下来的四行里,vmm 一行今天讲开了,报的是内核世界立起。heap 一行只指了住处,来路在《物理内存管理》。114514 在两行里各躺了一回。echo 的回话是 two worlds one machine,跟实况一字不差,数对上了就算成。

题二:下不了。页照样挂进客人的世界,可 kUser 位没带,页就归内核独享,map_user 注释里 `without it` 那半句说的正是这个下场。跳板落地,客人在 0x400000 取指头一脚,ring 3 迈不进只有内核能走的页,14 号当场报到。当年中间一级漏了这个位,客人也是在门口摔的,病根同源:您推的是位,屏上见的是号。

题三:您数下来,help、echo、yield 三单都过了柜台,exit 进了门再没回来,handle_exit 把当值的换下场,串口从此无声。echo 的一递一回,递的是客人,他在自己的世界里把话装进 rdi 与 rsi,回话的手是内核的,柜台里一字一字落屏,借的是高半的镜像。两头各在各的世界,门上过的是同一台机器。

## 延伸

- xv6 书第 3 章(书页 33、38 与 39):本章当面引的就是这几段。PTE_U、satp、每进程一张表、guard page、页顶的 trampoline,原句都在,这几句是本章头一回当面引,您对原书核,借得准不准您说了算。
- 正文《内存当家:一页一页地精修》一卷:地图的全本在《挂在墙上的地图》,走查在《一份走查,两个世界》,那一抄与回家的路在《换世界》。教科书层把它们收拢成一副读法,您要现场,那一卷的门开着。
- 正文《下楼开张:ring3、syscall 与第一位常客》一卷:客人的世界与跳板五条在《客人的世界》,实弹那一幕在《第一跳实弹》,首跳的次序 2.1 交过底,两处对读,机理与成绩单就都齐了。
