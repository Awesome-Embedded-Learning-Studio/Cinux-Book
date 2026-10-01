---
title: 06 · 五项的表,一步不能错的序列
description: "三项的表加到五项,L=1 与 D=0 的硬关系,选择子算到 0x18;为什么这一次不需要第二次 lgdt;CR3、PAE、LME、PG、远跳,一步都不能错的序列真编真跑。"
chapter: 5
order: 6
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - gdt
  - long-mode
---

# 五项的表,一步不能错的序列

远跳要带的那个选择子是 `0x18`,所以在推门之前,咱们得让表里真有 `0x18` 那么一项,而且它的 L 位得亮着——GDT 要从三项加到五项了。描述符的解剖学,上一卷咱们已经做全了:八个字节、access 和 limit 的每个字段,咱们全都一位一位认过。所以新东西只剩 flags 那个字节的高半边。您还记得 32 位代码段的 flags 半边是 `0xC`,也就是 G=1、D=1 的组合。而 64 位代码段把它换成了 `0xA`:

```cpp
inline constexpr unsigned char kFlagsLongCode = 0xA;
inline constexpr unsigned char kFlagsLongData = 0x8;

inline constexpr unsigned short kSelectorCode64 = 0x18;
inline constexpr unsigned short kSelectorData64 = 0x20;
```

`0xA` 的二进制是 `1010`:G=1、D=0、L=1。其中的 L 就是 Long 位,它是长模式代码段的身份证——CS 指到了它身上,CPU 才会用 64 位的宽度来译码。而 L=1 的时候 D 必须为 0,Intel 在手册里把 D=0 写成了 L=1 的前提,咱们用断言把两位各验了一遍:

```cpp
static_assert(kTemplate.code64.flags_limit_high == 0xAF);
static_assert(((kTemplate.code64.flags_limit_high >> 5) & 1U) == 1U);
static_assert(((kTemplate.code64.flags_limit_high >> 6) & 1U) == 0U);
```

咱们把 `0xA` 左移四位,再拼上 limit 的高四位 `0xF`,就得到了 `0xAF`。当年第一遍的时候,这一项是手写的一长串十六进制 `0x00AF9A000000FFFF`,咱们对着手册一位一位地核。而这一遍,它成了三个命名位的组合,咱们让工厂函数照着拼、断言照着验。而配套的 data64 用的是 `0x8`,也就是 G=1、D=0 的档位,access 沿用的是数据段的 `0x92`。不过您也看得出来,它在长模式里是个礼仪性的存在:长模式不再看 DS、ES、SS 的段基址和限长,FS 和 GS 的 base 是例外、经 MSR 还能生效,不过咱们眼下只把选择子刷干净。咱们保留它,是为了让选择子的算术保持整齐——`0x18` 的后面顺位就是 `0x20`,落了地以后,五个段寄存器刷的都是同一个值。而段的深究,咱们等内核将来重建自己的大表时再算。

表加到了五项,还牵出一件省心的事:这一次的切换序列里,是没有 lgdt 的。咱们看 `gdt.cpp` 那头,GDTR 的 limit 是 `sizeof(kBootGdt) - 1`,表加到了 40 字节,limit 就自动跟到了 39。而整棵 boot 树里,咱们一共只写过一次 lgdt,就是 16 位世界切换保护模式时的那一次。如今表有了五项,同一条指令装进去的 limit 就自动罩到了表尾,64 位的两项,从那一刻起就在门内候着了。切换序列里自然就没有第二次 lgdt 的位置。

对照当年的写法,咱们看得更清楚:第一遍的同一张表配了两份指针、装卸了两回,6 字节的 `gdt_ptr` 伺候保护模式,10 字节的 `gdt64_ptr` 伺候长模式。而它当年连直接写 `.quad` 的胆子都没有,得写成低 32 位加一段清零的高 32 位,因为 32 位的 ELF 里,一个 `.quad` 的符号地址会生成 64 位重定位,链接器当场就报了错、把它拒了。而在这一遍里,第二次的 lgdt 无处安身,两份指针的包袱也就一起消失了。

切换序列咱们安在 `boot/lm/lm_switch.cpp`,它跟上一卷的切换件是同一个模子:汇编直写、`extern "C"` 立边界、`[[noreturn]]` 收尾:

```cpp
extern "C" [[noreturn]] void EnterLongMode() {
    asm volatile(
        "movl %[pml4], %%eax\n"
        "movl %%eax, %%cr3\n"
        "movl %%cr4, %%eax\n"
        "orl %[pae], %%eax\n"
        "movl %%eax, %%cr4\n"
        "movl %[efer], %%ecx\n"
        "rdmsr\n"
        "orl %[lme], %%eax\n"
        "wrmsr\n"
        "movl %%cr0, %%eax\n"
        "orl %[pg], %%eax\n"
        "movl %%eax, %%cr0\n"
        "ljmp %[code64], %[entry]\n"
        :
        : [pml4] "n"(cinux::boot::lm::kPml4Phys), [pae] "n"(cinux::boot::lm::kCr4Pae),
          [efer] "n"(cinux::boot::lm::kMsrEfer), [lme] "n"(cinux::boot::lm::kEferLme),
          [pg] "n"(cinux::boot::lm::kCr0Pg), [code64] "n"(cinux::boot::gdt::kSelectorCode64),
          [entry] "n"(cinux::boot::kLmEntryVma)
        : "ax", "cx", "dx", "memory");
    __builtin_unreachable();
}
```

咱们一步看一步。开头的两条 `movl` 把 `kPml4Phys` 送进 EAX、再送进 CR3,干的是桥面交接:从此 CR3 就指着 0x1000 的 PML4 了。您可能会问,CR4 和 CR0 咱们都走读-改-写、只动认得的一位,凭什么 CR3 整个换新?因为 CR3 里装的是整张表的位置,咱们要的就是一次全量的替换,不存在只动一位的写法。跟着是 CR4 的读-改-写、只或一个 `0x20`,开的正是 PAE 的那个位。只动一位、别的不碰的习惯,咱们上一卷就立成终身条款了。而再往下的四条,是一组拆不得的小手术:咱们在 ECX 里放好 EFER 的抽屉号,用 `rdmsr` 读出它的当前值,拿 `orl` 把 `0x100` 或进 EAX 的低半、再用 `wrmsr` 写回去。您会注意到 EDX 全程都没被碰过,EFER 的高 32 位就住在那儿,咱们只动 EAX 低半里的其中一位,其余的原样奉还。跟着 CR0 的那轮读-改-写,或上的是 `0x80000000`——咱们推的正是 PG 的门。末了的 `ljmp` 带着选择子 `0x18` 跨过去。

这串 asm 里咱们还有两处门道要看。一处是七个常量全走了 `"n"` 约束当立即数,所以 asm 串里一个魔法数都没有。上一卷咱们给栈顶常数走的就是它。常量的家在头文件里,所以当年那个 `0x100` 若是再写错,它头一个错的就是 `lm.hpp` 里的名字。另一处是 clobber 列了 `ax、cx、dx、memory`,因为 rdmsr 和 wrmsr 踩的就是这三颗寄存器。而中断的门,上一卷的那句 `cli` 到现在还一直关着,序列里连一条 `sti` 都是没有的。所以没有中断表,咱们就不开中断,而这样的纪律分成两半:上一卷隔着一句远跳,这一卷又延长了一句。

序列的次序,咱们拿手册对一遍。Intel SDM 第 9 章印的参考序是 PAE、CR3、LME、PG、远跳,而咱们的代码是 CR3、PAE、LME、PG、远跳,前两步换了位。而两者都合法,因为 PG 之前的这几步,全都是在备料的阶段,装的次序硬件不管。真正不可让步的只有两条。头一条:PG 必须排在所有备料的后面,因为从它亮起的那一拍起,翻译就换人了,门后必须已经有路——上一节那晚的三重故障,就死在了这一步上。另一条:远跳必须排在 PG 的后面,因为 L 位是长模式才认的,而门要是没亮,咱们就带着 `0x18` 跳过去,是不会有好下场的。而名义与事实之间,隔着的正是这一跳——上一卷置了 PE 之后要远跳一次,这里的 PG 之后也要再跳一次、道理跟上一卷是同一个。

序列的尾巴上,`__builtin_unreachable()` 跟上一卷的一样,写的都是那句“没有之后了”。而远跳的目标,您也看到了,写的是一个名叫 `kLmEntryVma` 的数,而不是任何符号。为什么非得是数、这个数又是怎么定下来的,咱们去构建系统里看。
