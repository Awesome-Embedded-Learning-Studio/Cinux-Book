---
title: "13 · 换个大目标:在自己肚子里造一只新的自己"
description: "串口壳里的自举成了,这一章把目标抬到顶,在 Cinux 自己的盘上从空对象目录编译完整内核,机内生成字体与 initrd 资源对象和 kallsyms 符号表,走无符号与最终两轮链接,封装启动镜像并 fsync。宿主关掉 QEMU 后提取产物,核对机内记录的 SHA-256,再启动这只新内核,用 GCC 编译运行 hello。自举的输入摆在明处,源码卷不带宿主内核对象或已链接内核,宿主只供配置、命令、工具链、Python 与引导器,这一轮没有重新编译 GCC 或引导器。复跑入口三行命令,烤出 256 MiB 独立 ext2 源码卷,512 MiB 内存、两颗核、KVM,构建树与 QEMU 串行。ext2 保留扇区里埋一段非零哨兵,AHCI 自检存下人家的原扇区、用完原样归还,构建启动之后宿主把哨兵读回,一字不差。目标立了,头一轮真跑,一串真实负载的洞就冒了出来。"
chapter: 34
order: 13
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - kernel
  - selfhost
  - toolchain
  - ext2
---

# 换个大目标:在自己肚子里造一只新的自己

串口壳里的自举,咱们跑到这里成了,连冷缓存重新开张那一遍都走完了。按说见好就收的话,也是个体面的收法。可编译自己压着的最后一层还没有兑现,hello 是自家 gcc 编的没错,可它坐的终究只是小客人的座。整只内核得从自己的肚子里做出来,编译自己才算真正落地。这一章咱们把最后一层抬上桌,它也是全卷压轴的那场大戏。

目标不能停在豪言上的话,咱们就把它写成能验收的步骤。机内的这一半,从空的对象目录起,把全部内核源文件都编译了一遍。字体和 initrd 的两包资源,机内用 objcopy 包成了目标文件,跟着一块儿链了进去。kallsyms 的那一步,拿 nm 去读头一轮链接产物的符号表,把名字和地址抄了出来,生成一份表编进了内核,往后排查的时候,它负责把地址翻回函数名。链接走的是两轮,无符号的一轮,生成 kallsyms 之后的最终一轮。末了封装启动镜像,主引导记录摆在最头里的位置,第二段引导器跟在头一个扇区的后面,内核从 16 KiB 的地方躺好,镜像凑足的至少是一 MiB,末了再 fsync 一回,把页缓存里的字节按到盘上。少了 fsync,宿主下一刻就把 QEMU 硬关了,还没写到盘上的字节就跟着内存一起没了。

机内的活干完,另一半的验收在宿主。QEMU 关掉了,宿主把产物从卷里提取了出来,和机内记录的 SHA-256 对上号。再让造出来的新内核自己开机,开机之后咱们用 GCC 编 hello、跑 hello。两头都过了,咱们才算把这一炮打中。

再往下的一步,就得把自举的输入摊开清点,这里头是最容易糊的。什么样的自举算数,什么样的算作弊,界线咱们立死,源码卷里不带宿主的内核对象,也不带已链接的内核,对象和内核必须是在机内做出来的。宿主供的输入有五样:配置、命令、工具链、Python、引导器。

清单就这么五样,咱们一样不多认。

机内跑的每一条编译命令和两条链接命令,都来自宿主上配好的构建,构建系统把它们汇成了一份清单原样导出,路径改写成机内的 /src。咱们这一轮验的是编译这双手,配置那摊照宿主的来。

Python 凭什么也算输入?它又不编内核。机内开车的司机是构建驱动脚本,它的真名是 scripts/build-kernel-in-guest.py,本身就是 Python 写的,不把它连同标准库一起搬进卷的话,咱们机内一步都起不了步。工具链是盘上的 gcc 全家,nm 和 objcopy 两件小工具也一并搬进了卷。引导器是主引导记录和第二段的两枚二进制,原样进了卷。这一轮咱们没有重新编译 GCC、也没有重新编译引导器,哪些是借的、哪些必须自己动手做,全写在明处让大家看。

复跑的入口三行,您在检出的仓库里照着敲就行:

```bash
cmake -S . -B build
cmake --build build --parallel 4 --target kernel_selfhost_volume
python3 scripts/test_station34_kernel_selfhost.py
```

头一行配好的是构建树。第二行烤出一只 256 MiB 的独立 ext2 卷,烤法是这一卷请师傅进门那一章立下的新烤法,mkfs.ext2 直灌目录树就行了。卷就是机器的盘,工具链的家当照旧在,源码和那五样输入也一并塞了进去。第三行开的是 QEMU 实弹。咱们给到 512 MiB 内存、两颗核、KVM,构建树与 QEMU 都是按串行跑的。纪律也接着生态那一卷作坊的老样子,开车的是脚本自己,判据到齐了才算过,半路出了错就停,但是不靠人眼盯串口。

还有一件护身的事,咱们单独摆出来说。ext2 的开头一 KiB 是保留区,超级块从第二个 KiB 的地方才起头,512 到 1023 的一个扇区没人认领。卷烤完了,宿主挑来埋哨兵的就是这里,一段从 255 倒着数到 0 的字节花样,写两遍正好填满了一个扇区:

```python
    with image.open("r+b") as target:
        target.seek(512)
        target.write(bytes(range(255, -1, -1)) * 2)
```

哨兵的用处很直接,回头原样读得回来的话,就证明这块地方没被人动过。专挑这个扇区的缘故,是内核 AHCI 驱动起身时的那场自检拿它当草稿纸。请您盯着 saved 这个名字的两回现身,一回存的是原样,一回还的也是原样:

```cpp
    const BlockSpan                   kOneBlock{.value = 1};
    std::array<uint8_t, kSectorBytes> saved{};
    if (!read_blocks(Lba{.value = 1}, kOneBlock, saved.data())) {
        print::Println("[kern] ahci scratch save failed");
        return false;
    }
    const bool kWritten = write_blocks(Lba{.value = 1}, kOneBlock, pattern.data());
    const bool kRead    = kWritten && read_blocks(Lba{.value = 1}, kOneBlock, readback.data());
    const bool kMatches =
        kRead && cinux::base::EqualBytes(pattern.data(), readback.data(), pattern.size());
    if (!write_blocks(Lba{.value = 1}, kOneBlock, saved.data())) {
        print::Println("[kern] ahci scratch restore failed");
        return false;
    }
```

自检写的是自己的花样,正着数的是 0 到 255,写完读回、比对了,再把 saved 里存的原样写回去。咱们把两套花样摆在一起看,哨兵是倒着数的,自检是正着数的,长相却是不同的,真要是自检把草稿留在了盘上,或者存的根本不是原样,宿主读回来一对就露了馅。这一炮从头到尾要开的是两回机,判据落在头一回跑完之后:构建那一轮启动结束、QEMU 关了机,读回来对的是宿主,对上的是一字不差,这一关才算过了。第二回开机的时候,新内核还要再跑的是同一场自检,哨兵那一回没人再读、守在原处了。这场自检将来跑在谁的盘上,谁的数据就在它手里,草稿纸不是白拿的,借之前存好了,还的时候一字不差。

到了这儿,咱们把目标写成步骤,点下来的输入样样有名有姓,卷烤好了,哨兵埋好了,三行命令也候着了。目标立了,头一轮是真跑了,一串真实负载的洞就冒了出来。
