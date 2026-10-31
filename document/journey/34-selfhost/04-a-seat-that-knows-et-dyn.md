---
title: "04 · 座位认得 ET_DYN"
description: "头一件活交得利索,可它成立的前提按住没讲:老师傅 as 自己就是一副 PIE 的身子,主程序座按老座位表只收类型 2,exec 的头一步就把人送了回去,原本不动内核的盘算被冒烟当场掀翻,收官大戏的前半场没等到收官就被拽上了台。当年第一遍笔者补过最小的子集,认形态、选基址、入口加基址。这一遍把单子还成正式的座位语义:程序座放宽成类型 2、类型 3 都收,解释器座照旧只认位置无关的共享形态,链址定死的可执行件进不了那个家。新立的小函数报出装载偏置的需求,exec 给需要的客人挑原点:地板 512MiB,坐在解释器家 256MiB 的正楼上,每次进门再摇一个页对齐、不超一 MiB 的滑动,偏置随 LoadWorld 的 base 进装载算术,入口与头表落点一律加它。案卷跟着语义翻了两条:拒 e_type=3 的旧断言从册上划掉,换成程序座收可搬迁形态并报偏置需求,解释器座拒固定基址形态单独立案,断言跟着真实的行为走。类型 2 的老路一页没动,生态那一卷的实弹重跑全过,ET_EXEC 的老客人一个不惊动。座认了 PIE,下一位客人 cc1 是四十七兆上下的胖子,整只搬进屋要出事。"
chapter: 34
order: 4
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - elf
  - pie
  - execve
---

# 座位认得 ET_DYN

头一件活交得利索,可它成立的前提,咱们当时按住没讲:老师傅 as 本人,头一步就被拦在了门外。原本的盘算是,这一段压根不用动内核,两位老师傅从宿主整机搬来,喝的血是现成的 glibc,安了座就该开工。冒烟,拿最小的一串活把链从头点一遍,火刚点着,盘算就被掀翻。老师傅是在咱们开发它的电脑上出生的,如今发行版出的二进制,默认就长一副位置无关的身子,您敲 `readelf -h /usr/bin/as` 看 Type 一栏,印出来的是 DYN。exec 的头一步拿主程序座一量,类型 3,当场把人送了回去。上一节手链里的 hello 之所以要压着 `-fno-pie` 预编,为的就是让自家的产物别长成这副还没座可坐的身子。

拦人的座位表,是咱们在一台解析器,两张座位那一篇里排的:程序座只收类型 2 的可执行,解释器座只收类型 3 的共享对象,拿错了座,拒得跟截断的文件一个形态。说来也有意思,解释器座从立座起就只认类型 3。同一副身子,坐解释器的座能进,坐主程序的座就被拒。咱们如今回头看,身子没毛病,毛病在座位表:程序座还不认这一类身子。

生态那一卷收卷时,这笔单子上挂着名:位置无关的身子至今没有座,开关攥在手里,进路垫在作坊里。盘算里它排在收官大戏那一段。老师傅上门头一天就来讨,大戏的前半场被拽到了开场。当年第一遍,笔者也补过这课,做的是最小的子集:认出类型 3,选一个基址,入口加上基址,程序能跑就算交差。这一遍的单子大了整整一圈,收形态、报需求、挑原点,一样一样都得立成正式机制。

改的就一处,落在解析器量类型的那几行判词上,咱们看它现在的判法:

```cpp
    const std::uint16_t kType    = base::ReadWord16(data, kIdentBytes);
    const std::uint16_t kMachine = base::ReadWord16(data, kIdentBytes + 2);
    const bool          kTypeOk  = role == ImageRole::kInterpreter
                                       ? kType == kTypeShared
                                       : kType == kTypeExecutable || kType == kTypeShared;
```

解释器座的判法一字没动,还是只认共享对象的形态。咱们动的只有程序座,从只收类型 2,放宽成类型 2、类型 3 都收。放宽不等于把门撤了。立这道拒的时候,一台解析器,两张座位那一篇讲过理由:坐错座比参数错重得多,越早拒越干净。今天咱们敢收,是程序座真的学会了安置这一类身子,收进来不是往老座上硬塞,后头有机制接得住。

两种形态的落址法,差别在链址上。类型 2 的链址就是定死的落址,生态那一卷上桌的 hello、busybox 那些老客人,住在 0x400000 上下,原点取零,数值一步不用挪。类型 3 的链址从零起算,而咱们用户地的地址零那一页从来不许映,空指针得在第一脚就踩空,所以原点不能是零,得有人给客人挑一个。这个差别还得报得出来,不然 exec 那头无从知道该不该摇基址。报法是一个新立的小函数,全文就这几行:

```cpp
bool ProgramNeedsLoadBias(const std::uint8_t* data, std::size_t bytes) {
    const base::Result<ElfImage> kParsed = ParseElf64(data, bytes, ImageRole::kProgram);
    if (!kParsed.ok()) {
        return false;
    }
    return base::ReadWord16(data, kIdentBytes) == kTypeShared;
}
```

判词就一行:类型 3,要偏置。装载偏置这个词,咱们当作加在每一条链址上的原点就行。把后半场交给解释器那一卷立原点的时候,主程序的基址是零,加了个寂寞。今天,轮到主程序自己用上非零的原点了。

挑原点的活,咱们交给 exec 那头一个函数:

```cpp
unsigned long long image_load_bias(const unsigned char* program, std::size_t program_size) {
    if (!loader::ProgramNeedsLoadBias(program, program_size)) {
        return 0;
    }
    constexpr unsigned long long kExecSlideMask = 0xFF'000;
    return mm::kUserExecImageBase + (arch::RandomWord() & kExecSlideMask);
}
```

不需要偏置的老形态,原点照旧零,一字不挪。需要的,地板是 512MiB,地板之上再摇一个滑动:掩码把低十二位清零,滑动天生页对齐,顶到头也不超一 MiB。两层地板,咱们写在用户地的布局文件里:

```cpp
inline constexpr unsigned long kUserInterpreterBase = 256_MiB;
inline constexpr unsigned long kUserExecImageBase   = 512_MiB;
```

解释器的家在 256MiB,PIE 客人的地板在 512MiB,坐在解释器家的正楼上。咱们在给解释器一个固定的家那一篇盘过一笔相邻的数:那时候主客人住楼下 0x400000 的小屋,堆贴着它往上长,长过 256MiB 才够得着解释器家门。如今 PIE 的主客人搬到了楼上,老礼数的客人照旧住楼下,三层住户,地盘隔得远远的,谁也啃不着谁。同一位客人每次进门,落点还带一点不同的滑动,不写死。这滑动买的是让落点难猜,存心按地址打主意的人,头一回摸到的门,第二回就对不上了。

偏置挑好,进装载的路也现成:exec 把它塞进 LoadWorld 的 base 一格,装载循环里段的落点、入口、头表落点一律加它。这套算术咱们在给解释器平移 256MiB 的时候用过一回,骨架没变,原点换人当了。那一卷里主程序加了个寂寞的老话,对今天住主座的客人,不再成立。

机制立完,咱们的案卷得跟着语义走。旧案里有两处把拒写死在这一形态上。头一处是拒绝族那一案,一行 `refuse_mangled_word(kGoodImage, 16, 3);` 把第十六字节写成 3,专等解析器翻脸,这一行从册上划掉了。第二处案名就叫 each seat refuses the other's file type,两张座对着拒。重写之后是两案,您看头一案:

```cpp
TEST("elf: the program seat takes the relocatable shape and names its bias need") {
    TestImage relocatable = kGoodImage;
    relocatable.bytes[16] = cinux::loader::kTypeShared;
    const auto kParsed    = cinux::loader::ParseElf64(relocatable.bytes, kImageBytes,
                                                      cinux::loader::ImageRole::kProgram);
    ASSERT_TRUE(kParsed.ok());
    ASSERT_TRUE(cinux::loader::ProgramNeedsLoadBias(relocatable.bytes, kImageBytes));
    ASSERT_TRUE(!cinux::loader::ProgramNeedsLoadBias(kGoodImage.bytes, kImageBytes));
}
```

同一份良构镜像,只把类型栏改成 3:座收下了,偏置需求报了真。没动的那一具,需求报假。咱们一案两问,问的正是新语义的两条腿。另一案单独立给解释器座:

```cpp
TEST("elf: the interpreter seat refuses the fixed-base shape") {
    ASSERT_TRUE(!cinux::loader::ParseElf64(kGoodImage.bytes, kImageBytes,
                                           cinux::loader::ImageRole::kInterpreter)
                     .ok());
}
```

拒还在拒,咱们只是看着它从两张座对拒的一张表里搬出来,单独立了案。生态那一卷收卷时点过这个章法,原话是改断言不是放水,断言跟着真实的行为走。长凳上,ELF 的案卷这一刻十五案,全过。

咱们不妨再问一句:座这么一改,吓不吓着老客人?

类型 2 的路一页没动:原点照旧零,算术照旧老算术。咱们把生态那一卷的实弹验收整个重跑了一遍,glibc 的 hello、十四位 applet、一号座上当家的,全是按老礼数出的 ET_EXEC,一个不惊动,全过。两位老师傅这才算真正在盘上落了脚。

座认了 PIE。咱们桌上下一位排队的客人,是 cc1,四十七兆上下的胖子。装载眼下的礼数还是老一套,整个文件读进内存再开工。这么大的身子要整只搬进屋,后面要出事。
