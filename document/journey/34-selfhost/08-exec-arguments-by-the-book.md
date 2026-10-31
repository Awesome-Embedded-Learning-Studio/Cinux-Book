---
title: "08 · 交接按正本:exec 的参数与入口图"
description: "收活的当口,笔者一条改令:exec 的参数按 Linux 正本重写,上限不是拍脑袋放宽的事。预算按当前软 RLIMIT_STACK 算:四分之一与六 MiB 取小,一百二十八 KiB 保底,扣掉指针数组再抄字符串;单串连结尾 NUL 至多三十二页,条数上限 INT_MAX;指针坏答 EFAULT,超预算答 E2BIG,两种回话分开。NULL 向量按空列表办,空 argv 补一个空字符串,从前偷注的默认 PATH 撤掉。入口图按实际字符串、指针与 auxv 量尺寸,十六字节对齐逐页铺开,栈区按限值与所需页计算,底下留一页护页;AT_EXECFN 单独立名条,不借可能被改名的 argv[0]。次序铁则:会失败的复制与映射全排在信号清零、关描述符、换身份换地址空间的前头,失败永远落在旧世界,半成品一个不留;prlimit64 同给新旧两指针时旧值留底在前。execprobe 六面实弹:一千零二十四参数配三百环境、单串卡边界、一 MiB 半参数、限值收紧、下限保底、坏指针与 NULL 向量,每回失败后 PID 与 CLOEXEC 描述符仍存活。交接的章程立了,名单上还有一笔被改判的:sendfile。"
chapter: 34
order: 8
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - execve
  - syscall
  - elf
---

# 交接按正本:exec 的参数与入口图

exec 交割参数的家当,这时候还是从前给壳备的三样。指针数组十六项封顶,暂存池五百一十二字节,入口的画面一页为限。画里的方向,是 UDP 与 socket 那一卷翻对的,一直没动。参数清单一长,咱们手最顺的动作就是放宽数字,十六项改一百二十八项,往后不够,再改二百五十六。收活的当口,笔者一条改令下来,直接把放宽数字的路封了,只有一句:上限不是拍脑袋放宽的事,按正本来。

正本给得出处:Linux 内核源码 fs/exec.c 里的 bprm_stack_limits 与 copy_strings,binfmts.h 里的常量,再拿 man7 的 execve(2) 手册页核一遍。咱们照着它们把 exec 交割参数的章程重立一遍,多少算多、多长算长、什么算失败,一概不再自己发明。

预算从栈的软限值起头,就是 prlimit64 能读能改的那个数,任务默认八 MiB。预算怎么算,您看这几行,一步都不藏:

```cpp
const auto kLimit =
    base::math::Max(base::math::Min(proc::kDefaultStackCeiling / 4 * 3, stack_limit / 4),
                    static_cast<unsigned long long>(kMaxArgumentStringBytes));
const auto kPointers = (base::math::Max(argc, 1UL) + envc) * sizeof(unsigned long);
```

软限值除以四,跟六 MiB 取小。六 MiB 是正本 _STK_LIM 的四分之三,咱们默认的栈顶也是八 MiB,数对得上。保底一百二十八 KiB。预算里扣掉指针数组的占地,每个参数一根指针,每条环境一根,argv 空着也按一根算,给待会儿补上去的空串留座。剩下的,才是字符串的地。

抄写分两趟,数数在前。咱们挨个读用户递来的指针,两种失败在门口就分了家。指针指着读不了的地界,答 EFAULT。条数过了上限,或者光指针就把预算挤爆,答 E2BIG。条数的墙立在 MAX_ARG_STRINGS,正本里的值是 INT_MAX,二十多亿条,真正拦人的其实是字节数。抄字在后,动态开的暂存地里一段一段搬,单串连结尾的 NUL 至多三十二页,正本的名目叫 MAX_ARG_STRLEN。串抄满三十二页还不见 NUL,到头了,答 E2BIG。搬字的钳,六十四字节一块、按页内余量截短,是 UDP 与 socket 那一卷破残案时带出来的,原样进了新家。

空与 NULL 也有章程。向量本身传 NULL,按 Linux 当空列表办。argv 数出来零项,内核补一个空字符串,新程序手里至少握着一个 argv[0]。exec 从前还揣着一条好意,客人没带环境,就偷偷替它塞一条默认的 PATH 进去,免得壳找不到 /bin。好意是咱们自作主张的好意,正本里没有这道菜,撤了。环境原来什么样,落地就什么样。

然后是入口图,argv、envp、辅助向量 auxv 的字符串与指针在新地址空间里的铺法,程序进门时随身的行李。方向咱们沿用 UDP 与 socket 那一卷翻对的那个,字符串从高处往下住,指针表从 RSP 往上排,两边相遇留十六字节的界。翻新的是量法与地界。量法数完向上凑整,头一行就是它:

```cpp
const auto kPictureBytes = base::math::AlignUp(loader::EntryPictureBytes(kRuns, facts), 16UL);
```

字符串多少字节,指针多少根,argc 加两根收尾的空指针,辅助向量每对两格,一笔一笔数完,凑成十六的倍数。对齐这门课,咱们家交过四代学费,进门那一下 rsp 得落在整十六上,一分不省。地界跟着重算:

```cpp
const auto    kStackSpan  = base::math::Max(
    base::math::AlignDown(
        base::math::Min(static_cast<unsigned long>(kStackLimit), kAvailableStack),
        arch::page::kSize),
    base::math::AlignUp(kPictureBytes, arch::page::kSize));
stack_range.start      = kStackTop - kStackSpan - cinux::arch::page::kSize;
```

栈区的大小,咱们让软限值跟可用上界取小、按页取整,再跟画面所需页取大,底下另留一页护着。画面从此不再单页封顶,一页装不下就两页、十页,页数跟着内容走。RSP 下方也不再是省出来的一页空白,整段栈区都空着,栈往下长,缺页补页。

辅助向量的情报桌上,还单独立了一张名条,AT_EXECFN。调用 exec 的人可以把 argv[0] 写成任何名字,叫它 sh 也行,叫它 gcc 也行,argv[0] 只是客套。名条记的是内核真正执行的路径,不跟 argv[0] 走,真要追问执行的到底是哪个文件,咱们认名条,不认 argv[0]。路径在暂存的头一笔就抄下了,画图时单独立条。

交割最要紧的一条在次序。您翻进 exec 的主流程数一数,会失败的活,折路径、抄参数、装 ELF、画图、铺页,全排在头里。轮到下面几行动手,已经没有失败可出了:

```cpp
kSelf->signals.reset();
kSelf->clear_child_tid = 0;
kSelf->files->close_on_exec();
note_program_identity(*kSelf, path);
```

信号清零、close-on-exec 的描述符关门、身份换名、地址空间换血,全是不可回头的动作,一件可能失败的活也不许排在它们后头。exec 失败,永远落在旧世界里。信号还在,描述符还开着,身份没换,地址空间原封不动,半成品一个不留。咱们顺手把 prlimit64 也对齐了正本。新旧两根指针同给,留底在前、改动在后,old 里落的是改动前的数。cur 超 max,EINVAL 当场拒。

章程立得稳不稳,实弹说了算。execprobe,一只专门考 exec 交割边界的小客人程序,上机,考六面。头一面咱们就把场面撑大,一千零二十四个参数配三百条环境,画面跨好几页,孩子在另一头把每一字节核对完,参数条条是 x,环境句句相同,才交回 42。

第二面卡单串的边界:咱们造一根三十二页减一字节的串,连结尾的 NUL 正好顶到上限,过。多给一个字节,E2BIG。

第三面再压分量:二十四段十六页的长串,合计一 MiB 半的参数,咱们看它过。

第四面动限值,同一份载荷,RLIMIT_STACK 收到四 MiB,预算跌到一 MiB,装不下了,当场 E2BIG,预算跟着限值走。第五面反着来,限值收到二百五十六 KiB,四分之一只剩六十四 KiB,保底的一百二十八 KiB 顶上来,二十五页的单串照样进门,下限也是正本的一部分。第六面咱们考指针,向量本身的地址坏,EFAULT。向量里挂着的条目坏,也是 EFAULT。NULL 向量按空列表办,孩子照常活。

每一回失败的 exec 之后,探针当场核两件事:PID 没变,带 CLOEXEC 的描述符还开着。咱们要看的判据就一行,活着就算过:

```c
const int  kAlive  = getpid() == kBefore && fcntl(kFd, F_GETFD) == FD_CLOEXEC;
```

交接的章程,咱们立住了。点名名单上还有一笔被改判的——sendfile。
