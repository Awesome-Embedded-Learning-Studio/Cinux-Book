---
title: 06 · Page Cache 与写回
---

# 06 · Page Cache 与写回:八格柜,一记除八,一枚脏位

缺页那一课的末一程,是从一座柜子上走完的:文件页要用到的那一拍,向盘要的是一整页 4096 字节,而字节从柜子里递了出来。柜子当时没摊开,今天摊开。摊开之前咱们把地界划清:2.5 立下一张脸面,管客人与文件世界见面,脸在客人那一头。本章的柜子立在另一头:脸面背后,是正文里的 ext2,2.5 写下的那句期票如今兑了,它进门坐了根,兼当后端。柜子底下,是替盘值班的队列,一块盘一次只听一个命令,替所有任务排班的那间值班室。柜子夹在这两头之间,不见客,只伺候字节。

1.7 立过一座塔,越往下,慢一截,大一圈,内存给盘当上一层。塔立了图,没安家什,咱们今天往内存与盘中间那级楼梯上搬一座柜子:八格,每格 4KiB,盘上的页按盘页号对号入格。读,柜子替盘挡。写,柜子敢扣在手里。凭什么敢扣,扣了以后谁看着,还的时辰定在哪两处,咱们一条一条过。

这一章的活,咱们数下来三样。头一样,认出生:老缓存生下来什么形状,凭什么要长个头。再一样,认手术:槽怎么从 1KiB 长到 4KiB,键怎么从扇区号换成盘页号,一记除八怎么来回无损,零号槽的案子怎么发、怎么破。末一样,认写回:写穿翻成脏页,省的是什么,欠的是什么,flush 的脸为谁开。

量法还是三条。码,主角两件,block_cache.hpp,177 行,头注把本章的门牌全立好了。block_cache.cpp,152 行,活动的部件都在里头,咱们逐段过。尺寸与格数在 block_config.hpp 里立成三枚常量:一扇区 512 字节,一格盖 8 个扇区,柜子拢共 8 格。屏,五块,实况锚 r19_lazy_alloc_page,切版的老话 1.7 念过,不重印:起机面板两行,长凳五案,机内两案,机内又八案,末一块回指缺页那一课的文件幕。书,xv6 翻到第 10 章,书页 91 与 92 两页逐字核过,原话整段摘进对照节,您对着书核。一句披露搁在前头:长凳五案的存档只拍下两案的案名与合计行,余下三案的案名,您跑长凳那一件的时候自己认。

> **一份 177 行的文件,凭什么敢替盘挡读,还敢把写扣在内存里?**
>
> 凭头注自己报的家底。block_cache.hpp 的头注开篇就点了差事:文件系统问来问去,问的是同几块,位图、inode 表、superblock,如今又添了缺页路上的整页文件页,而每一问过队列都要花一条盘命令,所以立柜,`answers those repeats from memory`,重复的问,内存里答。往下立了两根梁。一根管换算:`A caller still speaks in sectors`,调用方不改口,报的还是扇区号,柜子替他换算成页,交回的指针是 `a pointer into the slot's middle`,指进槽的中间。另一根管写:`Writes land in the slot alone and mark it dirty`,写只落槽,置一枚脏位,盘等逐出或 flush 才追。代价头注也报了:`a live slot may run ahead of the disk`,活着的槽,可能跑在盘前头。敢与不敢、跑了算谁的,咱们正文里一处一处对。

## 出生:八格小槽,写穿当家

这座柜子不是生下来就这个个头。老形状是正文《写穿的缓存》那一篇立下的,咱们把老家底数清:八槽,每槽 1KiB,盖两个扇区,键记扇区号。两张脸从那时候就有。view,问一扇区,命中就把指针交出去。patch,把调用方的字节落进槽里。写路的老家法叫写穿:patch 的收尾,把整槽的字原样写到盘上,这一拍写完,槽与盘是同一份字,谁读也读不出两样。

拒收的家法也在那时候立下。盘要是拒收,槽标死,宁可不答,不伺候假数,下一回再问,重新请进门。腾位的家法同样朴素:命中不上,轮转指针指到谁,谁当受害者,顶班让格,不挑不选,不记冷热。那一篇给它留了一句原话,教学内核认笨不认巧,咱们署名借来,这层咱们照实认。

锁有两把,咱们看它们各守哪层。gate_ 一把押着槽表,头注原话 `The lock holds across those queue commands on purpose`,过队列的命令一路睡过去,槽表不在谁的脚下挪动。次序那一层不归柜子:ext2 的写面自己揣着 write_gate_,一整段元数据的写序由它自家押着。柜子的说法就一句:护槽表,不管次序。装柜的位置在 file_world.cpp 的 :39,`cache_ = new driver::BlockCache(*queue_, driver::kBlockCacheSlots)`,文件世界当柜主,格数从 block_config 端进来。

## 长到页:一记除八,来回无损

逼它长个头的是缺页路。补一页文件,一口咬的是 4096 字节,老槽 1KiB 的饭量,补一页得串四个槽,还得指望四槽在盘上恰好挨着,不是它干得了的活。手术分两刀,头一刀动槽,末一刀动写路,咱们一刀一刀来。

头一刀改的是构造。格数没变,还是 8,变的是每格的饭量与键:槽字节固定成 8 乘 512,键从扇区号换成盘页号。构造的头几行咱们看着:

```cpp
BlockCache::BlockCache(BlockQueue& queue, unsigned long slot_count)
    : queue_(queue), slot_bytes_(kCachePageSectors * kSectorBytes) {
    const unsigned long kWanted = base::math::Min(slot_count, kBlockCacheSlots);
    for (unsigned long index = 0; index < kWanted; ++index) {
        auto* const         kFresh = new uint8_t[slot_bytes_]{};
        const unsigned long kTaken = slots_.insert(Slot{.page = 0, .live = false, .bytes = kFresh});
        if (kTaken == kBlockCacheSlots) {
            delete[] kFresh;
        }
    }
    slot_total_ = kWanted;
}
```

槽表里插进来的每一格,键记零,活标记一律 false。`.live = false` 就一处改动,可它是零号槽案的修法本体,咱们把案子摆完,您再回头看它。page_of 与 offset_of 两件在 block_cache.cpp 的 :29 到 :35:

```cpp
unsigned long page_of(Lba sector) {
    return sector.value / kCachePageSectors;
}

unsigned long offset_of(Lba sector) {
    return (sector.value % kCachePageSectors) * kSectorBytes;
}
```

换算咱们挨着看:扇区号除以 8,商是页号。余数乘 512,是页内偏移。除得尽也罢,除不尽也罢,两个词说的都是同一块盘,类注给一来一回下了断语:`two names for one platter fact, so the translation is lossless in both directions`,一个盘上事实的两个名字,来回无损。调用方那一头一个字不用改:他报的还是扇区号,view 落在 :62 的 `*view_out = kSlot->bytes + offset_of(first)`,交回的指针指进槽的中间,偏移他自己拿着用。

就是头一刀闯了祸,咱们把案子完整摆出来。症状:ext2 的机内测试,八案死全死在挂载那一关。挂载是文件世界的头一道关,superblock 读不回来,后头全塌,可读路径一行没动。咱们真起机,拿调试器一步一步走,走到该发盘命令的那一拍,看住设备的缓冲区:纹丝不动,盘命令的影子都没有。一个读请求没发命令就拿到了数据,来处只剩一处,缓存说它命中。可这盘是冷启动的,缓存里不可能有任何真实数据。

咱们顺着命中条件回去查,病根在构造里,还是笔旧债。老形状的键是扇区号,零号扇区当引导块,ext2 从不读它,那个出厂自带活标记、键记零的槽,一辈子没人问,病埋下了不发作。槽长到页,零号页盖的是 0 到 7 号扇区。盘上谁住哪个扇区,是正文《认字》那一卷《卷摊开来的样子》那一篇摊开的版式,superblock 住 2 号扇区,偏偏就落在零号页里。挂载头一件事问 superblock,问的正是零号页,那个从没见过盘的槽举手说命中,交出去的是一槽从未上过盘的全零。八案全灭,病根就压在出厂那一行。

修法就压在构造里,拢共一处,`.live = false`。槽出厂一律不带活标记,见过盘,才有活的资格。命中认的凭据只能是见过盘,出厂的状态,一个事实都不许自带。咱们后来复盘,要不是真起机看一眼盘命令,纸面上想破头也想不到。

**停一停,命中认的凭据为什么只能是见过盘?给出厂的活标记另想两条堵法,您推一推各自的漏。**

堵法看着有不少,咱们挨个想。一条是绕:挂载别问零号页,superblock 单独伺候。可问零号页的不止挂载,后头还有排着队的调用方,症状按住一处,别处照冒。一条是填:出厂给每格发一圈真读,把盘上的字请进门。听着干净,可它把凭据换成了出厂即见过,冷盘头一问就得排队,而且格是轮转的,出厂填的字转头就让位,白请一趟。两条都在外围打转,唯独抹掉活标记,把活与见过盘绑死,凭据才立得住。出生一节的家法,宁可不答,不伺候假数,在这儿守住的是同一条界:没有事实,不许应名。

案发地是挂载头一问。今日机内 ext2 八案齐绿,当年全灭的那一关,今天照样挂上卷,屏咱们实况里过。

## 写回:一枚脏位,两个时辰

末一刀动写路。写穿的老实,出生一节数过了:写完那一拍,槽与盘同一份字,谁也不欠谁。可盘命令是最贵的动作,能省一趟就省一趟,如今轮到写路学着省,咱们把 patch 的收尾从整槽下盘改成两行:

```cpp
    base::CopyBytes(kSlot->bytes + kInPage + offset, source, bytes);
    kSlot->dirty = true;
```

CopyBytes 把字节落进槽,dirty 置真,往下 return,盘命令一趟不发。patch 开头另有一道岗:改动要是跨出本页的边界,当场拒绝,头注写的是 `cross out of its page`,一格只管自家页里的事。这一改立下的唯一新事实,就是一枚脏位:槽里的字换过了,盘上还是旧的。Slot 里那枚位的注释只写了半句:`Whether the platter runs behind these bytes`,盘落后于这些字节,笔者的原注,咱们原样摆着。

省的一面,头注报的是 `a burst of patches into one page costs a single write`,连着往同一页补字,几趟 patch 攒下来,还的时候并成一趟写。正文《懒到家》那一卷当年在这段路上记过一笔数:块缓存聚到页粒度,ext2 的读聚合了两成五,一趟盘命令捎回的字节多了,排队的就少了。这说的是当年。咱们这一遍,功能由实弹验过,收益的计数没另做,当年那笔数,咱们不冒领。

欠的一面头注也报了,就是章首那框里那句跑在盘前头。隔着缓存看盘的人,跑测试的也罢,看崩溃现场的也罢,看到的可能是旧字,要新的,得显式 flush 一遍。当年三处隔着缓存直接窥盘的测试,那一批全补上了显式的 flush,清偿完了再看盘。还的时辰定在两处,一处逐出,一处 flush,咱们挨个看。

逐出的时辰,咱们看 claim_slot 里领到受害者之后那几行:

```cpp
    Slot* const kVictim = slots_.get(victim);
    if (kVictim == nullptr) {
        return nullptr;
    }
    if (kVictim->live && kVictim->dirty && !spill(*kVictim)) {
        return nullptr;
    }
    const Lba       kPageStart{.value = page * kCachePageSectors};
    const BlockSpan kWhole{.value = kCachePageSectors};
    kVictim->page  = page;
    kVictim->live  = false;
    kVictim->dirty = false;
    if (!queue_.read(kPageStart, kWhole, kVictim->bytes)) {
        return nullptr;
    }
    kVictim->live = true;
    return kVictim;
```

咱们接着读:承重的是 spill 前头那个条件,受害者是活的、又是脏的,头一步就是回写,写回的另一个叫法。spill 要是被盘拒收,整场逐出当场作罢,受害者原地保留,脏位照旧,等下一趟重试。腾干净了才换键、清位,把新页整页请进门,读的长度是 8 个扇区,一次要齐。这样槽里的字始终有人答,一个字也丢不了。

**停一停,一笔回写被盘拒收,逐出当场作罢,您琢磨琢磨:出生时那句宁可不答的家法,到写回的时代,守住了哪一层?**

咱们把家法再念一遍,还是那句,不伺候假数,只是假数的脸换了两张。出生那会儿,假数是从没上过盘的字,所以拒收就标死,格子里不许有来历不明的字。写回时代多了一张:盘上落后的旧字。柜子的答法是两不丢,拒收的回写作罢,脏位留着,字还在槽里答得上来。命中的问照答,答的是槽里的新字。丢的只有一样,盘上新字的迟到,补法也只有一条路,重试。头注把同一层意思也写明了,拒收之后 `leaves the victim in place and still dirty`,受害者原地不动,还脏着,下一趟逐出或 flush 再来。

flush 是显式还的另一个时辰,原身拢共十行,咱们全本看:

```cpp
bool BlockCache::flush() {
    const proc::MutexGuard kGate(gate_);
    bool                   all = true;
    slots_.for_each([this, &all](unsigned long, Slot& slot) {
        if (slot.live && slot.dirty && !spill(slot)) {
            all = false;
        }
    });
    return all;
}
```

活着又脏的格,咱们挨个请它们 spill,拒收的不拦后面的,只把 all 拉成假,末了把成没成报回来。老实话搁在这儿:产品的代码里,到今天还没有谁真的调它,全仓检索,调用面零命中,走它的只有测试。门开着,是给往后同步与关机留的路,日常兜底的,是逐出的自动回写。

## Code: Cinux 实况:长凳五案,机内十案

五块屏,咱们从起机面板那两行起:

```text
[kern] ahci QEMU HARDDISK                           , 8192 sectors
[kern] ahci scratch write-readback matches
```

咱们认头一行:盘自报家门,QEMU HARDDISK,8192 扇区,正文《验明正身》那一卷立的报到。末一行是盘报到时的设备级自检:盘后头的 AHCI 控制器在 scratch 区写一笔、读回来、对字,写读回路当天验当天用。柜子就立在刚刚验过的回路上头。

长凳一件,您自己跑:

```bash
cmake -B build-dbg
cmake --build build-dbg --target test_block
build-dbg/test/test_block
```

屏尾咱们原样抄:

```text
[PASS] block: an out-of-range request carries the device refusal back
[PASS] block: interleaved regions keep their bytes apart through the queue

=== Results: 5 passed, 0 failed ===
```

存档只拍下这两案与合计行,开篇说过,余下三案的案名您跑出来自己认。就这两案,恰好把本章两处家法各演了一案:越界的请求,设备的拒收原样递回调用方,宁可不答。两家的字节隔着队列各走各的,谁也不污染谁。

机内的两件咱们接着跑,起机即跑,不用喂键。block 那件两案:

```text
[ktest] block stage: ahci up

[RUN] block: many tasks hammer the queue and each round trip holds
[PASS] block: many tasks hammer the queue and each round trip holds
[RUN] block: the queued drive answers single read and write
[PASS] block: the queued drive answers single read and write
```

头一案,许多任务轮着砸队列,咱们要它每一趟来回都立得住,验的是柜子底下的地基。末一案排队里的盘答一读一写,咱们看单笔的读写走通整条队。命令三步:

```bash
cmake -B build
cmake --build build --target boot_ktest_block
ctest --test-dir build/test -R kernel_test_block --output-on-failure
```

ctest 是构建带来的测试跑器,咱们拿 -R 按名挑要跑的那件。

ext2 那件八案是本章的正席,零号槽案的案发地,咱们整屏过:

```text
Running 8 case(s)...

[RUN] ext2: a made directory lists and empties
[PASS] ext2: a made directory lists and empties
[RUN] ext2: a written file reads back and unlinks
[PASS] ext2: a written file reads back and unlinks
[RUN] ext2: a fresh file takes bytes and grows
[PASS] ext2: a fresh file takes bytes and grows
[RUN] ext2: the root directory lists the baked entries
[PASS] ext2: the root directory lists the baked entries
[RUN] ext2: stat reports the baked file's own stamp
[PASS] ext2: stat reports the baked file's own stamp
[RUN] ext2: hello.txt reads back its baked bytes
[PASS] ext2: hello.txt reads back its baked bytes
[RUN] ext2: hello.txt is found with its baked size
[PASS] ext2: hello.txt is found with its baked size
[RUN] ext2: the baked volume mounts
[PASS] ext2: the baked volume mounts

=== Results: 8 passed, 0 failed ===
```

八案咱们挨案认。头一案建目录,列得开也空得掉。第二案写文件,读回对字,unlink 走完。第三案新文件吃字节、长个头。第四案根目录点烤好的条目。第五案 stat 报文件自家的时戳。第六、七案 hello.txt 读回烤好的字节、带着烤好的大小被找到。末一案烤好的卷挂载,当年八案全灭的那一关,今天它压轴。命令三步:

```bash
cmake -B build
cmake --build build --target boot_ktest_ext2
ctest --test-dir build/test -R kernel_test_ext2 --output-on-failure
```

咱们跑的时候起机走 KVM,TCG 的快档,借 CPU 里的虚拟化抬速,没开 /dev/kvm 的机器上,ctest 自己喊停,这是机内两件共同的注。

末一块屏是回指。缺页那一课的文件幕里,motd 那一路,烤在盘里的两行开机问候,open、file mmap、缺页那一拍 xmm0 到 xmm7 全数无恙、read、mapping agrees with read、close,案案 ok。您现在知道那一幕的幕后了:每一问的字节,都是从这座八格柜里递出去的,补页那一口 4096,一格正好一页。

## 对照:两份差事,两张门牌,两把锁

xv6 的缓存领两份工,咱们把书页 91 的原话整段摘来:

> “The buffer cache has two jobs: (1) synchronize access to disk blocks to ensure that only one copy of a block is in memory and that only one kernel thread at a time uses that copy; (2) cache popular blocks so that they don’t need to be re-read from the slow disk.”

一份同步,一份缓存。它家拿两把锁分:每块缓冲各配一把睡锁,管一个线程一块的独占,外带一把 bcache 总锁,管查表与收录这一段的原子。咱们把头一份工拆到两层:gate_ 押槽表,一路睡到队列回来。写序交给 ext2 写面的 write_gate_,柜子只认一句,护槽表,不管次序。同一份差事,两家的锁切在不同的缝上,缝切在哪,看各家的写路把什么当一段。

一份只有一个主人,咱们从书页 92 摘半句:

> “It is important that there is at most one cached buffer per disk sector, to ensure that readers see writes”

一个扇区至多一份缓冲,读的人才看得见写。咱们同一条家法,find_if 押在 gate_ 里查活槽,同一盘页号只许一格应名。不同的只在一份的大小:它家的一份是一个扇区,咱们的一份是一页 8 扇区。为什么咱们的份大,缺页路要的就是整页,长到页一节交过底。

章题借的是 Linux 的正名,咱们把两名当面对一对。Linux 的 page cache 按文件身份加页序认页,文件里第几页,缓存里就是第几页。咱们按盘页号认页,盘上第几页,柜子里第几格。饭是同一锅,内存替盘挡读。门牌是两套,一家认文件,一家认盘址。正文《懒到家》那一卷顺着用途叫过它页缓存,码上的正名是 BlockCache,3.3 路过不开柜,叫它的是块缓存,说的也是这座柜子,三个名字今天都认下,谁问也答得上来。它家怎么把两套门牌并成一套,咱们手里没有引得动的出处,不猜,话到这儿为止。

它家回收谁,咱们在书页 91 也摘到原话:

> “The buffer cache recycles the least recently used buffer for the new block.”

最久没被用到的让位,LRU。咱们下一位轮转,认笨不认巧,正文《写穿的缓存》的原话。类注把取舍写明了:`Replacement is a plain round robin`,后面还跟着 `a teaching kernel favours obvious over clever`,教学内核认显然,不认巧。LRU 是真家伙的手艺,记冷热的机关得另立一套,八格的柜子,轮转一眼看得穿。

## 这一章留下了什么

| 术语 | 一句话口径 |
|---|---|
| 槽表 | BlockCache 的八格,键是盘页号,每格盖一页 8 扇区 |
| 盘页号 | 扇区号除以 8:键的新名,与扇区号来回无损 |
| 写穿 | 老写路:patch 完整槽立即写盘,槽盘同一份字 |
| 脏位 | patch 只落槽的事实标记:盘落后于槽里的字 |
| 写回 | 脏页追上盘:逐出时先回写,flush 显式请,写回与回写,同一个动作两个叫法 |
| 逐出 | 新页进格前的腾位:受害者活且脏,头一步回写,拒收整场作罢 |
| 受害者 | 轮转指针指到的下一位:认笨不认巧,不挑不选 |
| flush | 显式请所有活脏格回写的面:拒收不拦后面,只把 all 拉假 |

这一章收进兜里的,咱们数四样:一座八格柜,一记除八,一枚脏位,末了是两把锁与一张脸。柜是家什,除八管门牌,脏位记欠下的写,锁分两层各押各的,一张脸是 flush 的,门开着,给同步和关机留着。

第 3 部到这儿收官,咱们把六课的家底盘一遍。3.1 立了地址空间,门牌、客人的天空、高半与低半的分家。3.2 立了分页与 TLB,四级下楼,速记的小本本,作废那一枪。3.3 立了缺页与按需分页,没映的页用到才补,讲了裁决与补页。3.4 立了物理内存,名册发页,堆窗开张。3.5 立了内核地图,五扇窗,三条换算。今天立的是这座柜子,内存替盘挡读,写押在脏位上,逐出与 flush 两个时辰还。柜前两把锁各押各的,次序到底归谁管,第 4 部头一章《竞态与临界区》接手。

## 练一练

咱们留三道题,一道动手,一道猜想,一道读屏。

**题一,动手。** 长凳与机内各跑一件,命令都在实况节里。您抄屏的时候认一认:长凳五案,哪一案把设备的拒收原样递回,哪一案隔着队列把两家的字节分开?机内八案,末一案案名念的是什么,它跟本章哪桩旧案对得上?

**题二,猜想。** 盘上 2 号扇区,您拿 page_of 算,报哪一号,offset_of 又报多少?再进一步,哪天手滑,把 kCachePageSectors 敲成了 4,superblock 搬没搬家,补文件页的饭量还够不够?

**题三,读屏。** 起机面板头一行,盘自报 8192 扇区,您把它折成盘页,多少页,零头有没有?末一行 scratch write-readback matches,写的什么,读回的什么,谁对的字,您说说。

### 参考答案

题一:长凳的屏尾您对上 `5 passed, 0 failed`。递拒收的是 `block: an out-of-range request carries the device refusal back`,分字节的是 `block: interleaved regions keep their bytes apart through the queue`。机内 `8 passed, 0 failed`,末一案 `ext2: the baked volume mounts`,烤好的卷挂载,零号槽案的案发地,当年八案全灭的头一关。

题二:您算下来,page_of 报 0,2 除以 8,商 0 余 2。offset_of 报 1024,余数 2 乘 512。superblock 就躺在零号页偏移 1024 的地方。改成 4,2 除以 4 商还是 0,superblock 没搬家,还在零号页。可一格只剩四个扇区,一口 4096 凑不齐,咱们要补的文件页当场断粮。键认得门,柜子盛不下饭,这就是为什么格子的尺寸跟页绑死。

题三:咱们把除法做出来,8192 除以 8,1024 页,整除,零头没有,盘的大小恰好页对齐。末一行是 AHCI 的自检:往盘上的 scratch 区写一笔字节,读回来,跟写下去的逐字对,对上了才报 matches。写读回路当天验当天用,柜子立在上面才立得稳。

## 延伸

- xv6 书第 10 章书页 91 与 92:缓存的两份工、一扇区一份、LRU 三处原话都在这两页,bread、bwrite、brelse(取块、写回、归还)三张脸也在这两页,您对原书核。
- 正文《认字:盘上的字怎么读、怎么写,真盘怎么当根》一卷:《写穿的缓存》是老形状的全本,写穿的道理、宁可不答的家法、认笨不认巧的原话,都在那一篇,出生一节哪里嫌短,您回原文补。
- 正文《懒到家:从登记册到补页》一卷:《缓存长到页》是两刀手术与零号槽案的全本,当年真起机破案的那一段,您翻得到。
- 正文《验明正身:找盘、对讲、认盘、排队》一卷:柜子底下那条队列的来历、scratch 写读回自检的来历,都在那一卷,值班室一间一间怎么盖起来的,您到那儿看全。
- 第 5 部的《块设备与驱动》《崩溃一致性与日志》两章在册待写:队列与引擎的正戏,断电之后盘上的字还作不作数,归那两章讲。
