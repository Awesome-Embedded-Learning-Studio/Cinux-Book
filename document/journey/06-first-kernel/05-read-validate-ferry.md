---
title: 05 · 四段流水线
description: "读一扇区、验九判、127 扇区一窗循环摆渡、头一窗的魔数回执;溢出安全的四则运算,和 stage2 收尾的次序——问完内存、配完屏、装完内核,才说再见。"
chapter: 6
order: 5
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - loader
  - stage2
  - real-mode
---

# 四段流水线

流水线的头一段:不是搬,是读头——此刻 boot 手里一个数的影子都没有,鞋盒报了什么咱们还没见过,又从哪儿验起?读头的活住在 `boot/load/loader.cpp`:

```cpp
ImageHeader ReadHeader() {
    asm volatile("lgdt kGdtr" : : : "memory");
    g_window_dap.count = 1;
    read_window();
    RunFerry(kHeaderScratch, static_cast<unsigned>(kFerryWindow), 512);
    return *base::PtrAt<ImageHeader>(kHeaderScratch);
}
```

短短的一个函数,咱们一行一行看。lgdt 打头——摆渡的来回要过 32 位的世界,远跳要查的是描述符表,而表得在动第一趟之前装好,讲摆渡的时候咱们交代过,如今它就是这里的第一行。然后读盘:按一扇区的粒度从盘上第 32 号 LBA 读回一扇,收货地址还是那扇暂存窗。然后咱们跑一趟摆渡,把窗里那 512 个字节接了下来,送到三张表盘上沿紧挨着的一块低址空地:0x4000。为什么要多这一趟?64KB 牢笼的法条还热着:窗在 0x10000,C++ 的手在 16 位世界里够不着窗里的任何一个字节。所以头一趟摆渡搬的不是内核,是标签:标签下不来的话,咱们连读它的指头都伸不出去。读回来的这一扇区有 512 个字节,标签用掉的只有头 40 个,剩下的躺在暂存里没人动——读盘的粒度就是一扇区,标签搭的是顺风车。

第二段管的是验。stage2 把九判请了出来当堂宣判,咱们看它怎么开口:

```cpp
cinux::boot::LoadStatus const kVerdict =
    cinux::boot::ValidateImage(kHeader, usable, kUsableCount, kOwned, 4);
if (kVerdict != cinux::boot::LoadStatus::kOk) {
    Println("[stage2] kernel image rejected: %s", cinux::boot::NameOf(kVerdict));
    cinux::console::Halt();
}
cinux::console::PutString("[stage2] kernel image ok\n");
```

usable 是从 E820 存档里拣出来的可用区,kOwned 装的就是上一节那四块脚印。面板上装载的头一行新字 `[stage2] kernel image ok` 在这儿亮。要是宣判不过,亮的就是那行拒绝的人话,然后 Halt——boot 的世界体面地停住:机器不复位,咱们还有面板可看。

第三段干的是循环摆渡,整条流水线的主干,咱们把整段循环摆开:

```cpp
LoadStatus LoadKernel(const ImageHeader& header) {
    uint64_t remaining    = header.file_size;
    uint64_t lba          = kKernelImageLba;
    uint64_t paddr        = header.load_paddr;
    bool     first_window = true;
    while (remaining > 0) {
        auto const kSectors = SectorsFor(remaining, kFerryWindowSectors);
        g_window_dap.count  = static_cast<unsigned short>(kSectors);
        g_window_dap.lba    = lba;
        if (!read_window()) {
            return LoadStatus::kDiskError;
        }
        unsigned long const kBytes = SectorBytes(kSectors);
        unsigned long const kChunk =
            remaining < kBytes ? static_cast<unsigned long>(remaining) : kBytes;
        RunFerry(static_cast<unsigned>(paddr), static_cast<unsigned>(kFerryWindow),
                 static_cast<unsigned>(kChunk));
        if (first_window) {
            RunFerry(kHeaderScratch, static_cast<unsigned>(paddr), 8);
            if (LoadWord(kHeaderScratch) != kImageMagic) {
                return LoadStatus::kMagicMismatch;
            }
            first_window = false;
        }
        lba += kSectors;
        paddr += SectorBytes(kSectors);
        remaining -= static_cast<uint64_t>(kChunk);
    }
    g_kernel_entry     = static_cast<unsigned long>(header.entry);
    g_kernel_end_paddr = static_cast<unsigned long>(header.load_paddr + header.mem_size);
    return LoadStatus::kOk;
}
```

循环里记着三样数:盘上还剩多少、读到哪个扇区了、内存里写到哪儿了,对应的变量名分别叫 `remaining`、`lba`、`paddr`。每转一圈咱们算这一窗要几个扇区,读进了窗、摆渡到了 paddr,三样数各记了一笔——再转下一圈。

圈里的算术,两处值得咱们停一停。`SectorsFor` 把字节数向上取整成了扇区数,封顶 127:窗的容量是 64KB,填满它的正好是 128 扇区,咱们留一扇区的余量——不顶到窗沿。这个 127 不是咱们的发明,GRUB 家的 biosdisk 里躺着同款数字的注释,BIOS 时代传下来的手艺。kChunk 管的是末一窗:剩余不足一整窗时,只搬 `remaining` 那么多、一个字节也不多带。还有一处藏在 SectorsFor 的内部,算的还是那个 (bytes + 511) / 512。在九判里验过回卷的就是这笔加法,所以这儿用得放心。判据和算术这么一搭,咱们一头验一头用,流水线的两头都踏实。

咱们这版内核 2613 个字节,取整下来才占了 6 个扇区,离 127 还有的是余量,一窗就搬完了。可流水线的形状是照着大内核长的:真把内核养到 16MB,鞋盒上的数跟着长,循环自己就多转了两百多圈——链路一个字节都不用改。这一点咱们真拿一个 16MB 的疏松载荷压过:两百五十九窗,搬完逐字节地比对,没有一个字节是差的。搬完的收尾有两笔。入口记进了 g_kernel_entry,那是末班船的货。尾巴记进 g_kernel_end_paddr——装载的物理尾,后面开两扇门的时候要用它算门数。

第四段管的是回执,就藏在圈里的 first_window 那一块。头一窗摆完了,从 paddr 把头 8 个字节再摆回了 0x4000,读出来的必须是 CNKZ,不是的话 kMagicMismatch 照样拒。这就是讲摆渡的时候立下的法条,如今在这儿兑现了:BIOS 说成功,不等于数据就送到了——SeaBIOS 那趟假成功,回执把好报了一路,目的地预埋的毒记号原封没动。所以回执这东西,咱们不认 AH 的脸色,认内容:魔数真的从目的地搬回来了才算送达。回执只配了头一窗:出事就出在头一趟,这一验的成本,不过是又一次 8 个字节的往返。

四段接完了,还差一个挂载点:流水线排在 stage2 的哪一步?咱们看 `Stage2Main` 的尾巴:问完内存、配完屏、然后装内核,三件要用 BIOS 的活全部干完才打 `[stage2] leaving real mode` 那行字、进保护模式。这就是本卷开头说过的单向门流水线落到代码里的形状:凡是还要用 BIOS 的活,全排在了告别之前。装载的第二行面板新字 `[stage2] kernel ferried`,在循环转完的时候亮。此刻内核在 2MB 躺好了,BIOS 的应答还在、窗也还开着——可咱们不再回头。

内核进了内存,可它还是两手空空的。图谱、画布、它自己的身份——得誊成一份单子递过去,咱们下一节办家底的交接。
