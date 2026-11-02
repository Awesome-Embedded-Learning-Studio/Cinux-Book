---
title: 06 · Page Cache 与写回
---

# 06 · Page Cache 与写回:八格柜,一记除八,一枚脏位

缺页那一课的末一程,是从一座柜子上走完的:文件页要用到的那一拍,向盘要的是一整页 4096 字节,而字节从柜子里递了出来。柜子当时咱们没摊开,今天把它摊开了。摊开之前咱们把地界划清:2.5 立下一张脸面,管的是客人与文件世界见面,脸面朝的是客人那一头。本章的柜子立在另一头:脸面背后,是正文里的 ext2,2.5 写下的那句期票如今兑了,它进门坐了根,兼的是后端。柜子的底下,是替盘值班的队列,一块盘一次只受一个命令的差遣,替所有任务排班的那间值班室。咱们这座柜子夹在两头之间,谁的客都不见,伺候的只有字节。

1.7 里立起的是一座塔,越往下的一层,慢了一截,大了一圈,内存给盘当上了一层。塔立了图,家什是一件也没安的,咱们今天往内存与盘中间那级楼梯上搬一座柜子:八格,每格装的是 4KiB,盘上的页按盘页号对号入格。读:柜子替咱们挡。写:柜子敢扣在咱们手里。凭什么敢扣,扣了以后谁看着,还的时辰定在哪两处,咱们一条一条过。

这一章的活,咱们数下来三样。头一样认的是出生:老缓存生下来什么形状,凭什么要长个头。再一样认的是手术:槽怎么从 1KiB 长到 4KiB,键怎么从扇区号换成盘页号,一记除八怎么来回无损,零号槽的案子怎么发、怎么破。末一样认的是写回:写穿翻成了脏页,省的是什么,欠的是什么,flush 的脸为谁开。

量法还是老的三条。码:咱们的主角有两件,一件是 block_cache.hpp 的 177 行,头注把本章的门牌全立好了,另一件是 block_cache.cpp 的 152 行,活动的部件都在里头,咱们逐段过。尺寸与格数在 block_config.hpp 里立成了三枚常量:一扇区 512 字节,一格盖的是 8 个扇区,柜子拢共摆了 8 格。屏:拍下的是五块,实况锚挂的是 r19_lazy_alloc_page,切版的老话 1.7 念过,不重印:起机面板的两行,长凳的五案,机内的两案,后面又跟了八案,末一块回指缺页那一课的文件幕。书:咱们把 xv6 翻到第 10 章,书页 91 与 92 的两页逐字核过,原话整段摘进的是对照节,您对着书核。一句披露搁在前头:长凳五案的存档只拍下两案的案名与合计行,余下三案的案名,您跑长凳那一件的时候自己认。

> **一份 177 行的文件,凭什么敢替盘挡读,还敢把写扣在内存里?**
>
> 凭头注自己报的家底。block_cache.hpp 的头注开篇就点了差事:文件系统问来问去,问的是同几块,位图、inode 表、superblock 都在问的单子上,如今又添了缺页路上的整页文件页,而每一问过队列都要花一条盘命令,所以立柜,`answers those repeats from memory`,重复的问,内存里就答了。往下立了两根梁。一根梁管的是换算:`A caller still speaks in sectors`,调用方的口风不改,报的还是扇区号,换算成页的差事柜子替他担,交回的指针是 `a pointer into the slot's middle`,指进槽的中间。另一根梁管的是写:`Writes land in the slot alone and mark it dirty`,写落的地方只有槽,记下的是一枚脏位,盘的追赶要等逐出或 flush。代价头注也报了:`a live slot may run ahead of the disk`,活着的槽,可能跑在盘的前头。敢与不敢、跑了算谁的,咱们正文里一处一处对。

## 出生:八格小槽,写穿当家

这座柜子生下来时的个头,跟如今的不一样。老形状是正文《写穿的缓存》那一篇立下的,咱们把老家底数清:八槽,每槽 1KiB 的身量,盖的是两个扇区,键记的是扇区号。两张脸从那时候就有了。view:问的是一扇区,命中了就把指针交出去。patch:把调用方的字节落进槽里。写路的老家法叫写穿:patch 的收尾,把整槽的字原样写到盘上,这一拍写完了,槽与盘存的是同一份字,换了谁来读也读不出两样。

拒收的家法也在那时候立下。盘要是拒收的话,槽把死标挂上了,咱们宁可不答,假数是不伺候的,重新请进门的事,留到下一回再问的时候。腾位的家法同样朴素:命中不上,轮转的指针指到谁,谁当的就是受害者,顶班让格的时候,咱们也不挑不选,冷热是一概不记的。那一篇给它留了一句原话,教学内核的路数认笨不认巧,咱们署名借来,这层咱们照实认。

柜子里的锁有两把,咱们看它们各守哪层。gate_ 一把押的是槽表,头注的原话 `The lock holds across those queue commands on purpose`,过队列的命令一路睡过去,槽表不在谁的脚下挪动。次序那一层不归柜子:ext2 的写面自己揣着 write_gate_,一整段元数据的写序由它自家押着。柜子的说法就一句:护槽表,不管的是次序。装柜的位置在 file_world.cpp 的 :39,`cache_ = new driver::BlockCache(*queue_, driver::kBlockCacheSlots)`,文件世界当的是柜主,格数是从 block_config 端进来的。

## 长到页:一记除八,来回无损

逼它长个头的是缺页路。要补的是一页文件,一口咬的是 4096 字节,老槽 1KiB 的饭量,补一页要串的是四个槽,还得指望的是四槽在盘上恰好挨着,不是它干得了的活。手术分的是两刀,头一刀动的是槽,末一刀动的是写路,咱们一刀一刀来。

头一刀改的是构造。格数还是原来的 8,变的是每格的饭量与键:槽字节固定成 8 乘 512,键从扇区号换成了盘页号。构造的头几行咱们看着:

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

槽表里插进来的每一格,键记的是零,出厂的活标记一律是 false。`.live = false` 的改动拢共就一处,可它是零号槽案的修法本体,咱们把案子摆完,您再回头看它。page_of 与 offset_of 两件在 block_cache.cpp 的 :29 到 :35:

```cpp
unsigned long page_of(Lba sector) {
    return sector.value / kCachePageSectors;
}

unsigned long offset_of(Lba sector) {
    return (sector.value % kCachePageSectors) * kSectorBytes;
}
```

换算咱们挨着看:扇区号除以 8,得的是页号。余数乘 512 得的是页内偏移。除得尽的也罢,除不尽的也罢,两个词说的都是同一块盘,类注给一来一回下了断语:`two names for one platter fact, so the translation is lossless in both directions`,一个盘上事实的两个名字,来回都是无损的。调用方那一头一个字不用改:他报的还是扇区号,view 落在 :62 的 `*view_out = kSlot->bytes + offset_of(first)`,交回的指针指进槽的中间,偏移是他自己拿着用的。

就是头一刀闯了祸,咱们把案子完整摆出来。症状:ext2 的机内测试,八案全数死在了挂载那一关。挂载是文件世界的头一道关,superblock 读不回来的话,后头就全塌了,可读路径的代码一行没动。咱们真起机,拿调试器一步一步走了一遍,走到该发盘命令的那一拍,看住设备的缓冲区:纹丝不动,盘命令的影子都没有。一个读请求没发命令就拿到了数据,来处剩的只有一处:缓存说它命中。可这盘是冷启动的,缓存里不可能存有任何真实的数据。

咱们顺着命中条件回去查,病根的位置在构造里,还是欠着的旧债。老形状的键是扇区号,零号扇区当的是引导块,ext2 是从不读它的,那个出厂自带活标记、键记零的槽,一辈子是没人问的,病埋下了不发作。槽长到了页,零号页盖的是 0 到 7 号扇区。盘上谁住的是哪个扇区,是正文《认字》那一卷《卷摊开来的样子》那一篇摊开的版式,superblock 住的是 2 号扇区,偏偏就落在零号页里。挂载的头一件事问的是 superblock,问的正是零号页,那个从没见过盘的槽举手说命中,交出去的是一槽从未上过盘的全零。八案全灭了,病根就压在出厂的那一行。

修法压的也是构造,改的拢共就一处,`.live = false`。槽出厂的时候一律不带活标记,见过盘的槽,才有活的资格。命中认的凭据只能是见过盘,出厂的状态,一个事实都是不许自带的。咱们后来复盘,要不是当时真起机看了一眼盘命令,纸面上想破了头也想不到。

**咱们停一停,命中认的凭据为什么只能是见过盘?给出厂的活标记另想两条堵法的话,您推一推各自漏在哪?**

堵法其实看着有不少,咱们挨个想。一条是绕的路子:挂载别问零号页,superblock 走单独伺候的路。可问零号页的不止挂载,后头还有排着队的调用方,症状按住的是一处,别处是照旧要冒的。一条是填的路子:出厂给每格发一圈真读,把盘上的字请进门。听着挺干净的,可它把凭据换成了出厂即见过,冷盘的头一问就得排队,而且格是轮转的,出厂填的字转头就让位,白请了一趟。两条都是在外围打转的,唯独抹掉活标记的改法,把活与见过盘绑在了一起,凭据是这时候才立得住的。出生一节的家法,咱们宁可不答,假数是不伺候的,在这儿守住的是同一条界:没有事实,是不许应名的。

案发地是挂载头一问。今日的机内 ext2 八案是齐绿的,当年全灭的那一关,今天照样把卷挂上了,屏咱们实况里过。

## 写回:一枚脏位,两个时辰

末一刀动的是写路。写穿的老实,出生一节数过了:写完那一拍,槽与盘留的是同一份字,谁也不欠谁的。可盘命令是最贵的动作,能省一趟的话就省一趟,如今轮到写路学着省了,咱们把 patch 的收尾从整槽下盘改成两行:

```cpp
    base::CopyBytes(kSlot->bytes + kInPage + offset, source, bytes);
    kSlot->dirty = true;
```

CopyBytes 把字节落进了槽,dirty 置成了真,往下 return:盘命令是一趟也不发的。patch 开头另有一道岗:改动要是跨出本页的边界,当场就拒绝了,头注写的是 `cross out of its page`,一格只管自家页里的事。这一改立下的唯一新事实,就是一枚脏位:槽里的字换过了,盘上还是旧的。Slot 里那枚位的注释只写了半句:`Whether the platter runs behind these bytes`,盘是落后于这些字节的,笔者的原注,咱们原样摆着。

省的一面,头注报的是 `a burst of patches into one page costs a single write`,连着往同一页补字的话,几趟 patch 攒了下来,还的时候并成一趟写。正文《懒到家》的那一卷当年在这段路上记过一笔数:块缓存聚到页粒度,ext2 的读聚合了两成五,一趟盘命令捎回的字节多了,排队的就少了。这说的是当年。咱们这一遍,功能是实弹验过的,收益的计数没另做,当年的那笔数,咱们不冒领。

欠的一面头注也报了,就是章首那框里的那句跑在盘前头。隔着缓存看盘的人,跑测试的也罢,看崩溃现场的也罢,看到的可能是旧字,要新的,显式 flush 一遍是免不了的。当年三处隔着缓存直接窥盘的测试,那一批全补上了显式的 flush,清偿完了再看盘。还的时辰定在两处,一处落的是逐出,一处落的是 flush,咱们挨个看。

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

咱们接着读:承重的是 spill 前头那个条件,受害者是活的、又是脏的,头一步做的就是回写,写回的另一个叫法。spill 要是被盘拒收的话,整场逐出当场作罢了,受害者原地保留,脏位是照旧的,等的是下一趟重试。腾干净了才换键、清位,把新的一页整页请进门,读的长度是 8 个扇区:一次要齐。这样槽里的字始终有人答,一个字也丢不了。

**咱们停一停,一笔回写被盘拒收的话,逐出是当场作罢的,您琢磨琢磨:出生时那句宁可不答的家法,到写回的时代,守住了哪一层?**

咱们把家法再念一遍,还是开头的那句,不伺候的还是假数,只是假数的脸换了两张。出生的那会儿,假数是从没上过盘的字,所以拒收就标死,格子里不许有来历不明的字。写回时代多了一张:盘上落后的旧字。柜子的答法是两不丢,拒收的回写作罢,脏位是留着的,字在槽里是答得上来的。命中的问照答,答的是槽里的新字。丢的只有一样,盘上新字的迟到,补的路也只有一条:重试。头注把同一层意思也写明了,拒收了之后 `leaves the victim in place and still dirty`,受害者原地不动,还是脏的,下一趟的逐出或 flush 再来。

flush 是显式还的另一个时辰,拢共十行的原身,咱们全本看:

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

活着又脏的格,咱们挨个请它们 spill,拒收的不拦后面的,只把 all 拉成了假,末了把成没成报回来。老实话搁在这儿:产品的代码里,到今天还没有谁真的调它,咱们全仓检索过,调用面是零命中的,走它的只有测试。门是开着的,是给往后同步与关机留的路,日常兜底的,是逐出的自动回写。

## Code: Cinux 实况:长凳五案,机内十案

屏咱们拍下五块,从起机面板的那两行起:

```text
[kern] ahci QEMU HARDDISK                           , 8192 sectors
[kern] ahci scratch write-readback matches
```

咱们认头一行:盘自报的家门是 QEMU HARDDISK,报的数是 8192 扇区,正文《验明正身》那一卷立的报到。末一行是盘报到时的设备级自检:盘后头的 AHCI 控制器在 scratch 区写一笔、读回来、对字,写读的回路当天验当天用。柜子就立在刚刚验过的回路上头。

长凳的一件,您自己跑:

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

存档只拍下了这两案与合计行,开篇说过:余下三案的案名您跑出来自己认。这两案恰好把本章两处家法各演了一案:越界的请求,设备的拒收原样递回调用方,咱们宁可不答。两家的字节隔着队列各走各的,谁也不污染谁的。

机内的两件咱们接着跑:起机即跑,喂键也免了。block 那件的两案:

```text
[ktest] block stage: ahci up

[RUN] block: many tasks hammer the queue and each round trip holds
[PASS] block: many tasks hammer the queue and each round trip holds
[RUN] block: the queued drive answers single read and write
[PASS] block: the queued drive answers single read and write
```

头一案演的是许多任务轮着砸队列,咱们要它每一趟来回都立得住,验的是柜子底下的地基。末一案排队里的盘答一读一写,咱们看单笔的读写走通整条队。命令走的是三步:

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

八案咱们挨案认。头一案建的是目录,验的就是列得开也空得掉。第二案写的是文件:读回对上字,unlink 也走完了。第三案的新文件吃字节、长个头。第四案根目录点烤好的条目。第五案 stat 报文件自家的时戳。第六、七案 hello.txt 读回烤好的字节、带着烤好的大小被找到。末一案烤好的卷挂载,当年八案全灭的那一关,今天压轴的是它。命令走的是三步:

```bash
cmake -B build
cmake --build build --target boot_ktest_ext2
ctest --test-dir build/test -R kernel_test_ext2 --output-on-failure
```

咱们跑的时候起机走 KVM,TCG 的快档,借 CPU 里的虚拟化抬速,没开 /dev/kvm 的机器上,喊停的是 ctest 自己,这是机内两件共同的注。

末一块屏管的是回指。缺页那一课的文件幕里,motd 的那一路,烤在盘里的两行开机问候,`open`、`file mmap`、缺页的那一拍 `xmm0` 到 `xmm7` 全数无恙、`read`、`mapping agrees with read`、`close`,案案是 ok 的。您现在知道那一幕的幕后了:每一问的字节,都是从这座八格柜里递出去的,补页的那一口 4096,一格装的正好是一页。

## 对照:两份差事,两张门牌,两把锁

xv6 的缓存领两份工,咱们把书页 91 的原话整段摘来:

- `The buffer cache has two jobs: (1) synchronize access to disk blocks to ensure that only one copy of a block is in memory and that only one kernel thread at a time uses that copy; (2) cache popular blocks so that they don’t need to be re-read from the slow disk.`

一份管的是同步,一份管的是缓存。它家拿的是两把锁来分:每块缓冲各配一把睡锁,管一个线程一块的独占,还外带一把 bcache 的总锁,管查表与收录这一段的原子。咱们把头一份工拆到两层:gate_ 押槽表,一路睡到队列回来的时候。写序交给 ext2 写面的 write_gate_,柜子认的只有一句:护槽表,次序是不管的。干的是同一份差事,两家的锁切在不同的缝上,切缝的位置在哪,看各家的写路把什么当一段。

一份是只有一个主人的,咱们从书页 92 摘半句:

- “It is important that there is at most one cached buffer per disk sector, to ensure that readers see writes”

一个扇区名下的缓冲至多一份,读的人才看得见写。咱们同一条家法,find_if 押在 gate_ 里查的是活槽:同一盘页号只许一格应名。不同的只在一份的大小:它家的一份是一个扇区,咱们的一份是一页 8 扇区。为什么咱们的份大,缺页路要的就是整页,长到页的小节里交过底。

章题借的是 Linux 的正名,咱们把两名当面对一对。Linux 的 page cache 按文件身份加页序认页,文件里的第几页,缓存里记的就是第几页。咱们按盘页号认页,盘上的第几页,对上柜子里的第几格。饭是同一锅的,内存替盘挡的是读。门牌挂的是两套,一家认的是文件,一家认的是盘址。正文《懒到家》的那一卷顺着用途叫过它页缓存,码上的正名是 BlockCache,3.3 路过的时候不开柜,叫它的是块缓存,说的也是这座柜子,三个名字今天咱们都认下,换了谁问也答得上来。它家怎么把两套门牌并成一套,咱们手里没有引得动的出处,咱们不猜,话到这儿也就为止了。

它家回收的是谁,咱们在书页 91 也摘到原话:

- “The buffer cache recycles the least recently used buffer for the new block.”

最久没被用到的让位:LRU。咱们下一位轮转,认笨不认巧的路数,正文《写穿的缓存》的原话。类注把取舍写明了:`Replacement is a plain round robin`,后头还跟着的是 `a teaching kernel favours obvious over clever`,教学内核认的是显然,不认的是巧。LRU 是真家伙的手艺,记冷热的机关得另立一套,八格的柜子,轮转是一眼看得穿的。

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

这一章收进兜里的,咱们数四样:一座八格柜,一记除八的换算,一枚管欠的脏位,末了是两把锁与一张脸。柜当的是家什,除八管的是门牌,脏位记欠下的写,锁分两层各押各的,一张脸是 flush 的,门是开着的,留给的是同步和关机。

第 3 部到这儿就收官了,咱们把六课的家底盘一遍。3.1 立了地址空间,门牌、客人的天空、高半与低半的分家。3.2 立了分页与 TLB,走的是四级下楼,速记的小本本,作废的那一枪。3.3 立了缺页与按需分页,没映的页用到才补,讲了裁决与补页。3.4 立了物理内存,名册发了页,堆窗开了张。3.5 立了内核地图,开了五扇窗,立了三条换算。今天立的是这座柜子,内存替盘把读挡了下来,写押在了脏位上,还的时辰定在逐出与 flush。柜前两把锁各押各的,次序归谁管的一桩,第 4 部的头一章《竞态与临界区》接手。

## 练一练

咱们留三道题,动手的一道,猜想的一道,读屏的一道。

**题一咱们动手。**长凳与机内您各跑一件,命令都收进了实况节。您抄屏的时候认一认:长凳五案,哪一案把设备的拒收原样递回,哪一案隔着队列把两家的字节分开?机内八案,末一案案名念的是什么,它跟本章哪桩旧案对得上?

**题二咱们猜一猜。**盘上的 2 号扇区,您拿 page_of 算,报的是哪一号,offset_of 又报多少?再进一步,哪天您手滑,把 kCachePageSectors 敲成了 4,superblock 的家搬没搬,补文件页的饭量还够不够?

**题三咱们读屏。**起机面板的头一行,盘自报的是 8192 扇区,您把它折成盘页:多少页,零头有没有?末一行 scratch write-readback matches,写的什么,读回的什么,谁对的字,您说说。

### 参考答案

题一:长凳的屏尾您对上 `5 passed, 0 failed`。递拒收的是 `block: an out-of-range request carries the device refusal back`,分字节的是 `block: interleaved regions keep their bytes apart through the queue`。机内跑出的是 `8 passed, 0 failed`,末一案念的是 `ext2: the baked volume mounts`,烤好的卷挂载,零号槽案的案发地,当年八案全灭的头一关。

题二:您算下来,page_of 报的是 0,2 除以 8 的商是 0,余的是 2。offset_of 报的是 1024,算的是余数 2 乘 512。superblock 就躺在零号页偏移 1024 的地方。改成 4 的话,2 除以 4 的商还是 0,superblock 的家没搬,住的还是零号页。可一格剩下的只有四个扇区,一口 4096 就凑不齐了,咱们要补的文件页当场断粮。认门的是键,盛不下饭的是柜子,这就是为什么格子的尺寸跟页绑死。

题三:咱们把除法做出来,8192 除以 8 的结果是 1024 页的整除,零头是一点没有的,盘的大小恰好页对齐。末一行是 AHCI 的自检:往盘上的 scratch 区写一笔字节,读回来的一笔,跟写下去的逐字对,对上了才报 matches。写读的回路当天验当天用,柜子是立在上面才立得稳的。

## 延伸

- xv6 书第 10 章书页 91 与 92:缓存的两份工、一扇区一份、LRU 三处原话都在这两页,bread、bwrite、brelse(取块、写回、归还)三张脸也在这两页,您对原书核。
- 正文《认字:盘上的字怎么读、怎么写,真盘怎么当根》一卷:《写穿的缓存》是老形状的全本,写穿的道理、宁可不答的家法、认笨不认巧的原话,都在那一篇,出生一节哪里嫌短,您回原文补。
- 正文《懒到家:从登记册到补页》一卷:《缓存长到页》是两刀手术与零号槽案的全本,当年真起机破案的那一段,您翻得到。
- 正文《验明正身:找盘、对讲、认盘、排队》一卷:柜子底下那条队列的来历、scratch 写读回自检的来历,都在那一卷,值班室一间一间怎么盖起来的,您到那儿看全。
- 第 5 部的《块设备与驱动》《崩溃一致性与日志》两章在册待写:队列与引擎的正戏,断电之后盘上的字还作不作数,归那两章讲。
