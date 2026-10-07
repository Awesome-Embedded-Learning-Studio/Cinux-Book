---
title: 11 · 内存当家:一页一页地精修
description: "上一卷开了粗门,这一卷内存当家。把整个虚拟世界画成一张挂上墙的地图,再从 CPU 手里把页表的根接管过来:种子柜备粮、直接映射通车、帧缓冲搬家、栈进城、低半边整片拆掉。走查一份代码两个世界跑,堆把整页切成零钱、接管全局 new 与 delete,AddressSpace 让每个世界一套页表,能造、能进、能回家。"
chapter: 11
order: 0
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - memory
  - paging
  - heap
---

# 11 · 内存当家:一页一页地精修

上一卷收工的时候,咱们把话放在了明处:门的故事开的是一扇粗门,内存当家是下一卷的事——虚拟内存的管理,要用 4KiB 的页一页一页地精修,堆和地址空间的活都排着队。上一卷留下的活,咱们数了数正好三件,这一卷咱们一件一件来收。可真到了动工,咱们盘了盘家底,盘出来的现状比那三句话更扎眼:三扇门是 boot 画的,内核树里提到 CR3 的文件一个没有,交接单上也没有页表的字段。而内核天天踩着页表过日子,却不知道自己的页表在哪,改过的页表项更是一个没有。物理的那一半,名册那一卷早立了册。虚拟的这一半,咱们到上一卷为止一天都没开过张。所以本卷的活比精修两个字更彻底:读 CR3 把根拿到手,咱们在种子柜里备好表页,低半边整片拆掉——页表的活,从今往后就是内核自己的了。

接管是一场有次序的搬家。咱们画图在前:一份 layout.hpp 把虚拟世界整个挂上墙,PML4 的 256 到 259 项谁住哪家,直接映射窗的换算就一个加法公式,用户半边只立了五条约定。然后咱们动工:真相在 CR3 里,一行 movq 读出的就是根,新表页从低位的种子柜里取,不赌名册的分配顺序,直接映射按 E820 的图谱一窗一窗铺大页。帧缓冲在前一步接上 ioremap 的新门,栈从 0x90000 搬进了镜像,末了掏空共享的旧表、PML4[0] 清零、ReloadCr3 换血——低半边整片拆掉,顺序错了一步,QEMU 当场黑了屏。

接管之后页表得是一副能自己生长的骨架。走查的三件套(映射、解除、翻译)降形成纯函数模板,咱们拢共写一份代码,长凳和内核的两个世界一起跑:宿主世界拿一个缓冲池当物理内存,内核世界拿的是直接映射加名册,跑的是同一份走查。中间还藏着一整套的义务:新表出厂必须全零,unmap 之后必须补的那一枪就是作废,缺表才分 map 与 translate 的高下。骨架之上开铺子:页是最小的面额,可内核常要的是几十字节的零钱。堆把整页切开:32 字节的头、8 字节的尾巴、按地址排队的空闲链、first-fit 的挑块、分裂与合并,再接管全局的 new 和 delete,从这一天起 C++ 的动态分配在内核里是合法公民。这里头还埋着一段公案:编译器会把结果未逃逸的 new 整个删掉,断言偏偏红得莫名其妙,咱们把汇编一开,那通 call 压根没了。三次自以为修好了的爬梯爬到头,答案是把这个值送出编译单元可见的边界,顺手立起一行永久的证据。末了是 AddressSpace:每个世界一套页表,内核的半边从根上镜像,所以任何世界都看得见内核,而谜底就在这一抄。activate 换根进了场,KernelPageRoot 是回家的路。按需调页的这一段,机制咱们讲明白,而代码按住不动,等的就是第一个真用户。

<StationPanel tag="r11_heap" build="cmake -B build && cmake --build build --target run" />

<ChapterNav variant="sub">
  <ChapterLink num="1" href="01-a-map-without-an-owner" desc="三扇门是 boot 画的,内核树里没有一个文件提过 CR3,交接单上也没有页表的字段。谁告诉内核页表在哪,答案在硬件寄存器里躺着。动工前盘三件低位的事:屏幕、栈、交接单">页表还没有主人</ChapterLink>
  <ChapterLink num="2" href="02-the-map-on-the-wall" desc="layout.hpp 一张公告头:PML4 的 256 到 259 项各住一家,直接映射一个公式换算,用户半边只立五条约定不盖房子。十二条 static_assert 互锁,首版漏了符号扩展,镜像窗和模块窗两条在编译期当场被抓">挂在墙上的地图</ChapterLink>
  <ChapterLink num="3" href="03-the-seed-pantry" desc="读 CR3 一行 movq 拿到根,可新表页从哪来?从名册取页是押注分配顺序,换映射策略就炸。低位 128KiB 的种子柜全包,名册从引导路径退场。交接单归档引出一段轮子的弯路:头一个想 base,第二个想 kernel,永远别想 boot">种子柜</ChapterLink>
  <ChapterLink num="4" href="04-the-old-town-comes-down" desc="直接映射按 E820 一窗一窗铺大页,帧缓冲在前一步接上 ioremap 新门,栈搬进镜像,然后掏空共享 PDPT、PML4[0] 清零、ReloadCr3 换血。次序错一步就是黑屏。陈旧镜像差点骗走一次验收,面板亮出 magic 114514">旧城拆掉</ChapterLink>
  <ChapterLink num="5" href="05-one-walk-two-worlds" desc="MapPage 降形成 TableWorld 概念之上的模板,表怎么取、新页从哪来各世界自带:宿主拿缓冲池当物理内存,内核拿直接映射加名册,同一份走查两边真跑。档位立名 WalkLevel,零初始化是页表的出厂纪律">一份走查,两个世界</ChapterLink>
  <ChapterLink num="6" href="06-what-unmap-owes" desc="CPU 把翻译记在小本本里,表改了它不知道:unmap 不补作废等于没解除,invlpg 是手术刀,ReloadCr3 是大换血。TranslatePage 与 map 的全部差别只在缺表怎么办,大叶的偏移自洽在档位里。1GiB 移位写反和一条测了没装的断言,两案俱破">unmap 欠的那一下</ChapterLink>
  <ChapterLink num="7" href="07-cutting-pages-into-change" desc="64 字节的结构体不该占一整页。32 字节头记身家,8 字节尾巴记跨度供回走,空闲块按地址排队,first-fit 挑块、够大就分裂、还回来两边合并。三个真 bug 全让长凳按住:Ceil 数的是单位数,漏记的 40 字节,忘了标闲的头">把页切成零钱</ChapterLink>
  <ChapterLink num="8" href="08-pages-in-blocks-out" desc="heap_runtime 是懂页的半边:保护页之上 64KiB 开张,不够吃就经走查按页扩容、多带一页防头部挤兑,再 grow 重试。全局 new 四件薄转发进堆,失败走 Check 急停——内核里分配失败没有体面的恢复路。面板亮出 heap 行">页进,块出</ChapterLink>
  <ChapterLink num="9" href="09-the-vanishing-call" desc="扩容案断言红得莫名其妙,反汇编一看 operator new[] 的 call 根本不存在:结果未逃逸的 new 连同读写会被整个删掉。指针比较被内存模型公理折叠,整数化只解一半,唯一可靠的出口是把值印出编译单元——证据行连弄丢的 delete 都抓得住">消失的那通 call</ChapterLink>
  <ChapterLink num="10" href="10-switching-worlds" desc="AddressSpace 一个世界一套页表:取一页、清零、内核半边从根上镜像——为什么任何世界都看得见内核。map 拿根当入参,与在跑的 CR3 无关。activate 换根进场,KernelPageRoot 是定格的回家路。按需调页讲明白机制、代码不动,第一个真用户在用户态">换世界</ChapterLink>
  <ChapterLink num="11" href="11-audit-and-the-noteless-days" desc="面板十行逐字对数,pmm 落在 32339,141 页一笔笔核。长凳 22 件,机器里六个测试内核各 0.3 秒。考古箱翻到四月二十:三件事挤在同一天写完,那一段没有任何笔记,当年的调试现场是仅有的第一手">验收,和没有笔记的日子</ChapterLink>
</ChapterNav>

机器如今算是站在了咱们画的地图上:半兆的名册记着物理的家底,空闲剩了 32339 页。一副活页表管着虚拟的城,映射、解除、翻译这样的活,都是它自己动手干的。堆也开了张,开机第一发 new 已经交了作业。下一卷的差事归进程:进程对象从堆里 new 出来,自己的世界造好了等着,调度的一声响,切进去当家的就是它。
