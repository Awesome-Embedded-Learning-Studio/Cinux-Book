---
title: "06 · 装载器认 ELF"
description: "kernel/loader 目录在这一卷出生:elf_layout 纯词汇件校验头,装载件逐段逐页,整页清零加文件字节前铺,bss 天然零出身。页内偏移是装载循环的主角:超过一页的段,第二页起全零的毛病当年炸过,这一遍换了衣裳又来,连同 Floor 页号语义同日二犯。手搓 ELF 字节流三处字段错,改程序化造像根治。UBSan 线逮住 0x40105F 的未对齐读。"
chapter: 17
order: 6
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - loader
  - elf
---

# 装载器认 ELF

fork 会造孩子了,孩子拿什么当新身体?盘上的程序。装载格式摆在面前的两条路:接着用平坦二进制,自己发明一套哪里是代码、哪里是数据、占几页的约定。或者认 ELF——世上程序自带的说明书。咱们选了 ELF。平坦二进制表达不了多段、表达不了 bss,咱们得替它编一套私房格式,而私房格式是活不长的。而史实上,当年第一遍的 execve,本体就是一台 ELF64 的加载器,头校验、段循环、NX 位全都配上了,exec 不讲 ELF 的话在史实上都说不通。还有远景:往后装库跑别人程序的那一卷,musl 编出来的静态 ELF,认的就是这套格式,咱们今天把门开对,到那天走的就是熟路。教学面上只取最小一角:静态可执行文件的装载,动态链接的学问,咱们留给装库那卷再展开。

新家也开了:kernel/loader。咱们的做法是,顶层目录在需要它的那一卷才出现。debug 当年就是为切换排查生的,这一回轮到了 loader。里头的两件:elf_layout,一件纯词汇的构件,管的是把字节认成字段,内存和分配咱们都没让它碰,宿主和机内两侧的测试驱动同一份代码。而 loader 本体管的是把段铺进世界。

ParseElf64 的检查单,咱们念一遍:头四个字节得是 0x7F ELF,类是 64 位的小端,机型 0x3E 的 x86-64,类型是 2 的 EXEC——动态的、别的机器的,和截断的文件一个待遇,咱们一概拒绝。段表的偏移、每条的尺寸、条数,整张表都得完整地落在文件界内。读段的事交给 ReadSegment,调用方循环拿 ElfSegment:虚地址落在哪里,文件打头的是哪个字节,文件里占了多少,内存里占多少——memory_bytes 比 file_bytes 多出来的那截,就是 bss 了。这些位号咱们一个都没有拍过,全数出自 System V 的 ABI,一格一格地定死,宿主测试照着 ABI 的偏移独立推导字段,两边的数对上,才算真的认得。

装载的循环,主角是页内的偏移。咱们把循环本体端出来,那四行算术就长在它的头上:

```cpp
        const std::uint64_t kPageNumber = cinux::base::math::Floor(segment.virtual_address, kSize);
        const std::uint64_t kPageBase   = kPageNumber * kSize;
        const std::uint64_t kInPage     = segment.virtual_address - kPageBase;
        const std::uint64_t kSpan = cinux::base::math::Ceil(kInPage + segment.memory_bytes, kSize);
        for (std::uint64_t page = 0; page < kSpan; ++page) {
            const std::uint64_t kSpot        = kPageBase + (page * kSize);
            const std::uint64_t kInto        = kSpot - segment.virtual_address;
            const std::uint8_t* chunk_source = data + segment.file_offset + kInto;
            const std::uint64_t kRemaining =
                kInto < segment.file_bytes ? segment.file_bytes - kInto : 0;
            const std::size_t kChunkBytes =
                kRemaining > kSize ? kSize : static_cast<std::size_t>(kRemaining);
            map_segment_page(space, kSpot, chunk_source, kChunkBytes);
        }
```

段的虚址不页对齐——链接器把 .text 放在 0x400000,.rodata 紧跟着排在页的中间,是它的常态。所以跨度用的是页数:kInPage 是段头在头一页里占掉的偏移,kSpan 是连头带尾跨过的页数,循环从 kPageBase 起一页一页地领帧。承重的是 kInto 那一行。它回答的是,这一页的起点折回段内,踩在段内的第几个字节上,咱们就让文件字节从那里接着往下铺。kRemaining 管的是零头,铺到 file_bytes 就止住了。每一页的铺法交给 map_segment_page:整页清了零,文件字节从头铺了进来——后面剩下的天然是零,bss 就落在了这个零里,咱们不用专门清。装载的活要领帧、要 map_user,动的都是机内世界自己的页表设施,这些家什宿主的测试世界里没有,所以装载器机内独跑,宿主上能考的只有词汇件。顺带交代一句它的性质:装载的全程不碰 CR3,新帧的写走的是直接映射,不抢当前世界的表。

这个主角当年咬过咱们一口:execve 装出来的程序,从第二页起 .rodata 读全是零——页循环的步进没算页内偏移,文件字节全铺去了第一页。这一遍它换了衣裳又来:初版循环把 Ceil 算出的页数当字节步进使,超过一页的段从第二页起根本没映出去。页内偏移这类毛病最阴的地方在于,单页的测试全过,两页的程序才炸。回头再读装载循环的时候,咱们把它揪了出来,改成了页计数循环,才算断了根。

同一天还犯了同一课的另一遍。loader 里算的是页基址,把 Floor 的返回值当地址使了——上一节刚说过,它返回的是页号,页号乘回页大小才是地址。偏偏没乘,映射整体建去了虚地址零页,探针打出了一行:translate 返回 0,当场就定了案。上午咱们刚在同一个词上错了一回,下午这里又挨了一回,同一天错了两回,说的都是一件事:动笔用词汇之前,把它的 Doxygen 返回值语义朗读一遍。页号、页地址这对易混的词,咱们在这一卷的笔记里收成了一张对照表。

测试的家底也交代。头一回手搓的 ELF 字节流,三处字段错:entry 和 phoff 写反了,给段头塞了一项不存在的 machine 检查,payload 偏移和数组的实际位置脱了节。手摆头是易错的工艺,咱们重写成程序化造像——put_word、put_dword、put_qword 按位号逐字段填,两个测试文件各自摆了一份字节流,摆的时候用的是同一套造法,字段错位从根上没了。万一两份拷贝犯了一样的错,风险就还是藏着的,把造像收成两家共享的测试家底是往后的活,咱们在这儿照实记下。造轮的检查同一天也做了:base 的 byte_order 里,ReadWord32 是立字库那一卷就进了 base 的家底,ReadWord16 是认字那一卷添上的十六位伙伴,那卷的正文没提过它,咱们在这儿头一回正式带入。64 位的缺口是真缺口,ReadWord64 咱们这一卷补上,第一落点直接进了 base,组合两个 32 写了出来,Doxygen 也配全了。咱们手搓的读字三兄弟就此退役,测试里的那几个 put,也是 WriteWord16、WriteWord32 的轮子,一并换了。

还有一桩是 UBSan 构建线的功劳,咱们记给您:bss 检查读 0x40105F 上的八字节,地址不在八的倍数上,其余的构建线什么感觉都没有,UBSan 线当场就 trap 了,违例报出来的就是一条 ud2,想装聋也装不了。修成了单字节读。四条构建线里管对齐纪律的这一项,UBSan 档是唯一的守门员,这一卷的实录又添了一笔。

末了咱们到盘上见真章:构建链把用户程序直接出成 ELF,挂上 -nostdlib -static 的参数,入口的位置在 _start,readelf 验过了,报的正是 EXEC、x86-64,入口的位置排在 0x401000。第一份上盘的 hello,链接器实际给它排了三个 LOAD 段,段段都是页对齐的,各占一页——三页的程序、三个段,合成一条逐段多页的装载路,真真切切走了一遍。造盘脚本往卷里添了 /bin 目录,hello 躺了进去,盘跟着程序一起重新烤了,您构建完手里就是新的。装载器有了,下一节咱们让 execve 把它接上。
