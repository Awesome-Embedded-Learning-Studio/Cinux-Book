---
title: 02 · 锁的实现
---

# 02 · 锁的实现:三句实话,三处修法,几位护法

4.1 收卷,咱们留了三句实话,说好下一章开门就办。今天门开了,头一件事,还这三句旧债。三句原样再念一遍,立在本章门口:那把转圈的门闩,单核时代立的,其实不会跨核。重入的检查,到了双核会把正当的争抢误当成自己人。进门存进锁里的中断快照,会被后进门的对手覆盖。前一章末尾还说,病根您都已经攥在手里了。攥了一章,今天派用场:三句对三处修法,一处一个伤口,咱们两代代码对读,把每一针缝在哪一行看清楚。

这一章的活,咱们数下来还是三样。头一样,还旧债:快照进口袋,重入认核号,核号一格当登记,三处修法,每处都对着旧形状读。再一样,认护法:护法这个词今天头一回用,配句白话,护住共享财产的家什,统称护法。门闩之外还有几位,base 里一枚光秃秃的原始互斥件,对象身上一件住进去的计数,输出那里一条串行的道,各配各的仗。末一样,办两桩引用的案:创建者那笔没人还,还有一场预分配六十五个地址的寿命局。

量法还是三条。码,两代对读:旧形状是 r12 的 sync.hpp,261 行,sync.cpp,120 行,4.1 量过。今天的主角是 r26 修好的形状,r26 就是本章实况锚 r26_smp 的简称,sync.hpp 长到 393 行,sync.cpp 长到 263 行。base 两件新面孔,spin_mutex.hpp,76 行,ref_count.hpp,73 行。屏,实况锚两站,照 1.4 跨站的成例:r12_process 4.1 刚走过,面板不重印。第二站换 r26_smp,双核的正脸头一回进教科书层。书,xv6 第 7 章,4.1 核过的五页您随时翻得回去,本章回望 70 与 71,新核 67。CSAPP 2e 的 12.5 节,992 到 995 复用,新核 998 到 999。原话一律整句摘,页号写明,您对着书核。

> **门闩的心脏,test_and_set 那一拍,咱们一格没动。您想:单核时代立的这把锁,哪一层天生就会跨核,哪一层压根没想过跨核?**
>
> 天生就会的是翻旗那一拍。原子指令本来就是给两条执行流抢同一格内存造的,单核上它防的是中断这只脚,双核上它照样防得住对面的手。压根没想过跨核的,是旗周围的簿记:快照存给谁,重入认谁的手,登记记在谁名下,三本册子全是按全世界只有一只手立的。三句实话,句句戳在簿记上,一格都没戳在旗上。

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

修法一,快照,收进口袋。旧形状进门就把中断状态快照写进锁对象自己的成员里,成员正是 irq_state_,这也是正文《请第二颗脑子上线》那一卷的原话。单核上它独享。双核上两颗核一前一后进门,后到的把早到者存的覆盖了,早到者出门还原的,是后来者进门时的中断状态,开与关从此成了随机,这句是正文里的原话。修法您在代码里看得见:SaveAndDisableIrq 落进局部 kIrq,那是自己的口袋。转圈,抢锁,抢到了才转存进锁对象(:101)。转存的那一刻起再没人能覆盖它,因为能写它的只有持锁人,这话也是正文里的。

修法二,重入,认核号。旧 Check 断言的是锁没被持有,单核上它拦的确实只有重入。双核上对面核名正言顺地持锁,本核名正言顺地争抢,断言劈头把自己人骂了一顿,争抢还没开始就误判成了自锁。修法:PerCpu 报自己的核号,断言改问 owner_cpu_ 是不是等于本核。本核重复进门,照旧当场叫。对面持锁,咱们安安静静转圈等。那行消息 “spinlock re-entered on one core” 一字没改,成色变了:单核年代它是全文,双核上它字面为真,叫出来的唯一情形,真是同一颗核。次序也挪了一处,Check 从头一排挪到 SaveAndDisableIrq 之后。4.1 的题二推过 Check 走在闸前的窗口,闯不了祸,顶多多等一轮。r26 干脆把窗口关死,问核号的判断也落进闸里做。

修法三,核号一格,兼了登记。锁对象上新添一格 owner_cpu_,sync.hpp:237,出生负一:核号从零起数,零号核是一颗真核,空位只能往负数借。出门时清回负一,顺便当了持有者的登记。咱们再看它与 holder_ 的分工:holder_ 记哪个任务,给放锁验手用。owner_cpu_ 记哪颗核,给重入检查用。一格一格,各记各的。

出门的一侧,r26 的 unlock 与 r12 比是三笔小改,咱们不整段印,逐笔念。头一笔,快照挪进局部 kIrq(:109),后头清持有者、销核号、落册、倒旗,谁也碰不到它。第二笔,核号清回负一(:115)排在旗倒(:116)之前:上一位的收尾,得在交旗之前落定。次序要是反过来,另一颗核抢到旗、写好自己的核号之后,上一位迟来的清号才落到,把新登记抹掉,重入检查当下看不见这把锁真正的主人。末一笔,RestoreIrq 还的,就是局部里存着的。验手的 Check 没动,放这把锁的必须是持有者本人,别人代放照旧吃到响亮的拒绝,与旧年代一字不差。

## 登记簿搬家:一册变一排

门闩修完,登记簿跟着搬家。4.1 认过它:拿锁记一笔,放锁减一笔,depth() 报数,零才许睡。单核年代全机一册就够,深度天然只有一份。双核上,全机一册开始冤枉人,您拿两颗核想一遍:一颗核揣着门闩没放,另一颗核要睡,岗哨翻开册子,深度不是零,把正当的睡觉拦下了,拦的声音还是 Check 喊的,喊的名字是无辜那颗核。

修法,咱们把册子按核发。depth() 的新真身,sync.cpp 的 :16 到 :18:

```cpp
unsigned int SpinLedger::depth() const {
    return depth_[arch::per_cpu::PerCpu::self().cpu_id];
}
```

深度从一册一个数,变成一格数组,sync.hpp:105,depth_[arch::per_cpu::kMaxCpus],八格,机器至多八颗核,一核一格,各查各的。家法一个字没改,调度器的岗哨问的还是 depth(),答数换了来源:本核的深度。两颗核守好自己的岗,谁也不碍谁的事,这句是正文里的原话。同样的待遇也给锁序图:每核一条私册的持有链,全机共享一张记谁在谁前的有向边图,边图自己另有一把原始的小锁护着。图的官司与生卒的正戏,归《死锁与锁序》,咱们点到名就收。

睡觉那两件家什,4.1 押给《条件等待》的票,本章照旧认,咱们只点名一句:Mutex 与 Semaphore 的门,本身也是一把自旋锁,门里办的是短活,出了门才睡。4.1 讲长凳的时候,案三留过半句,门闩套门闩的里层结构,下一章细看,看的正是门里的自旋锁。门身上还有一笔措辞差,咱们原样摆开,不互相迁就。Mutex::lock 的门头挂着一句注释,`keep IRQs masked until park_current() has committed`,sync.cpp:143。注释念的名字 park_current,说的是连着的两步。两步连写的真身,如今只在调度器那边整存着,kernel/proc/scheduler.cpp:380,前一半 `prepare_to_wait`,后一半 `schedule_blocked`。门这边早改成分家的走法:prepare_to_wait 在锁里(:154),schedule_blocked 在锁外(:156),注释底下挂的调用(:156)已经换了新名字。注释旧,身子新,两边都原样摆着,谁粗谁细,归《条件等待》,本章只记下,不裁断。

**停一停,全机一册的冤枉,您拿两颗核推一遍:那声 Check 喊出来的时候,真凶在哪,无辜者在哪?**

咱们把 Check 喊出来的现场走一遍。真凶在另一颗核上,活得好好的,门闩揣在手里,自己的岗自己没过。无辜者是本核,两手空空,被一本记着别人忙碌的册子拦在床外。按核发册之后,岗哨只翻本核的一格,一颗核的忙碌不再连坐另一颗核的睡眠。家法没换,管的手换成了各管各的。

## base 的互斥件:光秃秃,零耦合

门闩一身家什,咱们数一数:护甲、登记簿、验手、核号,全是内核的属地,关中断动的是 arch 的真旗,登记簿连着调度器的家法。base 住的世界比内核低一层,那边也有共享的财产要护,咱们得有一件不带这些羁绊的家什。它叫 SpinMutex,spin_mutex.hpp,76 行,拿锁的真身,:49 起:

```cpp
void lock() {
    while (held_.test_and_set(std::memory_order_acquire)) {
        while (held_.test(std::memory_order_relaxed)) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
    }
}
```

放锁一行,:62,held_.clear(std::memory_order_release),与门闩同款。头一层,进门那一拍与门闩同宗:test_and_set 配 acquire,翻成了就算拿到。第二层,咱们看它省着过的圈:没翻成的退到圈外,只用普通读 test 盯旗,旗立着就等,旗倒了再上去翻一次。空转的圈里咱们不做原子的交换,省得一遍遍地惊动缓存,正文里的原话。白话补一句,原子那一拍要把旗所在的缓存行攥在手里独占,两颗核对同一格旗轮流攥,谁都快不了,普通读只看不动手,便宜得多。第三层,圈里那声 atomic_signal_fence,拦的是编译器:不许它把等待优化没了,也不许把圈里圈外的读写挪错位。处理器层的顺序,由拿与放自身的语义担着,往深处去归《内存序与缓存一致性》。

咱们拿它与内核的门闩对一张表:门闩带护甲带登记带验手带核号,防中断、防对面、防自己手滑。SpinMutex 光秃秃,只防对面伸过来的手。各配各的仗,内核里的临界区三路对手都在,base 的临界区只当心另一颗核。

用它的人,咱们点名:页分配器 pmm.hpp:139 一把,堆 heap.hpp:110 一把,堆增长的长事务走 heap_runtime.cpp:23 的 g_heap_gate,console.cpp:12 的 g_console_gate,锁序图的边图自己,lockdep.cpp:15 的 g_graph_gate。五件各住各处:页与堆三件在 kernel/mm,console.cpp 在 kernel/console,lockdep.cpp 在 kernel/proc。它们护的是全内核共享最狠的状态:页的名册,堆的链,屏上的字,图的边。单核时代这些名册靠关中断撑腰,双核上关中断只是本核的清静,挡不住对面伸过来的手。挂锁之后,守卫照旧:一个防本核的中断,一个防对面的手,两层各管各的对手。

分工的活证,咱们请 console 的 PutString 出来,:31 起:

```cpp
void PutString(const char* text, bool newline) {
    const arch::IrqGuard kIrq;
    const auto           kLock = g_console_gate.guard();
```

两行一前一后穿在同一个函数身上:护甲防本核的中断,门闩防对面的手。您回头看上一节的表,在这儿落了地。

**停一停,SpinMutex 为什么干脆不带护甲,让用家自己穿?您想:它一碰关中断,户口还保得住吗?**

保不住,您跟着真旗走一遍。关中断要动真旗,真旗住在 arch。SpinMutex 住在 base,base 不认识 arch 的旗,也不认识任务,零系统耦合是它住 base 的户口本。把护甲长在身上,它就得搬去内核,宿主长凳那些没有真旗的世界,也就用不了它。用家自己穿,谁有哪路对手谁自己配:console 两行证人,一层防本核中断,一层防对面的手。pmm 与堆那边守卫照旧,道理同源。

## 计数这门亲戚:丢更新,与两桩案

门闩护的是路,进去一次只许一双手。还有一类财产不是路,是数:对象活着,因为还有几只手攥着它,册上记的是手数。计数这门亲戚,4.1 在 badcnt 里见过它丢更新的病历,咱们自家的病历更贴近:共享指针用的那个计数类,头注释写的是,调用方自己串行。单核年代这句兑现过,调度的时序恰好把交错挡在门外,宿主的案子一路绿。双核真跑共享信号处理,调用方换成了两颗核,谁也不给谁串行的照顾,绿就绿到头了。

裸的加一减一在两颗核上丢更新,丢法您在 4.1 的汇编三拍里见过:两边同读一个旧值,一头加一写回,另一头也拿手里的旧值加一写回,后写的盖掉早写的,一次加法凭空蒸发。计数错一次,两头都是祸。数早到零,提前析构,踩空指针。数到不了零,永不析构,漏内存。badcnt 丢的是答案,这里丢的是攥着对象的手数,祸更近身。

修好的真身,ref_count.hpp,73 行,计数住进对象自己的一格。retain 与 release,:48 与 :60,咱们整段摆:

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

fetch_add 与 fetch_sub,读改写捆在不可分的一拍,后写盖不掉早写,丢更新的窗口从根上堵死。count_ 出生是一,创建者的头一笔从第一格就记上。咱们看 retain 两道 Check:零不许再拿,满了不许再涨。release 的 Check 拦瞎还,返回的是 kBefore == 1:减之前的数是一,放的就是最后一人,析构的活归这一下。零与溢出的检查照留,原子化也没换来什么宽恕,这话也是正文里的原话。宽恕没有,顺序倒给得了一对:减到零的那颗核,必须看得见所有曾经持引用的核写下的全部操作,保证这一点的,正是拿与放那一对顺序。

说到顺序,另一笔措辞差就在这儿,咱们照旧并行摆开。正文《请第二颗脑子上线》那一卷当时说的是,顺序咱们选 acquire 配 release 那一档。代码今天的实况:fetch_add 与 fetch_sub 落的都是 std::memory_order_acq_rel,一档。一边嘴里是两样,拿配获取,放配释放。一边纸上是一档。两边都不改,也不替它们圆场,凭什么一档当得起两样,往深处去归《内存序与缓存一致性》。

案子一,创建者那笔没人还。管道创建那一刻,裸指针造出来,自带一笔创建者的初始引用。文件表的两端各留一笔,合着三笔。两个文件描述符都关干净了,只从三减到一:您看描述符那边的数,笔笔对得上,悬空的是创建者那一笔。测试一波接一波开关描述符,管道一根一根攒着,悬着的笔不会安静躺到天亮,正文里的原话。修法:创建路径改用带归还语义的共享指针,创建那一刻的初始引用,从此每一次返回、每一个出口都自动归还,错误路径上手工删对象的老写法一并清掉。

案子二,寿命案,防的是测试自己撒谎。设局:预分配六十五个不同地址的管道,逐一用完、销毁、换下一个。任何时刻活着的只有一组锁,而历史用过的地址越过了六十四。这案子专门抓死身份的累积:测试要是图省事复用同一个地址,新对象顶着旧对象用过的门牌,测出来是假的绿。为什么偏偏是六十四这道坎,那本记锁身份的册子,容量几多,锁的生卒怎么管,下一章开门就讲,咱们记下数就走。

## 输出串行:一口气说完一条消息

末一位护法,管的是咱们自己的嘴。串口与屏幕原来一个字一个字往外蹦,没有人排队,双核同印,字节交织,面板上出来谁也不认识的火星文。考古一句,正文里记过:更早的工程线上这病当过路障,日志都读不利索,办案的人连现场都看不清,当年是给输出加了行级锁,才把后面的案子破了。

这一次的修法直接到位:整条格式化输出串行,一条消息从头到尾一口气出完,持锁的是整条消息,不是单个字节。文本控制台暂吃一把粗的锁保正确,细粒度的锁缓办,这事正文里记着。粗锁您已经见过,console.cpp:12 的 g_console_gate。PutString 的两行证人,上上节摆过。锁里只有搬字的活,一口气搬完,正是自旋件该配的短仗。

## Code: Cinux 实况:双核的正脸,二十二案

实况锚两站,报法照 1.4 的成例:旧形状锚在 r12_process,4.1 刚走过,面板与起机咱们不重印。本章的实况锚换 r26_smp,第二颗核上线之后的家。您要亲手翻,git switch --detach r26_smp 切过去,看完 git switch - 切回来,克隆在手的读者,切回来落回的还是 main。

起机面板,这一趟是带着 ahci 与 ext2 的盘起的机,双核的正脸咱们抄一段:

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

头三行是 r26 的正脸:两颗核各自报了到,APIC 各领各的门牌。pmm 一行,链上的数咱们续一笔:1.7 报 32350,r12 报 32336,3.3 报 31272,今天 r26 报 31204,各认各锚。后头 rtc 对时、键盘上岗、boot task 让位给 kernel_init、root mounted,一路到 cinux> 提示符,shell 是从盘上起的身。

长凳那边,您自己跑:

```bash
git switch --detach r26_smp
cmake -B build-r26
cmake --build build-r26 --target test_sync test_lockdep test_ref_count
build-r26/test/test_sync
build-r26/test/test_lockdep
build-r26/test/test_ref_count
```

两笔注记,咱们交代在前。宿主的全量构建在本机的 GCC 下,test_heap 会被 -Werror=array-bounds 误伤:那个测试故意把池外的指针递给 free,验的是 free 的防御,本机的编译器把故意当成了越界,与本章无关,只建上面三件就绕开。起机要照 CMakeLists 里 CINUX_QEMU_DEVICE_ARGS 的配置,-accel kvm -smp 2。boot 镜像之外要带上 ahci 与 ext2 的盘,不带的话 file_world init 挂盘失败,当场 panic。

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

二十二案,咱们挨个认。test_sync 九案,头四案是 4.1 长凳上的旧面孔,原名重跑:持有者计数与报号,嵌套层数回卷,互斥锁一进一出,有存货的信号量不停车,案名一字没改。头两案验的还是登记簿那一加一减,册子换了按核记。案三里层的自旋锁,前头点名时接过 4.1 留的半句。第五到第七案,completion 三案,睡觉一路的家什,案名点到为止,正戏归《条件等待》。第八案,具名锁应答报号,验 sync.cpp 的 set_name(:124)。第九案,无名的按地址报、retire 清名,sync.cpp 的 retire(:86),生卒正戏归《死锁与锁序》。

test_lockdep 八案,头两案是跨核的正脸:一案验各核的持有链是私册、边图跨核共享,正是登记簿那一排册子的对面证人。一案验一颗核看得见另一颗核立下的锁序环。后六案,环、传递、容量,全是次序的官司,咱们点到案名为止,正戏归《死锁与锁序》。

test_ref_count 五案,头三案单线程,一案验一条语义:初始一人末笔销毁,各持一笔的最后一人销毁,窄格计数到顶只报末笔。案四,真受保护计数:两个线程各做十万次的加一减一,断言终值分毫不差,4.1 认过的丢更新,这一案就是它的反证。正文里记了一句,咱们头一回敢在自己的机器上写这个数。案五,SpinMutex 的实弹:两个线程同抢一把 base 件,临界区里的每一笔加一都在,本章的新实弹。sync 九案,lockdep 八案,ref_count 五案,合起来二十二案,案案都过。

## 对照:同一件事,别的机器怎么做

书页 67 翻开,咱们看它家 acquire 的转圈:

```c
while(__sync_lock_test_and_set(&lk->locked, 1) != 0)
```

与咱们两代同款:翻旗,报旧值,旧值不是零就再问。书里讲了这记指令的底细,RISC-V 上落在 amoswap,读改写捆成不可分的一拍,谁也插不进脚。单核年代咱们的门闩天生就会跨核的部分,正是这一拍,开篇问框里咱们答过的那半。它家也记持锁的核,原话咱们摘来:

> “acquire records, for debugging, the CPU that acquired the lock. The lk->cpu field is protected by the lock”

记的是拿到锁的那颗 CPU,给调试用,字段由锁自己护着。咱们的 owner_cpu_ 一格,出身比它多一职:它家记核只为调试,咱们兼做重入检查,外加出门清负一的登记。放锁的一头,它家用一记 __sync_lock_release,书页的交代是普通的 C 赋值可能被编译器写成多条 store,散了的赋值就不再是原子的一拍,所以它不用。咱们的 held_.clear 与 owner_cpu_.store,两格各有各的原子,同病同防。

书页 71 的 push_off 与 pop_off,4.1 引过它的次序警句,本章换一个角度,不重印旧引文,单摘半句:它家数的嵌套层数,口径落在 “the nesting level of locks on the current CPU” 这半句上,当前这颗 CPU 上的嵌套。咱们的登记簿,从一册一个数到一排一核一格,把同一句口径写成了数组的形状。它家数到零还原最外层进门时的中断状态,咱们按快照还旗,同一门纪律,两家的写法像得很。书页 70 的禁重入立场,4.1 也引过,这里补一个新看点:它家拦的是 a CPU 重拿自己持有的锁,口径本来就按核说。咱们的 owner_cpu_ 把这句口径记进了锁对象的身子,认的就是,握着锁的手属于哪颗核。

CSAPP 那头,计数这门亲戚它家也认。badcnt 案 4.1 引过:两个线程各加一百万次,答案回回不同。它家的修法在书页 998 到 999,信号量,原话咱们摘来:

> “A semaphore, s, is a global variable with a nonnegative integer value that can only be manipulated by two special operations, called P and V”

一个信号量,是一枚全局变量,里面装着不许变负的整数,只许两只手摆弄:P 拿走一个,数目不够就等。V 放回一个。它家的 sem_wait 对 P,sem_post 对 V。咱们 Semaphore 的 wait 与 post,名字就是它家的亲戚,睡觉的正戏归《条件等待》,咱们单看立场。badcnt 的修法里,信号量初始为一,当锁使,加一的几行夹在 P 与 V 当中,计数本身还是普通读写。咱们的修法,是把每一笔加减本身换成原子的一拍,数格自己就是锁。同一桩丢更新,它家给计数器配一把锁,咱们把计数器自己炼成锁,两家的路子。

Linux 那头,咱们手里还是没有翻过的手册、读过的源码,比不了的就是比不了,留给将来的书。

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

本章收进兜里的,咱们数一数:三句实话句句还清,快照进口袋,重入认核号,核号一格当登记。一本登记簿从一册发成一排,一条家法换了口径,立场没换。四位护法各就各位:门闩修齐了跨核的功课,门里门外两层守卫各防各的对手,base 里一枚光秃秃的互斥件,对象身上一件住进去的计数,输出串行一条道。两桩引用的案,一笔创建者的欠款,一场六十五个地址的寿命局。两笔措辞差并排摆开,谁也没迁就谁。

门闩毕业了,旧债还清了,可锁自己的一生,咱们一个字没管。锁生要登记,死要销册,那本册子的容量是六十四个身份。双核实弹里,锁序图那套检查的镜像,一声容量断言炸响:满员了,第一个进不去门的,是管道的加写者路径。册子为什么满,满的又是什么,下一章《死锁与锁序》开门就办,您带着断言过去。

## 练一练

三道题留给您:一道推演,一道回望,一道读屏。

**题一,推演。** 快照的转存要是被撤掉,退回旧形状,进门就把快照写进锁对象。两颗核一前一后进门,您把早到者出门的一步走一遍:RestoreIrq 还的是什么?要见着真祸,早到者与后来者得各是什么状态进的门?

**题二,回望。** 重入改认核号之后,您拿单核推一遍:这圈子是缩小了,扩大了,还是只是画准了?再看那行消息 “spinlock re-entered on one core”,单核年代与双核今天,各是什么成色?

**题三,读屏。** 屏上二十二案,您点点名:哪几案是 4.1 长凳四案里的旧面孔?哪一案是 SpinMutex 的实弹?总数怎么加出来的?

### 参考答案

题一:后来者的一笔把早到者存的快照覆盖了。咱们跟着早到者走:它抢到锁,出门 RestoreIrq,还原的却是后来者进门时拍下的样子。真祸得凑一对儿:早到者关着闸进门,比如身上穿着护甲,后来者开着闸进门。这一覆盖,早到者出门就把闸还原成了开,它进门时明明是关。凭空开出来的闸,让本核的中断落进了还指望闸关着的路段。同一段代码,两次跑出两种旗,开与关从此成了随机,预审第三句的病,退回旧形状它就原样复发。

题二:没缩也没扩,咱们说是画准了。单核上核号永远同一个值,重入照旧当场叫,当年 Check 拦的是重入,今天拦的还是重入,一条不多一条不少。圈子真正变宽的地方在双核:对面核持锁,本核去抢,旧 Check 会叫,新 Check 不叫,放出来的正是正当的争抢。这行消息也跟着变诚实了:单核年代它是全文,全文只有一只手。双核上它是字面,叫出来的唯一情形,真是同一颗核。

题三:旧面孔四案,咱们点名:`spinlock counts holders and names them`、`nested spinlocks unwind their count`、`uncontended mutex lock and unlock round-trips`、`semaphore counts units and never parks while stocked`,4.1 长凳四案的原班,r26 原名重跑。SpinMutex 的实弹是 test_ref_count 第五案,案名整句是 `spin mutex: concurrent critical sections retain every increment`。总数这么加:sync 九案,lockdep 八案,ref_count 五案,合二十二案,几块屏各报各的 passed,0 failed。

## 延伸

- xv6 书第 7 章(书页 63 到 74):本章新引 67,连同 4.1 的五页,咱们累计引了六页。没引的几页里,睡眠锁一段与《条件等待》接边,死锁一段是《死锁与锁序》的,各归各家,您拿着页号提前翻也无妨。
- CSAPP 2e 第 12 章 12.5 节(电子版正文页 992 到 999):badcnt 与信号量连着读,994 的汇编三拍到 998 的 P 与 V,正是丢更新从病历到药方的全程。印本页码随版式走,您认节名就好。
- 正文《请第二颗脑子上线:APIC、蹦床与共享调度》一卷:《同步层的旧债》是本章三处修法与几位护法的全本,《锁的生卒》是下一章那声容量断言的全案。
- 正文《轮流当家》那一卷《转圈的门闩》一篇:旧形状的出生地,4.1 引过,本章两代对读的旧半边,全在那篇。
- 《死锁与锁序》《条件等待》几章在册待写:册子的容量官司、睡觉的正戏,各归各家。
