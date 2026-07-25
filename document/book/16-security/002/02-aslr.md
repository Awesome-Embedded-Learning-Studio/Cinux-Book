---
title: 02 · 三件套落地与为什么代码段没随机化
---

# 三件套落地与为什么代码段没随机化

## 三件套:栈顶 / mmap 起点 / brk 起点

随机化落在 `aslr.hpp` 的三个内联函数上,各回一个页对齐的随机偏移:

```cpp
inline uint64_t aslr_stack_offset() { return g_random.next64() & 0x7FF000ULL; }     // 0 .. 8 MiB
inline uint64_t aslr_mmap_offset()  { return g_random.next64() & 0x3FFFF000ULL; }   // 0 .. ~1 GiB
inline uint64_t aslr_brk_offset()   { return g_random.next64() & 0xFFF000ULL; }     // 0 .. 16 MiB
```

（`aslr.hpp:36` / `:44` / `:51`。）三个掩码末尾都是 `000`——保证结果是页对齐(4 KiB 的整倍数)。三个窗口大小不一样,是按各自的地址区规模定的:mmap 区间最大(上 GiB),栈和 brk 小些。这三个函数是**唯一的随机源出口**,所有随机化都从这儿取,好处是改窗口只动一个地方。

三个消费点,各管一块布局。**栈顶**在 `launch_user_program`(建用户栈、跳用户态的那个函数):

```cpp
const uint64_t stack_top = cinux::arch::USER_STACK_TOP - cinux::lib::aslr_stack_offset();
```

（`user_launch.cpp:44`。）往下那几处(预映射栈基、Stack VMA 的起止、`jump_to_usermode` 的 RSP)全都引用这个局部 `stack_top`,所以改一个局部变量,整条栈路径都跟着随机挪。**mmap** 在 `sys_mmap` 的非 `MAP_FIXED` 分支,把「从哪开始找空闲区」的起点随机化:

```cpp
const uint64_t hint = cinux::arch::USER_MMAP_BASE + cinux::lib::aslr_mmap_offset();
auto area = task->addr_space->vmas().find_free_area(hint, aligned_len);
```

（`sys_mmap.cpp:117`。)`MAP_FIXED` 还照用户指定的地址(那个不随机),只在「用户没指定、内核自己挑」时随机。**brk** 在 `execve` 算堆起点时加一个随机 gap:

```cpp
uint64_t brk_gap = cinux::lib::aslr_brk_offset();
task->brk_initial = brk_start + brk_gap;
```

（`execve.cpp:344`/`:348`。)`brk_initial` 是程序 break 的起点(malloc 长大从这儿开始),给它加随机偏移,堆的地址就每次不同。

> **为什么偏移必须页对齐。** x86_64 的栈 ABI 要求进入函数时 `%rsp % 16 == 8`(CALL 推 8 字节返回地址,被调者开头要对齐到 16)。`stack_top` 减一个**页对齐**的随机值(4 KiB 倍数,本身是 16 的倍数),不会改变 `%rsp` 的对齐——`usermode.hpp` 里那条 `static_assert` 仍然成立。要是偏移随便是个非页对齐的数,栈对齐就破了,用户程序跑着跑着在 SSE 指令上崩。所以三个掩码末尾那几个 0 不是装饰,是 ABI 安全的硬要求。

## 为什么代码段没随机化:一条调研出来的死路

这是这一章最该讲清楚的地方。直觉上 ASLR 最该随机化的是**代码段**(text)——栈/堆/mmap 都动了,text 不动,攻击者照样知道代码在 `0x400000`,能在里头找 ROP gadget。但 Cinux 这一步没动 text,原因是调研出来「直接随机化 text base」是**死路**,不是没想做。

用户程序(以 `shell` 为例)是 **non-PIE** 编译的:`-fno-pie -no-pie -mcmodel=small -static`。small model + non-PIE 的后果是:程序里所有全局变量、字符串字面量的地址,都被编成了 **32 位绝对立即数**。反汇编 `shell` 能看到 `mov $0x4014c0, %edi` 这种——`0x4014c0` 是某个字符串/全局的地址,直接写死在指令里;整个程序近百处 32 位绝对寻址引用(绝大多数是 `mov $0x40????,%reg` 这种立即数,另有少量绝对内存操作数),0 处 RIP-relative。ELF 头是 `ET_EXEC`,入口 `0x400270`,单个 `PT_LOAD` 钉在 `p_vaddr=0x400000`。

这就有问题了。这些绝对立即数全都假设「程序加载在 `0x400000`」。要是 ASLR 把代码段挪到 `0x400000 + 随机偏移`,那些写死的 `0x40????` 全部指向错误的地址——程序一跑到引用全局/字符串的指令就崩。「给 `p_vaddr` 和 `e_entry` 加个 offset」这个最直白的捷径,在 non-PIE + small model 下是**死路**。

要让代码段也能随机化,只有两条真路,都不是捷径:

- **路 A:把用户程序改成 PIE**(`ET_DYN`,代码用 RIP-relative 寻址,base 可挪)+ 内核 ELF loader 处理动态重定位(apply `R_X86_64_RELATIVE`)。现在的 `execve` 只拷贝段、不处理 `.rela.dyn`,得加上重定位逻辑,还得放开 `elf_types.cpp` 里那条「`e_type != ET_EXEC` 就拒」的检查。
- **路 B:不做 text 随机化**(这一步的选择)。

这一步选了路 B,把随机化集中到栈/mmap/brk——这三个不用碰 ELF loader、不用改用户程序编译形态,纯内核侧改动。代价是 text 地址固定,ROP 类攻击防不住。这条路留到单独一步(PIE 迁移 + 内核动态重定位)再走。

所以「代码段没随机化」不是遗漏,是当前用户程序形态(non-PIE + small model + 绝对寻址)下的**必然取舍**——硬要随机化 text,得先把用户程序和 ELF loader 都升级一圈。

## 诚实的边界

**代码段(text/data base)没随机化。** 上面讲过了:non-PIE 绝对寻址把 base 钉死,随机化 text 要先做 PIE 迁移。所以这套随机化的攻击面覆盖是:栈溢出(栈随机了)、mmap 区(mmap 随机了)、堆(brk 随机了);**唯独 text 地址固定,ROP gadget 的位置可预测**。这是已知缺口,不是声称「全 ASLR」。

**栈顶随机化的端到端效果在 `make run` 下看不到。** GUI 模式里 shell 不会自动启动(得在桌面点 Shell 图标),所以默认的 `make run` 根本不触发 `launch_user_program`,日志里看不到 `[PROC] jumping ... stack_top=` 每次不同。但这不意味着栈随机化没生效——`aslr_stack_offset()` 的随机性由 `test_aslr` 验证,`launch_user_program` 必调它,逻辑确定生效。想直观看「每次 stack_top 不同」,跑非 GUI 构建(那种会自动 fork shell)或者翻 test_aslr 的抽样。

**KRandom 没有独立的 PRNG 数学测试。** 这是个记录在案的 test gap:理想情况下该有个 host 单测直接验 xoshiro 序列的数学正确性,但 KRandom 的熵混合依赖 `PIT::get_ticks`(host 单测环境没有 PIT),mock 起来麻烦,所以暂时靠 ASLR 间接验证——ASLR 每次跑地址不同,就证明 `next64` 在推进、非零、在变。PRNG 序列的严格数学验证留 host 单测 follow-up。

验证该看到什么,见配套 lab。下一章(用户态凭证 UID/GID 那一块)接着给用户态加 UID/GID——那是另一条线。
