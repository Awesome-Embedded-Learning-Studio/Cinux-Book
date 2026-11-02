---
title: "06 · 收权学会劈段,浮点旧债销案"
description: "地基上挂着的话在这一章兑现:收权学会与注销同款的劈段,碎片备齐了才动册子,界外权限与文件偏移留在原地,RELRO 的中段收权有了回归案,惰性绑定的跳转表不再陪葬。mprotect 的门面补齐参数验证,改表项保住执行位与 NX,PROT_NONE 的页摘掉 User 位,fork 的克隆不许把它重开。册边四处收紧:堆被劈后断点不动,栈碎片继承原护页底线,文件映射攥着打开的文件,后端寿命经一条租约贯穿。末了浮点旧债销案:实弹里 sleep 0.01 踩响蹦床那一章种图像时写错的 MXCSR 偏移,默认值应落字节 24,旧值写在 32,老断言常量对常量查不出它,新案子真执行 fxrstor 与 stmxcsr,保存并还原宿主原态。地基考完,轮到盘面,整具 busybox 换血上桌。"
chapter: 33
order: 6
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - memory
  - vma
  - fpu
---

# 收权学会劈段,浮点旧债销案

hello 三修进了门,可这不算地基考完。占址重建那一章撂在明处的观察项还挂着:注销学会了劈段,半覆盖的段留头留尾,收权却还是整段的连坐,谁在一段的中腰上改权限,倒的霉是整段都得跟着换新词。整具的 busybox 还没上桌,咱们趁它没来,把欠的还上。

收权的叫法,咱们跟着解释器 ld.so 的装库三步来:圈地、塞段之后,第三步拿的是一记 mprotect,把权限收成那一段在镜像里本来的样子。逼它学劈段的,是 glibc 的一道讲究,名字叫的是 RELRO,重定位过了、改只读。麻烦上一章末尾已经撂在明处:这片地界从段中间起、到段中间止,界外紧挨着惰性绑定的跳转表 .got.plt,收权要是整段连坐的话,只读顺带扣到界外的表头上,客人头一回调库函数的时候,那一记写了下去,当场就缺页了。

修法与注销是同款的,也是劈的路数,不一样的地方在提交的次序。咱们把收权的帮手整个看一遍:

```cpp
bool stage_protection(RegionBatch& batch, const VmaRegion& source, const VmaRegion& cut,
                      VmaProtection terms) {
    const unsigned long kCutStart = base::math::Max(cut.start, source.start);
    const unsigned long kCutEnd   = base::math::Min(cut.end, source.end);
    const unsigned long kCuts[]   = {source.start, kCutStart, kCutEnd, source.end};
    for (unsigned long index = 0; index < 3; ++index) {
        if (kCuts[index] == kCuts[index + 1]) {
            continue;
        }
        auto* const kPart = new VmaRegion(source);
        if (kPart == nullptr) {
            return false;
        }
        kPart->start = kCuts[index];
        kPart->end   = kCuts[index + 1];
        kPart->file_offset += kPart->start - source.start;
        if (index == 1) {
            kPart->readable   = terms.readable;
            kPart->writable   = terms.writable;
            kPart->executable = terms.executable;
        }
        batch.regions.push_back(*kPart);
    }
    return true;
}
```

您看 kCuts 那一行,四个数摆出来:旧段的起点、切口的头、切口的尾、旧段的终点,相邻的两个数夹出一片,一段切出来的至多是三片。每片都是打旧条目整个复印来的,再各自把自己的边界改掉,头尾两片是原词原样的,中片换上的是新词,文件偏移按新起点的位置随迁。切好的碎片不进册子,攒的地方是备片区 RegionBatch,等旧段删干净了,新片这才排上队入了册。哪一片分配不到的话,函数交的就是失败,册子是一个字都没动的,改了一半的状态生不成。

回归案入了册,案名起的正是考点:`vma: relro protects only its middle pages and preserves file offsets`。文件段铺的地界是 0x1000 到 0x6000,偏移记的是 0x8000,收权只收 0x2000 到 0x4000 的那一截,咱们把断言逐片数下来:头片可写,中片是不可写的,尾片又是可写的,中片的偏移读出 0x9000,尾片读出的是 0xB000,随迁是一步不差的。案尾还追了一记,拿 0x3000 到 0x7000 的地界去收,半截是悬在册外的,protect 拒了。改词不收回地界,可册外的页无词可改,页页都是有主的才许改,把着这道关的是门口的 span_known,悬空的答 EINVAL。

mprotect 的门面,咱们也补了参数的验:地址要页对齐,长度顶到上界的话就不许回绕,权限旗只认的是读、写、执行三位,多出一位答的就是 EINVAL。零长度是不算事的,答的是零,咱们放它过去。

已经落地的页当场改表项,咱们看那几行:

```cpp
        cinux::arch::page::Entry bits =
            cinux::arch::page::UserLeafTerms(kTerms.writable && !kShared, kTerms.executable);
        if (!kTerms.readable && !kTerms.writable && !kTerms.executable) {
            bits = bits & ~cinux::arch::page::kUser;
        }
```

执行位从此就有人过问了。从前的改法,咱们翻出来一比:只问写不写,User 位是逢改必发的,执行位是从来不看的,谁被收过权的话,表项就丢了 NX,只读的数据页反倒变回可执行。这一遍换的是 UserLeafTerms 按三问重排:收掉执行,NX 就位了,写与执行分家的老话在收权的路上不破例。三问全否的话,说的就是 PROT_NONE,表项是连 User 位一起摘掉的,硬件这一层当场拦下了,用户态一碰就缺了页,缺页了再翻册,三问还是全否的话,咱们往真越界里判。

收权立了,册子边上有四处旧约定是跟着收紧的,咱们挨件看。

头一件办的是堆。注销劈堆劈的是打洞留尾,堆里砌护墙的那一章办过。收权劈进堆是另一副局面:盖住的那几页从堆名下升格成普通条目,堆头堆尾各自成了段,断点也就是 brk 问到哪儿的那道游标,原样是不动的。堆长到了 0x108000,中段 0x102000 到 0x104000 收成了只读,断点还是停在 0x108000 的。回头咱们让它涨到 0x10A000,涨出来的照旧归堆管,再缩回 0x104000 的时候,只读的那截守着只读,裁掉的部分,页面上是再查无此址的。

第二件办的是 fork。PROT_NONE 在表项上摘了 User 位,克隆的走查要是照常规给新页发 User 位,孩子在无权页上反倒有了落脚点,收权经 fork 就漏了,咱们不能容它。share_leaf 里那两行管的就是它:

```cpp
    Entry terms = cinux::arch::page::UserLeafTerms(slot.has(cinux::arch::page::kWritable),
                                                   !slot.has(cinux::arch::page::kNoExec));
    if (!slot.has(arch::page::kUser)) {
        terms = terms & ~arch::page::kUser;
    }
```

父页的身上没有 User 位,孩子的表项也把 User 位摘干净,咱们顺着克隆的走查读下来:无权经 fork 还是无权。

第三件办的是栈。栈条目的身上记着一道底线,底线下头的那一页是护页,踩了就叫。劈了段之后,碎片继承的还是原来那道底线,不能劈一回就把每片的第一页都当成新护页,中段的首页要是被误认,栈就白短了一页。案子里咱们看栈从 0x1000 铺到 0x8000,中段收权劈成了三片,底线下头照旧只有原来那一页的护页。

第四件办的是文件。咱们看文件映射的条目手里的两件家当:

```cpp
    base::Shared<fs::MountLease> file_lifetime{nullptr};  ///< Runtime file backend ownership.
    base::Shared<fs::OpenFile>   open_file{nullptr};  ///< Pins a file after its descriptor closes.
```

一件是打开的文件,描述符关了它也不松手。另一件是后端的租约,一条从 resolve、open 通到装载、登记册的租约贯穿到底。后端的寿命不再挂在挂载表的某一项上,咱们数的是引用:哪个视图还指着它,它就活得好好的,全退了场,它才散得了。册上的映射,来历就登记在条目自己的身上,文件页的承诺不会因为哪张表换了内容就断了出处。

地基的欠件清完,浮点的旧债自己找上了门,咱们把案卷翻开。

实弹里的客人敲下 sleep 0.01,雷响了,炸点在咱们自家的地基里。串口上那一声没给咱们留下案卷,翻得着的记录全是修好之后的复跑:sleep 0.01 走进去,回来的是一行 APPLET_6_0。

带小数的秒数得换算成纳秒才能交给钟,咱们跟着这一换算走,路过的全是浮点算术,浮点算术一进场的话,就轮到 MXCSR 开口说话了。它是 SSE 的控制寄存器,低六位是它的异常掩码,哪一位写了一,哪一类异常才算遮得住了。新任务的进场图像,从轮流当家那一卷立蹦床的那一章起就是清零、种默认值,MXCSR 的默认是 0x1F80,案卷在那边是记着的。栽种的那几行,修前长的就是这个样子:

```cpp
void plant_clean_fpu_image(cinux::arch::CpuContext& ctx) {
    cinux::base::SetBytes(&ctx.fpu, 0, sizeof(ctx.fpu));
    auto* const kFcw   = reinterpret_cast<unsigned short*>(&ctx.fpu.words[0]);
    kFcw[0]            = static_cast<unsigned short>(kX87ControlWord);
    auto* const kMxcsr = reinterpret_cast<unsigned int*>(&ctx.fpu.words[4]);
    kMxcsr[0]          = static_cast<unsigned int>(kMxcsrDefault);
}
```

雷埋在最后一行的下标里。FXSAVE64 的五百一十二字节图像,MXCSR 住的是第 24 字节,笔者当年那一指落在 `words[4]`,落的是第 32 字节。图像是整个清过零的,默认值写去了隔壁,真 MXCSR 的四字节留在零上,六位的掩码,是一位都没遮的。修后动的就只有那个下标:

```cpp
    auto* const kMxcsr = reinterpret_cast<unsigned int*>(&ctx.fpu.words[3]);
    kMxcsr[0]          = static_cast<unsigned int>(kMxcsrDefault);
```

它安静了这么久,咱们自己的程序个个都有功劳:从登记册一路走到网络四部,只会做搬 xmm 的活,浮点算术是不做的,魔数词装进去、取出来、对个拍的功夫,一次舍入的事都没有,掩码开不开的话,说话的事轮不到它。glibc 的 sleep 不同,0.01 在二进制里是写不整的,换算的乘法一凑,舍入例外冒出头了,掩码外圈的例外是真异常,雷就是这样响的。自家程序没走过的路径,客人一走就现了形,这颗雷从立蹦床的那一章埋下,头一回有人踩了上去。

还有一桩是得交代的,长凳底下躺了这么久,老案子是拦不住它的,而这不奇怪:咱们把老断言翻出来看,它拿的是与生产同一个下标,代码写的地址是 `words[4]`,测试读的也是 `words[4]`,写的人落错了格子,查的人照着同一格去数,数字当然对得上。常量对的是常量,错是对不出来的。新案子咱们改成真执行:

```cpp
    cinux::arch::FpuImage saved{};
    cinux::arch::SaveFpu(saved);
    unsigned int actual_mxcsr;
    asm volatile("fxrstor64 %1; stmxcsr %0; fxrstor64 %2"
                 : "=m"(actual_mxcsr)
                 : "m"(ctx.fpu), "m"(saved)
                 : "memory");
    ASSERT_TRUE(actual_mxcsr == 0x1F80);
```

宿主自己的浮点现场存下来,咱们把种好的图像用 fxrstor64 装回真寄存器,stmxcsr 从活的 MXCSR 读出真值,末了把宿主的原态还回去。断言认的是寄存器里读出来的 0x1F80,而不是从图像里再数一遍下标。下标挪回了一格,默认值住回了字节 24,旧债销了案。

咱们收卷。内核的地基考完了,接下来轮到的是盘面,该整具 busybox 换血上桌了。
