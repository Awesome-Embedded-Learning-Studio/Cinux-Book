---
title: "05 · 画像三版,窗口三处"
description: "execve 的画像补成全参:字符串从页底向上,指针从 rsp 正向排,argc、argv、envp、auxv 相遇为止,页装不下答 E2big。三版才定形,头两版的病都出在布局没人锁。完成件入队与泊车之间的窗口被崩溃循环逼出原形。I/O error 的案卷请外面的眼睛看了一遍,回来三刀:泊车竞态的第二层、块缓存的悬空指针、整读的 Result 化。战后又冒出 not found 的新悬案。"
chapter: 23
order: 5
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - exec
  - concurrency
  - history
---

# 画像三版,窗口三处

探索期那幅 argc 等于一的画像,是内核替客人捏的过渡。真客人不这样:musl 的 spawn 递自己的 argv 与 envp,咱们得照单铺。ABI 规定的样子咱们摆清楚:字符串们从栈页的页底向上摞,指针们从入口的 rsp 向上正向排:打头的是 argc,argv 的指针们跟上,一个零词收的尾,envp 的指针们再跟上,再来的还是一个零词,末了 auxv 摆一个裸的 AT_NULL。指针与字符串在这一页里相向而行,碰上了就是满,满了答 E2big,errno 的七号是新词。进门还有一道预算:客人递来的指针,内核是不能直接信的,都得拷进自家的暂存池落脚,数组的上限是十六支,字符串的上限是五百一十二字节,数组的每一个字都得过可读的区间检查,RSI 递零的时候 argv 就用路径本身,保住单参的老兼容,init 那一路铺的 argv 是 /bin/sh。

画像咱们画了三版,返工的经过原样记档。第一版指针区的起点算错了一个字,一整排指针写穿了页外。第二版起点改对了,方向还是倒的:ABI 要的是从 rsp 向上,咱们仍从页底倒着来,argc 又被磨成了零,客人的入口读到一个空荡荡的世界。第三版改对了方向,定了形。三版同源的病就一条:布局的事,眼睛看对了也不算数,得有机内的案子锁着:argc 几支、指针朝哪、零词落几处,断言一条条地上齐。这笔待办咱们记在明处。定形的真码咱们请出来,看两个方向是怎么起的头:

```cpp
unsigned long WriteEntryPicture(unsigned char* stack_page, unsigned long entry_rsp,
                                const char* const* argv, const char* const* envp) {
    const unsigned long kPageBase  = entry_rsp - (entry_rsp % kSize);
    unsigned long       strings_at = 0;
    unsigned long       words_at   = entry_rsp - kPageBase;
    const unsigned long kNullWord  = 0;
    const auto          kRoomLeft  = [&] { return words_at + 16 < kSize - strings_at; };
    const auto          kPutNull   = [&] {
        cinux::base::CopyBytes(stack_page + words_at, &kNullWord, sizeof(kNullWord));
        words_at += 8;
    };
```

咱们给两个方向各立了一支游标:strings_at 从页底零起,words_at 从入口的 rsp 起,各自记的是页内偏移,正文前头说的相向而行,落在代码里的就是它们。kRoomLeft 是满的判:指针列的顶要是追上了字符串的底,这一页就装不下了,留的十六字节是给末尾几个零词的余量。写字与摆指针的活,咱们交给了一支 lambda:

```cpp
    const auto kPutString = [&](const char* text) -> bool {
        if (text == nullptr) {
            return false;
        }
        unsigned long length = 0;
        while (text[length] != '\0') {
            ++length;
        }
        if (!kRoomLeft()) {
            return false;
        }
        cinux::base::CopyBytes(stack_page + strings_at, text, length + 1);
        const unsigned long kLanded = kPageBase + strings_at;
        strings_at += length + 1;
        cinux::base::CopyBytes(stack_page + words_at, &kLanded, sizeof(kLanded));
        words_at += 8;
        return true;
    };
```

满的判走在落笔的前面:地方不够,咱们就让整个画像交零退出,这就是 E2big 的门。kLanded 那一行换算的是地址:字符串落在页底,记进指针列的必须是真正的虚地址,得拿页基加上偏移算出的那个数,偏移本身是不算数的。铺的次序与 ABI 一条条对:

```cpp
    unsigned long argc = 0;
    while (argv[argc] != nullptr) {
        ++argc;
    }
    unsigned long envc = 0;
    if (envp != nullptr) {
        while (envp[envc] != nullptr) {
            ++envc;
        }
    }
    cinux::base::CopyBytes(stack_page + words_at, &argc, sizeof(argc));
    words_at += 8;
    for (unsigned long index = 0; index < argc; ++index) {
        if (!kPutString(argv[index])) {
            return 0;
        }
    }
    kPutNull();
    for (unsigned long index = 0; index < envc; ++index) {
        if (!kPutString(envp[index])) {
            return 0;
        }
    }
    kPutNull();
    kPutNull();
    kPutNull();
    return entry_rsp;
}
```

咱们照着 ABI 的次序走:argc 打的头,argv 的指针们跟上,零词收的尾,envp 的指针们再来一遍,末了连摆两个零词:envp 的终止项加上 auxv 那个裸的 AT_NULL,退回的仍是入口的 rsp。三版返工错的两处,起点的字与方向,如今都锁在上面的真码与机内的断言里。

画像铺好的同一晚,咱们听见机器响了一嗓子 panic:

```text
unblock of a task that is not parked
```

现场正是 busybox 的崩溃循环:ash 一崩,init 重拉了它,拉了起来再崩,重拉的高频率把完成件逼出了原形。完成件是验明正身那一卷立的一次性判决书,它的等法里有一个窗口:把自己挂进等待名单,与真正的泊车之间,隔着的是几步活。中断那一侧的完成偏偏赶在这个窗口里喊了 unblock,喊的是一个还没睡下的人。修法是把置阻塞的那一步挪进自旋锁的临界区:名单与状态一口气办完,窗口关上了。这是那一卷资产的潜伏旧病,busybox 的崩溃循环是第一个高频触发者,慢工细活的日子里它一声不响。ash 的段错误还没好,它的病根还有另一层,咱们记着。

轮到 I/O error 那桩了。咱们两轮 gdb 跟读,走的是同一根路径,kernel_init 读了一百二十一万四千三百五十二字节,全量成功了,fork 出的孩子读同样多,败了,精确的失败层定不死:想在目录查找的函数上下断点,断点落了空,调用方把它内联了,咱们对着一份不存在的代码副本守了半天。咱们把案卷整理成稿,请外面的眼睛帮着看了一遍。回来的是三刀,对的都是真病。

第一刀修的是泊车竞态的第二层。咱们自己修的那一次只关了半扇窗:自旋锁放了、泊车还没提交的当口,进得来的仍然是中断。互斥锁、信号量、完成件三个等处统一加了中断守卫,从关中断的那一步守到泊车提交为止。为什么关得住:跟泊车抢窗口的正是中断那一方,中断被关在了门外,窗口里就再没有人喊 unblock 了,窗口整个关上了。效果立竿见影:ash 在外部命令后的段错误连生案,消失了。第二刀修的是块缓存的悬空指针。view 把槽的指针递出去的时候,锁已经放了,调用方在无锁的状态下慢慢读槽里的字节,两个任务并发的时候,另一个把同一槽腾给了新块,前头的一方读到的就是被逐出的野数据。新增了一张直读的脸:认槽、拷贝、放锁,在同一把锁里一口气办完了,ext2 的超级块、数据块、位图三处消费点全部换了过去,fork 的孩子与重拉的 init 并发读同一个 1.2M 的文件,争的正是同一条路。I/O error 消失了。第三刀修的是整读文件的结果:从真伪改成了带错误码的 Result,哪一层失败的,从猜变成了读。

战后实弹:I/O error 没了,hello 走通了全链,ash 也活得好好的,echo 实时了。可咱们把话说在前面:三刀都是有源码依据的真病,不是当场逮住的现行,起头那桩 I/O error 的原始现场,其实谁也没抓到过。这话到了下一篇就要兑现。战后又冒出了新的悬案:fork 的孩子 exec busybox,现在稳稳地报 not found,整读递回来的错误号是三,找不到的是名字,读倒是读得动。fork 的孩子读到的目录块内容不对?ext2 从来是按单任务的前提写的,挂载的任务独占卷,并发读头一回有人这么干。下一刀断在哪?咱们心里已经有了数,到了凌晨才见真章。
