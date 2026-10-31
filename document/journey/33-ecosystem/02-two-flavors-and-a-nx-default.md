---
title: "02 · 情报桌添两味,栈的户口自报"
description: "发下的考卷在这一章交。情报桌添两味:十五号 AT_PLATFORM 给的是指针,机型名 x86_64 随画像的字符串区落址,没有就整味不出场,十六号 AT_HWCAP 恒上桌、值恒零,x86-64 上的 glibc 起来自己发 CPUID 探能力,内核不代报,零是实话。栈改户口自报:程序头里类型号 0x6474E551 的一条 GNU 栈登记,execve 与开机直拉两路都扫,执行位进栈的户口,首页的页表词与生长页跟着走,缺头或缺位默认不可执行,安全那一卷的写与执行分家落到了栈的户口上。另有号位一道小题,pread64 坐十七号,第四参坐在 r10,负偏移答 EINVAL,后端的读面本来就带位置,描述符的游标一动不动,读了不算走过。当年为 glibc 把 brk 上界从 64MB 抬到 240MB 的判例,这一遍结构性免修,堆与 mmap 的分界线画在 2 的 46 次方。头一个 glibc 客人 hello 上盘,非 PIE 的身子,解释器、libc 与它自己三件座自动安好,座位拿环境变量可换。画像案长厚,全向量走查数到十五对加 AT_NULL,静态缺省验无则不出场,GNU 栈两案入册,号位长凳七案全过。"
chapter: 33
order: 2
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - ecosystem
  - elf
  - auxv
  - glibc
  - syscall
---

# 情报桌添两味,栈的户口自报

发下的考卷,头一题指着的是栈页上那桌 auxv。桌上的辅助向量,是内核递给程序启动代码的一桌情报,把后半场交给解释器那一卷的时候,咱们照着 musl 的单子把桌摆齐,连收桌的 AT_NULL 一起数,数齐了十四味。轮到 glibc 入座的时候,单子上多出的是两行,一行写的是十五号 AT_PLATFORM,另一行写的是十六号 AT_HWCAP。头一味的值是个指针,指向的是机型名 x86_64,字符串随画像的字符串区落地,桌上摆的是落地后的地址。第二味就爽快了,恒上桌的是它,值是恒零的。

两味在 EntryFacts 里是各领一格的,咱们连注释一起看,脾气都写在了注里:

```cpp
    const char*         platform{};      ///< AT_PLATFORM payload source; null omits the entry.
    std::uint64_t       hwcap{};         ///< AT_HWCAP word; zero advertises no features.
```

落桌的差别在 put_facts 的尾巴上,您顺着这段看:

```cpp
    unsigned long platform_landed = 0;
    if (facts.platform != nullptr) {
        platform_landed = land_string(plane, facts.platform);
        if (platform_landed == 0) {
            return false;
        }
    }
    if (platform_landed != 0 && !put_pair(plane, kAuxPlatform, platform_landed)) {
        return false;
    }
    if (!put_pair(plane, kAuxHwcap, facts.hwcap)) {
        return false;
    }
```

platform 空着的时候,put_pair 是压根不被叫到的,AT_PLATFORM 就整味不出场了。这是桌上的老礼数,AT_RANDOM 与 AT_EXECFN 缺席也是合法的形态,静态客人走的正是空着的那一路。hwcap 的那一对没有条件,是恒落的。值给零的道理在客人那边:x86-64 上的 glibc 不等内核报能力,靠的是自己起来发 CPUID 探,咱们热心替它报,报错一位的后果比不报还坏,所以这一味端上桌,值写的是零,算是明说了不代报。喂的法门也省事,execve 的装载路与开机直拉的路,两路递进 facts 的都是 kPlatformName 同一个词,头一个程序与后来的客人吃同一桌菜。

栈是第二件要办的。安全那一卷立写与执行分家的时候,栈那页的执行位是内核替客人定死的,一律是不给的,谁来都是一样的。这一回咱们把它改成户口自报:客人的程序头里要是带着类型号 0x6474E551 的一条登记,登记的名字是 PT_GNU_STACK,是 GNU 家给栈预备的户口页,页里立着的是执行位,栈的可执行是这么挂上的。头没有的,或者头在而位没立的,按最严的来,默认就是不可执行的。读法是解析器的老手艺,沿着段表挨段地找过去,找到就把头里的旗交出来:

```cpp
    for (std::uint16_t index = 0; index < kImage.segment_count; ++index) {
        const std::uint8_t* const kAt =
            kImage.segments +
            (static_cast<std::size_t>(index) * static_cast<std::size_t>(kImage.segment_size));
        if (base::ReadWord32(kAt, 0) != kProgramGnuStack) {
            continue;
        }
        flags = base::ReadWord32(kAt, 4);
        return true;
    }
    return false;
```

execve 那一路拿到了旗,咱们问的就是一句能不能执行:

```cpp
    unsigned int gnu_stack_flags = 0;
    const bool   kStackExecutable =
        loader::ReadGnuStackFlags(program.get(), program_size, gnu_stack_flags) &&
        (gnu_stack_flags & loader::kSegmentExec) != 0;
```

答案一路往下传:登记册里栈那段的 executable 按它记,栈首页在页表里组词的时候,UserLeafTerms 的执行位跟着它走。往后栈往下长的时候,缺页补页认的就是登记册的位,生长页是自动跟上的,一行都不用咱们多叮嘱。execve 这一路是要扫的,开机直拉那一路也是要扫的,咱们用的是同一杆尺。分家的家法没有松动,松动的只是户口的写法:从内核定,改成的是客人自报,不自报的话,就没有执行位了。

考卷上还有一道开号位的小题,咱们给 glibc 开一个号位。客人要的是一种读了不算走过的读:从客人给的位置读,描述符的游标不动。号位开的是十七号 pread64,第四个参数按内核 ABI 的次序坐在 r10。下楼开张那一卷立下的北极星,寄存器照人家的坐,这里照用的就是它。负偏移是没得谈的,进门答的就是 EINVAL:

```cpp
    const auto kOffset = static_cast<long long>(frame.r10);
    if (kOffset < 0) {
        return -kEinval;
    }
```

文件后端的这门手艺,咱们本来就备好了。后端的 read 生来带一个 offset 参数,读哪儿是由参数报的,游标住在打开的描述里:普通 read 拿描述的游标当位置,读完了就把游标往前推。pread 只换位置的主人,拿 r10 里的数当地址,调的还是同一个 ops->read,描述的游标从头到尾没人碰。

上一章留了半截旧事,说好了到这一章交底。懒到家那一卷讲过 brk 的家法:堆不上链,骑在登记册自己的成员上,头顶画的是天花板,当年两百四十兆的绝对上限也记在案。没记的是前半截:当年探 glibc 的兼容面,brk 的上界起手是 64MB,一用就不够使了,笔者拍的板,上界从 64MB 抬到了 240MB。这一遍是不用抬的,杆画在哪儿的问题,一行常量就说尽了:

```cpp
inline constexpr unsigned long kUserBrkMmapSplit    = 0x0000400000000000UL;
```

这一遍咱们把堆与 mmap 的分界线画在 2 的 46 次方那个地址上,堆从程序影像的末尾种下,往上是一直到分界线的,隔着用不完的地界。当年要抬的那道杆,这一遍的图纸上压根没有。

题都答完了,头一个 glibc 客人该上盘了。小脚本用咱们开发它的电脑上的 gcc,链一个非 PIE 的 hello 出来。解释器的路径,烙的是客人的 /lib64/ld-linux-x86-64.so.2,库的搜索路也定在 /lib。

```bash
"$gcc" -no-pie -Os -Wl,--build-id=none \
    -Wl,-dynamic-linker,/lib64/ld-linux-x86-64.so.2 \
    -Wl,-rpath,/lib \
    "$src" -o "$out"
```

造盘的脚本认得客人的路数,咱们只管跑,三件座是自动安好的:解释器进的是 /lib64,libc 进的是 /lib,hello 自己进的是 /bin。默认的座位照本机工具链的自报来取,各家机器摆法不一样的时候,拿 CINUX_GLIBC_ 打头的环境变量可以换座。试盘出来的时候,咱们把三件座点验一遍,验讫了,客人的行李齐了。

画像的案子跟着长厚。test_elf 十四案全过:全向量走查的那案,数出来的对子是十五,收尾的是 AT_NULL,全桌凑齐了十六味,platform 给的地址顺藤读过去,打头的是 x,收尾的是 4,这读出来的就是 x86_64 了。静态缺省的那案验无则不出场,platform 不递的话,桌上就查无此味了,hwcap 照旧是在的,值是恒零的。GNU 栈两案入册:带头的镜像,旗是 0x1 的也好,0x6 的也好,读回的是原样,缺头的那案答没有,您要是不信,喂进去 99 的时候,出来的还是 99,被动过的格数是零。号位与类型号都拿 static_assert 锁给宿主的 elf.h,把后半场交给解释器那一卷的老手法,记岔的号是零个。test_syscall_face 跑了七案,pread 那案把描述的游标故意拨到了 4,跟着从 0 定位读全了八个字节,又从 6 读了个尾巴,负一进门答的是 EINVAL,末了游标还一动不动地站在 4。

咱们把户口与情报都备齐了,可 glibc 起手还有一套装库的 mmap 序列要考,占址重建的替换语义,下一章立的就是它。
