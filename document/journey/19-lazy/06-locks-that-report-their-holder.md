---
title: "06 · 锁报名号"
description: "往后有一卷要请第二颗 CPU 进门,在那之前得有抓竞态的手艺。座次图是无盘起家那一卷给自旋锁立下也教过的:谁在谁里面,连成环就大声报,可它记的只是地址。这一卷给锁添名号:身份槽预插,转储面把持有链从外到内一行行报,检查失败亮链再响。拦路的两处:关闭检查的世界里转发体照样要编译过,空操作的镜像脸才是正解。默认构建里早退的用例,真行为要到开着检查的构建里跑过才算数。"
chapter: 19
order: 6
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - debugging
  - concurrency
---

# 锁报名号

本节看着不像内存的事,咱们把它安在这,图的也是再往后的一卷:要请第二颗 CPU 进机器的门。两颗核心在咱们机器里一起跑,两双手一起摸的常是同一把锁,竞态就成了日常。竞态一类的病,出了事再抓,现场早就没了,得靠平时攒下的名册。名册咱们其实攒了一半:无盘起家那一卷给全内核的自旋锁立过一张座次图,谁在谁的里面拿着,一层一层地记着,新拿一把锁要是跟已持有的连成环,当场报给你,递归拿自己的也照样报。那一卷还在自己的章节里把它的用法教过一遍,可它记下的只是地址。串口上响的那一行,报的是一个地址,您拿着地址对源码,翻了好一阵子才对上是谁。

这一卷把名字补上了。咱们抢在锁第一次被拿之前把名号登进名册:是哪个锁、叫什么,一个身份槽也预插了进去。往后持有链上的每一行,报的就是名号,不是光秃秃的地址。临时锁退役的时候,名号跟着清掉了,地址这东西是会被复用的,旧锁退了、新锁搬进同一个地址,名号要是不清干净的话,新锁会顶着旧锁的名字活在名册里,转储出来一行张冠李戴的口供。转储面也立起来了:持有链从最外层到最内层,一行一把地报,谁包住了谁,您一眼看完。往哪儿报?这里咱们多留了个心眼,报的地方做成一个函数指针的座,内核里装的是串口的桥,宿主的测试世界里装的是测试自己的捕获器,一行行接进测试的断言里。名册的内核不认识控制台,内核和测试各自配各自的出口,谁也不绑架谁的地盘。

最能救命的是次序:检查失败,头一步把持有链亮了出来,响是第二步的事。死锁这一类案子的现场,就藏在持有链的字里:A 抱着 B 等,B 的持有者是 C,而 C 又在等 A。您要是光让机器响一声停下来,重启之后什么都没有。转储的那一拍在前,响的那一拍在后,串口上留的是案发当时谁拿着什么,咱们拿这一行行当尸检报告。测试两头都按住了:名册自己的套件里,A 在 B 里稳稳地拿着,名册认了,倒过来 B 再 A 的话,名册当场拒了,隔着传递连成的环也拒。转储的路径,宿主的测试装上自己的捕获器去听:拿一把登了名的锁,转储的那一行必须报名字,没登名的报地址,退役的连名带地址一起清。

这一批有三处拦路的,咱们挨个记。头一处是编译世界的事,您听着可能有点绕,咱们慢慢说。锁序的检查,默认的构建里是关着的,开着拖的是速度,教学内核认了默认关。可名册的代码是一个模子:同一次转发,检查开着的世界里交给真的座次图,关着的世界里交给一副空操作的镜像。首版的如意算盘,是拿编译期的分支把镜像里的代码划掉,结果碰了壁:if constexpr 这门把式,救得了跟着类型参数走的代码,救不了不依赖谁的普通成员。编译器编译的是您写下的每一行,不是您正跑着的那个世界,关着的世界里,转发体引用的名字一样要存在、要合法。正解是给关着的世界也造一副合法的脸。咱们把两张脸并排端出来。头一张是真的,就是座次图的正身:

```cpp
class LockOrder {
public:
    /// Result of an acquisition or release validation.
    enum class Status : unsigned char {
        kOk,
        kCycle,
        kRecursive,
        kCapacity,
        kNotHeld,
        kOutOfOrder,
    };
    /// Maximum distinct live locks and maximum simultaneous nesting.
    static constexpr unsigned int kLocks = 64;
    static constexpr unsigned int kDepth = 16;

    /// Record an acquisition only when it preserves an acyclic graph.
    [[nodiscard]] Status acquire(const void* lock);

    /// Release only the innermost held lock, preserving IRQ restore order.
    [[nodiscard]] Status release(const void* lock);

    /// Remove a destroyed, unheld lock and every incident edge.
    [[nodiscard]] bool forget(const void* lock);

    /// Register the human name of a lock ahead of its first acquisition.
    void name_lock(const void* lock, const char* label);

    /**
     * @brief   Walk the held chain, one sink call per lock, innermost last.
     * @since   0.4.0
     */
    void dump_held();
```

真脸的这一头,acquire、release、forget 管的是记图、查环、退名号,转发体天天调的是末尾的两个入口:name_lock 管的是登名号,dump_held 管的是报持有链。再看关着的世界里的另一副,咱们在锁的基类里找到它:

```cpp
    /// Mirrors the LockOrder faces the ledger forwards to, as no-ops,
    /// so the forwarding code compiles unchanged in dump-free builds.
    struct InactiveOrder {
        void name_lock(const void* /*lock*/, const char* /*label*/) {}
        void dump_held() {}
    };
    using Order = std::conditional_t<kLockdepEnabled, LockOrder, InactiveOrder>;
    unsigned int                depth_ = 0;
    [[no_unique_address]] Order order_{};
```

镜像脸跟真脸是同名同参的,函数体是空的,什么都不干,但是真的在。承重的是 using 那一行,咱们让 std::conditional_t 按开关挑脸:开着的世界拿的是 LockOrder,关着的世界拿的是 InactiveOrder,转发体只认入口的名字,不认脸后面站的是谁,所以同一份转发代码,两个世界各自编译各自的,都绿了。注释里那句 Mirrors the LockOrder faces 把身份讲明白了:它是真脸的镜像,不是真脸的缺席。[[no_unique_address]] 还给它免了房租,关着的世界里这枚脸一个字节都不占。咱们要的就是这个效果:功能是可以有开关的,代码是没有的,写下的每一行,关着的世界也得养得起。

第二处其实不大,但是丢人,咱们照实记。转储报地址的那一行,首版把名册槽位的指针当成了锁的地址报出去。宿主的测试当场抓获:报出来的数对不上任何一把锁的地址。改法是把引用解了再报。抓它的是测试自己:捕获器把报出来的每一行都接进了断言,错了一行就当场红,这一案就是被自己的测试当场按住的。

末一处算不上什么难关,它其实是纪律,咱们立成家法:同一个用例,两个世界都跑过了才算数。默认的构建里检查关着,转储的用例进门就知道自己无事可做,早退了,绿了。这个绿什么都没证明。真行为在开着检查的构建里:环真的被造出来,链真的被转储,响真的响了。这一卷往后的验收,四个构建配置一起上了,这套家法就是从这儿来的。锁会报名号了,可这一卷还欠着您两笔生与收那一卷的旧话,下一节咱们销旧。
