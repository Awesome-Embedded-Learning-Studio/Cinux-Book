---
title: 03 · 缺页与按需分页
---

# 03 · 缺页与按需分页:一册许诺,四个号,五问补一页

同一拍 exec,两本记录摆出来对不上,咱们就拿它开场。装载器往页表里真挂出去的栈,只有顶上那一页。登记在册的栈段,却是从栈顶往下整整一兆字节,底下还多押了一页。两本记录差着一整段,而且差得心安理得:段是许给程序的,顶页实铺上,好让进场有处落脚。底下的兆字节,谁走到谁来领。这个心安理得,就是今天这一章的章程。

这一章欠着两处的愿,进门就得认。2.2 的桩报告完就 Halt,当的是遗言,当时留了话:能修好路再回去的故障处理,得等缺页的正戏开锣。3.1 咱们也问过,客人真的一脚踩在没映射的址上,谁来接?当时的答案诚实:把报告接住往下修的手续,机器没安,报告到了只能停在报告上。3.2 收卷又把两件事记到了本章名下:踩空的脚谁来接。向名册领走的页,来路还悬着。锣到今天敲响,两问这一章一起答。

活数下来三样。头一样,立懒章程:给予拆成两拍,登记那一拍许诺,缺页那一拍兑现。再一样,认两件新家当:登记册,每个任务一本,记虚拟地段上的许诺。四个号,mmap、munmap、mprotect、brk,程序递申请的窗口。末一样,把缺页的裁决从头走一遍:五问过堂,补页三步,栈往下长的路,写时复制那枚记号的归位。量法照旧三条:码,kernel/mm/ 底下 vma.hpp、vma.cpp、demand.cpp、vma_diag.cpp 当主角,sys_mmap、sys_exec、loader.cpp 配角进场。屏,起机面板、mapper 会话、机内自测、宿主测试,实况锚 r19_lazy_alloc_page。书,《深入理解计算机系统》书页 784、806、807,xv6 的书页 35 在第 3 章,书页 51 在第 5 章,原话整段录进对照节,您对着书核。

> **缺页的入口凭什么敢信一册记录?一册记录又凭什么敢自称许诺?**
>
> 凭条目自己报的身份。笔者翻 vma.hpp 的时候,在 VmaRegion 的头注里撞见它自己报的:`One recorded promise: a half-open range plus its terms`,一枚登记在册的许诺,半开区间加条款。成员 start 的注是 `First byte the promise covers`,许诺盖住的头一个字节。代码自己的注释,把许诺两个字写在了纸上。咱们这一章,就给那句注作注:许诺怎么立,怎么查,怎么兑现。

## 一拍许,一拍兑:懒章程

咱们从机器勤快时的样子看起。装载器把每个段的每一页都提前铺好:领一帧,整页清零,镜像里的字节从头铺到自己的份上,这是《内存当家》立下的页的纪律,纪律没有错。栈进场时领一页,应付 shell 敲命令够用。堆在册上种下的是零长的一段,一页都没有。这样的机器,跑 hello 和 shell 是体面的,可它伺候不了真程序:声明要用十兆字节的程序,从进场到退场真摸过的角落往往就几百页。配置扫一遍就再不碰。栈浮在顶上几页,深递归那几瞬才往下探。进场那一拍把十兆字节老实铺满,铺进去的字节绝大多数在等一个永远不来的访问,而物理内存是全机最紧的家当。

所以这一章教懒。咱们把懒章程立成三个词,后头每一节照它办。用什么:登记在前,登记在册的才给。到什么:哪页用到了补哪页,补的尺寸永远是正好一页。不用的不占:册上记十个区段不要紧,没碰过的页一片不发,物理内存只花在真发生过的访问上。三个词连起来,懒是把给予拆成了两拍:登记那一拍许诺,缺页那一拍兑现,两拍之间隔多少时间多少页,机器都等得起。

这门手艺有正经名目。《深入理解计算机系统》第 9 章在书页 784 给它落的定义,咱们整句录来:

> “The strategy of waiting until the last moment to swap in a page, when a miss occurs, is known as demand paging. … all modern systems use demand paging.”

等到 miss 那一刻才把页搬进来,这叫按需分页,所有现代系统都这么过活。教科书在这件事上没有流派之分,咱们照办,办得朴素:树形的册、成批的补,那些大件,几十条区间暂时用不上。懒真正图的是快。当年工程线量过一笔:装载从整份铺好改成按需补页,工程线上最大的一份用户程序,编译器的前端,exec 从盘上请进内存的那段路,二十二秒归了零。二十二秒去了哪?整份读盘整份铺页的工夫,被拆散成了千百次一页的小事,而绝大多数页压根没人来要。快的数咱们认当年那一笔,这一遍不另计时,本章收卷拿实弹对功能。

## 一枚条目,一句许诺:登记册

册子长什么样,咱们端一条条目出来。一段用户地址,三件事记全。从哪到哪:半开区间,含头不含末,这样的边界两段挨着也不打架。许做什么:读、写、执行三问各占一个布尔。背后站着谁:三态挑一,匿名的就是一张白纸,补页补零。文件的记下文件节点和页偏移,补页照着去读字节。栈的脾气是往下长,别的态没这脾气,单独算一态。条目前半截的原身在此:

```cpp
struct VmaRegion : base::container::SelfNode<VmaRegion> {
    unsigned long      start{};        ///< First byte the promise covers.
    unsigned long      end{};          ///< One past the last covered byte.
    VmaKind            kind{};         ///< What stands behind the bytes.
    bool               readable{};     ///< Loads through this range are promised.
    bool               writable{};     ///< Stores through this range are promised.
    bool               executable{};   ///< Running code from this range is promised.
    fs::FsNode         backing{};      ///< The source node when kind is kFile.
    const fs::FsOps*   file_ops{};     ///< The backend face that serves backing's bytes.
    void*              file_self{};    ///< The backend instance behind file_ops.
    unsigned long long file_offset{};  ///< File byte matching start, when backed.
};
```

条目里文件侧那三件,咱们把分工多说两句。backing 记的是文件的身份,按值整个抄进来,册子从不拿它去解引用。为什么不肯存指针,是当年的亏换来的:工程线的页缓存拿指针当过键,对象腾挪之后,新对象恰好住进旧地址,缓存里的旧记录还认这个门牌,一查吃到别人的字节。这一遍身份一律按值抄,条目认的还是从前的它,哪怕背后的对象换了住址。把身份变成字节,靠旁边那两件:file_ops 是后端的脸,file_self 是后端的实例,补页时读文件页,走这一对。

册子的形状拍得不花哨:按到达序排的单链。您要是见过红黑树版的同类件,别嫌朴素,这是有意的。一个任务的区间数来数去几十条,线性扫一遍就是诚实的代价,换来没有排序要维护、没有再平衡要教,查一回连一页堆的分配都不领。区间成千上万的年月再教树不迟,那不是今天几十条要愁的事。两道家法立在登记的门上:重叠的拒收,不沾合并的边。空区间和倒区间,起点压过终点的,连门都不进。exec 铺出来的段天然不搭界,mmap 发出来的地段互不重叠,谁拿一段跟册上重叠的地来,报的只能是调用方的毛病,错原样报上去,不悄悄替人圆。册子干净了,查册才敢把话说满:册上没有,就是真的没有。这话现在听着平常,等会儿判词敢不敢说满,全押在家法上。

册跟任务的缘分,咱们两头交代。exec 那头,新册在活册旁边另造,装载器往新册里报段,报成了才换进任务:旧世界的页逐叶清还,旧册被 ranges.release() 顶掉,就地丢弃,没走析构,小漏一笔,咱们照实记。报不成,一切退回,活着的程序毫发无伤。fork 那头是深拷:孩子把父亲的册整本抄走,抄到一半抄不动,已抄的全退,要的是全有或全无。父子写同一段地址会分家,那一幕正文《生与收》那一卷拍过全本。分家靠页表那一层,册这一层,父子各揣一本,各管各的。册立起来马上接一件真活:替缺页断案当证人。裁决者看完自己不受理的地址,转储之前多问册子一句,这一地址落在谁家。判词住 vma_diag.cpp,两种回答:落在册上的,那一段底细全亮出来,起止、三态、访问是读是写,一行看完。落在册外的,第二句判词整行抄来:

```text
[kern]  vma: addr=%X sits in no registered range; a true segfault
```

真越界,程序自己的错,串口上这一行就是第一份口供。咱们顺手把两位册子对个表:1.7 的名册,一位管一页,管物理的地。本章的登记册,一条管一段,管虚拟的地。名字差一个字,管的差一个世界。补页的时候两位才握手:登记册说该补,名册发帧。

## 窗口开张,一页不发:四个号

册立在任务里,程序还够不着,够得着的路只有系统调用。四个号在 syscall.hpp 里是四枚枚举:kMmap 坐九号,kBrk 坐十二号,九号和十二号跟 Linux 的一样。kMunmap 十号、kMprotect 十一号,咱们的表上换了座,Linux 的表上正好倒过来,这一对的座次债,正文《把后半场交给解释器》那一卷才换回,这儿记下不重叙。为什么照人家的号位,咱们守的是老理由:往后跑别人编好的程序,人家一发 syscall,号对上了就是咱们的门。

咱们跟着 mmap 进门看它办事。头一件,长度进页凑整,零长的请回。第二件找空档:mmap 区在栈底下、分界上,从顶往下长,候选地盘的屋檐从顶起步,册上哪段挡在路上,屋檐就压到那段更下方。一整圈压不动了,地盘才算真干净。这件家什的原身,vma.cpp 的 find_free_spot,整段端出来:

```cpp
base::Result<unsigned long> VmaStore::find_free_spot(unsigned long ceiling,
                                                     unsigned long bytes) const {
    if (bytes == 0 || ceiling < bytes) {
        return base::KernelError::kOutOfMemory;
    }
    unsigned long spot_end = ceiling;
    for (bool moved = true; moved;) {
        moved = false;
        if (heap_.start < spot_end && heap_.end > spot_end - bytes) {
            spot_end = heap_.start;
            moved    = true;
        }
        for (const VmaRegion* scan = regions_.head(); scan != nullptr;
             scan                  = regions_.after(*scan)) {
            if (scan->start < spot_end && scan->end > spot_end - bytes) {
                spot_end = scan->start;
                moved    = true;
            }
        }
    }
    if (spot_end < bytes) {
        return base::KernelError::kOutOfMemory;
    }
    return spot_end - bytes;
}
```

spot_end 是候选地盘的屋檐,起步在上限 ceiling,咱们要的是屋檐底下正好 bytes 那么长的一截。压的动作两处:堆跟候选搭上边,屋檐压到堆的起点。链上哪段搭上边,屋檐压到那段的起点。搭边的判法两头同一句:段起点落在屋檐之下,段终点又伸过候选的底,两边有了交集就算搭上。外头那层 for(bool moved) 是不动点循环:这一圈压动过了,把压下去的新位置再验一遍,说不定又跟更下面的段搭上。一圈下来一寸压不动,循环退出去,交回 spot_end 减 bytes,半开区间的起址。整个找法纯算术,册上扫一圈,分配的影子都没有,所以长凳上的测试世界也跑得动。堆为什么头一个被比?它不上链:链上条目要有起有止,堆生下来长度零,空区间进不了链,它骑在册子自己的成员上。第三件按旗标分两路:匿名的,册上添一条匿名条目,完事,一页不发。文件的,查描述符在不在、活不活,再翻保护位,这一版的文件映射只做只读,映射了还想写的,EINVAL 请回。

munmap 干摘除,mprotect 干改词,一样都不做劈半的事:munmap 摘的必须是完整落在给定范围里的条目,半截搭边的拒。mprotect 改的必须是一整段权限,半截的也拒。页表那头,munmap 一页一页地走,3.2 备下的 UnmapPage 等了这么久,头一个真消费者在这儿上岗:摘下的页还帧,还帧之后紧跟一声作废,invlpg,3.2 立的义务照章补上。次序有讲究,册动在前表动在后:册上不认了咱们再摘表,摘的当口缺页打进来,一查册查了个空,当场拒回。不会出现册上还有、页上已无的悬空瞬间被当成合法访问放过去。mprotect 还特意留了半截:册上当场改,页表上的位眼下不动,已映射的页还揣着昨天的表项。您要问这算不算言行不一,算,而且是故意的:改词要收全,得等缺页裁决的新业务开张才补得上。补的那一拍,册上的词是承诺,页表跟在承诺后头走。这半截怎么合上,下一节见。

老词新开的 brk 单独说,脾气跟前三个不同。堆的家当 exec 时种下:起点是程序影像的末页进上去,顶头是 layout.hpp 画的分界 kUserBrkMmapSplit,0x0000400000000000,堆到这儿为止。brk 的语义照 Linux:传零是问询,回当前断点。传目标地址,对齐到页的边界,夹在起点与分界中间,断点挪过去。注意咱们没说 map 也没说 unmap:brk 动的只是指针,断点挪上去,新地盘在册上有名,页的事等缺页来了再说。收缩同样只动指针,铺出去的页不急着摘,等进程退场,整个世界连册带表一起清算。还有一个脾气:brk 返回地址,错误码不返,目标落了空,返回的还是当前断点,让调用者自己比对出没挪动,这是 Linux 的老脾气,咱们原样学来。四个号开完,咱们盘点真发出去的页:零。册上添了行、摘了行、改了词,断点挪了位,物理内存的占用纹丝没动。窗口开了,兑现另算。

## 五问过堂,补页三步

咱们把正主请上台。demand.cpp 的文件头注写着 `One fault, one promise, one frame: the fill itself`,一次缺页,一句许诺,一帧,补的就是它。缺页的错误码自带口供:第一位说这错是整页没映还是权限不对,第三位说挨错的是客人还是内核。裁决的判定链如今是两段:头一段是写时复制的老脸,写路打头试,fork 出的父子合用一份帧,页表上带着 kCow 记号,谁写了谁挨缺页,裁决者见着记号才接案,走分家或者复权,全本在正文《生与收》那一卷。老业务说不受理的,新业务接手,问案顺序原身在此:

```cpp
    if ((error_code & cinux::arch::isr::kFaultFromUser) == 0 ||
        (error_code & cinux::arch::isr::kFaultProtection) != 0 || address >= kUserTop) {
        return false;
    }
    const VmaRegion* const kRegion = ranges.find(address);
    if (kRegion == nullptr) {
        return false;
    }
    const unsigned long kPage = cinux::base::math::AlignDown(address, kSize);
    if (kRegion->kind == VmaKind::kStack && kPage == kRegion->start) {
        return false;
    }
    KernelTables       world;
    const walk::SlotAt kSlot = walk::FindSlot(world, space.root(), kPage);
    if (kSlot.entry != nullptr && kSlot.entry->has(cinux::arch::page::kPresent)) {
        return false;
    }
```

五问挨个认。第一问,来自用户吗:错误码第三位是 ring 3 的印记,内核自己的缺页这一路不接。同一行还夹一判,地址过了 kUserTop,天空以上册子管不着。第二问,保护性的错吗:第一位立着的,那是权限的事,写路试过,不接,剩下的就是册和表都说不行,婉拒,死法是转储里一行判词。第三问,册上有这段吗:查册查空,就是真越界,接它的是上头那句 a true segfault。第四问,栈底那页吗:栈区间的头一页永不映,谁碰谁响,这页等下单独说。第五问,页真没映吗:FindSlot 走到叶表,槽里那项的现在位翻没翻。这一问看着多余,学费就交在这问上。判已映未映,手里有件现成家什,3.2 的走查件,走到叶表交一个指针。首版的判断写的是指针非空即已映,机内测试头一轮,十案七绿三红。三红根子分了层。头一层在走查件的语义:它走到叶表,哪怕碰到的是零叶槽,交回来的指针也非空,因为叶表本身在了,它给的是槽位的地址,不是槽里那项的生死。同一张叶表里的第二页,头一回缺页进了门,槽位记的是零,指针非空,首版判成已映,拒错了,程序白白死给咱们看。第二层在用例的根子上:堆的用例种下堆之后没挪过断点,册上堆区间长度还是零,任何地址自然查不中,挪了断点再缺页才是完整的戏。第三红红得冤:栈的用例期望写错了一处地址,对齐进页之后落点恰好是栈底那页,被拒是正确行为,错的是期望。三处各自修对,十案全过。当年首跑与今日的屏,实况节里今昔分开念。

**停一停,FindSlot 交回来的那枚指针,您认一认,它管保了什么,没管保什么?**

咱们把说明书念准。管保的:叶表在了,槽找到了,指针指的就是您要找的格子。不管保的:格子里那项活不活。指针非空说的是路走通了,不是页在着。页在不在,翻的是槽里那项的现在位。首版把前半张保单当成了整张,头一案红就红在这。判已映的手续从此固定:查位,不查指针。家什交回来的语义要读它的说明书,别干望文生义的事,这句当年笔记里的原话,咱们化来当家法。

过了五问,补页本体三步,咱们跟着一页走。头一步领帧:物理内存发一帧,发帧的是 1.7 的名册,allocate_page 一声,一页物理地到手。第二步整页清零:匿名页的出身就是零。文件页的短读尾巴也是零,bss 的答案提前写好,程序数据段里没初始化的全局变量该是零,道理落在这儿。第三步按三态分岔:匿名的到这儿完了,零页上表。文件的,把这一页对应的文件字节,经登记册上记的后端面读进这一帧,读多少算多少,文件尾巴短的,余下字节保持零,正好是内存大于文件那一截该有的样子。后端那头的块缓存,今天只是一座还没摊开的柜子,正戏归《Page Cache 与写回》那一章,咱们路过不开柜,一句带过。叶项的组装也换了脸:上表的件收一个现成叶项,谁调谁组装,册子说这段许写,叶项才带写位,栈段记的是许写,补出来的栈页就带写位。上表之后紧跟一发作废,invlpg,3.2 认过的义务,这儿照章补上。段权限的旧债也顺手在这儿清:装载器从前把用户页一律映成可写,ELF 段自己声明的旗标没人理,如今程序头里读、写、执行三位,装载时翻进册子,loader.cpp 里三条布尔对着段旗标各翻各的,写位再翻进页表,数据段是数据段的法,代码段是代码段的法。执行位有个如实的交代:它记在册上,页表那边的执行限制要动 EFER 里的开关,那是另外一件事,正文《安全》那一卷管它,这一章不越界。

栈的故事里最提气的一笔,咱们单独说。exec 那一拍,sys_exec 把册种下去:栈段从 kUserProgramStackTop 往下整整 kUserStackBytes,一兆字节,再多押一页,段的起点就压在一兆字节的最低处再往下一页。真铺的只有顶上一页,领一帧,挂上表,进场用的栈顶从它起。往后递归一层层往下压,压过真铺那页,缺页进门,查册,在栈段的,往下补的永远是一页,压了补,补了压。压到一兆字节的底,再往下一步,接它的就是脚下押的那页:判定链里 kPage == kRegion->start 拦的就是这一步,第四问挡下,响亮地死,串口一行判词,死因清清楚楚。这页永不映的地,手艺跟正文《页进,块出》那一篇同款,名也同款,叫保护页。

**停一停,栈的头上脚下,各押着一页进不去的地,您分分看,各拦的是哪种越界?**

咱们把两页摆开。头上那页,3.1 对照节里量过:栈顶上方那一页压根不画,layout.hpp 的断言原话是 one guard page separates the stack top from user top,拦的是往天花板那头窜出去的脚,程序的地界到栈顶为止,再上一步就踩空。脚下那页,本章 exec 押下的这页,站在一兆字节的底,拦的是递归往下压、要把栈段压穿的脚。两页还各带着一层暗活:头上那页画在册外,谁也发不走它。脚下那页登记在册,占着地,找空档的屋檐就永远不会把别人的地段发到紧贴栈底的位置。没有脚下这页,mmap 的地盘恰好贴着栈底安家的话,压穿的脚就写进别人家里,一声不吭。

还有一枚记号,咱们看着它到日子了。kCow 是页表上的写时复制记号,父子合用的页带着它,裁决者见着才接案。当年引擎把它留在分了家的私页上不摘,凭的是可写与可共享并存,碍不了事。这约定到 mprotect 补齐那一拍到期:改页表要判断这页跟不跟人共享,共享页的写位不能上,而页表里唯一的共享记号就是 kCow,私页还留着它,会被当成共享的,写位就收错了。所以这一批把分家后的私页摘干净:kCow 只标真共享的页,册、页表、帧计数三本记录说同一个口径的话。mprotect 欠的言行一致,这儿还上:册改完,表逐页跟上,共享页保只读,私页按新词,下一回缺页来办事,照的还是新词。到这儿懒章程两拍全通:登记当许诺,exec 种册,mmap 添行,brk 挪断点。缺页当兑现,过了五问,补一页,作废一声,挨那一记的指令回头重跑,戏接着演。2.2 留话里的急诊,这一章开张了。

## Code: Cinux 实况:面板三行,mapper 四幕,机内十案加十四案,长凳二十案

锚 r19_lazy_alloc_page,切版重打镜像的老话前几章念过,起机,面板上咱们认三行:

```text
[kern] kernel at 200000 size 4A07A0 entry 200040
[kern] pmm: 31272 pages free (probe 116000 ok)
[kern] heap: new/delete live (probe 114514)
```

空闲页从 1.7 面板上的 32350 走到 31272,内核的个头也长了:中间隔着正文十几站攒下的家当,盘上的字也进了内核,差的构成不复算,数认到差值为止,各站对面板走的都是这个旧例。heap 那行,咱们只认数不认来路,本章收卷有交待。

实弹咱们准备了 mapper 的独角戏,它住在盘上的 /bin 里。run /bin/mapper 敲下去,四幕自己往下走,会话从串口原文合录,档里什么样,纸上什么样:

```text
cinux> run /bin/mapper
anon mmap: ok
anon write+read back: ok
mprotect read-only: ok
read after mprotect: ok
munmap: ok
brk ask: ok
brk stretch: ok
heap pages on demand: ok
brk restore: ok
open motd: ok
file mmap: ok
xmm0-xmm7 survive file fault: ok
read motd: ok
mapping agrees with read: ok
close motd: ok
xmm0-xmm7 survive syscall: ok
parent retains xmm0-xmm7: ok
child inherits xmm0-xmm7: ok
wait for xmm child: ok
deep stack press: ok
mapper: all green
[3] exited with 7
cinux> exit
[kern] kernel_init: shell exited with 0, respawning
cinux shell - type 'help'
```

这一屏咱们头一遍按懒章程念。第一幕匿名:mmap 三页起步,写一个字母读回来,写到了页才补,补的是零页。改只读再读,改词当场生效。munmap 摘掉。第二幕堆:问断点,伸五页,半页一格写读,断点收回去,断点伸的那一拍一页没发,写到的拍才补。第三幕文件:打开烤在盘里的两行开机问候,只读映射,登记完事,头一次读,经后端把盘上的字请进内存,read 读回的字节跟映射里头四十个字节逐字节对拍。第四幕深栈:二百五十六层递归往下压,压过 exec 真铺那页,一路压出三十几页,全靠缺页一页一页地接,deep stack press: ok。文件幕里一行、幕与幕之间四行 xmm 报数,是同批里浮点验收的活,与本章的懒章程无关,咱们不念。末了退出码那行 [3] exited with 7,照屏抄,不解释。exit 敲下去,init 把 shell 又拉起来,提示符回来了。

补页的十案在机内,ktest 镜像起机即跑。屏上 RUN 行与 PASS 行文字一字不差,咱们只抄 PASS 行,RUN 行省去不抄:

```text
[ktest] kernel test world alive, 7 e820 entries

=== Cinux Kernel Test Runner ===
Running 10 case(s)...

[PASS] demand: faults nobody promised are declined, kernel-side included
[PASS] demand: the guard page and the sky above the stack decline
[PASS] demand: the stack fills downward toward its guard
[PASS] demand: a file promise without a backend face declines
[PASS] demand: a file tail past the bytes stays zero
[PASS] demand: a file-backed fault reads through the backend
[PASS] demand: the heap span fills, past its end declines
[PASS] demand: a read-only promise fills without the write bit
[PASS] demand: a present page and a foreign promise decline
[PASS] demand: an anonymous write fault earns one zeroed page

=== Results: 10 passed, 0 failed ===
```

咱们把十案对着五问认:头一案,没人许诺过的缺页拒,内核侧的也包括。第二案,栈底保护页与栈上天空一起拒。第三案,栈往下填。第四案,没有后端面的文件许诺拒。第五案,文件尾巴余下字节保持零。第六案,文件页真经后端读。第七案,堆段填得进、过了顶拒。第八案,只读的许诺补页不带写位。第九案,测试源里写的是同一段 16KiB 的许诺:已映的首页再缺页,第五问拦下,翻译一动不动。没映的邻页缺页,照常受理,另领一帧,跟首页那枚不是同一帧。案名只报了拒的那半,咱们两半都认。第十案,一次匿名写缺页,赚一页零页。当年首跑七绿三红,三红的根子上头讲过。今日这屏十案齐绿,今昔两笔分开记。

册子那件的两场,档里全录,咱们这儿只走要点。机内十四案:找空档两案,屋檐从 mmap 区顶起步、压过拦路的段,比一切缝都宽的找不着。断点夹在地板与分界中间。克隆在内核堆上另立一册,与父亲各管各的。重叠拒、挨着收。空区间倒区间拒。界外查空,册内起止边界上查得中。长凳上的宿主版,同一份册子码,在咱们开发它的电脑上直跑,档拢共三行,原样全抄:

```text
[PASS] vma: max_end reports the highest recorded end

=== Results: 20 passed, 0 failed ===
```

二十案全过。档里只录了案头一行和末行的总数,中间十八案的名单没进档,咱们不补造。还有一条如实交底:补页那件没有宿主版。册子是纯算术,长凳跑得动。补页要名册发帧、要页表、要真异常进门,这几样只有机器里有,咱们不硬造。

## 对照:同一件事,别的机器怎么做

咱们把 xv6 翻到第 5 章开头,书页 51 一句立场:

> “xv6 uses page faults for only one purpose: lazy allocation.”

它家把缺页用在一件差事上,懒分配,跟咱们这一章干的是同一件。分岔在开关上,书页 51 讲得直白,sbrk 得带 SBRK_LAZY 旗标才走懒路:

> “First, when an application asks for memory by calling sbrk with the flag SBRK_LAZY, the kernel notes the increase in size, but does not allocate physical memory and does not create PTEs for the new range of virtual addresses.”

书记下增量,不发物理内存,不建表项。它家原话是 The kernel implements lazy allocation transparently to applications,懒的机关对应用程序透明。可进不进懒门,xv6 还得程序自己递旗标。咱们的 mmap 和 brk 生下来就是懒的,没有开关。同一条懒路,一家把选择权交给程序,一家把默认值定在懒上,咱们跟后一家。代价两家都认:缺页自带一次用户与内核的来回,攒批补页能摊薄,它家没做,咱们的章程眼下也一页一页。

守边的姿势,咱们把三家对举,看得最清楚。xv6 的内核栈,书页 35:

> “The guard page’s PTE is invalid (i.e., PTE_V is not set), so that if the kernel overflows a kernel stack, it will likely cause a page fault and the kernel will panic. … A panic crash is preferable.”

栈底一页表项直接无效,内核栈溢过它,缺页,panic,宁可崩得响,不愿悄悄写花别人的内存。咱们的内核栈走另一路:栈底埋一句魔数,溢过了它机器照跑,验尸翻栈才看得见魔数被踩,是死透了才响的哨兵。本章的用户栈保护页是第三路:踩到那一拍缺页就进门,判词当场落屏,当场就响的警铃。同一件事,三种安法:它家安在表项上,咱们内核侧安在字节上,用户侧安在册子上。xv6 用户栈底下也压着一页,那一段 3.1 对照里引过,您翻回去对读正好。

轮到《深入理解计算机系统》。书页 806,它家给 Linux 的地段条目列字段:vm_start、vm_end、vm_prot、vm_flags、vm_next,五个名字咱们个个认得,起、止、三问权限、共享还是私有、链上下一条,咱们的 VmaRegion 逐项对得上,私有共享那一件咱们记在帧计数与 kCow 上,不在条目里。书页 807,缺页处理第一步,原话录来:

> “1. Is virtual address A legal? In other words, does A lie within an area defined by some area struct? To answer this question, the fault handler searches the list of area structs, comparing A with the vm_start and vm_end in each area struct.”

拿地址对每段的起止逐段比,这就是咱们的第三问,连比法都一样。它家紧跟着自己补了一句:顺序扫太贵,实践中 Linux 在链上叠了一棵树。咱们几十条区间的诚实,正好接在这句话的前半截上。后半截的树,等册子真厚起来再说。Linux 本尊那头,这一章照旧不描:它的缺页路数怎么走、VMA 树怎么养,咱们书架上还没有一份能引的材料,这一页留着,不拿印象垫,能引的书进门那天,咱们回来落墨。

咱们把自家的窄处交底两件:文件映射只做只读,映射了还想写的,EINVAL 请回。mprotect 和 munmap 都不做劈半的事,半截搭边的范围一概拒,想只动半段,调用方自己把段分开。C++ 在这一章出手的地方就落在登记册的单链上:VmaRegion 自己就是链结点,侵入式链,链长在条目自己身上,登记不领一页堆,摘除也不还,查一回零分配,几十条朴素区间的全部便宜,落点在这行继承上。三态用 scoped enum,枚举名 kAnonymous、kFile、kStack 带 k 前缀,值域写死在类型上,switch 漏一个分支,编译器就开口。

## 这一章留下了什么

咱们收卷,点名。一部懒章程:给予拆两拍,登记许诺,缺页兑现,不用的不占。一本登记册:半开区间,三问权限,背后三态,按到达序的单链,两道家法,exec 造新册,fork 深拷,判词两句敢把话说满。四个号:mmap 和 brk 正戏,munmap、mprotect 简笔,窗口全开,发页为零。一条裁决链:五问过堂,补页三步,领帧、清零、按册上写位。栈往下长一兆字节,脚下押一页保护页。kCow 只标真共享。五份实况:面板、mapper 四幕、机内补页十案、机内册子十四案、长凳二十案,面板之外案案都过。

| 术语 | 一句话口径 |
| --- | --- |
| 登记册 | 每个任务一本:半开区间加三问权限加背后三态,册上没有的就是真的没有 |
| 懒章程两拍 | 登记那一拍许诺,缺页那一拍兑现,中间隔多少页机器都等得起 |
| 四个号 | mmap 九、munmap 十、mprotect 十一、brk 十二,程序递内存申请的窗口 |
| 五问 | 用户、保护性、查册、栈底、真没映,补页前的过堂 |
| 补页三步 | 领一帧,整页清零,按三态分岔:匿名零页,文件页经后端读 |
| 保护页 | 栈段脚下永不映的一页,压穿栈的那一步,当场响 |

两桩悬案到这儿交割。3.1 埋的旧案结了:报告不再是遗言,缺页进门,裁决者接案,过了五问补一页,指令回头重跑。页从哪来也答了一半:补页三步头一步领的那帧,出处是 1.7 的名册。名册一位管一页,管到 4KiB 为止。面板上 heap 那行悬着的旧话、把整页细成零钱的铺子、堆的来路,这些咱们点名转递,记在《物理内存管理》名下,那一章开门等咱们。

## 练一练

题一,动手。您自己起机,敲 run /bin/mapper,把四幕的报数照实况抄一遍。抄完认一认:哪几行报的是登记了没铺页的戏?deep stack press 那一行,压出去的是多少页?

题二,推演。同一张叶表里的第二页,头一回缺页进的门,首版的判断为什么把它判成了已映?您拿 FindSlot 交回来的东西推,再对参考答案。

题三,推想。断点伸五页那一拍,机器里空闲页少几页?第二幕走完,又少几页?您拿懒章程三个词推,再对参考答案。

### 参考答案

题一:登记了没铺页的戏,咱们认下来。第二幕的 brk stretch 与 heap pages on demand 都算,断点伸五页那一拍零页,写到的拍才补。第三幕 file mmap 也是登记完事,头一次读才进内存。深栈二百五十六层压下去,exec 真铺的只有一页,一路压出三十几页,全是缺页一页一页接出来的。

题二:您看,FindSlot 走到叶表,交回的是槽位的地址:叶表在了,指针就非空,哪怕槽里记的是零。第二页与头一页共用一张叶表,头一页补过,叶表已经在了,第二页头一回缺页进门,槽位还是零,可指针非空,首版拿指针非空当已映,把还没补的页判成了在,拒错了。判已映要查槽里那项的现在位,不查指针。

题三:咱们推到头,断点伸五页那一拍,少的页是零:brk 只动指针,一页不发。第二幕走完,半页一格写读,五页全摸到,补了五页,空闲数少五页。零与五之间,隔的正是懒章程的到什么:用一页,补一页。

## 延伸

- 正文《懒到家:从登记册到补页》一卷:登记册、四个号、缺页的裁决、验收,全本都在卷里,咱们把它们收拢成一副读法,您要现场,卷门开着。正文《生与收:fork、execve 与收尸》一卷收着写时复制记号、分家与复权的全本,还有 exec 换世界的手续。
- xv6 第 5 章开头,书页 51:lazy allocation 的立场句和开关句都在。内核栈保护页那句在第 3 章,书页 35。用户栈保护页 3.1 引过,翻回去对读正好。《深入理解计算机系统》第 9 章书页 784、806、807 是定义句、字段表、缺页第一步,本章引的就是这三处,您对原书核。
