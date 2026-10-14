---
title: "06 · 写穿的缓存"
description: "ext2 的活压在同几块上,块缓存 8 槽替队列挡下重复的命令。patch 进槽再整槽下盘,写完这一拍槽和盘是同一份,盘拒收槽标死,宁可不答,不伺候假数。门面手艺第三间:这一卷一件转发都不擦,ext2 直接住进链里,编译期一颗 static_assert 盖章。"
chapter: 16
order: 6
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - storage
  - filesystem
  - block-device
---

# 写穿的缓存

ext2 干活,十有八九压在同几块上:superblock 一块,两张位图,inode 表,还有目录块。上一卷立下的章程,一块盘一次一道命令,想用盘,排队。咱们要是每回读 inode 都排一趟队,几个任务轮着读文件,队列就尽伺候重复的请求了。块缓存垫在 ext2 和队列中间,8 个槽,每槽 1 KiB:

```cpp
bool BlockCache::view(Lba first, const uint8_t** view_out) {
    const proc::MutexGuard kGate(gate_);
    const Slot* const      kSlot = claim_slot(first);
    if (kSlot == nullptr) {
        return false;
    }
    *view_out = kSlot->bytes;
    return true;
}
```

view 命中,槽里的字节原样交出去。未中,挑一个受害者腾格、填数。挑谁?下一位——朴素轮转,一根指针转圈。真家伙的 LRU 是一门课,教学内核认笨不认巧,这层咱们照实认。

写的那一半,才是这一章的主角,咱们看:

```cpp
base::CopyBytes(kSlot->bytes + offset, source, bytes);
const BlockSpan kWhole{.value = static_cast<uint16_t>(slot_sectors_)};
if (!queue_.write(kSlot->first, kWhole, kSlot->bytes)) {
    kSlot->live = false;
    return false;
}
```

patch 进来的字节落进槽,咱们再把整槽原样写回盘。写穿的意思,到这一拍就兑现了:槽里的字和盘上的字是同一份,读的人永远信得过槽。盘要是拒收呢?槽标死——live 翻假,这块地址的回答作废,宁可从此不答,不伺候假数。哪天有别的请求再点这块,重新填,填成了再开口。

缓存自己也有一把锁,也是睡眠款,而且睡眠就发生在锁里:未中得睡在队列上等盘,锁要是中途撒手,名册就乱了。那重叠的 patch,次序谁来管?咱们听听缓存的回答,它说得很干脆:我护名册,不管次序,管次序的是上面——ext2 的写面攥着上一章那把 write_gate_,重叠的修改根本排不进同一个瞬间。两把锁守的是两层东西:缓存的 gate_ 护名册,写面的 write_gate_ 护次序,名字都躺在代码里,谁是谁一翻便知。

末了,咱们说说门面。擦除那门手艺,名字那一卷开在文件系统头上,当年九个转发。这一卷柜台添了 stat,脸面长到十个方法,擦的还是同一副模子。上一卷开在盘头上,五个。这一卷,一间新的都不开——ext2 生下来手里就攥着缓存的地址,门面认它,凭的只是编译期的一颗章:

```cpp
static_assert(cinux::fs::FileSystem<Ext2>, "the ext2 backend satisfies the FileSystem face");
```

十个方法少一个、签名歪一个,构建当场停,这门厉害上一卷的块脸上咱们领教过。链子是现成的:ext2 找缓存要块,缓存找队列排号,队列牵着的,是上一卷认下的那块盘。客人还是只报路径——门面那一卷立的章程,后端姓甚名谁,客人从头到尾不知道。如今链子深了一层,这套章程一个字没改。

链子通了,读和写都在上面跑。可您在 shell 里 cd 进个子目录再 pwd,链子帮不上这个忙,帮忙的是另一件跟人走的家当——下一节,名字的相对论。
