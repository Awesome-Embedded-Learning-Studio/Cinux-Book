---
title: "13 · 下楼开张:ring3、syscall 与第一位常客"
description: "调度器开张了,队列里却全是内核自己人,这台机器还没跑过一条不属于内核的指令。这一卷请客:GDT 添座位、TSS 备锅、MSR 打工具、GS 安家,页表画出客人的世界,第一跳把执行流放进 ring3。SYSCALL 与 SYSRET 立起柜台,编号、寄存器、负 errno 三条约定一次定死贴 Linux,睡着的读接上键盘,第一位常驻的常客开张营业。收卷时三块砖交底,将来装库跑 busybox 的地基踩在这儿。"
chapter: 13
order: 0
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - userland
  - syscall
  - shell
---

# 13 · 下楼开张:ring3、syscall 与第一位常客

进程那一卷收工的时候,咱们数过家底:任务轮转过了,也抢占过了,睡觉的时候抱着锁,被一声唤醒也是常有的事。可调度器的队列从排头点到排尾,站着的全是内核自己人——演示的名字换了一茬又一茬,出身偏偏一天没变过。机器从加电到今天还没执行过一条不属于内核的指令。这一卷咱们请客:让 CPU 头一回把执行流交到楼下的 ring3,给内核立一座客人隔着递话的柜台,再迎来第一位常驻的常客。

请客的路咱们分三段走。头一段备桌:GDT 里添客人的座位,TSS 备一口换栈的锅,MSR 那排暗格配上了工具,GS 安下了落脚点,顺手的还有两件家务,启动的次序从 Main 搬出来变成一张表,构建的源文件清单换成自动收集。第二段造世界、往下跳:页表的 user 位一级一级传下去,客人的家安在 0x400000,跳板的五条指令把栈对齐收进一处,第一跳的串口上落下两行字,中间隔着的几百拍心跳全是安静的往返。第三段立柜台:SYSCALL 与 SYSRET 的快通道开张,写字、等键、退场这样的活,件件都是客人隔着柜台办的。

柜台前后咱们经手的案子一桩接一桩。STAR 是存段选择子的 MSR,基值里差了两位,当年头一个时钟中断就报了个 #GP,两百来行的救火,这一遍换成了编译期的四条断言。连写两行字丢了第二行,反汇编对质出一份没兑现的承诺。GS 的证据链会说谎,观察全是真的、判断全是错的,翻案翻出了一部 MSR 词汇表。末了一案丢了的 IF,机器抱着关掉的中断睡死,修法就是 run_until_done 入口的一个守卫,那个调用管的是把就绪队列排干。案案破完了,常客也开了张:read_line 逐字回显,yield 演了一回让位与回来,Main 退役当了看门的,键盘的主权交进 ring3。

咱们还有一句话得说在卷首。这一卷立下的三条约定,系统调用的编号、寄存器的座位、出错返负的 errno,全照 Linux 的原样一步不偏。为的是远景:机器将来要跑别人的程序,咱们把 musl 这个 C 库装进来,跑 busybox 里的常用命令,再往远处的日子,根文件系统 rootfs 里的程序都来上这套柜台递话,人家认的只是这套约定。咱们给这个远景起的名字就叫北极星。地基今天踩实,往后的卷只加号,约是一处不改的。收卷的时候,咱们再把这三块砖一块一块兑现来看。

<StationPanel tag="r13_userspace" build="cmake -B build && cmake --build build --target run" />

<ChapterNav variant="sub">
  <ChapterLink num="1" href="01-tables-before-guests" desc="调度器的队列里全是内核自己人,机器还没跑过一条不属于内核的指令。盘桌盘出四件缺:GDT 没有客人的座位,没有 TSS 换栈,MSR 工具一件没有,GS 没有落脚点。动手前两件家务,Main 里堆成山的初始化变成一张数据表,构建的源文件清单换成递归自动收集">桌还缺四件</ChapterLink>
  <ChapterLink num="2" href="02-seven-seats" desc="三槽的表坐七位,user 段的 access 只动特权两位,TSS 占双槽,104 字节里只填 rsp0。ltr 自带校验,tr=0x18 读回即过考。当年 STAR 基值 0x20 让 SYSRET 装出 RPL 为 0 的 SS,头一个时钟中断就 #GP,五步收网,这一遍是编译期四条断言">七个座位与一口备用锅</ChapterLink>
  <ChapterLink num="3" href="03-msr-and-gs" desc="wrmsr 只吃 EDX:EAX 各自的低 32 位,拆法长在函数里,防当年的截位旧病。PerCpu 三槽 gs:0/8/16,两个 GS base 指同一块,swapgs 交换相等量。SFMASK(进门清标志位的掩码)一桩公案:模拟器认识这个暗格,却把合法写入静默丢掉,五步排除法分辨模拟器与真 bug">MSR 件与 GS 落脚点</ChapterLink>
  <ChapterLink num="4" href="04-a-world-for-the-guest" desc="页表的 user 位收进词汇,新起的中间表一路传下去,AddressSpace 开出用户版,高半区的镜像把帧缓冲的门带进新世界。EFER 的 SCE 位解锁 SYSCALL 与 SYSRET,efer 从 500 翻成 501。跳板五条指令,把当年一下午换来的栈对齐收进一处">客人的世界</ChapterLink>
  <ChapterLink num="5" href="05-the-guests-origin" desc="user/ 目录出生,链接脚本把家安在 0x400000,-lgcc 让编译器运行时白得,ELF 抽成纯二进制嵌进镜像。第一位客人是验收员:三亿次 volatile 自增活着过心跳,一条 cli 求一次隔离。三件磕绊,blob 不落页界、objcopy 的长符号名、排版工具动了汇编">客人的出身</ChapterLink>
  <ChapterLink num="6" href="06-first-jump" desc="串口上就两行,中间隔着几百拍心跳的安静往返。第二行逐字读开:cs=33 是真住进了 ring3,rip 正落在 cli 上,err=0 是干干净净的权限拒绝,IF 全程开着。特权指令在 ring3 被拒,恰是保护在工作的成绩单">第一跳实弹</ChapterLink>
  <ChapterLink num="7" href="07-the-counter" desc="SYSCALL 寄存器直达,不查表不压帧,入口汇编 swapgs、GS 换栈、十二槽帧。编号、寄存器、负 errno 三条约定一次定死贴 Linux,分发表 constexpr 落座,空洞返 -ENOSYS。write 两道检查,内核不盲信客人递来的指针">柜台与三条约定</ChapterLink>
  <ChapterLink num="8" href="08-three-registers" desc="一切正常,直到连写两行字,第二条 write 无声丢失。反汇编对质:编译器把 fd 驻在 rdi 里跨 syscall 复用,而出口只还 callee-saved。Linux 只烧 rax、rcx、r11 的承诺要内核自己兑现,出口六拍自帧还原 caller-saved。当年 RBX 一案是它的姐姐">只烧三个寄存器的承诺</ChapterLink>
  <ChapterLink num="9" href="09-lying-evidence" desc="不添一行功能的翻案:观察全是真的,链条环环自洽,断出来的却是错的。swapgs 前后 gs base 从高半区地址掉到 0,疑心一度落在虚拟化层。复盘对字典,FS/GS/KERNEL_GSBASE 连座整体错了一位。判决是 msr.hpp 词汇表出生,十六进制从此不进新码">会说谎的证据链</ChapterLink>
  <ChapterLink num="10" href="10-the-sleeping-read" desc="当年空了就 pause 自旋一百万次,如今键盘内嵌信号量:IRQ1 入队即 post,等待的 take 睡上调度器的等待队列,醒来回头再查一遍。sys_read 双重检查后逐字搬,CR 转 NL,换行整行返回,进程那一卷铺好的睡与醒迎来第一位用户态消费者">睡着的读</ChapterLink>
  <ChapterLink num="11" href="11-the-first-tenant" desc="read_line 逐字回显加退格三字节序列,tokenize 就地切分,echo/help/yield/exit 四命令。考古翻过当年 user/libc 的成分:printf 是调试副产品,shell 一次没调过,所以零 libc 依赖。Main 退役为看门的,help/echo/yield 三轮实弹全链闭合">第一位常客</ChapterLink>
  <ChapterLink num="12" href="12-the-lost-if" desc="shell 阻塞在 read,键敲下去全无反应,连键盘中断里的诊断打印都不执行。gdb 断在 Main 的 hlt,eflags=0x46,机器抱着关中断停机。链条从 SFMASK 一路捋到切换器不碰 rflags,任务侧裸 sti 在宿主世界当场段错误,定稿修法是 run_until_done 入口一个 IrqGuard">丢了的 IF</ChapterLink>
  <ChapterLink num="13" href="13-acceptance" desc="面板十二行逐字:tr=18 是 ltr 白送的回执,gs base 两行同数是设计的目标态,efer 翻成 501 是 SCE 点的灯。三十四件测试全部通过,kernel.bin 43456 字节,三轮实弹全链走通。后见之明四条对照,这一卷收的全是红利。三块砖交底,装库那一卷的地基踩实">验收与三块砖</ChapterLink>
</ChapterNav>

合卷的时候,机器已经过上了常态的日子。咱们看着一记键进来,中断入了队,信号量喊了人,调度器把睡着的任务请上了 CPU,回显完一个字又睡了回去。您敲下的下一记键,走的还是这整条链。可您细看常客的家底:echo 打出来的字是您刚敲进去的,help 报的菜名是编进镜像里的,程序自己睡在内核镜像的怀里,盘上什么都没有。要请更多的客人进门,得等有了盘,有了文件,有了名字。下一卷开工的正是这摊差事,头一件活就是在内存里的盘上把文件和名字立起来,真正的盘排在更后面——程序从盘上被请进来的那一天,机器才真算开了张。
