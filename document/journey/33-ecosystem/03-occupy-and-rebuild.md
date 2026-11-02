---
title: "03 · 占址重建:拆旧立新,留头留尾"
description: "glibc 的解释器装库有一套固定手法:匿名 mmap 圈下整座库的地盘,再打着 MAP_FIXED 的旗把段一段一段塞进圈好的地界,末了 mprotect 收权。咱们的 mmap 从前对 MAP_FIXED 一律回绝,这一章给它立占址重建:起点对齐、六十四 KiB 护栏、上界防回绕,三验过后走替换语义,拆的活收进 ReclaimUserSpan 这道内部缝,与 munmap 共用一把扫帚。登记册的注销重造为劈段版,半覆盖的段留头留尾,劈两半时尾巴的文件偏移一字不差地随迁,堆被切则收缩,基座被盖则归零,ld.so 圈大块塞段的装库路数这才走得通。当年三组语义里与 libc 无关的三件照搬,两件已判例免修。语义一进化,旧断言挡路,首轮机内验收唯一没过的就是拒劈段的旧断言,按真实约定改写后二十一案案案过,宿主长凳 test_vma 二十二案。改断言是让测试跟着约定走。"
chapter: 33
order: 3
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - ecosystem
  - mmap
  - vma
  - memory
  - syscall
---

# 占址重建:拆旧立新,留头留尾

glibc 的解释器 ld.so 有一套自己的装库手法。它拿一次匿名的 mmap 把整座库的地盘圈下来,权限一概是关着的,然后把段一段一段往圈好的地界里塞,每塞一段都打着 MAP_FIXED 的旗,指名要地址,末了再用 mprotect 把各段的权限收到位。圈地、塞段、收权这三步连成了一气。这套手法咱们从前只接得住头一步,旧 mmap 脸上的态度写得明明白白:

```cpp
    if (kLength == 0 || (kFlags & kMapFixed) != 0) {
        return -kEinval;
    }
```

长度为零的要拒,带 MAP_FIXED 的也一样要拒。在旧 mmap 的眼里,指名要地址的客人与捣乱的客人是同一路人,统统请了回去。客人里头算好说话的是 musl,拿不到就悄悄算了,它的日子照过。glibc 是不行的,它的装库手法一步都省不掉,想请它进门的话,咱们的地基就得会一件事:客人指名的地界上,旧的拆掉,新的立起。咱们管这套章法叫占址重建,替换语义说的就是它。

新的 mmap 把占址的客人单独引上一条路,咱们从路口的检验看起:

```cpp
    if (kFixed) {
        if (kAddress < mm::kUserNullTop || kAddress >= kUserAddressLimit ||
            kBytes > kUserAddressLimit - kAddress) {
            return -kEinval;
        }
        start                 = static_cast<unsigned long>(kAddress);
        const auto kReclaimed = ReclaimUserSpan(*kSelf, start, kBytes);
        if (!kReclaimed.ok()) {
            return -ErrnoFrom(kReclaimed.error());
        }
    }
```

检验摆了三道,没有一道是含糊的。头一道住的地方不在这里,咱们去函数开头共用的校验表里把它找出来,是这么一行 `(kFixed && kAddress % cinux::arch::page::kSize != 0)`,起点必须是页的整数倍,不对齐的地址想都别想。这里的两道:最低的六十四 KiB 永不映射,内存当家那一卷挂在墙上的地图那一章立过的约定,给空指针留一个炸得整齐的地方,占址的客人也不许把手伸进去。起点不许越过用户半区的顶,起点加长度也是不许的,后一道防的就是加法回绕,长度一大的时候,加法就绕回低处去了,纸面上的地界看着还在用户手里,其实早出了天。三验全过了,替换语义才出场:ReclaimUserSpan 把地界上旧的住户请走,回来之后新的段照常登记。一借一还的功夫,占址重建在代码里就这么高了。

拆的活没有写在 mmap 自己家里,收进了一条内部缝,名字就是您刚才见到的 ReclaimUserSpan。它的定义住在 munmap 的文件里,这不奇怪:munmap 从前就要把一段用户地界连册带页拆干净,占址借的是同一把扫帚。咱们把缝的本体端出来:

```cpp
base::Result<void> ReclaimUserSpan(const proc::Task& self, unsigned long start,
                                   unsigned long bytes) {
    const base::Result<void> kGone = self.vmas->unregister_range(start, start + bytes);
    if (!kGone.ok()) {
        return kGone;
    }
    if (!self.space.holds()) {
        return {};
    }
    mm::KernelTables tables;
    for (unsigned long spot = start; spot < start + bytes; spot += cinux::arch::page::kSize) {
        const mm::walk::Entry kOld = mm::walk::UnmapPage(tables, self.space->root(), spot);
        if (kOld.has(cinux::arch::page::kPresent)) {
            mm::cow::ReleaseFrame(mm::walk::EntryTarget(kOld));
            cinux::arch::InvalidatePage(spot);
        }
    }
    return {};
}
```

活分成了两半,咱们一半一半看。前一半在登记册上把地界注销,后一半钻进的是页表,一页一页把还站着的映像拆下来,物理帧该还的还,快表该冲的冲。宿主长凳上考 mmap 的桩只留前一半,宿主的世界里没有这套用户页表,后一半在那里是本来就无处安放的。一条缝让两个世界各取走了自己的一半,munmap 自己的错误语义一个字没动。

真正的重头戏在注销本身。旧注销的要求很硬:区间碰到的每一段,必须是被区间整段包住的,有一段只被切到半截的话,整笔是一律回绝的,一个字都不带动的。旧界是有位门房看着的,叫的是 movable_within,这一回咱们连档带码一起请走了。新注销学会了劈段,判级的台阶原身在这里:

```cpp
            const bool kKeepsHead = scan->start < start;
            const bool kKeepsTail = scan->end > end;
            if (kKeepsHead && kKeepsTail) {
                auto* const kTail = new VmaRegion(*scan);
                if (kTail == nullptr) {
                    return base::KernelError::kOutOfMemory;
                }
                kTail->start       = end;
                kTail->file_offset = scan->file_offset + (end - scan->start);
                scan->end          = start;
                regions_.push_back(*kTail);
            } else if (kKeepsHead) {
                scan->end = start;
            } else if (kKeepsTail) {
                const unsigned long kOldStart = scan->start;
                scan->start                   = end;
                scan->file_offset += end - kOldStart;
            } else {
                regions_.remove(*scan);
                delete scan;
            }
```

咱们顺台阶走。段头露在区间外的,头是留着的,尾巴收到的是切口。段尾露在区间外的,尾留了下来,起点挪到了切口。两头都露的,段被劈成了两半,新起的一段接住尾巴。整段都在区间里的,咱们把它请出册子。四级台阶里最值得多看一眼的,写的是 file_offset 那两笔。文件映射的段,册上记的地界之外,还记着对着文件的哪一扇窗。尾巴的起点挪高了,咱们就得让窗跟着平移,不平移的话,尾巴里的页往后从文件里读出来的就是错位的字节,读是读得出来的,错是错得安静的。所以劈两半的那一级,新尾巴的 file_offset 是老值加上被切走的长度,一字不差地随迁。

册子里还有个特殊的住户:堆。brk 的堆不走段链,单独记着的是起点与顶。被切到中段的话,顶收到的是切口。连基座都被盖住了,整本都归了零。这里咱们正好点一笔旧债:把后半场交给解释器那一卷收卷时挂过,brk 的扩张不查重叠,堆长上去有理论上的机会啃进解释器的地盘,当时是没动的,挂上了欠单。占址重建立了起来,占址与堆的交界从此有人管,mmap 想占堆的地盘,得过的是注销这一道,堆怎么让、让到哪儿,都有章程了。

劈段为什么是刚需,咱们回到装库的现场。ld.so 圈地圈下的是整座库的地盘,塞段塞进去的是库里一段一段的家当。为什么每一次塞段都是半覆盖?段是比整座库小的,每塞一段的时候,圈好的块都得留着前后,等别的段来住。旧注销在半覆盖面前唯一的答案是回绝,ld.so 塞到第二个段的时候就得吃 EINVAL,装库的第二步当场卡死。新注销把半覆盖接住了,圈地与塞段这两步的活,是从这一刻起才算真能连起来的。

当年第一遍把动态客人请上盘的时候,笔者立过三组语义。这一遍换血换的是 libc,语义里跟 libc 无关的那部分原样照搬,一件都不用咱们重发明。头一件就是本章办的:低址放行。占址的手不设最低门槛,六十四 KiB 的护栏之上,客人指名要多低的地界,咱们都认。另外的两件,判例是已经立过的,这一遍就免修了。PROT_NONE 的段不补零页。PROT_NONE 是读、写、执行一概不给的,懒到家那一卷立的补页裁决,在三样权限上各拦了一道,三样全关的段哪一道都过不去,零分配自然就不会发生了,一件活都不用咱们干。座号是从零起的,开机的一号进程自己把零、一、二这三座 stdio 装好:头三号从假座换成真座,是假文件与真终端那一卷办完的事,这一遍照用就是了。

语义往前走了一步,测试里立过的旧断言就挡在了路上。旧注销的案卷里有这么一案,名字是叫得直白的,`vma: unregister refuses to split a span`。案身的硬话只有一句:拿区间去切一段的半截,注销是必须失败的,失败了之后,后半截是还得找得到的。您看它押的是什么。押的是拒劈段。约定改成的是劈段留尾,这句断言就从守门变成了挡路。宿主长凳上的案卷随着重造一起改了写,机内同名的案卷漏了改。首轮机内验收的时候,普通镜像与检查镜像两套各摆了二十一案,双双停在了二十上,唯一没过的就是它,那一轮是照实记的,不能算整轮通过的。断言按真实的约定重写,案名里的 refuses 换成了 splits,断言翻了面:切的是半截,注销是成功的,切掉的部分找不到了,留下的尾巴找得到,册上恰好留了一段。重跑了一轮,机内两套各二十一案案案都过了,宿主长凳上 test_vma 跑了二十二案,宿主的 test_syscall_face 里添了占址的一案,同址再占的案子,拿回的还是同一个地址,旧段被占掉的部分让位,没占到的尾巴照旧活着,护栏之下是拒的,不对齐也是拒的,该锚的都锚住了。

劈段立起来了,收权的那半还是老章法。收权走的是 mprotect,咱们的 protect 在半覆盖一段的时候,改的仍是整段的权限,界外的部分跟着连坐。glibc 装库的第三步收的恰恰是段中段:RELRO 这道手续,名字里 rel 是重定位、ro 是只读的意思,重定位写完的那片地界要改成只读。它伸手的中段里要是住着 .got.plt,咱们就得捏一把汗。.got.plt 是函数真地址的登记表,它是靠惰性绑定活着的,等函数头一回被调用的时候,才把真地址填进表里。整段被连坐成了只读之后,惰性绑定再想写它的话,等它的就是落笔即死。咱们把这笔观察项挂在明处,收权的那一章来兑现。
