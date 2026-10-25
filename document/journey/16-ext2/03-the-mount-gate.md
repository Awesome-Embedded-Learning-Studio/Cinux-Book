---
title: "03 · 认卷之门"
description: "mount 六道检查:魔数 0xEF53、形状(只认 1 KiB 块、不认不兼容特性、inode 尺寸下限加整除)、计数彼此咬合、组数至多 32 封顶 256 MiB、二次挂载拒载、根 inode 必须是目录。组描述符表一口气读进内存常驻,坏卷进门只有拒载一条路。"
chapter: 16
order: 3
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

# 认卷之门

版式对了的卷,还不一定认得。mount 要做的,是把卷头那几样东西读进来,一道一道验——这一幕您熟:上一卷盘认证件,六道准入一道一道数。这一卷轮到卷交证件,咱们摆开几道关。

头两道,咱们照着代码念:

```cpp
if (sb_.s_magic != kMagic) {
    return base::KernelError::kIOError;
}
if (sb_.s_log_block_size != 0 || sb_.s_feature_incompat != 0 ||
    sb_.s_inode_size < sizeof(Inode) || kVolumeBlockBytes % sb_.s_inode_size != 0) {
    return base::KernelError::kNotSupported;
}
```

头一道,魔数:第 56 字节上得读出 `0xEF53`,不对就是不对,回 `kIOError`。第二道全是咱们伺候不了的形状。块大小只认 1024,log 值非零就拒,人家大的卷,咱们没伺候过,这一版只教 1 KiB 一种。incompat 位一个不认,这类位在 ext2 的世界里写得很直白:不认识就别挂,咱们老实听劝。inode 的尺寸得装得下咱们认得的 128 字节,还得把块整除——半个 inode 跨两块,读回来就得拼,拼出来的东西咱们不担保。

第三道,计数彼此咬合:块的总数得比第一个数据块多——1 KiB 块的卷,第一个数据块是 1 号,前面的地界留给引导块,这是 ext2 给 1 KiB 卷排定的座次。一组的块数不能超过一张位图的容量,8192 块的天花板,来路咱们在版式一节数过了。每组的 inode 数不能是零,全卷的 inode 装下来,需要的组数不能超过卷里实有的组数。实有的组数是怎么算的?咱们把代码请出来:

```cpp
group_count_ =
    base::math::Ceil(sb_.s_blocks_count - sb_.s_first_data_block, sb_.s_blocks_per_group);
if (group_count_ > kMaxGroups) {
    return base::KernelError::kNotSupported;
}
if (base::math::Ceil(sb_.s_inodes_count, sb_.s_inodes_per_group) > group_count_) {
    return base::KernelError::kIOError;
}
```

咱们拿块总数刨去头一块引导块的地界,再除以每组的块数,拿 Ceil 向上取整——余下的零头块凑不成整一组,也得算成单独的一组,这就是卷里实有的组数。判罚也分了两档:组数过了 32,回的是 kNotSupported,那是咱们没学过的大卷,咱们伺候不了。inode 需要的组数比实有的多,回的是 kIOError,那是卷自己数不圆了,盘面跟版式对不上的坏卷。第四道,组数封顶 32:组描述符表进内存,住的是一张 32 座的定长表。您顺手就能算出这一版的天花板,32 组乘 8192 块乘 1024 字节,256 MiB——再大的卷,得连组描述符表一起扩。

组描述符表这一读,咱们停一步。整张表一口气读进 RAM,常驻:

```cpp
if (!read_block(sb_.s_first_data_block + 1, groups_)) {
    return base::KernelError::kIOError;
}
```

往后找 inode、查位图,哪一组在哪儿、表在哪儿,咱们全看内存里住下的名册。它是全卷的门牌册,不常驻可不行——每一步都回盘上问一遍,那日子没法过。

末了两道,咱们过一遍。第二回 mount,回 `kInvalidArgument`,一卷不挂两回——挂过了还挂,那是代码写岔了,当场顶回来。根 inode 是 2 号,读出来得真是个目录,不是就拒。2 号住根,这是 ext2 立下的老定例,卷要是自己都不守,后头的路全是假的。六道全过,mount 才把挂载的旗翻成真。往后卷上任何业务,进门前都查这道旗,没挂的卷,问什么都回 `kIOError`,不装懂。

坏卷在这一座门前怎么个遭遇,咱们也数得出:该报错的报错,该拒载的拒载,没有一处是硬着头皮往下读的。上一卷盘认证件,咱们认的是材料对不上的盘。这一卷认卷,咱们认的是版式对不上的盘。

门进了,根也捏在咱们手里了。可根也是个 inode,跟 hello.txt 的 14 号一样,躺在盘上某个块的某个字节里——哪一块、哪个字节,咱们拿算术翻出来。
