---
title: "02 · 一套工具链,四层旧病"
description: "宿主上没有现成 musl 包,自己编:官方源码、自家前缀,补五个 UAPI 头目录,busybox 1.38 静态编出 1.2M、四百零七个命令,tc 因新内核头删了老接口而关掉。段四的虚地址不页对齐,这件事记在前头。客人顶进 /bin/sh,四层旧病连着炸出来:exec 的栈窗、init 的第二现场、装载器的头半页、入口的画像。"
chapter: 23
order: 2
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - musl
  - busybox
  - loader
  - history
---

# 一套工具链,四层旧病

咱们要请客人,手头得有能把它编出来的工具链,所以这一篇从这儿动工。开发机上没有现成的 musl 包,提权也办不了,咱们就自己编:从官方的源码站取 1.2.5 的源码,configure 把家安在自家的目录里,make install 几分钟就完工了。头一道坎在头文件:musl 只带自家的那一套,不带 Linux 内核留给用户程序的那套接口头,行话里叫的是 UAPI,要编 busybox 的话,linux、asm-generic、asm、mtd、video 五个目录都得从宿主的 /usr/include 里拷进前缀,一个也少不了。

busybox 这边取的是上游最新的 1.38.0,咱们 make defconfig 之后改配置文件,把静态链接开了。当年工程线上跑的是 1.36,咱们这一遍按 1.38 走,版本上的差异注在这儿,机制倒是一个不差。改配置也有一处磕绊:busybox 的树里没有内核那套改配置的脚本,咱们直接编辑它的配置文件。编译器指的也是前缀里新装的 musl-gcc,busybox 身子里链进去的才是咱们请来的 musl,而不是宿主自带的旧家什。第二道坎在编译:新内核头文件删了 CBQ 流量控制类,busybox 里管网络的 tc 还指着那些旧名字,一编就错了。网络排在后面的卷,咱们把这一卷用不上的 tc 关掉,其余的照单全收。产物是 1.2M 的静态 ELF,EXEC 而非 PIE,分成了四个 LOAD 段,装了四百零七个命令。带符号的副本别删,排障的时候对地址、找函数,咱们全指着它。

段表咱们预习一遍,因为它是后面破案的引信。段一到三倒是老老实实,段段都是页对齐的。段四是可写的那一段,虚地址落在 0x525bf0,文件里占的是 0x375b 字节,到了内存里要 0x4ee0,而它的虚地址不在页的边界上。这个不页对齐的毛病,您把它记下了:生与收那一卷的装载器这些年只喂过页对齐的段,喂到不对齐的,它就要露馅了。

该上机了。咱们用盘上的工具把 busybox 写进 ext2 盘里 /bin/sh 的座位,开了机让 init 直接拉它。等来的哪里是提示符,等来的是四层的旧病,咱们一层一层说。头一层是 exec 的栈窗,生与收那一卷埋的。exec 把新程序的栈顶放在映射页的最顶上减八个字节,argc 勉强挤在页里最后的八字节,而 ABI 规定 argc 的上头紧跟的就是 argv 的指针列,argv[0] 的指针于是落在了页外的守卫页上。musl 的入口 __libc_start_main 头一件事就是读 argv[0],当场触发了缺页,gdb 断在缺页裁决的门口,肇事地址正是栈顶的守卫页。咱们自家的 hello 为什么从来没事?它的 _start 不读栈上的参数,不认栈上的字,这病它偏偏踩不着。修法咱们写两笔:栈页显式清零,物理帧里的旧内容从来是不可依赖的。第二笔把栈顶往页里挪了挪,挪多远写进了配置,给整个进程主题一个统一的说法。

第二层是同一个病的第二现场。init 拉 shell 不走 exec 的路,自己搭好了世界直接往 ring3 跳,栈顶一样压在页的边沿上。咱们照方抓药同修,顺路把那记往 ring3 的跳写明白:参数给到了哪,栈就落到了哪。

第三层的教益最大,主角还是那个不页对齐的毛病。栈修好了,busybox 死在 musl 遍历 init_array 的时候,一个个调进去的却是 NULL——init_array 是程序自带的一张初始化函数表,C 运行时在进主函数的那一步之前,要挨个把表里的函数调上一遍。咱们顺着查:init_array 表在内存里全零,可文件的字节里明明有真值,gdb 单步到装载器铺页的那一步,咱们看见文件字节落在了帧的开头,而这一页该落字节的位置在页内偏移 0xBF0。

错法有两代:原版的算式让头一页的拷贝量在无符号里下溢成了零,头页铺了个全零。第一版修复算对了重叠量,却把拷贝的起点仍当成了页首。终版的形状是页与段的文件区取交集:从哪拷、拷到哪、落进页内几号,三样都算了出来,交集为空的页整页清零就是全部。真码就在 LoadElfIntoSpace 的铺页循环里,咱们把它请出来,看每一段是怎么圈页的:

```cpp
        const std::uint64_t kPageNumber = cinux::base::math::Floor(segment.virtual_address, kSize);
        const std::uint64_t kPageBase   = kPageNumber * kSize;
        const std::uint64_t kInPage     = segment.virtual_address - kPageBase;
        const std::uint64_t kSpan = cinux::base::math::Ceil(kInPage + segment.memory_bytes, kSize);
        const std::uint64_t kFileEnd = segment.virtual_address + segment.file_bytes;
```

五行咱们分两步看。前三行把段头安进页里:虚地址除到页号再乘回页基,kInPage 就是段头在自家头一页里的座位,段四的这个数正是前头记下的 0xBF0。后两行数的是页:内存尺寸连着头部偏移一起向上取整,得出这一段共占的是几页。kFileEnd 标出文件区在虚地址轴上的末梢,是下一步行相交用的另一头。接着是逐页取交集的循环:

```cpp
        for (std::uint64_t page = 0; page < kSpan; ++page) {
            const std::uint64_t kSpot    = kPageBase + (page * kSize);
            const std::uint64_t kPageEnd = kSpot + kSize;
            const std::uint64_t kCopyFrom =
                kSpot > segment.virtual_address ? kSpot : segment.virtual_address;
            const std::uint64_t kCopyUntil = kPageEnd < kFileEnd ? kPageEnd : kFileEnd;
            const std::uint64_t kOverlap   = kCopyUntil > kCopyFrom ? kCopyUntil - kCopyFrom : 0;
            const std::uint8_t* chunk_source =
                data + segment.file_offset + (kCopyFrom - segment.virtual_address);
            const auto kChunkBytes = static_cast<std::size_t>(kOverlap);
            map_segment_page(world.space, kSpot,
                             ChunkPlan{
                                 .source      = chunk_source,
                                 .bytes       = kChunkBytes,
                                 .writable    = (segment.flags & kSegmentWrite) != 0,
                                 .dest_offset = kCopyFrom - kSpot,
                             });
        }
```

承重的就是 kCopyFrom 与 kCopyUntil 两行,咱们两头都取窄:页区间的头与段头,高的说了算,而页尾与文件尾,低的说了算。头一页上的段头比页首高,kCopyFrom 取的是段头——头半页的病根就压在这一行上。kOverlap 是交集的长,取窄之后交集要是空的,它就自动落了零。剩下的两个换算跟着定死:dest_offset 把拷贝起点换算成页内偏移,source 把虚地址差换算回文件里的字节差,再一页一页地喂给 map_segment_page。那一头接活的样子,是两行词汇的事:

```cpp
    auto* const kDestination = cinux::base::PtrAt<std::uint8_t>(mm::DirectMapVirt(kFrame.raw));
    cinux::base::SetBytes(kDestination, 0, kSize);
    if (chunk.bytes > 0) {
        cinux::base::CopyBytes(kDestination + chunk.dest_offset, chunk.source, chunk.bytes);
    }
```

交集为空的页整页清零,咱们要的落法就在这四行里:每一页都由 SetBytes 整页铺零,交集非空的话,再由 CopyBytes 从页内的偏移起落字节。装载的活也收成了 base 的两笔现成词汇,清零的一笔,按偏移落字节的一笔,memsz 超出 filesz 的零头、交集为空的整页,靠的都是打底的那层零。

为什么这么多年没炸?喂过的段都页对齐,头页的起点就是页首,交集退化成了整页,单页的测试怎么跑怎么绿。生与收那一卷交过同门的学费,页内偏移的毛病单页全过、两页才炸,这一课是它的续集,咱们把复现的案子收进机内,对齐喂了六个卷的绿,从此就有人看着不对齐了。

至于链接器为什么排出不对齐的段,咱们查过它的排法:它排段的时候常把下一段的头排进上一段末页的余量里,一页一页地省下来,这是人家的常态,不是 busybox 的怪癖,咱们的装载器必须认。

第四层是入口的画像。musl 全链跑起来了,busybox 的主函数头一件事读 argv[0],读到的却是 NULL:exec 从来没在栈上铺过参数。咱们给装载器立一个新函数,在清好零的栈页上画像:argc 是一,argv[0] 指着页底的路径串,envp 摆的是空,auxv 是内核递给 C 运行时的那串开机信息,这一版画像只摆一个裸的终止项 AT_NULL 交差,画完交回入口的栈顶。exec 与 init 两路共用的是同一张画像,往 ring3 跳的那一拍也改了:参数即落点,栈顶多减八字节的老尾巴删了。您别嫌画像简陋,它是全参形态之前的过渡,客人递自己的 argv 与 envp,是这一卷后面的正菜。

四层清完了,咱们验收:四条构建线宿主五十一件全过,机内的十三台照旧,探索用过的盘还原成了标准的教学盘。机器拉得起 busybox 了,不过它连一句 echo 都咽不下去。机器死掉那一刻屏上停着的那副样子,咱们叫它死面,它就在下一段里等着咱们。
