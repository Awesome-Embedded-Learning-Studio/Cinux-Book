---
title: "10 · C++ 的家当上桌"
description: "C++ 的家当上桌,烤盘的单子长出一大截:g++ 与 gcc 两位 driver 同版本同 target 把关对拍,cc1plus 与 collect2、liblto_plugin 入单,ldd 报出的运行时闭包逐件收齐,libstdc++ 实文件、SONAME 链、dev 链三件一套,libm 与 libatomic 跟进,C 与 C++ 两份头闭包各自算全。输入指纹把新前端、驱动、库、种子与探针的源码连同隐式的 stdc-predef.h 一起算进输入,当年 cc1 隐式包含、gcc -H 偏偏不列的那桩案子,遗产直接写在今天的施工里。libm 无版本后缀,SONAME 与文件名同名,只建 dev 链,免得软链指回自己。盘扩到一百九十二 MiB,ext2 的 lookup 学会跟软链走,相对、绝对、链式、目录、长软链五路全通,四十次封顶,断链与环钉进宿测,当年 do_openat 不跟软链、ld 把目标串当文件读的七桩案子里头一号,这一遍在后厨做全。用户的原始路径按 Linux 的 PATH_MAX 整条复制再折叠,GCC specs 里爬四次父目录的长路不再被截断,内部路径的尺子从九十六放宽到二百五十六,文件表从十六座加到六十四座。家当齐了,下一章加冕。"
chapter: 34
order: 10
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - gcc
  - toolchain
  - cpp
  - ext2
---

# C++ 的家当上桌

该上桌的家当,咱们得从单子说起。C 那一阵的单子短,两位老师傅 as 和 ld,一位匠人 cc1,外带各自的行李,一页纸写完。轮到 C++,g++ 一进门,单子翻了好几页。

咱们照着念。两位 driver 里,新来的是 g++,与 gcc 并排坐镇。专管 C++ 的前端匠人 cc1plus 也入单,跟 cc1 同门。collect2 和 liblto_plugin,是链接那头的家什。新来的每一位都得过一遍 ldd,牵出几件动态库,咱们全收进运行时闭包。重头是 libstdc++,三件一套,实文件落座,SONAME 软链指向它,dev 链再补一条伺候链接期。libm 与 libatomic 跟着入单。末了是头文件,C 一份,C++ 一份,两份闭包,一件都不能欠。

混门的班子咱们不要。脚本起手就对拍,g++ 和 gcc 的版本号一样一样核,目标平台那串名字也要一样,对不上当场退出,烤盘都不开始。一家人必须同门同 target,版本差半号都不收。

头的闭包,还是请编译器自己报,咱们只管收名单。gcc -H 编一遍 hello.c,一路上摸过的头全列出来。g++ -H 再编一遍 hello.cpp,又得一份名单。两份并起来,C 的头、C++ 的头,谁也不欠谁的。

名单里有一个名字,咱们得单独请出来,它就是 /usr/include/stdc-predef.h。没有谁在源码里 #include 它,可 GCC 每编一个翻译单元,都悄悄把它塞在最前头,隐式包含。偏偏 -H 不列它。家底是请编译器自己报的,它恰好漏了自己兜里的一张。

当年第一遍,笔者就在这儿停过工:cc1 在自家盘上找不着这个头,别的全齐,就它没有。当年的修法是显式补装。这一遍,判例的遗产直接写在施工里,指纹的输入清单上有它一笔,烤盘的时候也单独装它:

```bash
    printf '%s\n' "${here}/ext2_seed/hello.c" "${here}/ext2_seed/hello.cpp" "${here}/ext2_seed/execprobe.c" "${here}/ext2_seed/sendfileprobe.c" /usr/include/stdc-predef.h
```

您拿指头把这行挨个点过去,两份种子的源码,两位探针的源码,行尾最末的,正是那个谁都不 #include 的头。种子和探针都进指纹,源一动,盘就该重烤。隐式的头也在其列,当年漏的就是它,今天它一个字都跑不掉。

座位上另有一笔要咱们单独交代,libm。libstdc++ 的三件一套,实文件带着版本尾巴,SONAME 短一截,dev 链再短一截,链链指向实文件,坐得端正。glibc 家的 libm 照方抓药,抓到一半发现不对,它的实文件就叫 libm.so.6,SONAME 还是这个名,一字不差。照方建链,链子指向自己。

当年笔者真在这儿栽过。自环的软链把真文件盖掉,装载器顺着链一圈一圈转,cc1 报 cannot open libm.so.6,看着像缺库,其实是链在咬自己的尾巴。所以施工里立下一条,SONAME 与文件名同名的,链就不建,只留 libm.so 的 dev 链。说起来 libm 还是咱们的老熟人,生态那一卷给 busybox 换血的时候,它跟着 libc 和 resolv 当客人的行李进过盘,烤盘缺一件就大声失败。这一回它换了身份,进师傅的单子,成了链接 C++ 程序的家当,连坐法都得单独立条。

家当多了,盘子跟着大,这一回咱们把盘扩到一百九十二 MiB,一万六千三百八十四个 inode。当年第一遍也走到盘不够那一步,当场扩到一百九十二。

盘大了,盘上的软链也一片一片的,SONAME 的链,dev 的链。可咱们还欠着一件事:内核的 lookup,从来不会跟软链走。

这事当年炸在最热闹的地方。g++ 头一炮,ld 背后那套认文件格式的 BFD 报了错,话面是文件格式认不得一类的意思。咱们顺着报错查下去,要开的 libstdc++.so 是条软链,门上的 do_openat 把软链这个 inode 本身递了出去,ld 拿它当文件读,读进来的内容是软链里存的那串目标路径。拿路径字符串当目标文件,BFD 当场摇头。当年在门上修,让 do_openat 跟着软链走到真身再递。

这一遍咱们把活做进后厨,ext2 自己的 lookup 学会跟软链。走法就在开头几行:

```cpp
    char pending[kFsPathMax]{};
    if (path.size() >= sizeof(pending)) {
        return base::KernelError::kInvalidArgument;
    }
    base::CopyBytes(pending, path.data(), path.size());
    for (unsigned int followed = 0; followed <= 40; ++followed) {
```

咱们盯着这个循环数圈。每跟一条软链,重走一圈,四十圈封顶。Linux 自己给软链层数定的上限叫 MAXSYMLINKS,也是四十,环在这儿被掐死。目标串有两种存法,短的就地躺在 inode 里 i_block 那 60 字节的仓库,长过它的单独开数据块。相对的目标、绝对的目标、链着指的链、指到目录上的,读取都得认。

咱们把这几条路全钉上宿测的案板:

```cpp
TEST("ext2: lookup follows relative absolute chained directory and long symlinks") {
    auto& bake = baked();
    ASSERT_TRUE(bake.ok);
    const auto kFile = bake.volume->lookup("small.txt");
    ASSERT_TRUE(kFile.ok());
    for (const auto* path :
         {"relative", "absolute", "chained", "directory/small.txt", "longlink"}) {
        const auto kNode = bake.volume->lookup(path);
        ASSERT_TRUE(kNode.ok());
        ASSERT_EQ(kNode.value().ino, kFile.value().ino);
    }
    ASSERT_FALSE(bake.volume->lookup("broken").ok());
    ASSERT_FALSE(bake.volume->lookup("cycle").ok());
}
```

您再数名单上的五个名字,relative,absolute,chained,directory/small.txt,longlink,查回来的都得是 small.txt 那个 inode。broken 与 cycle 两个,必须失败。longlink 的目标是一长串 ./ 拼出来的路,长到 60 字节的仓库放不下,正经占着数据块。软链的目标要折进已走过的路上,咱们用的折尺还是认字那一卷立下的那把,名字的相对论那一章的家什,在这儿又上一回工。

当年 g++ 一路,笔者前前后后修了七个独立的 bug。头一号讲完了。余下的,有两件正是今天重列的单子和这块新盘,当年补闭包、扩盘,这一遍重列单、重扩盘,同一件事换了个遍数。两件是边角,打印系统补 %.*s 的精度,不然非结尾缓冲的串打不全,busybox 的断言改按 WEXITSTATUS 解码。还有 procfs 与 devfs 补单组件查找,不然 open /proc/meminfo 直接答不办,free 都跑不动。waitpid 的状态编码,exit(1) 存进去时没编码,glibc 解码一看低字节不是零,当成被信号挂断,collect2 一口咬定 ld 死于 Hangup,追凶追到 ld 头上,真凶却在收尸那一环。

最后一道坎在路径的长度上,咱们放在末了说。GCC 的 driver 肚子里揣着一张 specs,阶段怎么串、文件去哪儿找,全写在表里。它点 crt 那几位的名时,路径要爬四次父目录,../../../../ 一路点上去再折回来。这么一条路,不折叠着写,一百多字节。内核里量路的尺子,那会儿定的是九十六字节,折叠还没来得及把点折掉,入口那一关就把路拦下了,specs 里连着的多个 ../,在规范化之前被截断。

咱们两头一起改。入口这头,用户的原始路径按 Linux 的 PATH_MAX 整条复制,折完再拿内核的尺子量:

```cpp
long long FoldUserPath(unsigned long long source, char* out, unsigned long cap) {
    const base::Own<char[]> kRawPath(new char[fs::kUserPathMax]);
    if (!kRawPath.holds()) {
        return -kEnomem;
    }
    const long long kRaw = CopyUserPath(source, kRawPath.get(), fs::kUserPathMax);
```

kUserPathMax 写的就是 4096,Linux 的 PATH_MAX。复制排在折叠前头。尺子那头,咱们也从九十六放宽到二百五十六,给折叠后的路留足身位。文件表同一阵子从十六座加到六十四座,一并并入这轮验收。

单子点完,盘烤好,两位 driver、两位前端匠人、链接的家什、三件一套的 libstdc++,连同两份头闭包,都在盘上落了座。C++ 的家当齐了。下一章,咱们给这场自举加冕。
