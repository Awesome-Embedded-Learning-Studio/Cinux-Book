---
title: "06 · 九十声问询,三种答法"
description: "装载那关过了,拉锯挪到门上,咱们把真实编译当探针使:客人敲哪个空号,串口就报一行,报数本身就是需求清单,不用坐在屋里猜。当年的正典是一张五十二行的逐项清单,这一遍照单对齐、逐项清零,九十次问询里回数多的五位各自带着数:prlimit64 三十二回,getrandom 二十一回,rseq 十二回,set_robust_list 十二回,clone3 八回。答法落三样。查案卷:prlimit64 与 getrandom 合计五十三次,跑别人的程序那一卷的末段早已办成真的,交接单子上的缺失是旧话,销案不重做、不另造假成功。明说不办:madvise、chmod、umask、getrusage、set_robust_list、rseq、clone3 七位座上坐人,显式答 ENOSYS,用户库自己回退。真办:faccessat 两座只认 AT_FDCWD 的负一百,答话整个交给 access 老门,waitid 收退场的孩子,报告照 Linux 的 siginfo 布局写四个格子,sendfile 后来改判真办,单独立篇。装聋与明说是两码事:客人耳朵里回话一样,咱们的串口不一样,空号每被敲一次喊一行,验收的零 unserved 数的就是装聋。探路三件 rseq、set_robust_list、clone3 的回数大头来自 glibc 与 posix_spawn 的开工问路,ENOSYS 在它们耳朵里是换老路的信号。测试跟着真负载走:真实编译轮里任何一行 unserved 整轮记败,另有指空 wrapper 的开工失败探针。名单清零,最重的一块是师傅开工的方式,spawn 全家,下一节进门。"
chapter: 34
order: 6
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - selfhost
  - syscall
  - gcc
---

# 九十声问询,三种答法

装载那关过了,师傅一开工拉锯就挪到了门上。头一天咱们眼睛不盯编译的对错,盯的是串口。跑别人的程序那一卷铺号的时候,分发表上立着一个探针:空号被敲了,打在串口上的是一行 unserved,本是诊断期的临时件,后来留了下来。真编译跑了起来,driver、cpp、cc1、as、ld 就一路把门敲了下去,空号挨个现了形,每一行的报数都是一声问询:有人喊了号,门里是没人应到的。路子就此定了下来:拿真实编译当探针,客人敲的是什么,咱们缺什么,需求清单自己就排了出来,不用咱们坐在屋里猜。

套路当年也是这么走的。当年第一遍冲这一关的时候,笔者把清出来的单子立成了正典:拢共五十二行,一项一项地记着。这一遍开工的时候,单子是原样接过的,照单对齐了,一项一项地清。

问询的回数拢共是九十次,回数多的五位各自带着数:prlimit64 的三十二回,getrandom 的二十一回,rseq 的十二回,set_robust_list 的十二回,clone3 的八回。清单上别的名字还有,案卷里各自记着的,咱们不往正文里搬,也不替案卷凑什么总数。

咱们一位一位答下来,答法落进三样:查案卷的,明说不办的,真办的。

头一样查的是案卷。名单上的头两位是 prlimit64 与 getrandom,加起来是五十三次的数。接手的时候,单子上两位名下记的还是缺失。可这一句是旧话:翻回跑别人的程序那一卷的末段,四句假话换成真话的那一篇,prlimit64 立起了真表,getrandom 接上了 RDRAND 的真随机,两扇门后坐的都是真人。咱们把案卷翻回去,给两位销了案。重做是没有必要的,照着旧话再补一份假的,更是把办成的事再办坏一回。单子记的是旧话,门里早办成了,以门里办的为准。

第二样是明说不办:名单上的七位是 madvise、chmod、umask、getrusage、set_robust_list、rseq、clone3,咱们给七位安的座是同一位答话人。按号索引的静态表还是老章程,新添的行长这样:

```cpp
    {
        .number  = cinux::syscall::SyscallNr::kClone3,
        .handler = cinux::syscall::HandleUnsupportedProbe,
    },
```

咱们再看答话人本人,全副的家当就三行:

```cpp
long long HandleUnsupportedProbe([[maybe_unused]] const SyscallFrame& frame) {
    return -kEnosys;
}
```

kEnosys 是 errno 表里的第三十八号,话面就是一句:这个号上的事,没实现的。客人把这句拿了回去,用户库自己会挑回头的路走。头一件活那一章咱们记过一笔:getrusage 九十八号有人问询,glibc 是容忍、不挡路的,当时只记了未办。这一章把座位补上了,二百七十三与三百三十四两位也一并转成了明说。

明说不办与装聋的界线,在验收里是分明的,就在派发的总闸上。您贴着看:

```cpp
    const unsigned long long kNumber = frame.rax;
    if (kNumber >= cinux::syscall::kSyscallCount || kHandlers[kNumber] == nullptr) {
        cinux::print::Println("[kern] syscall %llu unserved", kNumber);
        return -cinux::syscall::kEnosys;
    }
```

号落了进来,表里是没人的,这样的就是装聋:串口喊一声,回话给的是负三十八。客人的耳朵里,空号与明说听上去是一样的,答的都是 ENOSYS。欸?那这座是不是白坐了?当然没白坐,喊声在咱们这边。空号每被敲一次的当口,咱们串口上就多一行。坐了人的座,话答得是安安静静的,清单上销了案。跑真实编译的那轮测试,立的是一条死命令:整轮串口记录里搜到一行 unserved,这一轮是整体记败的。验收念的零 unserved,数的是装聋,明说不办的七位一位也不在里面。空着的比挂假门的诚实,这句老话是 UDP 与 socket 那一卷立的。这一遍的客人敲门勤,诚实的形状从留空换成了坐人明说,假门照样是一扇都不挂的。

第三样办的是真办。faccessat 与 faccessat2 是一对双生的门,一座是坐在二百六十九的,一座是坐在四百三十九的,答话的是同一位。at 家族的问法,头一个参数放的是座位号,咱们认的只有一个特殊值:AT_FDCWD 也就是负一百,话面上写的是别找描述符,按的是当前目录。门房的全貌:

```cpp
long long HandleFaccessat(const SyscallFrame& frame) {
    if (static_cast<std::int32_t>(frame.rdi) != -100) {
        return -kEnotdir;
    }
    if (frame.rax == static_cast<unsigned long long>(SyscallNr::kFaccessat2) &&
        (frame.r10 & ~0x200ULL) != 0) {
        return -kEinval;
    }
    SyscallFrame flat{};
    flat.rdi = frame.rsi;
    flat.rsi = frame.rdx;
    return HandleAccess(flat);
}
```

您顺着末尾那三行看:新门的参数挪进老门的座位,faccessat 的活整个交给 access 那扇老门去办,自己添的判只有两个。座位号认的是负一百,给了别的值答的就是 ENOTDIR。faccessat2 多出来的旗标,放行的只有 0x200 一个位,别的组合答的是 EINVAL。两座新门共用的是一位答话人,背后是跑了好几卷的老路。

waitid 坐的是二百四十七号,咱们让它收退场的孩子。问话的等法只认一种,认的是等退场,等停止、等继续的那些等法答的是 EINVAL。孩子的报告照 Linux 的 siginfo 布局写,字是这么落的:

```cpp
        base::WriteWord32(info, 0, kSigChld);
        base::WriteWord32(info, 8, kSignal == 0 ? 1 : 2);
        base::WriteWord32(info, 16, static_cast<unsigned int>(receipt.pid));
        base::WriteWord32(info, 24,
                          kSignal == 0 ? static_cast<unsigned int>(receipt.code) >> 8U : kSignal);
```

咱们把四个格子各自数一遍:头一格写的是信号号,也就是恒为十七的 SIGCHLD,孩子那边是有交代的。第二格写的是来路,正常退场的写一,被信号杀的写二。第三格是孩子的号。第四格写的是交代本身,退场码是右移八位取出来的,或者杀他的那个信号。客人拿到了报告,wait 家族的老章法就都接得上了。

不过还有一位 sendfile,坐的是四十号,走的是另一条弧线。起初它也归在明说不办的名单里,后来翻生态那一卷留下的日志,BusyBox 实际敲了这扇门,回退的答法站不住,改判成了真办,数据是在页缓冲里按段搬的。那桩案子是单独立篇的,咱们到那儿细看。

rseq、set_robust_list、clone3 三位的回数不小,敲门的来头是一家:用户库的探路三件,glibc 进门安顿的时候问一遍,posix_spawn 起孩子前也是一样地问一遍,问的都是同一条路认不认。这句回话在它们的耳朵里另有用处,听完了,就地换了老路,用老办法把活干了。生态那一卷排起动即死的案子时,这串名字是在嫌疑单上露过面的,后来案子破在栈的对齐上,几位洗清了嫌疑,名字咱们记下了,这一章把它们的座正式安顿成了明说。

验收的家法也跟着真负载走。咱们在长凳上单立一轮真的编译,从 as、ld 两位老师傅的头一件活,到 gcc、g++ 的全链,产物是逐个点验的,不过整轮记录里搜出任何一行 unserved,这一轮就记败了。同一轮里还埋着一条失败探针:给 gcc 指一条不存在的 wrapper 路径,它得体面地失败,落下的退出码是一。问询到这里清了零,最重的一块才露头:师傅开工的方式,是要 spawn 全家的,咱们留到下一节进门再讲。
