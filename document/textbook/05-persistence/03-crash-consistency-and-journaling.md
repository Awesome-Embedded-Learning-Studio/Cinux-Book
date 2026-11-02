---
title: 03 · 崩溃一致性与日志:断在半路的笔,盘上的流水簿,没做的那一半
---

# 03 · 崩溃一致性与日志:断在半路的笔,盘上的流水簿,没做的那一半

第 5 部的第三章,咱们接着一笔旧债进门。5.2 收卷时自家交了底,原话咱们抄来:“断电之后盘上的字还作不作数,fsck 与日志的正课,5.3。”那一章的停一停也立过同一问:poweroff 敲下去、机器断电之前,谁被刷了,谁没有?那一章翻码对出的答案是控制台的字刷了、盘上的字没人管。断电之前补的那一道,咱们留到讲 sync 的段落看,进门来把债主的名单点齐。

欠下这笔债的也不止 5.2 一家。咱们再往前数,2.5 立脸的时候数过 xv6 的七层,自家缺的凑成一摞,原话咱们请回来:“这样缺的凑成四层,disk、buffer cache、logging、inode,咱们一层都没有。”如今 inode 5.2 认全了,buffer cache 3.6 开了柜,disk 的事归 5.4,logging 名下的那一笔一直没人应。今天咱们把它应下来:断电砸在写了一半的文件系统上,重启之后它怎么还能自圆其说,这门课的行话叫崩溃一致性,咱们当面讲。

这一章的活,咱们数下来三样。头一样是祸的样子:一次要在盘上写好几处的活,要是断在了半路上,盘上各处留什么,咱们拿自家建文件的三拍起手,拿 xv6 的截断例子压阵。再一样是 xv6 的答法:把要写的字记进盘上一本流水簿,记全了才誊到正地,断电之后咱们凭那本簿说话,书页 92 到 94 的书证咱们当面请。末一样是咱们的答卷与诚实单:ext2 的写路径在 5.2 之后又搓了一轮,批次住进了内存,sync 的席也凑成了三席,关机补上了断电之前的那一刷,可咱们的盘上仍然没有流水簿,没做的三样,咱们挨样交底。

量法还是原来的三条,打头的是码。实况锚是 r35_gui_fin 的克隆,行号咱们一处一处按它数。5.2 教过的内容,引的仍是 5.2 章里的行号,那一章按 r34 的克隆数,两套行号烧的是两口锅,咱们不混着舀。

屏的事,咱们如实交代:本章没有新起的屏。机器上的断电重挂咱们没有演过,机内连 fsck 的影子都没有,硬造出来的就是臆造。开篇问框的现场,从正文《认字》那一卷的验收篇请回一面旧屏,当年 4 MiB 教学卷的串口原文,咱们逐字抄录,出处也给您标在旁边。

书的事,咱们还是翻 xv6 的第 10 章,新翻的是书页 92、93、94,书页 89 取了两句,95 取了一句,引文咱们逐字核过。2.5 收延伸的时候说过:“您拿着正文里引的原句去核,借得准不准,您说了算。”本章的引文,您也这么核。

> **5.2 的停一停问过一回:poweroff 敲下去、机器断电之前,谁被刷了,谁没有?当年的答案,翻码对过:控制台的字刷了,盘上的字没人管,sync 更是没人叫的。那一章写完了,后面搓 ext2 写路径的那一程,这一拍又改了:断电之前补了一道 sync,补的那道不成,咱们连电都不许断。您接着想:sync 把脏页都送到盘上,断电就真的不丢字了吗?一次建文件要在盘上写好几处,盘写到一半断了电,谁还记得这次写到了第几拍?**
>
> 谁都不记得了。sync 只答应一件事:已经写下的字都送到盘上。它不答应另一件事:一趟活里的几笔,要么都落到盘上,要么一笔都不落。写到一半断了电,盘上有的地方记了新数,有的地方还揣着旧数,断电之后拿什么当凭据、谁来当裁判,就是崩溃一致性整门课要答的问。xv6 的凭据是一本记在盘上的流水簿,咱们的凭据眼下只在内存里,边界各在哪?这一章咱们挨着数。

那一问的现场,咱们把旧屏请回来。正文《认字》那一卷验收篇的末尾,当年 4 MiB 教学卷的串口原文,咱们照原样一字未动地转录:

```text
cinux> poweroff
cutting power through acpi
m
```

打头的是 poweroff,跟上的是 cutting power through acpi,ACPI 的断电路径走完,模拟器就退场了。屏末孤零零的那个 m,是断电之后串口上的残响,当年的转写如实,咱们留在原地。当年的这一拍里,盘上的字没人管,码里的 sync 一句都没有。今日的码补上了,咱们到讲 sync 的段落看它补成了什么样。看完再回头看开篇的问框,它照旧问着同一个问题:字送到了盘上,断电之后这些字作不作数?

## 断在半路的笔,盘上留什么

咱们拿自家最短的一次活起手:建一个新文件。seat_new_file 在盘上动的有三处,ext2_write.cpp 的 :146 位图里领一个号,:172 把刚领的号写成一只崭新的 inode,:176 添的是目录块里的一条名字。三拍是挨着落的,您把断电的剪刀往中间一落,听听各处报什么。

断在了第二拍之后,位图说这个号有人了,inode 表里也坐着一只活的 inode,目录块里却没有它的名字。它对客人是隐身的,位图也不肯把这个号再发给别的文件,一只占了号却没有名字的 inode,咱们叫它孤儿。断在第三拍之后呢?三拍在码上都成了,可写下的字兴许还躺在缓存里没下盘,盘上到底落了几拍,连作证的人都没有。

删除也是多拍的活。目录块里的并缝抹号是一拍,而位图还块还号又是一拍,断在了中间,名字没了、号还占着,几笔数就对不齐了。正文《认字》那一卷当年就咂摸出了这层味儿,原话咱们请回来:“删名字,走的却是另一路。中间的条目并缝:rec_len 并进前一家,名字的字节还在原地躺着。前面没有活条目的,抹号:inode 归零。您咂摸出味儿了吗——名字没了,字节还在。哪天卷没卸干净就断了电,fsck 那样的工具能顺着这些残字把东西认回来,这门手艺,咱们留给以后的某天……”名字没了字节还在,说的就是多拍活断在中间留下的中间态。当年立下的期票,这一章来数它为什么需要,手艺本身咱们还没做出来。

多拍的活还不止建与删。您往一只空文件里写头几个字节,底下也是多拍的活:内容块要领,inode 的长度和块指针要改,笔笔都碰盘上的字。断电的剪刀落在哪一拍,哪里就停了手,停出来的,又是一份没人作证的半成品。

多拍的活断在半路,盘上留的是一份没人认领的半成品,一半是新的,一半是旧的。怎么让半成品永远不被当成品,xv6 交了卷,咱们看完它的,再对照自己的。

碰上这层祸的不止咱们一家。xv6 的例子是文件截断,咱们在书页 92 把原文请来:

- “ … The problem arises because many file-system operations involve multiple writes to the disk, and a crash after a subset of the writes may leave the on-disk file system in an inconsistent state. For example, suppose a crash occurs during file truncation (setting the length of a file to zero and freeing its content blocks). Depending on the order of the disk writes, the crash may either leave an inode with a reference to a content block that is marked free, or it may leave an allocated but unreferenced content block.”

您咂摸这段:截断一只文件的时候,改的是长度,还的是内容块,合起来也是一趟多拍的活。断在了中间,而两头都可能是坏局,人家的判词跟着下来:

- “The latter is relatively benign, but an inode that refers to a freed block is likely to cause serious problems after a reboot.”

您看判词:孤儿块算的是良性,inode 指着自由块算恶性:重启之后内核把那块发给别的文件,两个文件指到了同一块上,写进去的就串了门。

更早的压阵话在书页 89 的 challenges 段,咱们把原文请来:

- “The file system must support crash recovery. That is, if a crash (e.g., power failure) occurs, the file system must still work correctly after a restart. The risk is that a crash might interrupt a sequence of updates and leave inconsistent on-disk data structures (e.g., a block that is both used in a file and marked free).”

一句话把整门课立了起来:崩溃之后,文件系统还得是能用的文件系统。这样既在文件里又标着自由的块,咱们 5.2 教过的认卷之门拦不住,那一验看的是形状:魔数、块数、组表齐不齐。几笔数对不对得上?门上是没有那一验的。

## xv6 的流水簿:记全了,才誊正本

xv6 的答法在书页 93 翻到 94,咱们把原文请来:

- “Xv6 solves the problem of crashes during file-system operations with a simple form of logging. An xv6 system call does not directly write the on-disk file system data structures. Instead, it places a description of all the disk writes it wishes to make in a log on the disk. …”

系统调用不直接动正地,把想写的字都记到盘上一本流水簿里,它的行话是 log,咱们叫它日志。

记全之后有一枚明写的凭据,咱们接着请:

- “Once the system call has logged all of its writes, it writes a special commit record to the disk indicating that the log contains a complete operation.”

commit 的中文是提交,记全了才叫提交。记全之后才把字誊到正地,誊完了把簿子擦掉,一趟活就算落了地,咱们把次序记下:记全、提交、誊正、擦簿。

这套做法里有两个词咱们头一回用,咱们给它们落户口。事务的行话是 transaction,出生句在书页 89 的七层句里,咱们请原文:

- “The logging layer allows higher layers to wrap updates to several blocks in a transaction, and ensures that the blocks are updated atomically in the face of crashes (i.e., all of them are updated or none).”

几笔写包成了一包,断电面前咱们要的要么全成,要么全不成。原子的行话是 atomically,说的就是括号里那半句:全更新或全不更新,中间是没有半截的。

簿子长什么样,咱们到书页 93 再请:“The log resides at a known fixed location, specified in the superblock. It consists of a header block followed by a sequence of updated block copies…”簿子住的是 superblock 指定的固定地界,打头的是一块头,后头跟着要誊的块。头块里存着扇区号的清单和块数,提交标记就是头块里的那枚计数,咱们接着把书里的话请来:

- `The count in the header block on disk is either zero, indicating that there is no transaction in the log, or nonzero, indicating that the log contains a complete committed transaction with the indicated number of logged blocks. Xv6 writes the header block when a transaction commits, but not before, and sets the count to zero after copying the logged blocks to the file system. Thus a crash midway through a transaction will result in a count of zero in the log’s header block; a crash after a commit will result in a non-zero count.`

count 为零的时候,咱们知道簿子是空的。count 非零的时候,里头是一趟记全了的活。

断电的剪刀有两处落点,您挨处咂摸。断在了记簿中间,该写的头块没写,count 记的是零,恢复的人看一眼就收工了,簿子是不算数的。断在了提交之后,非零的 count 摆在那儿,恢复的人照簿重誊。坏局两头都堵上了。恢复的章程人家也写全了,咱们请原文:

- “If the log is marked as containing a complete operation, then the recovery code copies the writes to where they belong in the on-disk file system. If the log is not marked as containing a complete operation, the recovery code ignores the log.”

咱们把章程念顺:标了记全的,恢复的人才誊,没标的就当没看见,末了把簿擦掉,恢复也就完毕了。

凭什么这样就算自圆其说,咱们把书里那句总判请出来:

- “In either case, the log makes operations atomic with respect to crashes: after recovery, either all of the operation’s writes appear on the disk, or none of them appear.”

断的地方有两种,出来的下场也有两种,可落到盘上的结果只有两种干净的:要么全在,要么全不在。咱们到这儿就看到原子这个词兑现了。

书页 94 讲的是四道工序。头一道 write_log 管的是抄簿,咱们略过原文,挑承重的句子请:

- ` … write_head() (4668) writes the header block to disk: this is the commit point, and a crash after the write will result in recovery replaying the transaction’s writes from the log. install_trans (4619) reads each block from the log and writes it to the proper place in the file system. Finally end_op writes the log header with a count of zero; this has to happen before the next transaction starts writing logged blocks, so that a crash doesn’t result in recovery using one transaction’s header with the subsequent transaction’s logged blocks.`

第二道的 write_head 写的是头。提交点的行话是 commit point,就落在写头的那一拍:写头之前断了电,所以这趟活等于没来过。写头之后断了电,恢复的人照簿一笔一笔再誊,这一步的行话是 replay,咱们叫重放。第三道 install_trans 誊正地,末一道把头清了零,而清零必须赶在下一趟记簿之前,不然一旦断了电,新簿的字配着旧簿的头,恢复的人要拿错。

恢复的入口人家也交待了,咱们请原文:

- “recover_from_log (4682) is called from initlog (4606), which is called from fsinit(4891) during boot before the first user process runs (2666).”

码面上写的是直白话:开机之后、头一个进程起跑之前,跑在前头的是恢复。更早一拍还是更晚一拍的讲究,书里的交代点到为止,咱们看到的就这么多。

簿子上还有两处省料的手艺,咱们挨着看。一处是护住要写的字,咱们把原文请来:

- `log_write (4790) acts as a proxy for bwrite. It records the block’s sector number in memory, reserving it a slot in the log on disk, and pins the buffer in the block cache to prevent the block cache from evicting it. The block must stay in the cache until committed: until then, the cached copy is the only record of the modification; it cannot be written to its place on disk until after commit; and other reads in the same transaction must see the modifications.`

要写的字在提交之前只活在缓存里,所以得有人护着,免得被逐出提前送下了盘。另一处的手艺是并笔,咱们接着请:

- “log_write notices when a block is written multiple times during a single transaction, and allocates that block the same slot in the log. This optimization is often called absorption.”

同一块写多回的活,簿里占的也只是一个位。并笔的手艺叫 absorption,咱们翻成吸收:把几笔并成了一笔,省的是簿子的地界,也省了一趟写。

几趟活并成了一趟提交,人家也起了名目,咱们请原文:

- “The idea of committing several transactions together is known as group commit. Group commit reduces the number of disk operations because it amortizes the fixed cost of a commit over multiple operations.”

组提交的行话是 group commit,摊的是一趟提交的固定开销。簿子咱们量过:是定长的,地界有限,一趟活要写的块多了装不下,限制也写在了书里:

- “No single system call can be allowed to write more distinct blocks than there is space in the log.”

所以按余量放活进门,大的 write 拆小,unlink 要碰的除了位图还有一枚 inode,好在实践里的位图拢共一张,书里都替咱们算好了。

末了一句钩子在书页 95,咱们请原文:

- “As with much of the code described in the remainder of this chapter, balloc and bfree must be called inside a transaction.”

说的是 balloc、bfree,5.2 教过的位图领块还块,人家把它们圈进了事务。最家常的活恰恰是多拍活,咱们自家的码里,它们有没有人罩着?下一节咱们看。

## 咱们的两阶段,住进了内存

5.2 开章的时候交过底:“ext2 的写路径在更往后的站里还要再搓一轮,咱们以克隆的正身说话,再搓的那一层归后面讲崩溃一致性的章。”今天正身换到了 r35,重搓出来的新件叫 Mutation,咱们管它叫私有批次,这词往后就是术语了。它来干什么,头注的原话写明了,咱们抄在引号里,ext2.hpp 的 :12:“a whole metadata sequence — find a free bit, set it, discount the counters, write the superblock — must land as one breath or two files can claim the same block”,整段的元数据必须一口气落,断了气口,能领走同一块的文件就有两个。把着这一口气的是互斥锁,5.2 教过的,今日的码又给它添了一层新的护法。

批次的正身,咱们整件抄来,ext2_mutation.cpp 的 :11:

```cpp
Ext2::Mutation::Mutation(Ext2& volume)
    : volume_(volume), batch_(volume.cache_), before_(new State) {
    base::safety::Check(volume.mutation_ == nullptr, "nested volume mutation");
    before_->superblock = volume.sb_;
    base::CopyBytes(before_->groups, volume.groups_, sizeof(before_->groups));
    volume.mutation_ = this;
}
Ext2::Mutation::~Mutation() {
    if (!committed_) {
        volume_.sb_ = before_->superblock;
        base::CopyBytes(volume_.groups_, before_->groups, sizeof(before_->groups));
    }
    volume_.mutation_ = nullptr;
}
bool Ext2::Mutation::commit() {
    committed_ = batch_.commit();
    return committed_;
}
```

咱们挨件看。构造的这一拍:把 superblock 与整张组表拍下快照,收进的是 before_,嵌套的批次当场断言拦下,同一时刻一只卷只许有一只活着的批次。析构的这一拍,没 commit 过的,就把快照写了回去,sb_ 与 groups_ 的内存值原样回滚。commit 的这一拍,交给手底下的 BlockCache::Batch 发布。发布不动盘上的东西,batch 的头注一句写死,block_cache.hpp 的 :68:“A failed preparation or commit leaves the cache's logical bytes unchanged”,备页或发布失败的时候,缓存的逻辑字节一个不动。您对一眼 xv6:人家的两阶段住在盘上,咱们的两阶段住进了内存,准备阶段的改动全落私有页,发布阶段的整页进缓存、置脏,您数下来,两阶段是都没碰过盘的。

批次活着的时候,读写怎么摸到暂存的字?靠一只路由器,咱们抄来,ext2_mutation.cpp 的 :33:

```cpp
bool Ext2::patch_storage(driver::Lba first, unsigned long offset, unsigned long bytes,
                         const void* source) {
    return mutation_ == nullptr ? cache_.patch(first, offset, bytes, source)
                                : mutation_->patch(first, offset, bytes, source);
}
```

没有批次的时候,patch 直落的是缓存,5.2 教过的老路。批次活着的时候,patch 落进的就是私有页。read_storage 是同款的分叉,ext2_mutation.cpp 的 :29,读的也是暂存的字。一趟活里自己写下的字,下一拍的自己也读得见:不用等发布。咱们跟 xv6 那句“other reads in the same transaction must see the modifications”对上了,想的是同一件事,只是人家的字写在书里,咱们的字写在码里。

私有页怎么长,咱们看 block_cache.cpp 的 Batch::patch,:228 起:头一回摸到某一页的时候,整页从缓存里读进的是一份私有拷贝,改动只落在私有的这一头,再摸到同一页的时候,回来的还是同一份。写十回的同一页,留着的私有份也只有一份,xv6 要靠吸收特意并的槽,咱们这儿天生一份。发布的两轮,咱们把正身抄来,block_cache.cpp 的 :260:

```cpp
bool BlockCache::Batch::commit() {
    if (failed_) {
        return false;
    }
    const proc::MutexGuard kGate(cache_.gate_);
    for (auto* page = pages_.head(); page != nullptr; page = pages_.after(*page)) {
        auto* slot = cache_.claim_slot(page->number);
        if (slot == nullptr) {
            unpin();
            failed_ = true;
            return false;
        }
        slot->pinned = true;
        page->slot   = static_cast<unsigned long>(slot - cache_.slots_.get());
    }
    for (auto* page = pages_.head(); page != nullptr; page = pages_.after(*page)) {
        auto& slot = cache_.slots_[page->slot];
        base::CopyBytes(slot.bytes, page->bytes.get(), cache_.slot_bytes_);
        slot.dirty = true;
    }
    unpin();
    return true;
}
```

头一轮的活是挨页 claim_slot,把要改的槽占住、置上 pinned,次序在头注里写明了,block_cache.hpp 的 :67:“Private page copies published together after all destination pages are pinned”,全部占住了才一起发布。为什么非得占住,码是没有细说的,咱们看得见的是:被占住的那些槽,两轮之间是挪不了窝的。

第二轮把整页拷进槽:置上的是脏位。末了统一 unpin,放掉的是标记。中间的任何一步不成,咱们照样放掉标记,整批标了失败,一笔也不发:发给盘的命令是零条。您对一眼 xv6 省料的两处手艺:人家护住缓存里的字,别被逐出抢在提交前下了盘。咱们占住发布中的槽,别在两轮之间被挪了窝。守的都是中间态,别在半路上走了样。

批次的覆盖面,咱们按码如实交底,交底的里面没有一个夸字。全仓起 Mutation 的活只有三处,unlink 在 ext2_write.cpp 的 :277,rename 的位置在 :431,到 sync 才还块还号的那一步另起一只,在 ext2_mutation.cpp 的 :69。rename 与 chmod 是那一程添的新脸,咱们一句带过,课是不开的。建文件、建目录、写内容的活,都没有批次:seat_new_file 的三拍,保障走的是失败当场退号,咱们把那两拍抄来,ext2_write.cpp 的 :172:

```cpp
    if (!write_inode(kIno.value(), kFresh)) {
        free_inode(kIno.value());
        return base::KernelError::kIOError;
    }
    if (!add_dir_entry(parent_inode, parent_ino, kIno.value(), leaf)) {
        free_inode(kIno.value());
        return base::KernelError::kIOError;
    }
```

write_inode 不成的时候,刚领的号当场退回去,添名再不成了,退的还是同一个号,这是手工的补偿。《认字》那一卷当年交底的原话:“建文件建到一半盘不收,刚领的号当场退回去,位图这一层的进出,当场就结清。”今日的补偿还是同款。靠的是互斥锁加补偿,活人抢不成的是同一块,运行时的失败也有退路。可您看清它管的是谁:活人与运行时错误。断电它是不管的,内存里的批次跟着内存一起没,commit 过的脏页还要等 flush 才下盘,下盘又一页一页地写。断电的窗口一路开到盘,咱们留到最后一节逐拍数。窗口的另一头,是谁把字往盘上送、送到了没有,咱们看 sync。

## sync 的脸:三席一号一扇门

flush 的门,3.6 开柜的时候就教过:显式还的时辰,逐出是自动的兜底。当年的实情,《懒到家》那一卷的《缓存长到页》记了原话:“产品的代码里,到今天还没有谁真的调它,门倒是开在这儿,是给往后同步和关机的路预备的”。更早的写穿脾气,正文《认字》那一卷的《写穿的缓存》也记了:“槽里的字和盘上的字是同一份,读的人永远信得过槽。”这一章兑现给咱们看:门后如今有三处活调用,写懒了下来之后,把字拉回正地的事,归了 sync 一家。

sync 的脸,号本上的样子看得最清楚,咱们抄来,syscall.hpp 的 :40:

```cpp
    kFsync            = 74,   ///< Flush a file and its filesystem metadata.
    kFdatasync        = 75,   ///< Flush file data.
    kSync             = 162,  ///< Flush pending filesystem data and metadata.
```

生在前头的是 74 fsync 与 75 fdatasync,162 sync 是搓写路径那一程新添的席,注释自家写明白了,照的是 Linux x86_64 的号本,咱们跟它同一本。这一程的正文卷未及记载,咱们以码为证。

三席怎么合走同一条路,咱们挨个翻。fsync 与 fdatasync 共用一处调用:2.3 教过的分发表里,两席接的是同一个 HandleFsync,syscall_dispatch.cpp 的 :121 与 :123。sync 自己占一处:接的是 HandleSync,同一张表的 :122。HandleFsync 认完了座、验完了类型,末一句走的是 FileWorld::sync,sys_fsync.cpp 的 :31。HandleSync 干的就一句话,落在 sys_sync.cpp 的 :8 到 :10。再往里的一层,file_world.cpp 的 :60,`volume_ == nullptr || volume_->sync()`,全世界的活递给卷。

卷上的那一把,咱们抄来,ext2.cpp 的 :66:

```cpp
bool Ext2::sync() {
    const proc::MutexGuard kGate(write_gate_);
    return mounted_ && reap_pending() && cache_.flush();
}
```

锁内的活,咱们挨着念:看的是挂着没,接着的活是收割,末了的是刷。收割排在刷的前头,它是什么,咱们马上看。flush 走的是一把到底,block_cache.cpp 的 :100 起,活着的脏页挨个 spill,整页是同步写的,:109 的末一行把 queue_.flush() 接在 all 后头,这就是队列的栅栏(行话 barrier),内嵌在末尾的一行里,内里的事归 5.4,咱们只念名。

头注也换了口。flush 的头注今日写的是 block_cache.hpp 的 :175:“Writes every dirty page and waits for the device flush”,写完了每一页,等的还是设备。sync 自己的头注在 ext2.hpp 的 :104,咱们请原文:

- “Flushes a complete metadata sequence and waits for the device.”

刷完一整段的元数据,等的也是设备。还有半句实话咱们记下:注释里 fdatasync 只许刷数据,码上它跟 fsync 走的是同一句 FileWorld::sync,许愿分了家,兑现是不分的,这段的差欠就摆在这儿,咱们不替码圆场。

收割的行话是 reap,这词往后咱们当术语使。5.2 讲 unlink 的时候,那时的码还是急脾气,links 一归零就当场把块和号还了。搓写路径那一程改了章程,drop_file_link 立了两道闸,ext2_write.cpp 的 :1066 与 :1073:links 归了零,还得是没人握着的,握的人还在,块和号就都不还了,只把 links 记了下来。握着它的是谁?开着它的户口。您在壳里 open 一只文件再 unlink,名字没了,字节您接着读,读的是没了名字的字。

什么时候真还?咱们看 sync 那一把里的 reap_pending,ext2_mutation.cpp 的 :86 起,扫的是 hold 链,握着的票数归了零的挨个收割:记下 dtime,还了块,还了号,再自己起一只包着这些的 Mutation,commit 了事。动手的时辰延迟到 sync,防的还是活人,握着文件的人不该看着它散架。断电呢?hold 链在内存里,链没了,盘上的 links 数还是上一次写下去的数,这笔也记进了“诚实对读”的清单。

门后的第三处活调用,藏的是关机的那一段,咱们把那几行抄来,sys_reboot.cpp 的 :28:

```cpp
    if (!fs::FileWorld::self().sync()) {
        return -kEio;
    }
    console::Flush();
    driver::acpi::PowerOff(kFadt);
```

验完了 euid、对完了魔数、找到了 FADT,断电路径之前 sync 是要走的。sync 不成的时候,回的就是 -kEio,电是不断的,拉闸的命令收回去。成了,刷字的才轮到控制台,落闸的是 PowerOff。5.2 那一回咱们翻的还是旧身:console::Flush 一句独守,盘上的字没人管。那一章写完了,这一拍补上了,console::Flush 也挪了家:5.2 那口锅的行号里它在 :27,本章这口锅的行号里在 :31。开篇问框的前一半,今天有了着落。

**咱们停一停,您来对时辰。** poweroff 敲下去、还没断电的时候,谁被刷了,谁没有?您拿方才看过的内容对一遍再回来。咱们对:面板的字刷了,刷的是 console::Flush。盘上的脏页也排空了,FileWorld::sync 的那一把,收割的事在前,flush 的事在后,次序的码写得明白。可一趟活的几笔有没有齐齐整整躺在盘上,作证的人还是没有,盘上没有 count 那样的凭据。5.2 立下的那一问,今天兑了一半,另一半下一节咱们交底。

## 诚实对读:没做的三样,断电的窗口

摊开对读的时候,咱们拿一张表收全章:

| 对照的哪一样 | xv6 | 咱们(r35) |
| --- | --- | --- |
| 两阶段住在哪 | 盘上的 log 区,superblock 指定 | 内存里的私有页与快照 |
| 提交的标记 | header 里的 count,提交那一拍才写 | 无,盘上没有凭据 |
| 断电之后 | count 零则忽略,非零则重放 | 残局留在盘上,没人裁 |
| 恢复的码 | recover_from_log,开机头一个进程之前跑 | 无,mount 只验形状 |
| 同一块写多回 | absorption,簿里并一个位 | 私有页天然只有一份 |
| 几趟活并一趟 | group commit | 无 |
| 空间的上限 | 簿子定长,按余量放活进门 | 私有页住堆,上限是内存 |

表里咱们短的,集中在提交的标记、断电之后、恢复的码三行,下头咱们挨样交底。

头一样咱们短的是 journal。盘上的 superblock 尾部确实躺着 journal 的字段,头注自家写明了,ext2_layout.hpp 的 :79:“carries the journal and algorithm blocks this kernel never reads”,尾部填着的是这本内核从不读的 journal 与算法块。有字段而不读,咱们就说到这儿,码里没写预留的意思,咱们不替它添。

第二样咱们短的是 fsck。走到今天的正文卷还没演过 fsck,ext2.hpp 的 :167 还挂着注释:“the space waits for the fsck story of a later station”,空地等着 fsck 的故事。咱们把话说清:这一章讲的是为什么需要它,工具本身眼下还是没生的。

第三样咱们短的是恢复的码。咱们看 mount:它只验形状。认卷之门缺的那一验,前头念书页 89 的时候点过,补的人到今天也没有,mount 信的从来只是形状。

断电的窗口,咱们逐拍数一遍,您跟着对。拿披了批次的 unlink 走:改动在私有页那一拍断电,正地未动,盘还是旧的,活丢了,盘是没坏的,这算批次白送的一道保险。commit 完了的那一拍断电,字在缓存的脏页里,脏页多半跟着内存一起没了,可逐出是随时可能把脏页提前送下盘的,咱们谁也说不准。

flush 走起来的那一拍断电,咱们看盘:一页一页同步写,断在了中间,前头的页新、后头的页旧,盘上就是半新半旧的。没披批次的活暴露得更早,patch 直落缓存的那一拍起,半截的元数据就可能被逐出送下盘。断电的哪一拍都可能留下残局,而盘上没有一枚 count 那样的字,能说清这残局算不算数的。

宿主侧的测试倒是替咱们演过这残局。测试把 rename 的每一笔写挨个断掉,断在哪一笔都试:断了之后 flush,另起一套干净的缓存与队列,把同一卷冷冷地挂回来,挂得上的场次里,名字底下至少还有一只指得回的文件。收场干净的那几场,落下的残局还得是 e2fsck 修得动的,这一场才算过得了关。写坏在半路的活也有当场验,rename 中途断了写,还开着的卷照样答得出两只文件各自的原字。可这全是宿主上的家什,裁判是测试请来的 e2fsck,机内的码里没有裁判,盘上的凭据依旧一格都没有。

收割欠下的也开在这窗口里。握着的人还在,块和号就压着不还了,断电赶在了收割前头,这笔就永远没了着落:盘上留着一只占了地、没了名的 inode,links 数停在上一次写下去的数,又是一份没人裁的残局。sync 段里咱们点过这笔,清单里记的是它。

更早一版实现的参考页给过一句口径,咱们标明这是哪一代的话,咱们引一句:“无 journaling、无 fsck。”同一页还把当年的 sync 说成写后刷盘、防掉电损坏的朴素版本,这版本是算不得日志的,咱们转述,不引它的原字。那是上一代的口径,今日的码边界没挪:写后刷盘,咱们有了,防掉电损坏的那一半,咱们还没有。

《认字》那一卷立下的期票,“这门手艺,咱们留给以后的某天”,这一章给的是为什么需要它。2.5 那笔四层的债,今天收到的是末一笔:logging,问过了,xv6 的答案请来了,自家的边界也认下了。disk 的那半边,块设备的队列与引擎,门是 5.4 才开的。5.2 收卷的那句“fsck 与日志的正课,5.3”,日志今天讲完了,fsck 的故事,咱们接着等。

## 本章留下了什么

崩溃一致性的这一门,咱们今天走了三程:祸的样子,盘上的流水簿,内存里的批次与诚实单。新词咱们入册:

| 术语 | 行话 | 一句话口径 |
| --- | --- | --- |
| 崩溃一致性 | crash recovery | 断电砸在写了一半的文件系统上,重启之后它还能自圆其说 |
| 孤儿 | orphan | 占了号却没有名字的 inode,客人看不见,位图也不放号 |
| 事务 | transaction | 几笔写包成一包,断电面前要么全成,要么全不成 |
| 原子 | atomically | 全更新或全不更新,中间没有半截 |
| 日志 | log | 盘上的流水簿,要写的字记全了才誊到正地 |
| 提交点 | commit point | xv6 写 header 的那一拍,此前断电当无事,此后断电照簿重放 |
| 重放 | replay | 恢复的人照着记全的簿,把几笔再誊一遍 |
| 吸收 | absorption | 同一块写多回,簿里只占一个位 |
| 组提交 | group commit | 几趟活并一趟提交,摊薄固定开销 |
| 私有批次 | Mutation | 一段元数据活的全套改动落私有页,发布才进缓存,盘两阶段都不进 |
| 收割 | reap | 有人握着的 inode,links 归零也不还,sync 时才清尾 |
| 栅栏 | barrier | 队列 flush 的一道闸,内里归 5.4 |

没讲的咱们如实交底:fsck 与恢复工具的正课,这一章只立了为什么,工具本身还是没生的。rename 与 chmod 报了名,课是没开的。软链接那些 5.2 交底的悬案,咱们没接。块设备的队列与引擎落在 5.4 那一章,下一章咱们下到块设备那一层,看 queue 的内里,栅栏的那一道,到时候咱们当面讲。

## 练一练

咱们留三道题:推残局的一道,对书证的一道,对时辰的一道。

**题一:咱们推残局。** seat_new_file 的三拍,咱们挨着数:146 领号、:172 写 inode、:176 添名。断电断在了第二拍之后、第三拍之前,盘上剩什么?位图、inode 表、目录块,您挨处说。再问一句:断在第三拍之后呢?

**题二:咱们对书证。** xv6 的 count,为什么只在提交那一拍写,早一步都不写?您拿断在记簿中间与断在提交之后两头各推一遍。再想一层:count 要是写早了,会出什么冤案?

**题三:咱们对时辰。** reboot 里的 sync 不成,回的是 -kEio,电是不断的。有人说这更安全了,您对着“诚实对读”的清单说:安全在哪儿,没安全在哪儿?

### 参考答案

题一:咱们推。断在了第二拍之后,位图里的号已经置上,inode 表里坐着一只崭新的 inode,目录块里没有它的名字。咱们看见的是一只孤儿,它对客人是隐身的,位图也不肯把这个号再发给别的文件。断在了第三拍之后,三拍在码上都成了,可写下的字还躺在缓存里,盘上落了几拍,要看逐出与 flush 把哪些页送下去了,作证的人还是没有。

题二:咱们两头各推一遍。断在了记簿中间,count 记的是零,恢复的人忽略簿子,盘上的样子像这趟活没来过。断在了提交之后,count 记的是非零,恢复的人照簿一笔不缺地重放。count 写早了,断在记簿中间的时候,头块喊的就是有整趟活,恢复的人照半本簿重放,誊出去的恰是半截活,比空着的簿子还冤。

题三:咱们对。安全的一头在:sync 不成不断电,客人拿得到的是 -EIO,知道出了错,活人还有处置的余地。没安全的一头:半截写的祸一点没少。sync 成了也只是把字送到盘上,一趟活的几笔对不对得齐,它是管不着的,断电的窗口照旧开着。回 -EIO 的意思是不拉闸,不是上了保险。

## 延伸

- man 2 fsync 与 man 2 sync:三席的正名,注释里分了家的承诺,手册里怎么分,Linux 的原页写着。
- xv6 书第 10 章,书页 92 到 94:logging 三节全本,本章请过的段落您再自读一遍,四道工序对着书里的码走一遍更好玩。
- 正文《认字》那一卷:开篇的旧屏出自验收篇,fsck 的期票立在《往盘上落笔》,写穿的脾气记在《写穿的缓存》,三处旧事都在那一卷。
- 《懒到家》那一卷的《缓存长到页》:flush 的门是那一章开的,显式还的时辰与逐出的兜底,当年的原话都在。
