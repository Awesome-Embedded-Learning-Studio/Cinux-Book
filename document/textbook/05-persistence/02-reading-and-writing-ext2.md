---
title: 02 · 文件系统实现:天下通用的图纸,写得住字的真盘
---

# 02 · 文件系统实现:天下通用的图纸,写得住字的真盘

第 5 部的第二章,咱们从两张债单进门。头一张是上一章收卷时自家交的底:lease 只报了名字,挂真根与卸载的门道、tmpfs 的可挂可卸、ext2 条目的内里、盘上的时钟,句句指到的是本章。另一张倒是更老,2.5 立脸面那会儿记下的:xv6 七层对咱们三层,缺的四层 disk、buffer cache、logging、inode,那会儿咱们手里一层都没有。今天咱们两笔一起对:inode 这一章认全,buffer cache 3.6 已经开了柜,本章看它姓了 ext2 的姓怎么当差,logging 与 disk 各有各的后站。咱们走进盘的内部:卷是什么形状,门怎么进,字节怎么念成文件,又怎么写回盘上,写回去的字还得活得过一次重启。

这一章的活,咱们数下来三样。头一样是认字:盘上的卷摊开是什么版式,认卷的门一道道验什么,一条路怎么落到块上、字节怎么念回您手里。再一样是落笔:块怎么从位图里领,三层间址怎么逆着读路的格子填,名字怎么住进目录、又怎么走得干净。末一样是安家:字从写穿到回写的合同怎么立,盘上的钟怎么问,真盘怎么坐上根、tmpfs 怎么可挂可卸。三样之外咱们还留了一场对照:卷尾对照一节请出 xv6 的盘与 Linux 的号,当年 4 MiB 教学卷的验收旧屏,今天咱们请回来压轴。

量法还是从前的三条。码:实况锚是 r34_compile_self 的克隆,行号一处一处地按它数。ext2 的写路径在更往后的站里还要再搓一轮,咱们以克隆的正身说话,再搓的那一层归后面讲崩溃一致性的章。屏:咱们不跑新机,本章的两面屏,开机面板与 shell 会话都是从正文《认字》那一卷请回来的旧屏,当年 4 MiB 教学卷的串口原文,出处也一并标了。今日的默认卷已长到 192 MiB,这一头的实情以码为证,正文卷未及记载的,咱们如实交代。书:咱们翻的还是 xv6 第 10 章,新翻的书页是 90、95、96、98,四段书证都逐字核过了。

> **5.1 讲的是名录,条目打包按的是 dirent64 那本册子,19 字节的头加名字。讲的是 stat,字节按 144 字节的册子誊。两本册子都是 Linux 早已印好的线上格式,咱们照册子写。可盘上还压着第三本:ext2 的目录条目,8 字节的头,名字凑 4 的倍数对齐,烤它的手艺传了三十多年。同样的一条名字,内核里的形状是一副,盘上的是另一副,中间谁迁就谁?**
>
> 迁就的是咱们,认的是盘。线上两本印的是给客人看的接口。盘上这本印的是 ext2 的版式,mkfs 烤卷照的是它,天下谁家的 ext2 都照它认,正文卷里管它叫天下通用的字。同一个名字在内核里其实不落户,谁要用就到父目录的块上现读,盘上 8 字节头的形状是它唯一的家,读与写在中间当然要当面翻译。这一章讲的就是盘上这本册子:怎么认,怎么写,怎么靠它把根从镜像里的行李,换成一卷写得住字的真盘。

## 卷摊开来的样子

2.5 的 ramfs 长在堆里,格式咱们自己定自己读,顺倒是顺,不过顺得让咱们学不到东西。这一章认的盘卷,是构建的时候烤出来的。当年教学卷的烤法:脚本拿 truncate 撑一块空文件,mkfs.ext2 把它变成一卷认得出的 ext2,debugfs 这把 ext2 家自带的小手术刀不挂载就能往里放种子,一个 etc 的目录,一份两行的 motd,再加一行的 hello.txt。烤盘的三个参数咱们挨个念。`-b 1024` 把块的大小定成 1024 字节。`-O none` 让新式的特性一样不加,给咱们一张老模样的卷。`-N 128` 把 inode 的定额定在 128 个,当年教学卷的定额就是这个数。版式的每一格都有来头,咱们一格一格认过去。

卷头的第一块是引导块,1024 字节让给开机的那阵仗,superblock 紧跟着坐的是 2 号扇区,ext2_layout.hpp 的 :64 记着这个座位。字怎么排,头文件里的一排 static_assert 早把位次定下来了,咱们抄一截,ext2_layout.cpp 的 :29:

```cpp
static_assert(sizeof(Superblock) == 1024);
static_assert(sizeof(GroupDescriptor) == 32);
static_assert(sizeof(Inode) == 128);
static_assert(sizeof(DirEntry) == 8);

static_assert(offsetof(Superblock, s_magic) == 56);
```

四样尺寸一枚字位:您动一动,编不过,把守的活交给了编译器。这一排往后其实还有十来条,i_block 的 40 在 :46,咱们不整排念。这与 5.1 给 144 字节的册子定位次是同一门手艺:凡要跟外人对着认的字节,位次的事从来不靠记性。

再大的卷,家当也不能都堆在卷头的地界,咱们看 ext2 的办法:把卷切成块组,一组自带的是三样,块位图记的是组里哪些块空着,inode 位图管的是哪些号空着,inode 表把 inode 一位一位地排开。三处的址加三笔计数,收在 32 字节的组描述符里,全卷的组描述符排成一张表,紧挨在 superblock 的旁边。一组的块数有天花板,来路是算出来的:位图自己只占一块,1024 字节合的是 8192 个位,一位记的是一块,位画到了头,组也就封到了头。当年的教学卷 4096 块,离天花板还有老大的一截,所以整卷就是一组。

盘上 inode 的实际尺寸以 superblock 为准,咱们结构体认的是 128 字节的下限,mkfs 烤的卷报 256 字节一个,多出的地界记着咱们不读的扩展字段。头 128 字节里最要紧的是 i_block,十五个格子:头十二格直接放数据块的号,第十三格放一张间址表的址,第十四格放两张套起来的表,第十五格留的是三重。咱们管这十五格合成的地图叫块影,这词从这儿起就当术语使了。一张间址表占的是一块,1024 字节按的是一格 4 字节,合出来的正好是 256 格。单间管文件的第 12 到 267 块。双间走的是 256 乘 256,合的是 64 MiB,是这一版认得动的上限。三重的第十五格,读侧写侧同声回的是 kNotSupported,两头走的是同一个口径。

块影的容量够不够用,当年咱们做过一场取舍,正文那一卷的《大个子客人,与当年的绕行》记得全案。它所在的《把后半场交给解释器》那一卷要装 822368 字节的库,当年的码里双间还没铺,绕行的办法是造盘把块从 1024 改成 4096,单间一跳的天花板从 256 KiB 顶到 4 MiB,大个子稳稳地落进单间。这一遍咱们把双间铺好了,块又回到了 1024,那串字节落进了双间,连绕都不用绕了。块大小是一个盘的性格:小块省的是地方,文件的尾巴浪费得少,可间址地界窄。大块走的路正好反过来,一跳倒是管得远,小文件的尾巴却个个吃大块。

目录条目是另一副身板:8 字节的头,装的是门牌号、rec_len、name_len 三样,名字跟在头的后面,整条凑的是 4 字节对齐,ext2_layout.cpp 的 :93 一行算式就是它,8 加名字的长度,往上凑 4 的倍数。这笔细活咱们留到往盘上落笔一节再开,这儿咱们只立形状。末了把当年 4 MiB 教学卷摊在桌上数一遍:引导块 1、superblock 1、组描述符表占不满一块、两张位图各 1、128 个 inode 按 256 字节摊开占 32 块、卷头家当拢共 37 块,余下 4000 出头全是数据的地界。哪一块里住的是谁,您都数得出来。

## 认卷之门

版式对了的卷,咱们也不能光看版式就认下来,mount 得把证件一道道地验过。咱们把中段抄来,ext2.cpp 的 :67:

```cpp
    if (sb_.s_magic != kMagic) {
        return base::KernelError::kIOError;
    }
    if (sb_.s_log_block_size != 0 || sb_.s_feature_incompat != 0 ||
        sb_.s_inode_size < sizeof(Inode) || kVolumeBlockBytes % sb_.s_inode_size != 0) {
        return base::KernelError::kNotSupported;
    }
    if (sb_.s_first_data_block != 1 || sb_.s_blocks_count <= sb_.s_first_data_block ||
        sb_.s_blocks_per_group == 0 || sb_.s_blocks_per_group > kVolumeBlockBytes * 8 ||
        sb_.s_inodes_per_group == 0) {
        return base::KernelError::kIOError;
    }
```

进门的头一件事:把 superblock 从 2 号扇区整个读回。跟着咱们一道道来:魔数 0xEF53 对不上,盘的话没通。形状的三问,块只认的是 1024,不兼容的特性一样不认,inode 的尺寸短于 128 或除不尽 1024 都不行。再问计数与形状的咬合,first_data_block 的值得是 1,全卷的块数得比它大,一组的块数得在位图画界之内。组数是按块数除上来取整的,超过 32 组的卷回 kNotSupported,32 组乘 8192 块合的是 256 MiB,这一版认得动的卷幅。当年的正文卷数过六道门,如今的码问话又密了几道,咱们按码一道一道数,您也不必死抠数目。

您留意回错分两档。kNotSupported 是咱们不认这门版式:版本对不上,不算盘的错。kIOError 说的是盘没答话,读不回、魔数不对的毛病都归它。咱们判罚各归各:坏盘与生客,不算同一种的病。过了关,咱们把组描述符表整张读进来常驻,封顶的 32 组乘 32 字节正好一块,读的时候一块,放的时候一世,往后每次定位 inode 咱们都靠它翻址。末一验咱们验的是根 inode:2 号,ext2 立的老定例,读出来的得真是个目录。全过了,mount 的旗翻成真,往后卷上的任何业务进门都查旗,没挂的卷问什么都回 kIOError,不懂的不装懂。

书页 90 的书证,咱们请一段:

- `The file system does not use block 0 (it holds the boot sector). Block 1 is called the superblock; it contains metadata about the file system (the file system size in blocks, the number of data blocks, the number of inodes, and the number of blocks in the log).`

人家把头一块让给了引导,superblock 坐的是 1 号块。咱们把头 1024 字节让给引导,superblock 坐的是 2 号扇区,走的是同一个道理,留的路宽窄不同。人家把卷摊成了一条直线:log、inode、位图、数据一排到底。咱们按组切,每组自带的是一套。两家同源的还有一句:

- `The superblock is filled in by a separate program, called mkfs, which builds an initial file system.`

卷是构建期一个独立程序烤出来的,咱们这把刀叫 mkfs.ext2。书页再往后还有 log 的一节,断电之后的字作不作数,这一问的答案 5.3 当面学,本章咱们一个字不给。

## 把字节念成文件

路进门的手续,折路、门面、最长前缀的那一套,5.1 都讲全了,ext2 这头只接门面递来的活:一个目录节点、一段名字。认名用的还是那一章那把刀,切的活不分家,咱们只补一句现状:路上遇见软链接也追,限的是四十跳,短链的字就住在 i_block 里,长链的字要走 read 去读,这笔细活正文卷的后续站会写。

号拿到了,inode 住哪儿?咱们看这一段,ext2.cpp 的 :350:

```cpp
    const unsigned long kGroup = GroupOf(static_cast<std::uint32_t>(ino), sb_.s_inodes_per_group);
    if (kGroup >= group_count_) {
        return base::KernelError::kIOError;
    }

    const unsigned long kIndex =
        IndexOfInode(static_cast<std::uint32_t>(ino), sb_.s_inodes_per_group);
    const unsigned long kBytes = kIndex * sb_.s_inode_size;
    const unsigned long kBlock = groups_[kGroup].bg_inode_table + (kBytes / block_size_);
    const unsigned long kShift = kBytes % block_size_;
```

咱们拿当年那卷上的 hello.txt:14 号,咱们真走一遍。inode 的号从 1 起数,一组的定额是 128 个:14 减 1 得 13,13 除以 128 商的是 0,余的是 13,落的是零号组、组内 13 号座。盘上一个 inode 合的是 256 字节,13 号座折的是 3328 字节。3328 除以 1024 商的是 3,余的是 256,14 号就住在 inode 表往后数的第 4 块上,块内的第 256 字节起。您拿计算器按一遍,跟咱们算的一个数。每回 lookup、read、stat 都是这么现算现读,内核连一座 inode 的缓存也没盖。当年的正文卷在这儿卖过关子:不盖,重复读靠什么接住?今天揭:块缓存接住。superblock、组描述符、inode 表的座位全在块上,热块在 3.6 的那口柜子里坐着,第二回读是柜子里的便宜,头注的原话 `the block cache covers these reads for free`,白送的。

inode 到了手,块影摊开了,数据块住的是哪一格,咱们一层一层指:头 12 块直取,12 到 267 走的是单间,再往上走的就是双间。读侧的章程是只认格子不填格子,格子里是零的就当洞,读回来补的是零。读到 i_size 之外回的是 0。目录递给 read 回的是 kIsADirectory,目录的读法另有家门。念目录走的是 walk_entries,块进门过的是四道自验,ext2.hpp 的 :338:

```cpp
                if (entry.rec_len < kDirEntryHeadBytes || entry.rec_len % kDirEntryAlign != 0 ||
                    cursor + kDirEntryHeadBytes + entry.name_len > block_size_ ||
                    kDirEntryHeadBytes + entry.name_len > entry.rec_len) {
                    return base::KernelError::kIOError;
                }
```

rec_len 不能短过 8 字节的头,整除按的是 4,名字不能出块也不能出自己条目的界。四道过不了大声回错,坏卷在门口就被拦下了,咱们不悄悄绕。念清单进门咱们还有一道要过:名字顶到 kFsNameMax,跳过的就是整条,连座次的边都不沾。当年正文《认字》那一卷的验收篇里做过一桩手术,把一条名字的长度手改到 40,同一桩手术的两个年头,拦法走的是两条道。当年那个名字在卷里排的是末位,rec_len 把块里剩的地全包圆,48 字节其实还在界内,四道全放行了,拦它的是进门那道,当年的 kFsNameMax 是 32,顶到线的是 40。如今 ext2 的测试自家现烤一卷 4 MiB 小卷,烤法正是当年教学卷的那一路,这卷里同一条名字后头住进了邻居,rec_len 紧到了 20,名字 40 出了自家条目的界,四道的末道当场拦下了,清单走到这儿就断了流,后面的条目一条也不念了。断言还是从前的那句,那个名字从清单上消失了,拦它的门换了。天花板如今在 fs_config.hpp 的 :31,定的是 256。

念清单的 read_dir 把条目誊进 dirent64 册子:门牌号一格誊盘上的真号,兑的是 5.1 那句“真盘填真号,内存假盘交白卷”。类型词咱们不读条目里的存货,拿孩子的号去读孩子的 inode,切的那一刀下在 mode 里,ext2.cpp 的 :291。盘上的册子不记类型,您问谁是文件谁是目录,inode 说了算,这是 ext2 的定例。

**您停一停,认的是座次。** 14 号咱们走了一遍,您拿 100 号自己走:落在 inode 表往后第几块、块内第几字节起?再问一句,这卷一组的 inode 只有 128 个,第几号起就出了卷的门、压根不存在?您推一推,节末的题等着您。

## 往盘上落笔

读侧认的只是格子,写要动的是家当,动家当的活锁在一把锁里。头注的原话在 ext2.hpp 的 :12:`a whole metadata sequence — find a free bit, set it, discount the counters, write the superblock — must land as one breath or two files can claim the same block`。咱们把它翻过来:找空位、占上、折计数、写 superblock,一口气落的是一整段元数据,断了气口,两个文件能领走的就成了同一块。用的锁是睡锁:临界段横跨队列命令,抱着一面自旋锁就睡过去了,是这棵代码树里记载得最全的一种死法。3.6 说过柜子的那句“护槽表,不管次序”,次序押的就是这把 write_gate_。

咱们看位图上怎么领一块,ext2_write.cpp 的 :469:

```cpp
        base::bit::WordMask words[kBitmapWords] = {};
        if (!bitmap_words(groups_[group].bg_block_bitmap, words, kBitsPerBitmapBlock)) {
            return base::KernelError::kIOError;
        }
        base::bit::Bitmap bits;
        bits.adopt(words, kBitsPerBitmapBlock);
        const unsigned long kBit = bits.find_clear_run(1);
        if (kBit == base::bit::kInvalidIndex || kBit >= sb_.s_blocks_per_group) {
            continue;
        }
        bits.set(kBit);
```

位图整块抄进栈里的词,盘上现成的词 adopt 进来当底子,找的是头一个空位,占了。写回去的只是一个字节,patch 到位图的块上。咱们拿三段凑成块号:first_data_block 加组号乘每组块数,再加的是位号。计数两处一起折:superblock 里全卷的空块数,组描述符里组内的空块数,咱们一趟下去,两笔都改了。空组咱们跳过,组组地找过去,全空了才回没块可给。

读是顺着格子找的,写是逆着格子填的,用的是同一张块影,走的是两个方向。ensure 的三层里,咱们看单间一层表内填格的那几行,ext2_write.cpp 的 :668:

```cpp
    if (table[outer] == 0) {
        base::Result<unsigned long> fresh = zero_fresh_block();
        if (!fresh.ok()) {
            return fresh;
        }
        table[outer]  = static_cast<std::uint32_t>(fresh.value());
        node.i_blocks = static_cast<std::uint32_t>(node.i_blocks + kBlockSectors);
        if (!cache_.patch(kTableLba, outer * sizeof(std::uint32_t), sizeof(std::uint32_t),
                          &table[outer])) {
            return base::KernelError::kIOError;
        }
    }
```

单间的一格是零:表还没有,起手领一块全零的新块当表。表到了,读回来看的是第 outer 格,格是零的就再领一块,填进格的是号,patch 写的只是四个字节,一张 1024 字节的表,添的是一个号,动的是四个字节。您留意每个号都是 zero_fresh_block 领来的:领来的块整块写零,上一位住户的残字一个不许带进来。双间的套法是再套一层,每查一层的空,就套一层的表。单间算是最省事的,格是零的才领,格非零就直接用现成的号。三重的两头同声不认,与读侧走的是同一个口径。

写内容的循环挨块切,咱们跟一笔:算好眼下这段字落在块影哪一格,ensure 把格子填实了,字节写了进去,轮到的又是下一块。收尾的三笔,咱们挨笔念。只许长不许缩的是 i_size,新旧取的是大。mtime 与 ctime 的落笔在同一秒。write_inode 写回盘的是整只 128 字节。截断走的是另一条门路,接着的活安排在 O_TRUNC 进门时,ext2_write.cpp 的 :342,咱们本章不开。立一座新的目录,make_dir 亲手打包头两条:点的条目 12 字节,点点的条目把块里余下的地全吃进 rec_len,收尾的是一块。目录的 i_size 生来一块,links 生来的也是 2:父目录里的名字一张,自家块里的点一张。点点的条目记的是父的号,票记的是父目录,父目录的 links 因此加一,组里的在用目录数也加一。

住进目录的名字,咱们看 add_dir_entry 在块里怎么找地,ext2_write.cpp 的 :813:

```cpp
            if (entry.inode == 0 && entry.rec_len >= kNeed) {
                return fill_dead_slot(kHere);
            }
            if (entry.inode != 0 && kRoom >= kNeed) {
                return shrink_live_entry(kHere);
            }
```

走的是两条路。门牌号是零的槽,尺寸够的就住进去,咱们管它叫死槽,这词从这儿起就当术语使了。活条目尾巴上的缝够宽,就把活条目缩了一道,新条目也住进了缝里,缩缝这词的起点也是这儿。缝都走不通怎么办?当年正文卷写过一句“给目录添条目,绝不给它添新块”,那是当年认下的省略。如今的码补上了第三条:给目录添一块新地,新条目的 rec_len 把整块包圆,父目录的 i_size 长一块,写回的活归 write_inode。

删名反着来:能并就把两条的 rec_len 并进前一条,并不了就抹门牌号,活条目变成了死槽。到这儿名字层的债清了,本体层的另一本:unlink 摘名后看 i_links_count,归了零就当场还,dtime 记的是最后一面,没有钟就回退到卷的生日 s_mkfs_time,数据块一块块地还,inode 也还了号。本体若是目录咱们再添两笔:父目录 links 减一,组里的在用目录数也减一。“名字没了字节还在”的那句老话,只在条目的那一层成立,本体层偏偏是急脾气,票尽了就收房,连一夜的宿也不留。半路里断了电,几笔数对不上怎么办,fsck 的故事归 5.3,咱们只立问。

再兑 5.1 问过的生死。ramfs 那头的票在节点身上,节点的死活看的是票数,不归零您就能接着读。盘这头的 retain 与 release 空着手,ext2.cpp 的 :300 两个空函数,头注在 ext2.hpp 的 :240,写的是 `Nothing to pin: the volume holds every inode in its tables`,卷上本来就有给每个 inode 留的座,内存里没有要护着的东西。票记的地方不在内存里而在盘上,那票的名字就叫 i_links_count。烤盘的时候,构建脚本拿 set_inode_field 把 busybox 的 links_count 手改成 27:一个 inode 的名下挂了 27 个名字,合起来过的是一条命。盘这头其实没给过“删了还能读”的承诺,摘了名、票尽了,座就还了,两副家底其实各自都诚实。

## 写穿、回写与一次 sync

ext2 的头一版落笔,走的路是笔笔写穿:每笔都当场写到盘上再回话,慢倒是慢,可答出来的数没有一句假话。当年的实话说在正文卷里:写与盘同一拍。咱们宁可不答,假数是从来不伺候的。教学内核认笨而不认巧。认笨是那一卷的选择,其实不是终点。后来柜子动了两刀,槽长到了页,补丁改落了槽里,零号槽案 3.6 讲全了,咱们不重开课,只看今日的合同。

合同定的是三条,咱们挨条看。落槽的 patch 不发盘命令,脏位亮了起来,活槽可以跑在盘的前头。受害者让座之前把脏字还给了盘。fsync 是显式的一刷,FileWorld::sync 的 :54 一行,`cache_->flush() && queue_->flush()`,缓存清完了清队列,两级是一起清的。74 号 fsync 与 75 号 fdatasync 同走的都是这一刷,sys_fsync.cpp 的 :27,判类型、认号的章程也都在同一个函数里。柜子自己也长了一回:哈希索引,二次机会挑的是受害者,个儿也按预算长了,file_world.cpp 的 :60 起三行,预算的走法是可用内存除以 32、封顶 64 MiB。当年正文卷说过产品的代码里还没有谁真的调 flush,如今 fsync 是活的系统调用,您在壳里就敲得响,咱们按今日的码讲,当年的话不冒领,今日的活也不夸口。

flush 的后半句在队列那间值班室:缓存清完,队列把自己积的命令也清干净,一块盘一次只听一个命令的排班,内里的故事归 5.4,咱们只念这一句。

**您停一停,对的是时辰。** 您敲下 poweroff、机器断电之前,谁被刷了,谁没有?您翻 sys_reboot.cpp 的 :27 看一眼再回来。咱们对:被刷的是控制台,console::Flush 一句就办完了,面板上的字走干净了。盘上的字没有刷,sync 是没人叫的。断电之后盘上的字还作不作数,立在这儿的是这一问,当面给答案的是 5.3。

## 盘上的时钟

时辰问主板上的 RTC 要。CMOS 的两个口:0x70 选的是寄存器,答话的是 0x71。选完了往 0x80 写个零,等芯片的节拍安顿下来再读,rtc.cpp 的 :28 记着这道小门。读之前咱们还有一关要等,A 号状态寄存器里的更新位在跳,这个寄存器也是 CMOS 里的一格,一跳就说明的是芯片正翻着日历,等它跳完了再抄,rtc.cpp 的 :54 一圈 while 就是等。抄的是七样:秒、分、时、日、月、年、世纪。一次是抄不稳的:芯片随时翻到下一秒,半新半旧就读岔了。now_seconds 的办法是读两遍,rtc.cpp 的 :71:

```cpp
unsigned long long Rtc::now_seconds() {
    RawClock raw = read_raw_once();
    for (RawClock again = read_raw_once(); !same_raw(raw, again); again = read_raw_once()) {
        raw = again;
    }
    return DecodeClock(raw, read_register(kRegStatusB));
}
```

两遍全同了才采信,不同的就以后一遍为准接着读,读到稳了才作数。钟面上的一次读数,值两遍的价钱,这道较真咱们记下。

芯片说话带的是方言,DecodeClock 一个 constexpr 的纯函数全接住。BCD 的拆位,四位四位地拆,拼回的是十进制。12 时制的 PM 位要加 12,可午夜 12 点偏偏是个特例,hours 不等于 12 那道判断是承重的,错了就把中午错摁到半夜。世纪寄存器在不少的主板上压根没有,按本内核发布的那年猜起,rtc.hpp 的 :103 把 2026 记在 kReleaseYear 里,一路往上猜到 2099 是不会错的。纯函数的好处是宿主上能直接驱,四个格式的象限:BCD 与二进制、12 时制与 24 时制,宿主测试挨个地喂过,机器上咱们只留两个读口。秒数换算历法的那门手艺,注释里注明借自 Howard Hinnant 的日期算法,来历是清白的。

ext2 其实不直接认识芯片,咱们构造它的时候递一枚 ClockFn 函数指针,ext2.hpp 的 :63。机器上递的是 wall_clock,问的是 RTC。宿主测试递一枚冻住的钟,当年验收篇里的那枚 1893456000 就是它,您案案都能对。盘上盖的是三处章:write 落笔,mtime 与 ctime 同的是一秒。unlink 还的是房,dtime 记的还是最后一面,没有钟就回退到卷的生日。5.1 的那笔“ramfs 的 mtime 老实填零”的债,盘这头今天还上的正是那笔旧债:钟问得到,戳就是真的。

## 真盘当根

卷认得了,还差的是坐上根。这么一坐,开机的故事换了个讲法:boot task 把机器带进了多任务,自己停在了原地,kernel_init 接了班,init_thread.cpp 的 :276 记着换岗。文件世界的活只能等接班,道理在队列:盘的队列只在任务上下文里当差,boot 栈上抱着一面锁就睡过去了,没有谁叫得醒睡着的它。kernel_init 接班后的头一件事,干的就是 FileWorld::self().init(),init_thread.cpp 的 :278 一行。关机走的 ACPI 正门也是正文《认字》那一卷立的路,表树一棵棵地找过去拿断电路径,咱们一句带过,逐层重走的事就免了。

init 里的五步,file_world.cpp 的 :58 起。队列绑上了 AHCI。缓存预算按的是可用内存除以 32、封顶 64 MiB,预算换的是槽数,当柜的是 BlockCache。Ext2 的构造,递进去的是 wall_clock。过门的是 mount,守底的是 Check 一句 `ext2 volume refused to mount`,挂不上的下场是当场喊停。末了 vfs_.mount 把根前缀交给这卷 ext2,真盘坐上了根。/dev 与 /proc 各自挂各自的家,内里的事归 5.7,咱们念过名字就走。

lease 的正戏在这儿,5.1 只报了名字。咱们把 mount_tmpfs 摆开,file_world.cpp 的 :109 起:新起的是一座 RamFs,铸的是一枚 MountLease,backend 与析构的章程封在票里,挂载带的是名字和票,失败了当场拆,不留半座的空壳。`umount_tmpfs`,file_world.cpp 的 :154 起:有人还用着,use_count 记的数目大于一,又不是 lazy 卸载的那一路,回的是 kWouldBlock,系统调用那头翻成的就是 EBUSY。号也对上了表:mount 165,如今只认的是 tmpfs 五个字母,别的类型回 ENODEV。umount2 领的号是 166,lazy 走的是 kDetach 旗标,值记的是 2。可挂、可卸、卸不动了还会说话,5.1 的那句“门道归 5.2”今天算是兑清了。

mounts_text 的头一列,如今填的还是 none,老实话还在说:mount 的签名里前缀、后端、类型名、lease 四样,设备名的座位是空的,vfs.hpp 的 :338 数得出来。哪天接了线,哪天那一列才有了字,咱们如实交代。tar 与盘的家世,咱们一句对清:档案包是一卷字节流,从头念到了尾,装车是它的本行。盘卷是带座次的版式,哪块住谁的座次都数得出来,安家是它的本行。5.1 送走了 ustar,这一章把 ext2 迎进了门,家世就此对完了。

盘也不是一天长成的。教学卷从 4 MiB 长到了 16 MiB,inode 从 128 涨到了 512,正文《大一号的盘,细一档的钟》记的就是这一程。今日默认卷是 192 MiB 的 glibc 盘,user 树是按 glibc 那套 C 库构建出来的,卷就得了这名,烤盘脚本 create_ext2_disk.sh 的 :86 记着数:192 MiB,-N 记的是 16384,mkfs -d 把整棵 user 树一次烤进了卷,busybox 的一个 inode 撑了 27 名,是 mkfs -d 保下的真硬链,共享库按 SONAME 小名立的软链也一并原样,脚本的头注 :23 写得明白。手改 links_count 的那笔在脚本 :165,走的是给 16 MiB 教学卷烤盘的 debugfs 路,跟咱们这卷不相干。192 MiB 的这一程正文卷未及记载,咱们以码为证。末了收 2.5 那笔四层的债:disk、buffer cache、logging、inode,当年的一层都没有。今天 inode 本章认全了,buffer cache 的开柜在 3.6,本章看的是它当差,logging 归的是 5.3,disk 归的是 5.4。名字立在了盘前,形状立在了介质前,介质的这一层,今天立住了。

## 对照:xv6 的盘上,与 Linux 的号

书页 96 的那段 inode 书证,咱们请一段:

- `Every inode is the same size, so it is easy, given a number n, to find the nth inode on the disk. … The nlink field counts the number of directory entries that refer to this inode, in order to recognize when the on-disk inode and its data blocks should be freed.`

同尺寸挨个地排,拿了号就能定位,咱们那三行算术跟它是同一门。票数记在 inode 的身上、数着名字等归零还房,咱们那 i_links_count 与 dtime 跟它也是同一门。busybox 的 27 名一条命,两家认的都是这门数。

书页 98 的间址那段,咱们请一段:

- `Thus the first 12 kB (NDIRECT x BSIZE) bytes of a file can be loaded from blocks listed in the inode, while the next 256 kB (NINDIRECT x BSIZE) bytes can only be loaded after consulting the indirect block. … If ip does not have such a block yet, bmap allocates one.`

人家走的是一张表,咱们三层。更要紧的是那句尾注,bmap 读到零位当场领了块,读写合的是一把刀。咱们把刀分了两把,读侧认的只是格子,领块的事永远不沾,写侧的 ensure 逆着填,教学上各讲各的直路。

书页 95 的位图那段,咱们请一段:

- `Xv6's block allocator maintains a free bitmap on disk, with one bit per block. A zero bit indicates that the corresponding block is free; a one bit indicates that it is in use.`

人家走的是一卷一张位图,咱们一组一张,组描述符里另记了空块空 inode 两笔,卷大了不必从头翻。Linux 的号,咱们只认自家注释里写明的,一笔一行的记法:mount 165、umount2 166,fsync 的 74、fdatasync 的 75,reboot 的 169,mkdir 的 83、unlink 的 87,咱们跟它同一个号本。ext2 的版式本身,咱们不去攀 Linux 的亲,对读对象是 mkfs.ext2 烤出的真卷,天下通用的字,认的是这个。

## 实况:当年 4 MiB 教学卷

本章就不跑新机了,压轴的屏从正文《认字》那一卷的验收篇请回来,当年 4 MiB 教学卷的串口原文,咱们逐字抄在这里。您看开机面板:

```text
[kern] irq on, tick 100Hz
[kern] rtc wall clock 1791889222
[kern] keyboard on, type to echo
[kern] boot task (pid 0) parks here, kernel_init (pid 1) takes over
[kern] kernel_init: root mounted, launching shell in ring 3
cinux shell - type 'help'
```

面板上的六行,咱们挑这一卷的新话念。rtc 的那一行,问钟一节的收成,开机报了一次,这一行的数每趟开机都不一样,两屏引的是同一趟机器。boot task 的那一行,开机新故事的开场白。`root mounted` 说的是 shell 下楼之前根已挂好,挂的就是那卷 ext2。shell 的会话,咱们整屏端上来:

```text
cinux> ls /
./
../
lost+found/
etc/
hello.txt
cinux> cat /hello.txt
hello from the platter
cinux> stat /hello.txt
  inode: 14
   mode: 33188
  links: 1
   size: 23
  mtime: 1791867901
cinux> cd /etc
cinux> pwd
/etc
cinux> cat motd
Cinux says hello from a real disk.
This line lives in an ext2 volume, baked by mkfs at build time.
cinux> cd /
cinux> echo a real note > note.txt
cinux> cat /note.txt
a real note
cinux> ls /
./
../
lost+found/
etc/
hello.txt
note.txt
cinux> poweroff
cutting power through acpi
m
```

这一屏咱们念三遍。头一遍看的是形状:./、../、lost+found,内存盘的清单上从来没有过这三样,它们是 mkfs 烤卷那一下就住进去的,ext2 的版式在屏幕上一眼认得出。第二遍看的是号:stat 报 14 号,正是咱们手算走过的那个座。size 的 23,hello.txt 那一行的字面是 22 字节,添上结尾的换行 23。mode 的 33188,换算成八进制的 0100644,读写归的是主人、旁人只读。mtime 比这一趟开机的钟早了六个钟头不到,烤盘是构建的时候,戳是 mkfs 盖的,盘上留的是谁也没动过的原样。第三遍看的是落笔:cd /etc 之后一个 cat motd,相对路认了。echo 落出了 note.txt,第二遍的 ls,它排上了清单,盘上写的字回得来。末了 poweroff 一句 cutting power through acpi,模拟器退了场,屏末那个 m 是断电之后串口上的残响,转写如实地留在了原地。今日默认卷已是 192 MiB 的 glibc 盘,note.txt 那一幕在今日的机器上还是这个章法,咱们不另起新屏,老屏作的是证。

## 本章留下了什么

盘的内部咱们走了一遍:卷按版式摊开了,门一道道地验,字节顺着块影念了回来,又逆着格子写了上去。写从笔笔写穿长到槽上的回写,fsync 一刷的是两级。钟问的是主板,戳写上了盘。真盘坐上了根,tmpfs 的可挂可卸,2.5 那笔四层的债今天对清。新词入了册:

| 术语 | 行话 | 一句话口径 |
| --- | --- | --- |
| 烤盘 | mkfs | 构建期把一卷空的字节文件变成一卷认得出的 ext2 |
| 块组 | block group | 卷切成的组,一组自带两张位图、一张 inode 表 |
| 组描述符 | group descriptor | 32 字节,记一组三处址与三笔计数 |
| 位图 | bitmap | 一位记一块或一号,零是空,一是在用 |
| 块影 | i_block | inode 里十五格合成的文件数据地图 |
| 间址表 | indirect block | 占一块、存一排块号的中转表,单间一层,双间两层 |
| 死槽 | unused entry | 门牌号记零的目录条目,尺寸够就能住新名字 |
| 缩缝 | shrink | 把活条目的 rec_len 缩一道,腾出缝住新条目 |

没讲的咱们如实交底:断电之后盘上的字还作不作数,fsck 与日志的正课归的是 5.3。块设备的队列与引擎,那间值班室的内里,归的是 5.4。devtmpfs 与 procfs 的内里,归的是 5.7。软链接的细活,正文卷的后续站会写。三重间址不是欠讲的课,读侧写侧同声回的都是 kNotSupported,两边不认的都是这门,谁来了也不开。当年正文卷认下的两处省略,目录添新块、名字的天花板,本章都按今日的码补讲了。

## 练一练

咱们留三道题,一道认的是座次,一道算的是字节,一道对的是时辰。

**题一认的是座次。** 当年的那卷教学盘,一组配的是 128 个 inode,盘上一个占的是 256 字节,inode 表从它自己的址往后排。100 号 inode 落在哪块、块内第几字节起?129 号的门让不让进?您推一推,再对底下的参考答案。

**题二算的是字节。** 一条名字 5 字节的目录条目,整条几字节?名字 3 字节的又是几字节?再进一步,一条活条目 rec_len 记的是 32、名字 5 字节,尾巴上的缝里要住进一条 3 字节名的新条目,缩了缝走下来,这块地上躺着什么?您按凑 4 对齐的算式算。

**题三对的是时辰。** echo 了一句进 note.txt,write 回了 0。这一拍的字在哪儿?跟着敲一发 fsync,两级各发生了什么?这一章为什么不肯再往下答一步?您对着合同三条说。

### 参考答案

题一这么走:100 减 1 得 99,99 乘 256 得的是 25344,25344 除以 1024 商的是 24,余的是 768,落在 inode 表往后的第 25 块、块内第 768 字节起。129 号减 1 得的是 128,128 除以 128 商的是 1 余的是 0,该去的是下一组,可这卷一组封了顶,号也超了 s_inodes_count,read_inode 头一道的号界检查当场拒,门倒是不让进。您拿 14 号的走法对着核一遍,次序是一样的。

题二的答案:您按算式走,8 加 5 得的是 13,凑 4 的倍数是 16。8 加 3 得的是 11,凑到了 12。缩缝的走法:活条目把自家的 16 缩回来,32 减 16 剩 16 的缝,新条目要的只是 12,所以住得进。新条目的 rec_len 不记 12,记缝的全长 16,把余下的地一并包圆。包圆的 16 里,新条目自家的字只占 12,余下 4 字节:最短的新客也要 12,其实谁也住不进,这片地荒着,等哪天它自家搬走了,并进前一条时整块地收回去。两条就这么挨着住,16 加 16 合回了 32:链一寸没断。

题三的答案:字在缓存的槽里,亮着的是脏位,追它的是盘。fsync 这么一刷,缓存 flush 把脏槽还给了队列,队列 flush 把积下的命令排到盘,字这才真正写到了盘上。再往下一步问的是断电之后作不作数,那是崩溃一致性的地界,开讲的是 5.3,咱们立问不给答,您要到那一章去对。

## 延伸

- xv6 书第 10 章,书页 95、96、98 的三段,您拿它与位图、座次、间址三节对读,同一件事的两副取法。
- man 2 fsync 与 man 2 sync,fsync、fdatasync 的词条在里头,POSIX 给这一刷的正名,您一段读完。
- man 2 mount 与 man 2 umount2,挂与卸的号、旗标与 EBUSY 的来路,本章跟它对过表。
- 正文《认字》那一卷,这一章的前身,门里门外的旧事、烤盘的脚本与验收的全案都在那儿。
