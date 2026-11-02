---
title: 02 · 锁的实现
---

# 02 · 锁的实现:三句实话,三处修法,几位护法

4.1 收卷的时候,咱们留了三句实话,说好了下一章开门就办。今天门开了,头一件事还的就是这三句旧债。三句咱们原样再念一遍,立在本章门口:那把转圈的门闩,单核时代立的,其实不会跨核。重入的检查,到了双核会把正当的争抢误当成自己人。进门存进锁里的中断快照,会被后进门的对手覆盖。前一章的末尾还说,病根您都已经攥在手里了。攥了一章,今天咱们派用场:三句对三处修法,缝的每一处都是伤口,咱们两代代码对读,把每一针缝在哪一行的位置看清楚。

这一章的活,咱们数下来还是三样。头一样还的还是旧债:快照进口袋,重入改认的是核号,核号的一格兼了登记,三处的修法,每处都是对着旧形状读的。再一样咱们认的是护法:这个词今天咱们头一回用,配的是句白话,护住共享财产的家什,统称的就叫护法。门闩之外咱们还有几位,base 里一枚光秃秃的原始互斥件,对象身上一件住进去的计数,输出那里一条串行的道,各配各的仗。末一样办的是两桩引用的案:创建者那笔没人还,还有一场预分配六十五个地址的寿命局。

量法咱们照旧还是三条。码的话,咱们两代对读:旧形状是 r12 的 sync.hpp,261 行的身子,还有 sync.cpp 的 120 行,4.1 都量过了。今天的主角是 r26 修好的形状,r26 就是本章实况锚 r26_smp 的简称,sync.hpp 长到了 393 行,sync.cpp 长到了 263 行。base 里添的是两件新面孔:spin_mutex.hpp 的 76 行,ref_count.hpp 的 73 行。屏的话,实况锚要立的是两站,照 1.4 跨站的成例:r12_process 4.1 刚走过,面板咱们不重印。第二站换的是 r26_smp,双核的正脸头一回进教科书层。书的话,xv6 看的还是第 7 章,4.1 核过的五页您随时翻得回去,本章咱们回望 70 与 71,新核的是 67。CSAPP 2e 的 12.5 节,992 到 995 是复用的,新核的是 998 到 999。原话一律整句地摘,页号也写明了,您对着书核。

> **门闩的心脏,test_and_set 的那一拍,咱们一格没动。您想:单核时代立的这把锁,哪一层是天生会跨核的,哪一层压根没想过跨核?**
>
> 天生就会的是翻旗那一拍。原子指令本来就是给两条执行流抢同一格内存造的,单核上它防的是中断踩过来的脚,双核上它照样防得住对面的手。压根没想过跨核的,是旗周围的簿记:快照存给谁,重入认谁的手,登记记在了谁的名下,三本册子全是按全世界只有一只手立的。三句实话句句戳的都是簿记,一格都没戳在旗的身上。

## 还旧债:三处修法,一处一个伤口

咱们把旧形状摆在头里。r12 的 lock() 全文 4.1 印过,本章只抄受伤的三行,sync.cpp 的 :20 到 :22:

```cpp
    cinux::base::safety::Check(!held_.test(std::memory_order_relaxed),
                               "spinlock re-entered on one core");
    irq_state_ = cinux::arch::SaveAndDisableIrq();
```

头两行是重入的检查,末一行是快照的落点,三句实话里两句的伤口都在这几行里。修好的形状,r26 的 sync.cpp :93 到 :106,咱们整段摆:

```cpp
void Spinlock::lock() {
    const auto kIrq = cinux::arch::SaveAndDisableIrq();
    const auto kCpu = static_cast<int>(arch::per_cpu::PerCpu::self().cpu_id);
    cinux::base::safety::Check(owner_cpu_.load(std::memory_order_relaxed) != kCpu,
                               "spinlock re-entered on one core");
    while (held_.test_and_set(std::memory_order_acquire)) {
        cinux::arch::CpuRelax();
    }
    irq_state_ = kIrq;
    owner_cpu_.store(kCpu, std::memory_order_relaxed);
    SpinLedger& ledger = ledger_ != nullptr ? *ledger_ : DefaultSpinLedger();
    ledger.note_acquire(this);
    holder_ = Scheduler::self().current();
}
```

修法一办的是快照,收进了口袋。旧形状进门就把中断状态快照写进锁对象自己的成员里,成员的名字正是 irq_state_,这也是正文《请第二颗脑子上线》那一卷的原话。单核上它是独享的。双核上两颗核会一前一后地进门,后到的把早到者存的覆盖了,早到者出门还原的,是后来者进门时的中断状态,开与关从此成了随机,这句是正文里的原话。修法您在代码里看得见:SaveAndDisableIrq 落进局部 kIrq,那是自己的口袋。您转圈,抢的是锁,抢到了才转存进锁对象(:101)。转存的那一刻起再没人能覆盖它,因为能写它的只有持锁人,这话也是正文里的。

修法二办的是重入,认的是核号。旧 Check 断言的是锁没被持有,单核上它拦的确实只有重入。双核上对面核名正言顺地持锁,本核名正言顺地争抢,断言劈头把自己人骂了一顿,争抢还没开始就误判成了自锁。修法:PerCpu 报自己的核号,断言改问的是 owner_cpu_ 等不等于本核。本核重复进了门,照旧当场就叫了。对面的核持锁,咱们安安静静转圈等。咱们的那行消息 “spinlock re-entered on one core”是一字没改的,成色变了:单核年代它是全文,双核上它说的是字面为真,叫出来的唯一情形,真的只剩同一颗核。次序也挪了一处,Check 从头一排挪到了 SaveAndDisableIrq 的后头。4.1 的题二推过 Check 走在闸前的窗口,闯不了祸,顶多多等的就那么一轮。r26 干脆把窗口关死了,问核号的判断也落进闸里做。

修法三办的是核号的一格,兼了登记。锁对象上新添的一格 owner_cpu_,户口落在了 sync.hpp:237,出生记的是负一:核号从零起数,零号核占的是真核,空位就只能借用负数的地界。出门的时候清回负一,顺便当了持有者的登记。咱们再看它与 holder_ 的分工:holder_ 记的是哪个任务,是给放锁验手用的。owner_cpu_ 记的是哪颗核,是给重入检查用的。一格管一格的事,各记各的。

出门的一侧,r26 的 unlock 与 r12 比是三笔小改,咱们不整段印,逐笔地念。头一笔的快照挪进了局部 kIrq(:109),后头清持有者、销核号、落册、倒旗的那一串,谁也碰不着它了。第二笔里核号清回负一(:115)排在旗倒(:116)之前:上一位的收尾,得在交旗之前就落定了。次序要是反过来的话,另一颗核抢到旗、写好自己的核号之后,上一位迟来的清号才落到,把新的登记抹掉,重入检查当下看不见这把锁真正的主人。末一笔是 RestoreIrq 还的,就是局部里存着的。验手的 Check 没动,放这把锁的必须是持有者本人,别人代放照旧吃到响亮的拒绝,与旧年代是一字不差的。

## 登记簿搬家:一册变一排

门闩修完了,登记簿也跟着搬了家。4.1 认过的它:拿锁记一笔,放锁减的是一笔,报数的是 depth(),深度得归了零才许睡。单核的年代全机一册就够,深度天然是只有一份的。到了双核上,全机一册就开始冤枉人了,您拿两颗核想一遍:一颗核揣着门闩没放,另一颗核要睡了,岗哨翻开册子的时候,看到的深度不是零,把正当的睡觉拦下了,拦的声音还是 Check 喊的,喊的名字是无辜那颗核。

修法的活,咱们把册子按核发。depth() 的新真身,sync.cpp 的 :16 到 :18:

```cpp
unsigned int SpinLedger::depth() const {
    return depth_[arch::per_cpu::PerCpu::self().cpu_id];
}
```

深度从一册的一个数,变成了格子的数组,户型落在了 sync.hpp:105,写开了是 depth_[arch::per_cpu::kMaxCpus] 的八格,八颗核至多是机器的上限,一核占的是一格,各查各的。家法是一个字没改的,调度器的岗哨问的还是 depth(),答数换了来源:本核的深度。两颗核守好自己的岗,谁也不碍谁的事,这句是正文里的原话。同样的待遇也给锁序图:每核一条私册的持有链,全机共享一张记谁在谁前的有向边图,边图自己另有一把原始的小锁护着。图的官司与生卒的正戏,归的是《死锁与锁序》,咱们点到名就收。

睡觉的那两件家什,4.1 押给《条件等待》的票,本章咱们照旧认,只交代一句:Mutex 与 Semaphore 的门,本身其实也是一把自旋锁,门里办的是短活,出了门才睡。4.1 讲长凳的时候,案三留过的是半句,门闩套门闩的里层结构,咱们下一章细看,看的正是门里的自旋锁。门身上还留着一笔措辞的差,咱们原样摆开、不互相迁就。Mutex::lock 的门头挂着一句注释,`keep IRQs masked until park_current() has committed`,落在了 sync.cpp:143。注释念的名字 park_current,说的是连着的两步。两步连写的真身,如今只在调度器那边整存了一份,家安在了 kernel/proc/scheduler.cpp:380,前一半叫的是 `prepare_to_wait`,后一半叫的是 `schedule_blocked`。门这边早改成分家的走法:prepare_to_wait 在锁里(:154),schedule_blocked 挪到了锁外(:156),注释底下挂的调用(:156)已经换了新名字。注释是旧的,身子是新的,两边都原样地摆着,谁粗谁细的官司,归的是《条件等待》,本章只记下了这笔、不替两边裁断。

**您停一停,全机一册的冤枉,您拿两颗核推一遍:那声 Check 喊出来的时候,真凶藏身的地方在哪,无辜的又是谁?**

咱们把 Check 喊出来的现场走一遍。真凶是在另一颗核上的,活得好好的,门闩揣在了手里,自己的岗自己没过。无辜者说的是本核,两手是空空的,被一本记着别人忙碌的册子拦在床外。按核发了册之后,岗哨只翻本核的一格,一颗核的忙碌不再连坐另一颗核的睡眠。家法是没换的,管的手换成了各管各的。

## base 的互斥件:光秃秃,零耦合

门闩的一身家什,咱们数一数:护甲、登记簿、验手、核号,全是内核的属地,关中断动的是 arch 的真旗,登记簿连着调度器的家法。base 住的世界比内核低一层,那边也有共享的财产要护,咱们得有一件不带这些羁绊的家什。它叫的是 SpinMutex,家安在了 spin_mutex.hpp,76 行的身量,拿锁的真身在 :49 起:

```cpp
void lock() {
    while (held_.test_and_set(std::memory_order_acquire)) {
        while (held_.test(std::memory_order_relaxed)) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
    }
}
```

放锁的那一行在 :62,写的是 held_.clear(std::memory_order_release),与门闩是同款的。头一层的功夫,进门那一拍是与门闩同宗的:test_and_set 配 acquire,翻成了就算拿到。第二层咱们看它省着过的圈:没翻成的退到圈外,只拿普通读的 test 去盯旗,旗立着的时候就等,旗倒了再上去翻一次。空转的圈里咱们不做原子的交换,省得一遍遍地惊动缓存,正文里的原话。白话要补的一句:原子那一拍要把旗所在的缓存行攥在手里独占,两颗核对同一格旗轮流地攥,谁都快不了,普通读是只看不动手的、便宜得多。第三层的机关,圈里的那声 atomic_signal_fence,拦的是编译器:不许它把等待优化没了,也不许把圈里圈外的读写挪错位。处理器层的顺序,由拿与放自身的语义担着,往深处去归的是《内存序与缓存一致性》。

咱们拿它与内核的门闩对一张表:门闩带护甲带登记带验手带核号,防的是中断、对面、自己的手滑。SpinMutex 是光秃秃的,只防对面伸过来的手。各配各的仗,内核里的临界区三路对手都在,base 的临界区只当心另一颗核。

用它的人,咱们数一遍:页分配器 pmm.hpp:139 一把,堆在 heap.hpp:110 的一把,堆增长的长事务走 heap_runtime.cpp:23 的 g_heap_gate,console.cpp:12 的 g_console_gate,锁序图的边图自己,lockdep.cpp:15 的 g_graph_gate。五件各住各的家:页与堆的三件在 kernel/mm,console.cpp 的家在 kernel/console,lockdep.cpp 的家在 kernel/proc。它们护的是全内核共享最狠的状态:页的名册,堆的链,屏上的字,图的边。单核的时代这些名册靠关中断撑腰,双核上关中断只是本核的清静,挡不住对面伸过来的手。挂了锁之后,守卫照旧:一个防本核的中断,一个防对面的手,两层各管各的对手。

分工的活证,咱们请 console 的 PutString 出来,从它的 :31 起:

```cpp
void PutString(const char* text, bool newline) {
    const arch::IrqGuard kIrq;
    const auto           kLock = g_console_gate.guard();
```

两行一前一后穿在同一个函数身上:护甲防本核的中断,门闩防对面的手。您回头看上一节的表,在这儿落了地。

**您停一停,SpinMutex 为什么干脆不带护甲,让用家自己穿?您想:它一碰关中断,户口还保得住吗?**

是保不住的,您跟着真旗走一遍。关中断要动的是真旗,真旗的家住在 arch 里头。SpinMutex 的家住在 base,base 不认识 arch 的旗,也不认识什么任务,零系统耦合是它住 base 的户口本。把护甲长在了身上,它就得搬去内核了,宿主长凳那些没有真旗的世界,也就用不了它。用家自己穿的话,谁有哪路对手谁就自己配:console 的两行证人,一层防的是本核中断,一层防对面的手。pmm 与堆那边的守卫也照旧,道理是同源的。

## 计数这门亲戚:丢更新,与两桩案

门闩护的是路,这样的路进去一次只许一双手。还有一类财产护的是数:对象活着,因为攥着它的手还有几只,册上记的是手数。计数这门亲戚 4.1 在 badcnt 里见过的是它丢更新的病历,咱们自家的病历更贴近:共享指针用的那个计数类,头注释写的是:调用方自己串行。单核的年代这句是兑现过的,调度的时序恰好把交错挡在门外,宿主的案子一路绿。到了双核真跑共享信号处理,调用方换成了两颗核,谁也不给谁串行的照顾,绿就绿到头了。

裸的加一减一在两颗核上丢更新,丢法您在 4.1 的汇编三拍里见过:两边同读一个旧值,一头加好了一写回,另一头也拿手里的旧值加一写回,后写的盖掉早写的,一次加法就凭空蒸发了。计数错了一次,两头讨到的都是祸。数要是早到了零,提前析构踩空的就是指针。数要是到不了零,永不析构漏掉的就是内存。badcnt 丢的是答案,这里丢的是攥着对象的手数,祸是更近身的。

修好的真身落在了 ref_count.hpp,73 行的身子,计数住进对象自己的一格。retain 与 release 的一对在 :48 与 :60,咱们整段摆:

```cpp
void retain() {
    const Count kBefore = count_.fetch_add(1, std::memory_order_acq_rel);
    safety::Check(kBefore != 0, "reference acquired without a live owner");
    safety::Check(kBefore != std::numeric_limits<Count>::max(), "reference count overflow");
}
```

```cpp
[[nodiscard]] bool release() {
    const Count kBefore = count_.fetch_sub(1, std::memory_order_acq_rel);
    safety::Check(kBefore != 0, "reference released without an owner");
    return kBefore == 1;
}
```

fetch_add 与 fetch_sub 的这对,读改写捆在不可分的一拍,后写的盖不掉早写,丢更新的窗口从根上堵死。count_ 出生记的是一,创建者的头一笔从第一格就记上。咱们看 retain 两道 Check:零不许再拿,满了不许再涨。release 的 Check 拦瞎还,返回的是 kBefore == 1:减之前的数是一,放的就是最后一人,析构的活归这一下。零与溢出的检查照留,原子化也没换来什么宽恕,这话也是正文里的原话。宽恕是没有的,顺序倒给得了一对:减到零的那颗核,必须看得见所有曾经持引用的核写下的全部操作,保证这一点的,靠的正是拿与放的一对顺序。

说到顺序的事,另一笔措辞的差就在这儿,咱们照旧并行摆开。正文《请第二颗脑子上线》那一卷当时说的是,顺序咱们选 acquire 配 release 那一档。代码今天的实况:fetch_add 与 fetch_sub 落的都是 std::memory_order_acq_rel,归的就是一档。一边嘴里说的是两样:拿配的是获取,放配的是释放。一边纸上记的是一档。两边的代码都不改,咱们也不替它们圆场,凭什么一档当得起两样,往深处去归的是《内存序与缓存一致性》。

案子一追的是创建者那笔,是没人还的。管道创建的那一刻,裸指针造了出来,自带一笔创建者的初始引用。文件表的两端各留一笔,合着的正是三笔。两个文件描述符都关干净了,只从三减到一:您看描述符那边的数,笔笔是对得上的,悬空的是创建者那一笔。测试一波接一波地开关描述符,管道一根一根地攒着,悬着的笔不会安静躺到天亮,正文里的原话。修法:创建路径改用带归还语义的共享指针,创建那一刻的初始引用,从此每一次返回、每一个出口都自动地归还,错误路径上手工删对象的老写法一并清掉。

案子二办的是寿命案,防的是测试自己撒谎。设局:预分配六十五个不同地址的管道,逐一地用完、销毁、换下一个。任何时刻活着的只有一组锁,而历史用过的地址越过了六十四。这案子专门抓死身份的累积:测试要是图省事复用同一个地址,新对象顶着旧对象用过的门牌,测出来是假的绿。为什么偏偏是六十四这道坎,那本记锁身份的册子,容量几多、锁的生卒怎么管,下一章开了门就讲,咱们记下数就走。

## 输出串行:一口气说完一条消息

末一位的护法,管的是咱们自己的嘴。串口与屏幕原来是一个字一个字地往外蹦,排队的人是没有的,双核同时印的时候,字节胡乱地交织,面板上出来谁也不认识的火星文。考古的一句,正文里记过:更早的工程线上这病当过路障,日志都读不利索了,办案的人连现场都看不清,当年是给输出加了行级锁,才把后面的案子破了。

这一次的修法直接到位:整条格式化输出串行,一条消息从头到尾一口气地出完,持锁的是整条消息,不是单个的字节。文本控制台暂吃一把粗的锁保正确,细粒度的锁缓办,这事正文里是记着的。粗锁您已经见过,console.cpp:12 的 g_console_gate。PutString 的两行证人,上上节是摆过的。锁里只有搬字的活,一口气地搬完,正是自旋件该配的短仗。

## Code: Cinux 实况:双核的正脸,二十二案

实况锚的两站,报法照 1.4 的成例:旧形状锚在 r12_process,4.1 刚刚走过了一趟,面板与起机咱们不重印。本章的实况锚换 r26_smp,第二颗核上线之后的家。您要亲手翻的话,去的时候 git switch --detach r26_smp,回来的时候 git switch -,克隆在手的读者,切回来落回的还是 main。

起机的面板,这一趟是带着 ahci 与 ext2 的盘起的机,双核的正脸咱们抄一段:

```text
[smp] 2 CPUs online
[cpu 0] APIC 0 online, LSTAR ffffffff80223e81
[cpu 1] APIC 1 online, LSTAR ffffffff80223e81
[kern] 64-bit C++ world alive
[kern] gdt+idt self-owned, sse on
[kern] tss loaded tr=18 gs base FFFFFFFF8023B000 efer 501
[kern] kernel gs base FFFFFFFF8023B000
[kern] bootinfo: 7 e820 entries, fb 1024*768*32
[kern] screen console 128 cols 48 rows
[kern] kernel at 200000 size 4E4FC0 entry 200040
[kern] pmm: 31204 pages free (probe 116000 ok)
[kern] vmm: address space up, magic 114514
[kern] heap: new/delete live (probe 114514)
[kern] irq on, tick 100Hz
```

头三行是 r26 的正脸:两颗核各自报了到,APIC 各领各的门牌。pmm 的一行,链上的数咱们续一笔:1.7 报 32350,r12 报的是 32336,3.3 报的是 31272,今天 r26 报的是 31204,各认的是各锚。后头的 rtc 对时、键盘上岗、boot task 让位给 kernel_init、root mounted,一路走到了 cinux> 提示符,shell 是从盘上起的身。

长凳的那边,您自己跑:

```bash
git switch --detach r26_smp
cmake -B build-r26
cmake --build build-r26 --target test_sync test_lockdep test_ref_count
build-r26/test/test_sync
build-r26/test/test_lockdep
build-r26/test/test_ref_count
```

两笔的注记,咱们交代在前。宿主的全量构建在本机的 GCC 下,test_heap 会被 -Werror=array-bounds 误伤:那个测试故意把池外的指针递给 free,验的是 free 的防御,本机的编译器把故意当成了越界,与本章是无关的,只建上面的三件就绕开了。起机的时候,您照 CMakeLists 里 CINUX_QEMU_DEVICE_ARGS 的配置来,写的就是 -accel kvm -smp 2。boot 镜像之外要带上 ahci 与 ext2 的盘,不带的话 file_world init 挂盘失败,当场就 panic 了。

下面的屏,咱们原样抄:

```text
=== Cinux Test Runner ===
Running 9 case(s)...

[PASS] sync: spinlock counts holders and names them
[PASS] sync: nested spinlocks unwind their count
[PASS] sync: uncontended mutex lock and unlock round-trips
[PASS] sync: semaphore counts units and never parks while stocked
[PASS] sync: a completion finishes once and publishes its verdict
[PASS] sync: a stocked completion answers wait without parking
[PASS] sync: rearm returns a terminal completion to pending
[PASS] sync: a named lock answers the dump with its label
[PASS] sync: an unnamed lock dumps by address and retire clears the name

=== Results: 9 passed, 0 failed ===
```

```text
=== Cinux Test Runner ===
Running 8 case(s)...

[PASS] lockdep: CPU held stacks are private while lock order edges cross CPUs
[PASS] lockdep: another CPU observes established lock-order cycles
[PASS] lockdep: remembers AB then rejects BA
[PASS] lockdep: rejects transitive cycle
[PASS] lockdep: invalid operations leave held stack intact
[PASS] lockdep: forgetting a destroyed lock permits address reuse
[PASS] lockdep: held depth exhaustion fails visibly
[PASS] lockdep: graph capacity is reclaimed on destruction

=== Results: 8 passed, 0 failed ===
```

```text
=== Cinux Test Runner ===
Running 5 case(s)...

[PASS] ref counted: initial owner alone destroys the object at release
[PASS] ref counted: separate owners keep an object alive until the last release
[PASS] ref count: narrow storage reaches its limit and reports only the final release
[PASS] ref count: concurrent owners leave exactly the original reference
[PASS] spin mutex: concurrent critical sections retain every increment

=== Results: 5 passed, 0 failed ===
```

二十二个案子咱们挨个认。test_sync 的九案,头四案是 4.1 长凳上的旧面孔,跑的还是原名:持有者计数与报号,嵌套层数的回卷,互斥锁的一进一出,有存货的信号量不停车,案名是一字没改的。头两案验的还是登记簿那一加一减,册子换了按核记。案三里层的自旋锁,前头交代时接的正是 4.1 留的半句。第五到第七案是 completion 的三案,睡觉一路的家什,案名咱们点到为止,正戏归的是《条件等待》。第八案验的是具名锁应答报号,落点在 sync.cpp 的 set_name(:124)。第九案是无名的按地址报、retire 清名,sync.cpp 的 retire(:86),生卒正戏归的是《死锁与锁序》。

test_lockdep 的八案,头两案是跨核的正脸:一案验各核的持有链是私册、边图跨核共享,正是登记簿那一排册子的对面证人。一案验一颗核看得见另一颗核立下的锁序环。后头的六案打的全是次序的官司:环、传递、容量,咱们点到案名为止,正戏归的是《死锁与锁序》。

test_ref_count 的五案,头三案是单线程的,一案验的是一条语义:初始一人末笔销毁,各持一笔的最后一人销毁,窄格计数到了顶只报末笔。案四是真受保护的计数:两个线程各做十万次的加一减一,断言的是终值分毫不差,4.1 认过的丢更新,这一案就是它的反证。正文里记了一句,咱们头一回敢在自己的机器上写这个数。案五是 SpinMutex 的实弹:两个线程同抢一把 base 件,临界区里的每一笔加一都在,本章的新实弹。sync 的九案,lockdep 的八案,ref_count 的五案,合起来的正是二十二案,案案都过了。

## 对照:同一件事,别的机器怎么做

翻开的还是书页 67,咱们看它家 acquire 的转圈:

```c
while(__sync_lock_test_and_set(&lk->locked, 1) != 0)
```

与咱们两代同款:翻旗,报的是旧值,旧值不是零的话就再问。书里讲了这记指令的底细,RISC-V 上落在了 amoswap,读改写捆成不可分的一拍、谁也插不进脚。单核年代咱们的门闩天生就会跨核的部分,指的正是这一拍,开篇问框里咱们答过的那半。它家也记持锁的核,原话咱们摘来:

- “acquire records, for debugging, the CPU that acquired the lock. The lk->cpu field is protected by the lock”

记的是拿到锁的那颗 CPU,是给调试用的,字段是由锁自己护着的。咱们的 owner_cpu_ 一格,出身比它多的是一职:它家记核只为调试,咱们兼做重入检查,外加出门清负一的登记。放锁的一头,它家用的是一记 __sync_lock_release,书页的交代是普通的 C 赋值可能被编译器写成多条 store,散了的赋值就不再是原子的一拍,所以它不用。咱们的 held_.clear 与 owner_cpu_.store,两格各有各的原子,做的是同病同防的活。

书页 71 的 push_off 与 pop_off,4.1 引过它的次序警句,本章换的是角度,旧引文也不重印了,单摘半句:它家数的嵌套层数,口径写的正是 `the nesting level of locks on the current CPU`,说的是当前这颗 CPU 上的嵌套。咱们的登记簿,从一册的一个数到一排的一核一格,把同一句口径写成了数组的形状。它家数到零还原最外层进门时的中断状态,咱们按快照还旗,走的是同一门纪律,两家的写法像得很。书页 70 的禁重入立场,4.1 早就引过了,这里补一个新看点:它家拦的是 a CPU 重拿自己持有的锁,口径本来就是按核说的。咱们的 owner_cpu_ 把这句口径记进了锁对象的身子,认的就是,握着锁的手属于哪颗核。

CSAPP 的那一头,计数这门亲戚它家也是认的。badcnt 案 4.1 引过的:两个线程各加一百万次,回回的答案都不同。它家的修法在书页 998 到 999,靠的是信号量,原话咱们摘来:

- “A semaphore, s, is a global variable with a nonnegative integer value that can only be manipulated by two special operations, called P and V”

一个信号量的身子是一枚全局变量,里面装着不许变负的整数,只许的是两只手:P 拿走一个,数目不够的话就等。V 放回的是一个。它家的 sem_wait 对的是 P,sem_post 对的是 V。咱们 Semaphore 的 wait 与 post,名字就是它家的亲戚,睡觉的正戏归《条件等待》,咱们单看立场。badcnt 的修法里,信号量的初始为一,是当锁使的,加一的几行夹在 P 与 V 当中,计数本身还是普通的读写。咱们的修法,是把每一笔加减本身换成原子的一拍,数格自己当的就是锁。同一桩的丢更新,它家给计数器配上的是一把锁,咱们把计数器自己炼成锁,两家的路子。

Linux 的那一头,咱们手里还是没有翻过的手册、读过的源码,比不了的就是比不了,留给将来的书。

## 这一章留下了什么

| 术语 | 一句话口径 |
|---|---|
| 口袋 | 局部变量。快照先进口袋,抢到锁才转存,能写它的只有持锁人 |
| 核号一格 | owner_cpu_,锁对象上的持有登记,出生负一,出门清回负一 |
| 按核一册 | 登记簿的深度从一册变一排,八格数组,一核一格,查自己那格 |
| 护法 | 护住共享财产的家什统称,门闩、互斥件、计数、串行,各配各的仗 |
| SpinMutex | base 的原始互斥件,零系统耦合,不关中断,圈外普通读盯旗 |
| RefCount | 住进对象的计数,retain 加一笔,release 末笔报真 |
| 丢更新 | 两边同读旧值各写各的,后写盖早写,一次更新凭空蒸发 |
| 输出串行 | 一条消息一口气出完,持锁的是整条,不是单个字节 |

本章收进兜里的,咱们数一数:三句实话句句都还清了,快照进了口袋,重入认的是核号,核号的一格兼着登记。一本登记簿从一册发成了一排,一条家法换了口径,立场是没换的。四位护法各就各位:门闩修齐了跨核的功课,门里门外两层守卫各防各的对手,base 里一枚光秃秃的互斥件,对象身上一件住进去的计数,输出串行的一条道。两桩引用的案:一笔是创建者的欠款,一场是六十五个地址的寿命局。两笔措辞的差并排摆开,咱们谁也没迁就谁。

门闩毕业了,旧债还清了,可锁自己的一生,咱们一个字没管。锁生的时候要登记,死的时候要销册,那本册子的容量是六十四个身份。在双核的实弹里,锁序图那套检查的镜像,一声容量断言炸响了:满员了,第一个进不去门的,是管道的加写者路径。册子为什么满,满的又是什么,下一章《死锁与锁序》开门就办的就是这桩官司,您带着断言过去。

## 练一练

三道题留给您:一道是动手推演的,一道是回头望的,一道是读屏的。

**题一:推演的是快照。** 快照的转存要是被撤掉,退回的是旧形状,进门就把快照写进了锁对象。两颗核一前一后地进门,您把早到者出门的一步走一遍:RestoreIrq 还的是什么?要见着真祸,早到者与后来者得各是什么状态进的门?

**题二:回望的是重入。** 重入改认了核号之后,您拿单核推一遍:这圈子是缩小了,扩大了,还是只是画准了?再看那行消息 “spinlock re-entered on one core”,单核的年代与双核的今天,各是什么成色?

**题三:读的是屏。** 屏上的二十二案,您来对号:哪几案是 4.1 长凳四案里的旧面孔?哪一案是 SpinMutex 的实弹?总数怎么加出来的?

### 参考答案

题一:后来者的一笔把早到者存的快照覆盖了。咱们跟着早到者走:它抢到锁,出门的那声 RestoreIrq,还原的却是后来者进门时拍下的样子。真祸是得凑一对儿的:早到者关着闸进门,比如身上是穿着护甲的,后来者是开着闸进的门。这么一覆盖,早到者出门就把闸还原成了开,它进门的时候明明是关。凭空开出来的闸,让本核的中断落进了还指望闸关着的路段。同一段的代码,两次跑出的是两种旗,开与关从此成了随机,预审第三句的病,退回旧形状它就原样地复发。

题二的答案:没缩也没扩,咱们说是画准了。单核上的核号永远是同一个值,重入照旧是当场叫的,当年 Check 拦的是重入,今天拦的还是重入,一条不多、一条也不少的。圈子真正变宽的地方在双核:对面核持锁,本核去抢的时候,旧 Check 是会叫的,新 Check 是不叫的,放出来的正是正当的争抢。这行消息也跟着变诚实了:单核年代它是全文,全文的世界里只有一只手。双核上它取的是字面,叫出来的唯一情形,真的就是同一颗核。

题三的答案:旧面孔的四案,咱们对号:`spinlock counts holders and names them`、`nested spinlocks unwind their count`、`uncontended mutex lock and unlock round-trips`、`semaphore counts units and never parks while stocked`,4.1 长凳四案的原班,r26 拿原名重跑了。SpinMutex 的实弹是 test_ref_count 第五案,案名的整句是 `spin mutex: concurrent critical sections retain every increment`。总数这么加:sync 九案,lockdep 的八案,ref_count 的五案,合的正是二十二案,几块屏各报各的 passed,failed 报的是 0。

## 延伸

- xv6 书第 7 章(书页 63 到 74):本章新引 67,连同 4.1 的五页,咱们累计引了六页。没引的几页里,睡眠锁一段与《条件等待》接边,死锁一段是《死锁与锁序》的,各归各家,您拿着页号提前翻也无妨。
- CSAPP 2e 第 12 章 12.5 节(电子版正文页 992 到 999):badcnt 与信号量连着读,994 的汇编三拍到 998 的 P 与 V,正是丢更新从病历到药方的全程。印本页码随版式走,您认节名就好。
- 正文《请第二颗脑子上线:APIC、蹦床与共享调度》一卷:《同步层的旧债》是本章三处修法与几位护法的全本,《锁的生卒》是下一章那声容量断言的全案。
- 正文《轮流当家》那一卷《转圈的门闩》一篇:旧形状的出生地,4.1 引过,本章两代对读的旧半边,全在那篇。
- 《死锁与锁序》《条件等待》几章在册待写:册子的容量官司、睡觉的正戏,各归各家。
