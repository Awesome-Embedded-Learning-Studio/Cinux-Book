---
title: 05 · 信号
---

# 05 · 信号:两段路,一扇回家的门,四笔如实交底

4.4 收卷时指了一座桥,咱们今天开正门。睡着的人被信号拍醒之后,睡觉的原语拿什么话回,4.4 借 xv6 书页 88 那句,把接头的地方点给了本章。同页咱们自己还要搭一座,那句它家没有信号,留给对照节接话。开篇把话搁稳:回话分两屋,等待圈里答 EINTR 的那一屋,和等待表上追不上的那一屋,咱们到诚实边界一笔一笔对清,正路今天修通。

这门活计为什么存在,正文《为什么教进程听懂喊声》一篇开卷盘过家底,咱们把三件缺的事原样搬来:跑飞的程序在自己的循环里转,机器没有一句号令喊得停它。父亲想听孩子的死讯,手上只有守在 wait 里睡死的一招。想让一条战线上的任务一起收摊,连喊一片的名字都还没有。三件共用同一个答案,答案的名字叫信号。那一篇的原话咱们整句摘:进程组是喊一片的门牌,喊人的嗓子是信号。

用户面的用法,前置卷《信号》一章教全了:产生、挂着、送到三拍,默认、忽略、自装句柄三种处置,kill 的 pid 怎么填,全教过了,本章只一句领走。教科书层看的是另一头,这些词在内核里怎么兑现。咱们顺手划一处地界:信号量与信号,名字里都有信号两个字,两家无涉,前者数货,归 4.2 与 4.3,后者传话,归本章。

本章的活,咱们数下来还是三样。头一样,立地基:号表与集合,帧从十二槽长到十六槽,发送与投递的两段路修通。再一样,装门:定制句柄怎么改帧、怎么靠蹦床与 sigreturn 回家,死讯怎么编码,喊一片的门牌怎么立,停与醒怎么兑现。末一样,对清:四笔诚实边界,一笔一笔如实交代,那是本章的正戏,不是脚注。

量法三条。码,实况锚是 r34_compile_self 的终态克隆。信号的机件在正文那一卷之后又长了几笔,投递让出红区、动作记录长到四词、返回槽按旗选路,教科书层以终态的正身说话,当年的旧面貌只在演进叙事里出现,依据是正文卷的记载。屏,本章零屏,sigdemo 的两段屏正文《验收:喊得应的机器》一篇印过,您回着看,test_signal 的案子引名不引屏。书,TLPI《Linux/UNIX 系统编程手册》教科书层头一回引,五段书证随各节走,原话整句摘、页号写明。xv6 只动 4.4 引过的第 9 章 88 页,同页添一句。

> **九号喊给一个正在跑的任务,发送那一拍,它多半正在 CPU 上干自己的活。您想:内核为什么不顺手当场把句柄跑了,非要等回程?**
>
> 答案是手里没有现场。跑句柄要在收信人的地址空间里、用它的栈,那一刻 CPU 上坐的是别人。收信人完整的用户现场只存在一个地方,就是它进内核时压下的帧。所以咱们只有一条正路走,发送的人只留记号,等收信人自己回用户态的前一刻,拿着帧办。回程窗口不是将就,是唯一拿得到完整现场的时机。

## 号表:二十二个名字,两个例外

号表头一个立。一号到二十二号,名册照的是 Linux x86_64 的原样:九号管杀,十一号管踩了不该踩的内存,十四号管闹钟,十七号是孩子的死讯,十八号到二十二号住着停与醒和终端那伙号。正文的原话:咱们一个字都不能自创。为什么原样照录,正文交代过理由,将来装库跑别人的程序,人家编译好的二进制认的就是这套约定。

集合不另造轮子。SigSet 是 base 家位词件的别名,底子一个无符号长整型,64 位,第 n 位喊的是 n 加一号,装二十二号绰绰有余。正文里专门记过一笔,这是用户世界第一条通向 base 的依赖边,除此之外再没有第二条。任务身上那座信号的小世界(signals.hpp:59 一带)装三样:一张 22 格的动作表,一个屏蔽集,一个待办集。待办集咱们配上白话:喊到谁,就在谁的位上立一记,号到了还没办,就是待办,里头一位一位的叫待办位。三样旁边住着第四件,一把自旋的小闸 gate,谁要改待办位谁拿它,名字您且记下,对清的时候它有正戏。

动作记录的正身,咱们把终态的四词一字不差抄来,signal.hpp 的 :89:

```cpp
struct SigAction {
    unsigned long long handler{};   ///< Handler address, kSignalDefault or kSignalIgnore.
    unsigned long long flags{};     ///< kSaRestart, kSaResethand, or zero.
    unsigned long long restorer{};  ///< User restorer address; delivery uses the kernel trampoline.
    SigSet             mask;        ///< Extra signals held while the handler runs.
};

static_assert(sizeof(SigAction) == 32, "Linux x86_64 kernel sigaction is four words");
static_assert(offsetof(SigAction, flags) == 8);
static_assert(offsetof(SigAction, restorer) == 16);
static_assert(offsetof(SigAction, mask) == 24);
```

记录有它自己的长成史。当年正文里它是三词,句柄、掩码、旗。后来开门迎客那一卷销掉真客人 ash 的段错误,照 musl 认的 x86_64 词序重排成四词,restorer 的格子从那天起就在。再往后返回槽按旗选路,那笔欠的活才还上,演进的正戏在改写帧一节演。词义您过一遍:句柄词零是默认,一是蒸发,别的任何数都是用户态句柄的地址。掩码是句柄跑的当口额外屏蔽的一伙。旗里住着三面,RESTART、RESSETHAND、RESTORER,RESTORER 的值是 0x04000000,RESTART 咱们收词不收机制,正文交过底,重跑的家什是没有的,这笔也画在诚实边界。

整张表只有两个例外:九号管杀,十九号管停。它们装不上句柄,也进不了掩码。两个问答函数,SignalCatchable 与 SignalBlockable,真身在 signals.cpp 的 :154。对谁都答同一句,不行。道理正文讲得直白,原话整句摘:一个跑飞的任务头一件事就是给九号装上自己的句柄,从此谁也杀不了它。掩码那边同一出戏换个演法,照样唱得成。两个号谁也碰不得,它们是咱们手里最后的家伙。

掩码操作的词里带着九号十九号怎么办?进门前静默剥掉再存(sys_signal.cpp:38 的 strip_uncatchable),Linux 也是这么办的。TLPI 书页 392 给过原话,咱们摘来:“This is the sure kill signal. It can’t be blocked, ignored, or caught by a handler, and thus always terminates a process.”九号在书里的名分,与咱们给它的地位,一句话对上了。

用户态使唤这套词汇的号开张两个,您记号位:rt_sigaction 认 13,rt_sigprocmask 认 14。字头那截 rt_ 的来历正文交代过,Linux 当年给信号机制换血留下的字头,借的只是名字和号位,与实时不实时没有关系。装句柄那一号,第四参坐在帧的 r10 槽里,报的是集合的字节数,报不对答 EINVAL。递进来的指针过 uaccess 的门,越界答 EFAULT。句柄词要待在用户半区之内,越界当场回 EINVAL。装的时候不查句柄那页映没映射,这个口子正文留着话:真跑不到页,是缺页进门的事,错在调用者身上。

## 帧长全:十二槽长到十六槽

您在 2.3 的柜台里读过的帧,印的是当年的旧印:十二槽,96 字节,rbp 打头,汇编里搬参,InvokeSyscall 收号加三个词。这里咱们点名对齐一句:那印的是当年的形状,长全的正戏在本章,机件长全的原委正是信号。

为什么非长全不可。信号要的不是借一眼寄存器,是接手整个执行现场:停下手上的用户代码,转头办别的事,办完从断点接上。接手落笔的地方是回用户态的现场,现场就是帧。您会问中断帧不行吗,不行的,咱们的中断帧是五个字段的身子,住着 rip、cs、rflags、rsp、ss,通用寄存器没有位置,想改写都找不到落笔处。

正路只有一条,把 syscall 帧缺的四格补上。咱们把入口汇编新的头四拍抄来,syscall.S 的 :8:

```asm
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
```

新四拍打头,r15 到 r12 倒着压进去,r12 住 96,r13 住 104,r14 住 112,r15 住 120,底下的十二拍一拍没动。十二槽长成十六槽,96 字节长成 128 字节。布局有断言把守,syscall.hpp:385 一带四条:rcx 在 8,rax 在 24,rbx 在 80,r12 在 96。帧从此是任务在内核期间的整套用户寄存器档,您要读任务的现场,读它就是了。银行跟着关门:当年 fork 缺那四格,在 per-CPU 块里开过一个小银行,入口汇编每次顺手白存四格。帧全了,银行缩回四格,fork 取孩子的 r12 到 r15,直接从父进程的帧里读,当年每次 syscall 白存四个 mov 的日子,结清了。

真正的改型在办事人的脸,您接着看。入口一句 movq %rsp, %rdi 把整张帧递进去,Handler 的脸统一成吃帧:号住在 rax 槽,参数散在 rdi 往后的槽里,五参六参自己伸手取,正文的说法是门已经在帧里开好了。整个系统调用面三十八处签名跟着换了装,这是正文卷的记载。写帧的差事收归两处,exec 的换世界,sigreturn 的回现场。分派对其余的 Handler 承诺只读。

## 发送:三关,待办上立一记

一个信号从生到办走的路是两段:发送占头一段,投递占第二段。发送干的是把号落在目标身上的活,投递是目标自己把事办了。为什么分两段,正文的原话照录:喊人的和办事的几乎永远不是同一个人。甲给乙发信号的那一刻,乙多半正在 CPU 上跑着别的活,咱们总不能让甲替乙把句柄跑了。

发送的正主 SendSignal,咱们把带闸的那一截抄来,signals.cpp 的 :213:

```cpp
    SigAction action{};
    {
        const arch::IrqGuard kIrq;
        const auto           kGate = target.signals.gate.guard();
        action                     = target.signals.actions[number - 1];
        if (action.handler == kSignalIgnore) {
            return true;
        }
        if (number == cinux::syscall::kSigCont) {
            target.signals.pending = target.signals.pending & ~stop_class_mask();
        }
        target.signals.pending = target.signals.pending | bit_for(number);
    }
```

三关的口径,正文立了口诀,原话咱们照录:除了号不合法,发送永远是成功的。号出了一号到二十二号,答不行,这是全函数唯一的拒绝。目标已经是僵尸或死透,答照收,死人收不到信,发信的人却没犯错。动作表里装的是蒸发,当场丢弃,连待办集的门都不进。头一段路没有失败这个说法,成败的裁判,归投递那一边。

闸里那几行您再认一遍:读动作,查蒸发,继续号进闸就清停类四位(19 到 22),末了置待办位,一口气在 IrqGuard 配 gate 的护下办完。为什么改一个位也要进闸,咱们挂个名字,到诚实边界对清。尾巴上还有三笔,继续或杀落在停走的人头上当场 resume,默认停落在排队的人头上当场 withdraw,那是停与醒在发送侧的差事,正戏留在后面。

继承的章程咱们顺带定下:fork 把动作表和屏蔽集整个抄给孩子,待办集不抄,孩子不背父亲没办完的事。exec 三样全回出厂(SignalState::reset,signals.cpp:182),新程序有新的章程。一处细节码里写着,重置的时候 gate 自己活着不重置,闸不是该归零的家当。

## 投递:回程一问,挑最小号

投递的窗口,咱们画在 syscall 的回程上。InvokeSyscall 把答案写回帧的 rax 槽之后,多问一句,syscall_dispatch.cpp 的 :285:

```cpp
    const long long kResult = kHandlers[kNumber](frame);
    frame.rax               = static_cast<unsigned long long>(kResult);
    if (cinux::proc::DeliverPendingSignals(frame)) {
        return static_cast<long long>(frame.rax);
    }
    return kResult;
```

这一问是全库唯一的生产调用点:投递只在回程发生,别处谁也不调它。您看窗口挑得刁不刁?TLPI 书页 388 有句对读,原话摘:“A signal is said to be generated by some event. Once generated, a signal is later delivered to a process…Between the time it is generated and the time it is delivered, a signal is said to be pending.”同页还有半句:“delivered as soon as it is next scheduled to run”,赶上下一回被调度就办。产生、挂着、送到,前置卷教过的三拍,学名对应的就是引文里的三个英文词,generated、pending、delivered。咱们的机器没有进程级的调度前夜,咱们的兑现时刻更具体,回程的那一问。

进门头一件事算 lowest_runnable:待办集抠掉屏蔽集,剩下的才是今天能办的,能办的里面挑号最小的。低号在前,POSIX 立下的口味,咱们照办。取号和清位住在同一把闸里,咱们把那一截抄来,signals.cpp 的 :246:

```cpp
        int       number = 0;
        SigAction action{};
        {
            const arch::IrqGuard kIrq;
            const auto           kGate = kSelf->signals.gate.guard();
            number                     = lowest_runnable(kSelf->signals);
            if (number != 0) {
                action                 = kSelf->signals.actions[number - 1];
                kSelf->signals.pending = kSelf->signals.pending & ~bit_for(number);
                if ((action.flags & cinux::syscall::kSaResethand) != 0) {
                    kSelf->signals.actions[number - 1] = SigAction{};
                }
            }
        }
```

循环的走法咱们走一遍:取到零,散伙。取到号,位进闸就清掉,动作抄一份出来。动作是蒸发,白捡一拍,接着挑下一个。动作是默认,按处置表分岔,下场四种:处置杀的 die_by_signal 带着号当场退场。处置停的,清了位自己停。处置蒸发的与照旧跑的,路是同一条,清了位接着走。RESSETHAND 的旗立着,送完这一次动作整个回出厂,就在闸里一并办了。

动作词是句柄地址,走定制路:frame_handler_entry 改帧,改得成,投递把句柄的掩码并进屏蔽集。改不成,按十一号处死,栈浅得摆不下投递要摆的东西,本来也就是个将死的任务。纪律一条,正文的原话:一句回程最多送一个定制句柄,送完了就散,ring3 认的就是这个形状,堆着送会把用户栈摆成什么样,咱们谁心里都没数。TLPI 书页 412 替咱们的待办集作过保,原话摘:“The set of pending signals is only a mask; it indicates whether or not a signal has occurred, but not how many times it has occurred…later delivered, just once.”位集只报有没有,不报几回,同一个号连发三发并成一个位,送一回。Linux 的 rt 信号为此另立了排队,咱们一句带过,不展开。

## 改写帧与回家的门

定制句柄的投递,是整套机制里最像变戏法的一段:任务明明要回自己的用户代码,咱们让它落在句柄上,办完了一字不差地回原来的断点。秘密全在栈上,咱们一段一段看。红区这个术语配上白话:ABI 划给叶函数的一块自留地,函数不再挪栈顶,把最多 128 字节的家当直接暂存在栈顶底下,调用者不看那块地方。投递要往用户栈下摆东西,贴着栈顶摆,就把人家暂存的家当压住了。

这段有笔演进史,咱们摆开。当年正文《改写帧与回家的门》一篇印的投递,最低处摆一个槽,写着归乡上下文的地址,往上才是蹦床槽和上下文本体,没给红区留地方。后来真客人进门,ash 的段错误把这笔旧事翻了出来,销案那一阵投递搬家:指针槽删了,栈顶往下让出 128 字节红区,再留 15 字节对齐余量,上下文和返回槽摆在更底下。两个名字住在 signals.cpp:31,kRedZoneBytes 与 kDeliveryBytes。栈区间够不够摆,如今走登记册问,vmas 的 stack_contains(signals.cpp:46),登记册说了算。

摆的是两样,咱们一样一样数。头一样,归乡上下文 SigReturnContext,144 字节 18 个词,白话就是一幅回家图:被中断的那行代码怎么回去,rip 回哪,栈在哪,当时的 rax 是多少,全写在这幅图里。词的构成是帧里十六个词按帧自己的次序镜像一份,外加投递以前的旧屏蔽字,末了补一个零词凑 16 字节对齐,144 这个数有断言钉着,一个不多。第二样,返回槽,8 字节,住在上下文底下。

帧只改四个词。咱们把选路与改写那一截抄来,signals.cpp 的 :138:

```cpp
    const unsigned long long kReturnAddress =
        (act.flags & cinux::syscall::kSaRestorer) != 0 ? act.restorer : SignalTrampolineAddress();
    if (!cinux::syscall::uaccess::CopyToUser(kContext, &image, sizeof(image)) ||
        !cinux::syscall::uaccess::CopyToUser(kReturnSlot, &kReturnAddress,
                                             sizeof(kReturnAddress))) {
        return false;
    }
    frame.user_rsp = kReturnSlot;
    frame.rcx      = act.handler;
    frame.rdi      = static_cast<unsigned long long>(number);
    frame.rax      = 0;
```

四个词,咱们一个一个认。user_rsp 落在返回槽上。rcx 换成句柄地址,syscall 进门那一拍硬件把回路记在 rcx,sysretq 出门照它跳,一进一出认的是同一个寄存器,把句柄地址换进去,出了内核的门落到的就是句柄。rdi 带信号号,C 的第一个参数。rax 清零,还它一张干净的脸。其余十二个词一动不动,栈上的镜像里也各存一份。返回槽的位置也讲究,栈顶除十六余八,正文的原话照录:刚被 call 完的函数,进门那一刻的栈顶就是除十六余八。句柄一进门,看到的就是一次普通调用刚发生完的栈形。

句柄跑完,ret 一弹,落在返回槽写的地址上。地址给谁,旗说了算:kSaRestorer 立着,落客人指定的 restorer,没立,落内核蹦床页。字段自己说了不算,restorer 格里写了字而旗没立,不算数,长凳有案钉着。蹦床是咱们起的名目:独立一页,可读可执行、永不许写,顶在匿名映射天花板的上一页,exec 出厂就装上,fork 的孩子靠共享白得。页里一共七个字节,signal_trampoline.cpp 的 :29 一行:

```cpp
    constexpr unsigned char kStub[] = {0xB8, 0x0F, 0x00, 0x00, 0x00, 0x0F, 0x05};
```

B8 0F 00 00 00 是 mov eax, 15,0F 05 是 syscall,句柄的 ret 就这样变成一次系统调用,十五号,rt_sigreturn。为什么住独立页,正文备着两笔旧话。当年头一遍的蹦床是摆在用户栈上的两字节机器码,能跑靠的是栈页可执行的便宜,开执行限制那天那两字节当场成了违禁品,被迫搬家。当年的定制投递走中断路,因为帧存不全 r12 到 r15,只好立专门的陷阱门收全帧。今天帧十六词俱全,那个前提整个没了,syscall 回程就是正路。那一篇的收话咱们照录:信号帧的形状,从来是由谁存着完整现场决定的。

进了门,sigreturn 按帧找人,咱们把走法说全。此刻帧里的用户栈顶,正是句柄 ret 之后剩下的那个上下文地址,当年的形状从栈上的槽里解引用去找,后来直读 user_rsp,少绕一扣。两道护栏排在头里:上下文要落在登记的栈窗之内,还要 16 字节对齐,答不上来按十一号处死,一段跑偏的栈就是任务自己的死法,内核不替它圆。护栏过了,十六词从上下文镜像回帧,旧屏蔽字归位,被中断的那行代码连着它当时的 rax,一字不少地回家。

rax 咱们单独再说一遍,在它身上踩过一脚。分派对每笔 syscall 都把返回值写回 rax 槽,sigreturn 若按寻常答个零,分派紧接着把零写进帧里,刚恢复出来的 rax 就被冲没了。修好的形状,sigreturn 答的是它刚恢复出来的那个 rax,出处 sys_signal.cpp:183。正文的原话是:写回写进去的,与上下文里躺着的,是同一句的真话,一个数都丢不了。

## 死讯:一纸收据,两种死法

信号的另一头,是内核自己的事件说这门语言,配对落地两对。头一对,缺页配十一号:裁决者不受理的缺页,看是谁惹的,来自用户的,不再劳驾全机转储,惹祸的任务以十一号的身份自己退场。正文的原话整句摘:从前是一个程序写飞了指针,全机器陪着它一起停了摆,现在还是一个程序写飞了指针,死的是它自己,父进程收得到的死因,机器照样跑它的活。内核自己惹的缺页,老章程一字不动。咱们跟着补一句诚实的边界:十一号的句柄装得上,可缺页不走投递的回程,句柄接住它之后要不要回出错那行重跑,这套语义没验过,门留着,路没修。

第二对,退场配十七号。exit 把任务安置成僵尸之后给父亲发十七号,次序咱们点出来:置僵尸在前,发信号随后,叫醒排在最后(scheduler.cpp:336 一带,置 kZombie、发十七号、叫醒,三步的次序就是生产排在消费前头)。十七号默认蒸发,装默认动作的父亲日子照旧,wait 的唤醒另走一路。想边干活边听死讯的父亲从此有路,死讯落在待办集上,回程自然办掉。

死法要写进收据,收据就是退出词。咱们把 exit 的码抄来,sys_exit.cpp 的 :10 一行:

```cpp
    proc::Scheduler::self().exit_current(static_cast<long long>((frame.rdi & 0xFFULL) << 8ULL));
```

自愿退场的,码只取低八位,左移八位住进高八位,低字节永远是零。死于信号的,退出词装的就是信号号,落的正是低字节。正文的原话照录:一个三十二位的收据,两种死法各说各的,谁也冒充不了谁。会读的人当场就能对上号:shell 认得两种报法,低字节非零报 killed by signal 加号,为零报 exited with 码,sigdemo 那一屏里两种正读各露一次脸,您回正文《验收:喊得应的机器》那一篇,那里教的念三遍,第三遍读的就是它。

孤儿的事,有一笔演进史要如实交代。当年正文明说,孤儿不送人收养,孩子就地散伙,因为咱们的收养人还不存在。如今的码里,收养人上了岗。detach_offspring 头一个问 find_adopter,真身在 scheduler.cpp 的 :398。问得到,孩子的父亲就改指收养人。已经是僵尸的孩子,补发给收养人一声十七号再叫醒他。来路咱们如实说:这笔正文卷未及记载,机器给 glibc 换血那一阵它悄悄上的岗,为什么收养,咱们不编,码在就是在了。

## 门牌:喊一片的地址

嗓子有了,喊一片还得有地址。Unix 的答案是进程组:一伙任务共挂一块门牌 pgid,门牌上面还有一层院子 sid,两个字段住在 Task 身上。出身正文定过,开机的骨干任务和 shell 各自成组自成会话,fork 出来的孩子抄父亲的。咱们对读 TLPI 书页 701,原话摘:“Each process has a numeric process group ID…A new process inherits its parent’s process group ID.”老家的常例,书与咱们的码说的是同一句。

换门牌走正门,咱们跟着 setpgid(号位 109)过两道问:动谁,自己随时可以,别人的话必须是我同一会话的亲生孩子。进哪个组,目标自己的 pid 是自立的新门牌,别的值必须是本院子里已立着的门牌。回话分三档:人找不着答 ESRCH,不是自家的答 EPERM,门牌查无此组答 EINVAL。setsid(112)简单些,已是组长则拒,不然自立门户。

kill 的号位是 62,它的 pid 参数有四种读法,咱们叫四读:正数报号,喊一个人。零是本组,连我自己在内。负一是除我之外的全机。再小的负数是组广播,喊门牌取负之后的那一组。还有个零号信号,只问人还在不在,东西一个不发,咱们叫探针。四读落进同一个 HandleKill,正数支如今的形状,sys_kill.cpp 的 :27:

```cpp
    if (kNamedPid > 0) {
        long long result = -kEsrch;
        scheduler.with_task(static_cast<unsigned long long>(kNamedPid), [&](proc::Task& target) {
            result = (kNumber == 0 || proc::SendSignal(target, kNumber)) ? 0 : -kEinval;
        });
        return result;
    }
```

正数支有笔演进,您对着正文当年的印子看:那时印的是自己 find_by_pid,后来的形状换成 with_task 守卫查找,拿着调度器的锁在册子上问人,只走访在册坐着的任务。真正承重的是括号里 kNumber == 0 那半句:号是零,压根不调 SendSignal,人找着了答零走人。探针只问不发的章程,落到码里就是它。

后头的三读合走一条,for_each_task 扫全机名册广播,听着的记进 heard,一个都没喊着的答 ESRCH,喊人没喊着不算错,话总得有个交代。您再看 wait 这边,它也学会了听门牌,收据上的 pid 同样四读挑人,外加一面 WNOHANG 旗(sys_wait.cpp:17,住在 rdx 最低一位):有匹配的孩子活着还没退场的,答零,人不停泊。正文的原话:这把钥匙开的是轮询的门。一个想看着孩子又不想睡死的父亲,从此有了正路的问法。

前台的事咱们只指门。终端把按键交给哪一伙,那一伙叫前台组,TLPI 书页 708 的原话摘:“The controlling terminal maintains the notion of a foreground process group.”咱们的终端那边如今真立着一格前台门牌,行规程问出一声信号,SendToGroup 就喊给前台组(console_node.cpp 里那一笔)。^C 那个字节怎么从键盘走进这套门牌,是第 5 部的戏,点名即止。

## 停与醒:第五种活法

三件缺里最难的一件:喊停了,它凭什么真的停。停不是死,死了尸身等收,也不是睡,睡了还挂在等待表上等叫。停是第五种活法,状态名册从五档长到六档,从前的五档,就绪、在跑、阻塞、僵尸、死透,新档叫 stopped,咱们叫停走。正文的原话是:人是不动了,可接下去的动作,咱们随时都接得上。

光有状态不够,停走的人得从所有能被选中的地方消失,而能被选中的地方握在调度类手里。SchedulingClass 添了一手纯虚的 remove,队列是类自家的家当,人排第几位、抽走后怎么补位,知道的只有类自己。这是正文讲的道理,咱们照着办进了码里。

调度器配三件兵器,咱们一件一件过。stop_current 自己停,进门两道查:头一道问在不在任务里,第二道对自旋的登记簿,停走的人抱着内核栈换人,攥着自旋锁没放的话,等锁的人就等成了没有头。状态改成停走之后,切栈走的是与睡觉共用的道(scheduler.cpp:519)。resume 唤醒,查的确实是停走,改回就绪,塞进类的队列。withdraw 就地摘,只对还在就绪队里排着的生效,摘完顺手把状态也改成停走。终态的 stop_current 比正文卷印的那版又收了一道,置停走的那一拍裹进了调度队列的锁里,睡觉那套口径当年也这么补齐过,来路 4.4 讲过。

要紧的分野在时机,咱们把线画下来:同一件停的事,发送办一半,投递办另一半。落在排队的人头上,回程什么时候来没有保证,默认停类的信号在发送那一拍就 withdraw,人就地出队。落在跑着的人头上,回程就在眼前,信号进待办,回程投递时清了位自己停,syscall 把该答的话照常答完。落在停走的人头上的,单独说:停走的人永不被调度,永远等不来自己的投递回程,所以继续和杀在发送那一拍由发信人亲手 resume。杀为什么也亲手,正文的原话:它得醒来走上自己的死路,死讯的编码才进得了退出词,父亲的 wait 才收得到。长凳另有一案配着这句话,stop and cont: kill wakes the halted task, a plain term stays parked,九号叫得醒,十五号叫不醒,只在待办集上多立一记。

继续这一号还有个前置:发送时把停类的四个号,19 到 22,清干净再置自己的位(stop_class_mask,signals.cpp:72)。不清的话,醒来转脸又吃一记停的招呼,继续也就不成其为继续了。长凳有一案替咱们记着这一课。案名是 stop and cont: a continue sweeps the pending stop-class bits,说的就是把停类的待办位扫干净。

停走的人醒在哪?继续来了,resume 放回队列,轮到它上 CPU 的那天,切换器把照片装回去,那一跳落在恢复点上。正文的原话整句摘:对 C++ 的世界来说,stop_current 像的是一次普通的函数调用,调了进去,过了很久,正常地返回了。被停的那笔 syscall 没有被打断半个字,这是停走和睡眠、和死都不一样的地方。末了一笔如实登记。跨类的次序,正文那一卷就没动过,正身的原话是今天的咱们只学会了摘人,排位是没有学会的。

## 诚实边界:四笔对清

本章的正戏之一,欠的活对清。四笔,咱们一笔一笔来,4.4 的桥,也在头一笔里接上。

头一笔,睡着的任务追不上。这句今天仍然是正身:睡在等待表上的任务,比如守着 wait 的父亲,信号落在待办集上,可它不回程,信号追不上,得等它自己醒了办完事走回程的那一步。这就是 4.4 桥上问的拍醒之后怎么回话。咱们把回答分两屋。

第一屋,等待圈里的睡眠,答 EINTR。正文《开门迎客》那一卷《亲手一玩,三件补齐》一篇的原话照录:听得懂喊声那一卷欠下的信号打断睡眠,在 ppoll 的门上还上了一角,别处的睡眠门口还是老样子,咱们如实记着,不把一角说成了一片。那一角后来长成了一排门。ppoll 的等待圈里,每个来回自查待办(sys_ppoll.cpp:188 一带的 HasPendingSignal),有就答 Eintr。read、sendfile、nanosleep、socket 的等待圈,终态里也都挂了同一问。ppoll 的 sigmask 参数,当年正文说收下不验,终态也不是了,真换,出门真恢复(armed 与 restore_blocked)。等待圈回话的那一截,咱们抄来,sys_ppoll.cpp 的 :195:

```cpp
            if (kDue) {
                return static_cast<long long>(ready);
            }
            if (kSignal) {
                return -kEintr;
            }
            return 0;
        }
        proc::Scheduler::self().yield();
```

第二屋,等待表上的真睡眠,还是老样子。等待圈是轮询的睡法,yield 出去又回来,每个来回有一次自查的机会。等待表是真睡,挂上队列就切了栈,中间没有回程可言。所以一角长成了一排门,等待表纹丝没动,咱们把地界说清,不冒充全好。SA_RESTART 的旗,词照收,重跑机制仍无,正文当年交的底,今天还押着。

第二笔,纯计算循环杀不掉。这笔的出身咱们交代清:正文没说过,是教科书层对着码看出来的一笔,不冒充正文的话。投递全库唯一的生产调用点是 InvokeSyscall 尾上那一问。一个不进系统调用的用户循环,九号喊过去只落待办,永不投递。任务会被时钟抢下 CPU,可每一拍抢回来还是那段循环,死讯的编码进不了退出词,父亲收不到,机器陪着它空转。它自己碰一次系统调用,哪怕是写一个字节,回程那一问就把欠的活结了。

第三笔,待办位的共同协议。单核上发送与投递不会同时改一个待办位。双核上会,咱们把现场摆出来:一颗核在给目标置位,目标自己恰在另一颗核上回程清位,两头都是读了改、改了写,后写的盖掉早写的,那一发信号就没了,发送的人手里还攥着成功。正文《请第二颗脑子上线》那一卷合卷时把这笔挂在明处,原话整句摘:信号位的共同协议、共享文件表的竞争这些边界,验收里逐条地写着。

后来正文《开闸,与开闸后的镜子》一篇收的笔,它与《还一笔旧债》同卷,原话摘了两处。头一处,咱们顺手收掉了名单上挂着的信号位共同协议那一笔。第二处,闸窗绝不沾调度器的边。终态的形状您已经见过两段了:发送侧 IrqGuard 配 gate,投递侧取号清位进同一把闸,exec 的重置也进门。闸外只剩一笔自家回程的写,投递尾上并句柄掩码的那一笔,写的是 blocked,碰的不是待办位,只有任务自己的回程会走到。按 RESSETHAND 清动作的那一笔,正文那一卷记它时还裸在闸外,后来重组投递,与取号清位一起收进了闸,投递一节您已经见过它进闸的样子。这把 gate 是 base 那枚零系统耦合的极小互斥件传下来的,正文《同步层的旧债》一篇立的。它拿的时候配获取、放的时候配释放,次序为什么非这么配不可,深一层的道理归《内存序与缓存一致性》,咱们交棒,点到为止。

第四笔,restorer 的旧债。动作记录长到四词那年,restorer 的格子就在了,咱们收下了客人的字,却从来没人读它:投递一律写内核蹦床页,客人明明说了收尾走谁家的门,咱们不认。正文《还一笔旧债》一篇的原话是:接口收了话,该办的事一件没办,这笔欠的活。还法小得可怜,就是返回槽按旗选路那一问,您在改写帧一节已经看过。长凳两案钉着它。signal delivery: without the restorer flag the kernel trampoline keeps the slot,旗没立,格里有字也不算。signal syscall: musl's four-word action preserves flags, restorer and mask,四词一个不丢。实弹两条路也走过:自家的 sigcatch 不设 restorer,自己给自己发一号,handler 记到了数。musl 的客人 trap 挂一号,收尾走自家 restorer,shell 活得好好的。

## 实况:二十九案,案名对号

长凳上信号这件家什如今二十九案,咱们引名不引屏,案名原文照录,您拿名字对科目。表与发送的底座,signal tables: the catchable set excludes kill, stop and the outside 与 signal tables: the blockable set answers the same two exceptions,两个例外各钉一案。signal send: bad numbers refuse and zombies shrug 与 signal send: an ignored signal drops, anything else parks a bit,三关各答各的话。

帧与投递那几案,咱们看下来最见功夫。signal delivery: a custom handler rewrites the frame onto its stack,改写的四个词逐个对。signal delivery: handler return restores registers, mask and the interrupted red zone,回家那幅图连红区一起对,红区 128 字节涂了记号,句柄跑一个来回,一个记号不许丢。signal delivery: SIGCHLD returns through a stack slid below the old window,投递把栈挪到旧窗口底下再回家,专防一种想当然的摆法。signal delivery: a delivery that cannot fit the stack window kills 与 signal delivery: the registered guard rejects SIGCHLD with SIGSEGV,两道死法都有案可查。

门牌与停醒的案子咱们接着点,案名各自钉着各自的话。signal scheduler: guarded lookup visits seated tasks and nobody else,守卫查找。signal pairs: an exit marks the parent's pending chld,死讯落位。stop and cont: a queued task stands stopped the moment stop lands,发送侧就地摘。stop and cont: a continue sweeps the pending stop-class bits,前置清位。stop delivery: the halting stop consumes itself and halts mid-delivery,投递侧自己停。

验证范围有句实话接着说。正文当年交过底,定制句柄这一路,验的是宿主长凳上的断言,栈是宿主里 mmap 出来的假栈,真客人登门的日子在后头。后来那日子来了,sigcatch 与 musl 的 trap 两条实弹路都把门真的走过了一遍,当年的期票兑了,咱们替读者记下了这桩兑现。sigdemo 的两段屏不再重印,您回正文《验收:喊得应的机器》那一篇,头一遍看名字和门牌,第二遍看停与醒,第三遍看组杀和死讯,念三遍的方法还给您留着。

## 对照:同一件事,别的机器怎么做

xv6 那头,信号这门活它家没有。4.4 在书页 88 引过被信号拍醒回 EINTR 的那句,本章开桥点的就是它。同页咱们再添一句新的,原话整句摘:“Xv6 doesn’t support signals and this complexity doesn’t arise.” 它家没有信号,复杂度压根不会出现,同类的事,sleep 与 wakeup 办。页码如实记:第 9 章 88 页,4.4 引过的页,本章只添同页一句,不另开新页。

Linux 那头,这门亲是实打实的:号表、rt_ 字头、四词动作记录、kSaRestorer 旗,咱们全是照它的内核 ABI 抄的,正文里照它家门牌开的门。收尾那一段两家走了两条路:通行的事实是,x86_64 的 Linux 内核要求 libc 这一类的客人在装句柄时递上自家的 restorer,内核不替客人备收尾的页。咱们的内核两头都开,自家人走内核蹦床页,立了旗的客人走自家的门。认亲归认亲,深浅咱们不比,它家源码咱们没翻过。

## 本章留下了什么

| 术语 | 一句话口径 |
| --- | --- |
| 两段路 | 发送只登记,投递在回程办差;喊人的和办事的不是同一个人 |
| 三关 | 号合法、生死、蒸发;除了号不合法,发送永远成功 |
| 待办集与待办位 | 喊到谁就在谁的位上立一记;位集只报有没有,不报几回 |
| gate | SignalState 身上那把自旋小闸,改待办位的共同协议,谁改谁拿 |
| 红区 | ABI 划给叶函数的 128 字节自留地,住在栈顶底下 |
| 归乡上下文 | 用户栈上 144 字节 18 词的回家图,十六词镜像加旧屏蔽字 |
| 返回槽按旗选路 | kSaRestorer 立着落客人 restorer,没立落内核蹦床页 |
| 蹦床 | 独立可执行页上的两条指令,mov eax,15 加 syscall,句柄 ret 的落点 |
| 四读 | kill 的 pid:正数报号,零本组,负一除我全机,再小是组广播 |
| 探针 | 零号信号,只问人在不在,东西不发 |
| 门牌 | 进程组 pgid,喊门牌就是喊全体;上面一层院子是 sid |
| 前台组 | 终端把按键交给哪一伙,那一伙;细目归第 5 部 |
| 停走 | 状态名册第六档,人不动了,动作随时接得上;永不被调度 |
| 退出词编码 | 自愿码住高八位低字节零,信号号落低字节,一纸收据两种死法 |

押出去的票咱们清一清。4.4 的桥,开篇接上,等待圈回 EINTR、等待表仍追不上,对清在诚实边界,票兑了。2.3 的十二槽旧印,帧长全一节点名对齐,长全的正戏在本章。前置卷的用户面三拍,开篇一句领走。正文《改写帧与回家的门》当年挂的期票,真客人登门的日子,实况一节记了兑。

边界如实挂。TTY 行规程与 ^C 的来路、线程与信号的交互,归第 5 部,终端里的前台门牌立着等它。rt 信号排队一句带过没展开。SA_RESTART 收词不收机制。十一号在缺页路上的重跑语义没验。纯计算循环杀不掉,诚实边界如实说了,它不是正文讲的,咱们也没往正文嘴里放过一个字。

往后的门两扇,咱们各交各的。gate 的拿配获取、放配释放,次序里头的事,《内存序与缓存一致性》开门。4.4 埋下的 `sti; hlt` 的深锅,也一并归它。另一扇在第 5 部,行规程把一个字节翻译成一嗓 ^C 的来路,和线程住进同一副身子之后信号给谁,都在那边开演。

## 练一练

三道题留给您:一道推演,一道回望,一道读册。

**题一,推演。** gate 撤了,待办位回裸读改写。一颗核在给目标发送九号置位,目标自己恰在另一颗核上回程,投递正清着十号的位。您推一遍:哪一发会丢,丢在哪一拍,发送的人拿到的是什么。

**题二,回望。** 开篇三件缺,喊不停、死讯追不上、喊一片没门牌,您说说各归哪一段路办的。再答一问:头一件在咱们的机器上为什么只兑了一半,缺的那一半卡在哪个窗口上。

**题三,读册。** 回 TLPI 书页 412 那句原文。您答:同一个号连发三发,投递送几回?挡住再解挡,又送几回?再往深一问:rt 信号为什么非另立排队不可,位集装不下的是什么。

### 参考答案

题一:丢的是九号那一发。两头都是读改写:发送读到旧集,投递也读到旧集,发送把九号位改了进去,投递把自己手里的,没有九号的旧集,写了回去,后写的盖掉早写的,九号无声地没了,发送的人手里还攥着成功。丢的那一拍,在投递写回的那一句上。咱们那把闸把它关死:两头进门,排好队改,谁也盖不掉谁。

题二:咱们按件数,喊不停归投递的默认处置带号死,死讯归十七号的配对加退出词编码,门牌归 kill 的四读与组广播。缺的那一半:投递只在 syscall 回程,纯计算循环没有回程,九号只落待办,任务抢得上 CPU 却死不成,直到它自己碰一次系统调用。窗口,就是那个回程窗口。

题三:三发并成一个位,送一回,挡住再解挡,还是一回,位集不记次数。rt 信号要表达的是每一发各自是一件独立的事,一发是一发,各算各的,位集只答有或没有,装不下次数与次序,所以 Linux 给它另立了排队。咱们那二十二号信号的位集,同款脾气。

## 延伸

- TLPI《Linux/UNIX 系统编程手册》:教科书层新添的一本,本章引 388、392、412、701、708 五页,三拍与回程、九号、位集不记次数、门牌继承、前台组,往后的章节接着记。xv6 的页码仍记在 4.4,第 9 章 88 页,本章同页添一句。
- 正文《听得懂喊声:信号、停走与进程组》那一卷:本章的娘家,号表、两段路、改帧、死讯、门牌、停醒六篇正戏的原卷,《验收:喊得应的机器》印着 sigdemo 两段屏。
- 正文《请第二颗脑子上线》那一卷:《同步层的旧债》立 base 互斥件,合卷的话挂着共同协议那笔。
- 正文里讲三道闸的那一卷:《还一笔旧债》是 restorer 的原卷,《开闸,与开闸后的镜子》是共同协议收笔的原卷。
- 《内存序与缓存一致性》在册待写:gate 的次序语义、`sti; hlt` 的深锅,各归各家。
