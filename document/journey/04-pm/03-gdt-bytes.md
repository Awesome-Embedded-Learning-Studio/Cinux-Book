---
title: 03 · 八个字节,一位一位认
description: "packed 结构体逐字段对上硬件的八个字节,0x9A 一位一位认,limit 两段拼出 4GB;constexpr 工厂让当年注释里的笔误无处藏身。"
chapter: 4
order: 3
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - gdt
  - static-assert
---

# 八个字节,一位一位认

GDT 的每个条目,在内存里就是连着的八个字节。八个字节的排布是硬件定的,咱们没有商量的余地,只能照单全收了。而当年第一遍,咱们是拿汇编的 `.word`、`.byte` 一个一个手码出来的,而这一遍咱们用 C++ 的结构体把它重新落了一遍,好处您后面就看到了。结构体长的是这个样子:

```cpp
struct [[gnu::packed]] SegmentDescriptor {
    unsigned short limit_low;
    unsigned short base_low;
    unsigned char  base_mid;
    unsigned char  access;
    unsigned char  flags_limit_high;
    unsigned char  base_high;
};
```

您一眼就能看出这排布有多拧巴:base 被切成了三段,limit 也被切成了两段,好好的两个字段,愣是散在了各处。这是 x86 一路兼容下来背的历史包袱,字段的排布得迁就当年 286 的十六位格式,后来 386 扩到了 32 位,新添的位就只能塞进旧格式的缝隙里了。咱们照着排就是,`[[gnu::packed]]` 会把编译器想垫的对齐字节全部赶走,保证六个成员严丝合缝地占满八个字节。这个手法咱们不是头一回用了,从 DAP、E820 条目到 VESA 的两份缓冲,排的都是同一副牌面。

咱们拿代码段逐位看一遍。access 字节取的是 `0x9A`,写成二进制的样子就是 `1001 1010`。从高位往低位数:开头的 `1` 是 P 位,声明了段是有效的、CPU 才肯用这个段。接着的两位 `00` 是 DPL,也就是特权级了,咱们是内核,填了零。再一位的 `1` 是 S 位,说明它属于代码或数据段的阵营,而不是系统段——系统段是门描述符、TSS 那类给 CPU 自己用的结构,编号跟咱们用的不通用。末四位的 `1010` 是 type,头一位的 1 圈定了它是代码段,后面拼出了可执行、可读。数据段只把 access 换成了 `0x92`,type 就变成了 `0010`:数据段、可写。两相对照下来您就看清了,可执行和可写的一对权限,靠的就是两个 access 字节。您可以亲手拨一拨 access 的两个字节,点中了哪一段,亮的便是哪几位:

<BitFieldViewer
  caption="access 字节:代码段与数据段对照"
  :variants="[
    { label: 'code · 0x9A', value: 0x9A },
    { label: 'data · 0x92', value: 0x92 },
  ]"
  :fields="[
    { bits: 7, name: 'P', desc: '声明了段是有效的,CPU 才肯用这个段。' },
    { bits: [6, 5], name: 'DPL', desc: '特权级。咱们是内核,填了零。' },
    { bits: 4, name: 'S', desc: '属于代码或数据段的阵营,而不是系统段(门描述符、TSS 那类)。' },
    { bits: [3, 0], name: 'Type', desc: '头一位圈定阵营,后面拼权限:代码段是可执行、可读,数据段是可写。' },
  ]"
/>

再往上的一位是 `flags_limit_high`,咱们填的值是 `0xCF`。这个字节里挤着的是两样东西:高四位是 flags,低四位是 limit 的尾巴,咱们把尾巴留到回头再认。flags 的取值是 `1100`,咱们照 access 那套从高位往低位数的法子认:开头的 `1` 是 G 位,它是 1 的时候,限长就按 4KB 的粒度记了。接着的 `1` 是 D 位,它置了 1,这个段默认的就是 32 位的操作数,咱们的新世界全靠它。再往下的 `0` 是 L 位,它得置了 1 才算 64 位的代码段,那是下一站长模式的事,咱们眼下填零。末尾的一位叫 AVL,available 的缩写,硬件不看它、留给系统软件自己记事用的,咱们也填零。这半个字节您同样可以亲手拨一拨:

<BitFieldViewer
  caption="flags_limit_high 字节:高四位 flags,低四位 limit 高位"
  :variants="[
    { label: 'flags+limit · 0xCF', value: 0xCF },
  ]"
  :fields="[
    { bits: 7, name: 'G', desc: '置 1,限长按 4KB 的粒度记。' },
    { bits: 6, name: 'D', desc: '置 1,段默认 32 位的操作数。' },
    { bits: 5, name: 'L', desc: '置 1 才是 64 位代码段,长模式的事,本站填零。' },
    { bits: 4, name: 'AVL', desc: '硬件不看,留给系统软件记事,本站填零。' },
    { bits: [3, 0], name: 'Limit', desc: 'limit 20 位里的最高四位,填 1。' },
  ]"
/>

咱们再看低四位,它们是 limit 的 19:16 位,填的全是 1。为什么要这么记呢?因为 limit 是 20 位的:limit 的低 16 位住在 `limit_low` 里,高 4 位藏在 flags 字节的低半边,两段拼起来就成了 `0xFFFFF`,再配上 G 位的 4KB 粒度,真实的限长就是:

```text
0xFFFFF × 0x1000 + 0xFFF = 4GB − 1
```

正好把 32 位的地址空间整圈圈了进来。base 的三段全是零,连同上面讲的两条,咱们就把扁平模型的秘密交代完了。咱们顺手把算好的 4GB 记作 `kFlatLimitBytes`,它的值正是 `0xFFFFFFFF`,后面的测试还要拿它对数呢。

对这些字节咱们不手码了,前面一位一位认过的每样东西,都交给一个 constexpr 工厂去落地:

```cpp
constexpr SegmentDescriptor MakeFlatDescriptor(unsigned char access) {
    return SegmentDescriptor{
        .limit_low        = 0xFFFF,
        .base_low         = 0x0000,
        .base_mid         = 0x00,
        .access           = access,
        .flags_limit_high = static_cast<unsigned char>((kFlagsFlat32 << 4) | 0x0F),
        .base_high        = 0x00};
}
```

咱们把六个字段挨个数一遍,别的要么是直给的字面值,要么是外部送进来的 access 参数,而轮到 `flags_limit_high`,咱们就得把两样东西装进同一个字节里,靠的还是两步小算术。咱们把式子分两截看:`kFlagsFlat32` 的值,就是咱们刚才逐位认过的 `1100`,拿 `<< 4` 把它从低半边推上了高半边,也就住进了 flags 的位置。`| 0x0F` 填的是低半边,把 limit 的 19:16 位填成了全 1。前面说 limit 两段拼起来的时候,还是纸面上的算式,而到了这一行,拼接才真的落进了字节,`0xCF` 就是这么拼出来的。

它只收一个 access 参数、其余的字段就全部写死了。然后 `MakeBootGdt()` 用它把三项拼成了整张表。这么绕一圈图什么呢?图的是这些数字从此有了两个身份。编译期里的身份是头一份,`constexpr` 的产物可以直接拿去喂 `static_assert`,咱们在头文件里立的就是这么一排:

```cpp
static_assert(sizeof(SegmentDescriptor) == 8);
static_assert(sizeof(BootGdt) == 24);
static_assert(sizeof(DescriptorTablePointer) == 6);

constexpr BootGdt kTemplate = MakeBootGdt();

static_assert(kTemplate.code.base_low == 0 && kTemplate.code.base_mid == 0 &&
              kTemplate.code.base_high == 0);
static_assert(kTemplate.code.access == 0x9A && kTemplate.data.access == 0x92);
static_assert(kTemplate.code.flags_limit_high == 0xCF);
```

这一排咱们从上往下认。头三条闸的是尺寸,咱们念出来是描述符 8 字节、整表 24 字节、表指针 6 字节。正中间 `kTemplate = MakeBootGdt()` 是接线的一句,咱们靠它把工厂的产物接成一份常量,底下三条读的就全是它了。您看 base 的一行,`&&` 把三段连成了一行,问的是 code 段的 base_low、base_mid、base_high 是不是全零,往下 access 的一行对的是 `0x9A` 和 `0x92`,末行认的是 flags 字节的 `0xCF`,一个数字咱们都不许它错。

data 段的 base 三段,断言倒是没有逐段去数。这里少查的两段不是漏:工厂函数里,base 的三段是写死的零,access 参数动不了它们,code 段的三段咱们已经验过全是零,data 段再核一道 base_low、认下它同样是工厂的出品,三段就全兜住了——测试里的那道 base_low 核对,跟工厂函数走的正是同一条来路。而运行期里,同一份工厂的产物落进镜像,`lgdt` 读的就是它了。检查的东西和被检查的东西出自同一次计算,也就不存在两张皮了。

这里笔者要翻的旧事,就出在当年那版的汇编里。代码段 base 的注释写的是 0x8000,而实际编出来的字节是零。代码是对的——扁平模型的 base 必须是零,注释反而是笔误了。可当年咱们对着源码排查的时候,注释就是仅有的线索,笔者顺着它白白怀疑了一圈正确的代码。手写的注释和手拼的字节各说各话,谁也管不了谁。到了这一遍,断言和测试用的 `kTemplate` 就住在头文件里,镜像里的实体 `kBootGdt` 直接初始化自 `kTemplate`,两份是天生同源的。想动 base?能改的地方只有工厂一处,改出来的值还得过断言那一关,那类笔误在这一遍就没有落脚的地方了。

表立好了,咱们还得让 CPU 知道它住在哪儿。`lgdt` 指令要的是一个六字节的小结构:16 位的 limit 加 32 位的 base,也就是咱们常写的 GDTR:

```cpp
extern "C" cinux::boot::gdt::DescriptorTablePointer const kGdtr = {
    .limit = sizeof(kBootGdt) - 1,
    .base  = reinterpret_cast<unsigned int>(&kBootGdt),
};
```

limit 记的是表长减一,24 字节的表就写 23,这是硬件的约定。base 里的那个指针转整数,咱们拿 `reinterpret_cast` 把它写得明明白白,还给它配了一块 NOLINT。这个转换干的事很具体:咱们把“表的地址”从指针的世界里请了出来,交给一个要被 `lgdt` 整体吞下的 POD。这个对象在 16 位的编译世界里落进了 `.rodata`,base 字段走的是一条纯重定位。重定位说的就是链接器的一道活:把符号的最终地址填进字段里。它在链接期就解析成了真值,而运行期一个构造都不跑。“boot 里不许有非平凡的全局构造”的纪律,是从第一遍的教训里得来的,这一遍照旧站稳了。
