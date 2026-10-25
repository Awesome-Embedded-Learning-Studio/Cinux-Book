---
title: "05 · 往盘上落笔"
description: "一把睡眠锁罩住全部写面:找位、置位、改计数、写 superblock,得一口气说完。文件长大走 ensure 一路:直接格现填,过 12 块立单间表,过 268 块立双间表,i_size 拿 max 只许长不许缩。位图收的是盘上的词,adopt 绑进来就用。目录添名字走两条路,死槽复用、活槽缩缝,塞不下就办不成,绝不给目录添新块。删名字只抹号,字还躺在块里,fsck 的路子留给以后的某天。"
chapter: 16
order: 5
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - storage
  - filesystem
  - ext2
---

# 往盘上落笔

读,出了错顶多是读不到。写,错一步,卷就伤了。所以写这一章,咱们走得最慢。

写面的四个门口,咱们立的是同一把锁:

```cpp
base::Result<FsNode> Ext2::make_file(std::string_view path) {
    const proc::MutexGuard kGate(write_gate_);
```

make_file、make_dir、unlink、write,进门头一行全是它,咱们一个个认过去。为什么是 Mutex,还是睡眠款?往盘上落一个字,动的是一连串地方:位图里找位、置位,组描述符和 superblock 里减计数,目录里插条目,inode 表里写正文。这一串得一口气说完——两个任务各写各的文件,要是插着来,位图里的同一块就发给了两个人。锁跨得久,自旋锁遭不住:临界区里要过值班队列的命令,一睡几十毫秒,持着自旋锁睡,上一卷的禁令把它列在头一条。睡眠锁就是为夜路生的,进程那一卷咱们跟它打过照面。

找块的手艺,咱们请老亲戚位图添了新差事:

```cpp
base::bit::Bitmap bits;
bits.adopt(words, kBitsPerBitmapBlock);
const unsigned long kBit = bits.find_clear_run(1);
```

位图块从盘上抄进栈上的词里,adopt 把这些词直接绑进来——一个字节不洗,盘上什么样,位图就什么样。find_clear_run 找一个空位,set 置上,咱们把改过的那个字节原样 patch 回盘,末了计数两处一起改:superblock 里的全卷总数,组描述符里的组内小数。删是同一套倒着走,clear、patch、加计数。盘坏没坏,两处计数就是哨兵,所以每一步都当场写回盘上,不攒。

正文的文件是怎么长大的,咱们落到码上一步一步看。write 进了门,头一件事是把 inode 抄成一份可改的副本,然后把字节流切成一个一个的块:

```cpp
unsigned long done = 0;
while (done < count) {
    const unsigned long kFileBlock  = (offset + done) / kVolumeBlockBytes;
    const unsigned long kBlockShift = (offset + done) % kVolumeBlockBytes;
    const unsigned long kChunk = base::math::Min(kVolumeBlockBytes - kBlockShift, count - done);

    const base::Result<unsigned long> kDisk = ensure_data_block(grown, kFileBlock);
    if (!kDisk.ok()) {
        return kDisk.error();
    }
    if (!cache_.patch(driver::Lba{.value = kDisk.value() * kBlockSectors}, kBlockShift, kChunk,
                      static_cast<const std::uint8_t*>(source) + done)) {
        return base::KernelError::kIOError;
    }
    done += kChunk;
}
```

循环的每一轮,ensure_data_block 拿文件的第几块换回盘上的块号,缺了就现场发一块,cache_.patch 把这一截字节写了进去。承重的换算在头三行:offset 加上已写的 done,除以块大小得出的是文件第几块,取余得出的,是块内的偏移,kChunk 拿 Min 把尾巴夹短了——末一截不满一块的,只写它的份。前一节铺过的 15 格分工,到这儿从读的地图变成了写的路线:MapFileBlock 算好了每一块归哪层管,直间的、单间的、双间的,咱们各备了一件 ensure 接着。

直接格的路数最短,咱们从它认起:

```cpp
base::Result<unsigned long> Ext2::ensure_direct_block(Inode& node, unsigned long slot) {
    if (node.i_block[slot] != 0) {
        return static_cast<unsigned long>(node.i_block[slot]);
    }
    base::Result<unsigned long> fresh = zero_fresh_block();
    if (!fresh.ok()) {
        return fresh;
    }
    node.i_block[slot] = static_cast<std::uint32_t>(fresh.value());
    node.i_blocks      = static_cast<std::uint32_t>(node.i_blocks + kBlockSectors);
    return fresh;
}
```

头一行就是全部的诀窍:格子里的数非零,块早就在了,奉还的就是原号。零的格子就是洞,咱们才去领。领块的手艺全在 zero_fresh_block 里:咱们在位图里找空位、置上位,再写一整块的零字下去,为的可不是洁癖:块里没写到的字节,读回来的得是零,而不能是上一位住户的残字。计数也是顺手的事:i_blocks 添上的是一块的扇区数,当场就结清了。

文件踩过了第 12 块进单间,新的麻烦来了:第 13 格原本指着的是零,谁去把间址表立起来?咱们把这桩活交给了 ensure_single_block:

```cpp
base::Result<unsigned long> Ext2::ensure_single_block(Inode& node, unsigned long outer) {
    if (node.i_block[kSingleSlot] == 0) {
        base::Result<unsigned long> table_block = zero_fresh_block();
        if (!table_block.ok()) {
            return table_block;
        }
        node.i_block[kSingleSlot] = static_cast<std::uint32_t>(table_block.value());
        node.i_blocks             = static_cast<std::uint32_t>(node.i_blocks + kBlockSectors);
    }
    std::uint32_t     table[kBlockWords] = {};
    const driver::Lba kTableLba{.value = node.i_block[kSingleSlot] * kBlockSectors};
    if (!read_block(node.i_block[kSingleSlot], table)) {
        return base::KernelError::kIOError;
    }
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
    return static_cast<unsigned long>(table[outer]);
}
```

查空查了两级,比直接格又深了一级:第 13 格是零的话,咱们就给间址表本身领一块,把号填进了第 13 格,再把整张的表读进内存。table[outer] 也是零的话,才给数据块本身领了一块,号也填进了表里。末尾的 patch 只写四个字节,改的是表里的一格,而不是整张表。间址表占的也是一个块,位图里有它的位,i_blocks 里也记了它的数,间址的架子不是白住的。

过了第 268 块进双间,还是原来的路数,只是又深了一层。前半截立的是外层表,咱们分开贴:

```cpp
base::Result<unsigned long> Ext2::ensure_double_block(Inode& node, unsigned long outer,
                                                      unsigned long inner) {
    if (node.i_block[kDoubleSlot] == 0) {
        base::Result<unsigned long> outer_table = zero_fresh_block();
        if (!outer_table.ok()) {
            return outer_table;
        }
        node.i_block[kDoubleSlot] = static_cast<std::uint32_t>(outer_table.value());
        node.i_blocks             = static_cast<std::uint32_t>(node.i_blocks + kBlockSectors);
    }
    std::uint32_t outer_words[kBlockWords] = {};
    if (!read_block(node.i_block[kDoubleSlot], outer_words)) {
        return base::KernelError::kIOError;
    }
    const driver::Lba kOuterLba{.value = node.i_block[kDoubleSlot] * kBlockSectors};
    if (outer_words[outer] == 0) {
        base::Result<unsigned long> inner_table = zero_fresh_block();
        if (!inner_table.ok()) {
            return inner_table;
        }
        outer_words[outer] = static_cast<std::uint32_t>(inner_table.value());
        if (!cache_.patch(kOuterLba, outer * sizeof(std::uint32_t), sizeof(std::uint32_t),
                          &outer_words[outer])) {
            return base::KernelError::kIOError;
        }
    }
```

第 14 格是空的,立的就是外层表,跟单间立表的模子一样。外层表第 outer 格也空着的话,咱们就再立一张内层表进去,patch 照样只写的是四个字节。后半截也接上了,用的还是同一个模子:

```cpp
    std::uint32_t     inner_words[kBlockWords] = {};
    const driver::Lba kInnerLba{.value = outer_words[outer] * kBlockSectors};
    if (!read_block(outer_words[outer], inner_words)) {
        return base::KernelError::kIOError;
    }
    if (inner_words[inner] == 0) {
        base::Result<unsigned long> fresh = zero_fresh_block();
        if (!fresh.ok()) {
            return fresh;
        }
        inner_words[inner] = static_cast<std::uint32_t>(fresh.value());
        node.i_blocks      = static_cast<std::uint32_t>(node.i_blocks + kBlockSectors);
        if (!cache_.patch(kInnerLba, inner * sizeof(std::uint32_t), sizeof(std::uint32_t),
                          &inner_words[inner])) {
            return base::KernelError::kIOError;
        }
    }
    return static_cast<unsigned long>(inner_words[inner]);
}
```

内层表第 inner 格也是空的,末一块数据这才发到了手上。三层查空是一层套一层的,跟读侧的 15 格地图是对偶的:读是顺着格子找块号的,写却是逆着格子把号填了回去,每层动的只是自己的一格,别人格子里的东西,咱们原样不碰。过了双间的地界,ensure_data_block 回的也是 kNotSupported,跟读侧的口径一致。

块都落了,i_size 谁来长?咱们看 write 的末尾几行:

```cpp
grown.i_size =
    static_cast<std::uint32_t>(std::max<unsigned long>(grown.i_size, offset + count));
grown.i_mtime = now_stamp();
grown.i_ctime = grown.i_mtime;
if (!write_inode(node.ino, grown)) {
    return base::KernelError::kIOError;
}
```

max 是这里的关键:offset 加 count 是这次写到的最远端,拿它跟旧的尺寸一比,尺寸只许往长了走——往回偏移写的时候,不该把文件写短了。两处时戳问的是同一秒,now_stamp 背后的钟打哪儿来,咱们留到问芯片的一节再交代。末了咱们请 write_inode 把这个副本整只写回 inode 表,i_size、i_block、i_blocks、时戳就都跟着落了地。

目录添名字,咱们有两条路。路一,死槽:块里躺着 inode 归零的条目,rec_len 又够长,新名字直接住进去,剩下的地再立一条空槽,剩不下一个头(8 字节),新条目就把整条死槽吞下。路二,缩缝:前一条活条目的 rec_len 缩到自家最小,让出缝来,新名字住缝里,新条目的 rec_len 把缝包圆。两条路都走不通呢?失败,就这么直接——给目录添条目,绝不给它添新块,塞不下就办不成。真 ext2 会给目录添块,咱们不添,这桩省略咱们当场认下,不往后藏。

删名字,走的却是另一路。中间的条目并缝:rec_len 并进前一家,名字的字节还在原地躺着。前面没有活条目的,抹号:inode 归零。您咂摸出味儿了吗——名字没了,字节还在。哪天卷没卸干净就断了电,fsck 那样的工具能顺着这些残字把东西认回来,这门手艺,咱们留给以后的某天。删除的时戳也有讲究:有钟,盖真戳。没钟的机器,回退到卷的生日 s_mkfs_time 上。不然一个零戳杵在 inode 里,fsck 看了要皱眉。

> 如实的闲话:目录删得删不得,咱们头一件事看屋里还有没有别人,. 和 .. 不算数。删整座目录,它的块和号都还给位图,位图上不留影子——没有一位还记着它。建文件建到一半盘不收,刚领的号当场退回去,位图这一层的进出,当场就结清。条目那一层若留了半截活,悬空条目的下落,归的就是前头说的 fsck 那条路。

落笔的每一步都过缓存、当场下盘。凭什么说槽上的字就靠得住?这就是下一节的主题,咱们到那儿把写穿两个字说全:盘拒收的那一拍,槽也跟着死,读的人拿不到假数。
