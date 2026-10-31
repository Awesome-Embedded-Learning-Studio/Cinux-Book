---
title: "05 · 惰性装载:cc1 不再整只进屋"
description: "装载这件活做到一半,开机冒烟就出了案子:一号进程坐上座,没等开台就倒,监护人把它拉起来,它又倒,启动循环没完,提示符的面都不露。根子在 exec 侧做了一半的惰性装载:整段 p_memsz 被登记成一片文件 VMA,缺页器按整页读,越过 p_filesz 之后零没等来,等来的是 ELF 文件里跟在段后面的节表等字节,BSS 进了赃,文件 EOF 的短读也表达不了段尾。销案照 Linux 的老三样把段三分:完整文件页只登记文件 VMA,缺页时清零新帧再从 backing 读整页,短读留在零里。文件尾页装载时就地铺好,清零加限长读。其余 BSS 登记匿名区域按需补零。进门四道验:段偏移与虚地址页内同余、段不出文件边界、filesz 不越 memsz、地址不出用户天顶,非法段答失败,登记册一张不留。backing 的归属是每个镜像一具 OpenFile,文件段、劈段与 clone 共享它,RamFs unlink 之后未触达的页照读,当年 VMA 裸持 inode、引用计数错成 UAF 的判例在这里得了正解。init、解释器、exec 三处读取收成 64 KiB 头前缀,约 47MB 的 cc1 不再整只预读。长凳新增脏文件尾、跨页 BSS、未预映射整页、保护劈段与 clone、unlink 后补页、错误 ELF 拒收,旧测试画像重画成页同余,临时诊断全撤。头一轮完整跑通,gcc 全链 3.533 秒、g++ 全链 8.535 秒,KVM、两颗 vCPU、默认 128 MiB,量的是编译命令墙钟,装载编译链接全算在内,cc1 装载耗时这几个字安不上去,本轮没重跑整只预读的对照,当年的 22 秒到 0 秒是当年的口径。装载这关过了,拉锯挪去门上,师傅一开工就要点名。"
chapter: 34
order: 5
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - selfhost
  - elf
  - execve
---

# 惰性装载:cc1 不再整只进屋

装载这件活做到一半,开机冒烟就出了案子。一号进程坐上座,没等当班表开台就倒了。监护人把它拉起来,它又倒。起来,倒下,再拉起,咱们眼看着开机卡进这个循环,提示符的面都没露。

查案的家底是懒到家那一卷攒下的,登记册、缺页裁决、劈段的诊断,件件趁手。当年在同样的路上还立过一笔判例:用户态来的缺页,裁决者不受理的,翻译成十一号信号,让肇事者自己退场,判例就立在当年 cc1 进门的现场。这一遍家法早在,咱们照用。病灶呢,不用往远处找,就藏在装载这件活已经动工的那一半里。思路是对的,exec 不该把客人的整份文件读进内核缓冲,cc1 约 47MB 的身子,gcc 每编一个文件都得请它进一回屋,整只搬进来再扔出去,谁也吃不消。坏就坏在活只做了一半。

咱们把做了一半的活翻开,病灶摆在明处。段的身子分两半:文件里存着的前一半,段头上 p_filesz 记着数。内存里要比文件多出来的后一半是 BSS,到 p_memsz 为止,进门该是零。没做完的实现把整段 p_memsz 登记成一片文件 VMA,册上写的全是从文件来,缺页器照册办事,按整页读。读到越过 p_filesz 的地方,零没等来,等来的,好家伙,是文件里跟在段后面的字节:节表、符号串,ELF 的尾巴全在里头,一古脑落进 BSS。指望文件 EOF 的短读来刹车,也没门:段的文件部分多半不在文件末尾,短读根本等不来。

> BSS 里的变量,按 C 的章程,起步就该是零:没人写过它们,它们也从没在文件里住过。字节一灌进来,该是零的地方成了随机值,程序死起来又快又怪,还怪不到别人头上。

病根找准,咱们裁段的刀法照 Linux 的老三样来:elf_map 管整页的文件部分,padzero 管尾页,set_brk 管 BSS。施工落在同一段代码里,头四个数把段切成三截:

```cpp
    const auto kPageBase = base::math::AlignDown(kSegmentVa, kSize);
    const auto kFileEnd  = kSegmentVa + segment.file_bytes;
    const auto kWholeEnd =
        segment.file_bytes == 0 ? kPageBase : base::math::AlignDown(kFileEnd, kSize);
    const auto    kMemoryEnd = base::math::AlignUp(kSegmentVa + segment.memory_bytes, kSize);
```

咱们一截一截看。`[kPageBase, kWholeEnd)` 是完整的文件页,登记成文件 VMA,页留在盘上,谁来读谁触发缺页。`[kWholeEnd, kMemoryEnd)` 是 BSS,登记成匿名区域,缺页补零。夹在中间的尾页躺在匿名那截的名下,装载时却当场铺好,铺的活就几行:

```cpp
    auto* const kDestination = base::PtrAt<std::uint8_t>(mm::DirectMapVirt(kFrame.raw));
    base::SetBytes(kDestination, 0, kSize);
    const auto kBytes = file_end - region.start;
    const auto kRead =
        world.file_ops->read(world.file_self, world.file, region.file_offset, kDestination, kBytes);
```

领一页新帧,整页清零,再限长只读页内残下的文件字节,零填顺手完成,短读这个词从需求里消失了。缺页那一侧,咱们顺手也对了理:补文件页,清零一页新帧,再从 backing 读整页。真碰到文件尾读不满,零就留在原地,只有后端拒读才算失败。段头要是不落在页界上,登记往回退到页基,文件偏移跟着回退,头一页连段前面的字节一起从文件里读,段从页中间起步的形态也接得住。

登记之前,咱们还立着四道验,就在裁段的开头:

```cpp
    if (segment.file_offset > world.file_bytes ||
        segment.file_bytes > world.file_bytes - segment.file_offset ||
        segment.file_bytes > segment.memory_bytes || world.base >= mm::kUserTop ||
        segment.virtual_address >= mm::kUserTop - world.base) {
        return false;
    }
    const std::uint64_t kSegmentVa = world.base + segment.virtual_address;
    if (segment.memory_bytes > mm::kUserTop - kSegmentVa ||
        segment.file_offset % kSize != kSegmentVa % kSize) {
        return false;
    }
```

文件两道,段不许越过文件边界,文件里的字节也不许比内存里要的还多。地址两道,段不许爬过用户天顶,段偏移与虚地址必须页内同余。同余这道,还请您多瞧一眼:文件字节按页对进内存,文件侧页内的位置与虚地址侧页内的位置得对齐,差上一位,整页错位,读上来的每个字节都安错了家。哪一道不过,非法段答失败,装载整体拒收,登记册一张不留,半截工程不留给下一棒。

惰性装载把读文件的活从 exec 挪给了缺页器,页在盘上躺多久,文件都得在。保这件事的是 backing 的归属,咱们把归属定成:每个镜像持有一具 OpenFile,文件 VMA、各段、clone 分身共享同一具,各自只多拿一份引用。RamFs 里 unlink,目录上销了名,东西还在 OpenFile 手里,未触达的页缺页照读。当年第一遍在这里交过重税:VMA 裸持 inode 的引用,计数错了一笔,slab 把那块内存复用出去,悬空引用当场变成 UAF,笔者当年追到 slab 复用那一层才见底。这一遍不数 inode,数 OpenFile,最后一具放手,文件才算真放。

读头的活,咱们也跟着瘦。开机把一号进程请上座、给动态客人请解释器、exec 交接,三处的文件读取都收成 64 KiB 的头前缀,exec 的读取长这样:

```cpp
    fs::FsStat                 probed{};
    unsigned long              program_size = 0;
    base::Own<unsigned char[]> program;
    if (kView.value().ops->stat == nullptr ||
        !kView.value().ops->stat(kView.value().self, kNode.value(), probed).ok() ||
        !fs::ReadFilePrefix(kView.value(), 64_KiB, program, program_size).ok()) {
        return -kEio;
    }
```

stat 那一声问的是整个节点多大,咱们拿它给文件边界那两道验当尺子。读取只读头 64 KiB,ELF 头与段表都住在头几页里,解析够用。段的身体留在盘上,交给登记册与缺页器,整份 cc1 不再预读,47MB 的胖子进门只递一张名片。

长凳跟着添案,您挨个对。脏文件尾,段尾之后文件里跟着的杂字节,一概不许漏进 BSS。跨页 BSS,补零跨着页界也接得上。未预映射的整页,登记完页表里查无此页,真读真缺页才算数。保护劈段与 clone,收权劈开、分身各持之后,缺页照旧补得对。RamFs unlink 后补页,名字销了货还在。错误 ELF 拒收,边界与同余任何一道不过,登记册干净得像没来过。长凳上原来的测试画像,段是早前随手画的,没守同余,新门一立就把它挡在外面,画像按同余重画了一遍。排查期间临时架的 fault、open、mmap 诊断,结案后全撤,树回到干净。

结案要念数,口径也得跟着交代清楚。头一轮完整跑通,KVM、两颗 vCPU、默认 128 MiB 内存:gcc 全链 3.533 秒,g++ 全链 8.535 秒。两个数,量的都是编译命令的墙钟,装载、编译、链接全算在内,把它抄成 cc1 的装载耗时,口径就抄错了。本轮也没有重跑整只预读的对照,那一头的数咱们没有,不编。当年的 22 秒到 0 秒,量法是当年的:第一遍 cc1 约 47MB,整只逐页过 AHCI,装载时要停 22 秒,段三分改惰性之后,停归了零。数记在当年的判例里,笔者念的是当年的口径。这一遍的测量,就是刚才写下的两个数。

销案。装载这关,咱们算是过了,真正的拉锯在门上——师傅一开工就要点名。
