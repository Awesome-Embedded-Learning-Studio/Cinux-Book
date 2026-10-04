---
title: 06 · 写:内联汇编
---

# 06 · 写:内联汇编,把机器的那几行接进 C++

讲函数门面的那一篇收尾的时候留了一句:那些汇编行眼下都得喂给汇编器过一遍,想让它直接住进 C++ 里,行话叫内联汇编,下一篇就动手。咱们这一篇把这句话兑现。

要还的东西不复杂。咱们写下的汇编字符串会落进编译器吐出的汇编输出:基本 asm 的指令原样照搬,扩展 asm 里的 `%0`、`%%` 还会被替换成具体的寄存器。GCC 只管它周围的那些代码。两边住在同一份函数体里,咱们得让它知道围栏里头动了什么。

## 基本 asm:原样塞进去的那几行

咱们抄起编辑器写个最小的:两个没有操作数、没有返回值的 asm 语句,存成 `e1a_basic.cpp`,让 `g++ -O2 -S` 吐一份汇编出来。

```cpp
int main() {
    asm("nop");
    asm("nop\n\tnop\n\tnop");
    return 0;
}
```

这是基本 asm,括号里只有一串指令,没有别的。咱们把产物搁进 `build/` 这个目录,它得建出来才能往里写。`.s` 也翻出来,找那对书签。屏里的行号和 `N-`、`N:` 前缀都是 `grep -n` 添上的:`-` 是上下文行,`:` 是命中的那一行。往后这类从 `.s` 里截出来的屏,咱们只截跟当前问题有关的那几行,函数收尾的 `.cfi_endproc`、`.size` 之类不一定贴全。

```bash
mkdir -p build
g++ -O2 -S e1a_basic.cpp -o build/e1a_basic_O2.s
grep -n -B1 -A9 '#APP' build/e1a_basic_O2.s
```

```text
9-	.cfi_startproc
10:#APP
11-# 2 "e1a_basic.cpp" 1
12-	nop
13-# 0 "" 2
14-# 3 "e1a_basic.cpp" 1
15-	nop
16-	nop
17-	nop
18-# 0 "" 2
19-#NO_APP
```

`#APP` 和 `#NO_APP` 是 GCC 划的围栏,围栏之间是咱们亲笔写的指令,编译器一个字没动。咱们还会发现一件小事:两条独立的 asm 语句被并进了同一对围栏。围栏内那四行,`# 2 "e1a_basic.cpp" 1`、`# 0 "" 2` 这样成对出现的,是给汇编器和调试信息看的行号标记,咱们读的时候可以跳过。想数自己嵌了几对围栏,写 `grep -c '#APP'`,这一段会回一个 1。要是写 `grep -c 'APP'`,数出来的就是 `APP` 这个串出现了几次,一对围栏含 `#APP` 和 `#NO_APP` 两处,同样的输入会回一个 2。两把尺子各量各的,咱们后面说数就认 `#APP` 这把。老版本 GCC 的书签拼成 `# APP`(中间带空格),GCC 16 不带,咱们手里的 pattern 最好两种拼法都认。

基本 asm 省事,代价也摆在那儿。手册说 GCC 对非空的基本 asm 有个保守假设:它不动任何通用寄存器,可是它可能读写任何全局可见的变量。咱们在机器上能验证这一层:换一个全局变量 `g` 写进 `e1b_basic_global.cpp`,让围栏去动它,两个优化档笔者都跑过一遍,围栏之后那次读 `g` 都落在了真加载上。屏只贴 `-O2` 那一档(`-O0` 档同样,只是前后多几行):

```text
26-#APP
27-# 4 "e1b_basic_global.cpp" 1
28-	movl $99, g(%rip)
29-# 0 "" 2
30-#NO_APP
31-	movl	g(%rip), %eax
32-	ret
```

这段保守是白送的,不过别的门都关着:模板里没有操作数通道,寄存器假设交给编译器,连 `%` 的写法都跟外面不一样。那句假设还会随版本变,老一点的 GCC 换一套行为。前面跑的那两档在本机都没复现出毛病,所以咱们只把它当版本敏感性记着:咱们在这里验不出问题,不代表别人机器上也验不出来。咱们要的是能跟编译器谈条件的手艺,所以往下走。

## 扩展 asm:模板四段,编号从 0 起

想跟编译器谈条件,就得在模板之外再挂几段。完整的形状长这样:`asm(模板 : 输出 : 输入 : clobber)`,四段用冒号隔开,每一段都能空着。空段后面连着还有内容的时候,冒号得照样写出来。输入和 clobber 都空、模板后面再没有别的段,后面那些冒号就可以省掉。咱们把一个最小的例子摆出来,把 42 装进一个变量,存成 `e2a_min.cpp`:

```cpp
int Load42() {
    int x = 0;
    asm("movl $42, %0" : "=r"(x));
    return x;
}
```

模板里那个 `%0` 是笔者给输出占的位置。`"=r"(x)` 分成两部分看:`=` 说的是“这段只负责写”,`r` 说的是“请给我一个通用寄存器”。咱们看看它在 `-O2` 下变成什么:

```text
6:_Z6Load42v:
7-.LFB0:
8-	.cfi_startproc
9-#APP
10-# 3 "e2a_min.cpp" 1
11-	movl $42, %eax
12-# 0 "" 2
13-#NO_APP
14-	ret
```

围栏里 `%0` 换成了 `%eax`。这里最容易想岔的一步来了:咱们别把 `%eax` 读成承诺,分配器挑中它只是为了眼下这一档顺手。换一条命令、换一个函数、换一次内联,挑出来的可能是别的寄存器。咱们摆一个两操作数的现场,模板里同时用 `%0` 和 `%1`,存成 `e2b_ext.cpp`:

```cpp
int AddOne(int in) {
    int out;
    asm("leal 1(%1), %0" : "=r"(out) : "r"(in));
    return out;
}

int main() {
    return AddOne(41);
}
```

咱们看 `-O0` 那一档,独立函数里 `%1` 落在 `%eax`:

```text
5:_Z6AddOnei:
6-.LFB0:
7-	.cfi_startproc
…
13-	movl	%edi, -20(%rbp)
14-	movl	-20(%rbp), %eax
15-#APP
16-# 3 "e2b_ext.cpp" 1
17-	leal 1(%eax), %eax
18-# 0 "" 2
19-#NO_APP
20-	movl	%eax, -4(%rbp)
21-	movl	-4(%rbp), %eax
```

咱们再看 `-O2`:参数原样留在 `%edi` 里,内联进 `main` 的一份拷贝又回到了 `%eax`:

```text
6:_Z6AddOnei:
7-.LFB0:
8-	.cfi_startproc
9-#APP
10-# 3 "e2b_ext.cpp" 1
11-	leal 1(%edi), %eax
12-# 0 "" 2
13-#NO_APP
14-	ret
```

咱们再把内联进 `main` 的拷贝也翻出来:

```bash
g++ -O2 -S e2b_ext.cpp -o build/e2b_ext_O2.s
grep -n -A8 'main:' build/e2b_ext_O2.s
```

```text
22:main:
23-.LFB1:
24-	.cfi_startproc
25-	movl	$41, %eax
26-#APP
27-# 3 "e2b_ext.cpp" 1
28-	leal 1(%eax), %eax
29-# 0 "" 2
30-#NO_APP
```

咱们对着这两屏数一遍:同一个文件、同一条模板,`%1` 在独立函数里落一处,内联进 `main` 的拷贝里又落一处,一处是 `%edi`,一处是 `%eax`。编号规则本身倒是稳的,操作数从 0 起数,输出排在前、输入排在后,`%0` 是输出 `out`、`%1` 是输入 `in`,写模板的人只管位置。

模板里还想写字面的寄存器名,笔画就得反过来,写成 `%%eax` 这样的双百分号。咱们抄一份同时用到操作数编号和字面寄存器名的源码,存成 `e2c_pct.cpp`:

```cpp
int Pct(int in) {
    int out;
    asm("movl %1, %0\n\t"
        "addl $1, %0\n\t"
        "movl %%eax, %%ebx"
        : "=r"(out)
        : "r"(in)
        : "ebx");
    return out;
}
```

咱们把整段交给汇编器,`%%` 会变回单个 `%`,围栏后紧跟的是函数尾声:

```text
6:_Z3Pcti:
7-.LFB0:
8-	.cfi_startproc
9-	pushq	%rbx
…
12-#APP
13-# 3 "e2c_pct.cpp" 1
14-	movl %edi, %eax
15-	addl $1, %eax
16-	movl %eax, %ebx
17-# 0 "" 2
18-#NO_APP
19-	popq	%rbx
```

咱们把 `e2c_pct.cpp` 里的 `%%eax` 写成 `%eax`,另存成 `e2c_bad_pct.cpp` 试试,`%` 后面跟着的东西会被当成操作数编号来读:

```text
e2c_bad_pct.cpp: In function 'int BadPct(int)':
e2c_bad_pct.cpp:3:5: error: invalid 'asm': operand number missing after %-letter
    3 |     asm("movl %1, %0\n\t"
      |     ^~~
e2c_bad_pct.cpp:3:5: error: invalid 'asm': operand number missing after %-letter
exit_status=1
```

编译器当场把这次尝试拦了下来,咱们收到的诊断是“`%` 后面缺一个操作数编号”。这层错在编译前端,连汇编器都到不了。末尾那行 `exit_status=1`,是咱们在命令后头接了一句 `echo "exit_status=$?"` 报出来的,不是编译器打的。

## 约束:值住寄存器,还是住内存

`r` 说的是“给我一个通用寄存器”,它有个走另一条路的兄弟 `m`:操作数不进寄存器,直接落在内存上。同一个语义、同一行模板,咱们只把约束字母从 `+r` 换成 `+m`,两份源码的差别就落在约束串上。一份写一个文件,存成 `e3_r.cpp`:

```cpp
int BumpR(int x) {
    asm("incl %0" : "+r"(x));
    return x;
}
```

另一份咱们只换约束字母,存成 `e3_m.cpp`:

```cpp
int BumpM(int x) {
    asm("incl %0" : "+m"(x));
    return x;
}
```

咱们把两份源码各自编成目标文件,再把反汇编倒出来(产物照旧搁进 `build/`):

```bash
g++ -O2 -c e3_r.cpp -o build/e3_r_O2.o && objdump -d build/e3_r_O2.o
g++ -O2 -c e3_m.cpp -o build/e3_m_O2.o && objdump -d build/e3_m_O2.o
```

```text
0000000000000000 <_Z5BumpRi>:
   0:	89 f8                	mov    %edi,%eax
   2:	ff c0                	inc    %eax
   4:	c3                   	ret
```

```text
0000000000000000 <_Z5BumpMi>:
   0:	89 7c 24 fc          	mov    %edi,-0x4(%rsp)
   4:	ff 44 24 fc          	incl   -0x4(%rsp)
   8:	8b 44 24 fc          	mov    -0x4(%rsp),%eax
   c:	c3                   	ret
```

`+r` 版全程活在寄存器里,`inc %eax` 两个字节 `ff c0` 就完事。`+m` 版把 `%edi` 在 `-4(%rsp)` 安了家,`incl -0x4(%rsp)` 是内存形态,编码 `ff 44 24 fc` 四字节,加完再取回 `%eax`。哪个好,看指令有没有寄存器形态,`incl` 两种都行,那当然寄存器快。往后咱们会碰上只有内存形态的指令,像装描述表的 `lgdt`、`invlpg` 这类,它们的操作数天生只能落在内存,那时候 `m` 就是唯一的路。这里还漏出一句行话:`-4(%rsp)` 落在红区里(讲栈那一篇交代过的那 128 字节),只有不再往下 call 谁的叶子函数才敢白用。这层福利只归用户态,进了内核就被关掉。

## 漏掉 `"memory"` 的那个 bug

四段里的最后一段 clobber,咱们用一个真错来认识它。下面这段源码存成 `e4_bug.cpp`,它的意图很直白:把 `g` 的当前值收进 `first`,让 asm 往 `g` 里存一个 99,回手再把 `g` 收进 `second` 打印出来。输出和输入两段都空着,所以模板后面只跟了两个冒号,第四段还没有影子。咱们照它跑,它会给出一份错答案:

```cpp
#include <cstdio>

int g = 1;

int main() {
    int first = g;
    asm("movl $99, g(%%rip)" ::);
    int second = g;
    std::printf("first=%d second=%d\n", first, second);
    return 0;
}
```

笔者在 asm 里明明白白把 99 存进 `g`,回手再读一次,`-O2` 下跑出来是这样:

```bash
g++ -O2 e4_bug.cpp -o build/e4_bug_O2 && build/e4_bug_O2; echo "exit_status=$?"
```

```text
first=1 second=1
exit_status=0
```

没有崩溃,没有警告,退出码干干净净的 0,`second` 就是 1。您别急着看修法,咱们按老办法翻 `.s` 找机制,汇编里那几行关键代码在这儿:

```text
10:main:
11-.LFB15:
12-	.cfi_startproc
13-	subq	$8, %rsp
14-	.cfi_def_cfa_offset 16
15-	movl	g(%rip), %esi
16-#APP
17-# 7 "e4_bug.cpp" 1
18-	movl $99, g(%rip)
19-# 0 "" 2
20-#NO_APP
21-	leaq	.LC0(%rip), %rdi
22-	movl	%esi, %edx
23-	xorl	%eax, %eax
24-	call	printf@PLT
```

真相在这一行里:第一次读 `g` 落进了 `%esi`,围栏之后的第二次读压根没有发生,GCC 直接把 `%esi` 顶了上去。它还给了自己一个很像样的理由:咱们只写了 `::`,clobber 段是空的,那 GCC 凭什么认为围栏动过内存。它敢拿围栏之前知道的旧值顶上,这次是咱们的申报漏了它。

咱们换一换优化档,这事还有花样:

```text
-O0  →  first=1 second=99
-O1  →  first=1 second=1
-O2  →  first=1 second=1
-O3  →  first=1 second=1
```

`-O0` 老老实实每次读都摸内存,`-O1` 起就翻了车。“没优化没事,一优化出事”这句话,咱们往后夜里还会遇上它。

咱们换个写法还能更狠。下面这段存成 `e4_bug_b.cpp`,还是让 asm 去写局部变量,只是改成经输入指针进去写:输出段空着,输入段挂的是变量地址,末尾照旧空着:

```cpp
#include <cstdio>

int main() {
    int x = 1;
    int first = x;
    asm("movl $99, (%0)" :: "r"(&x));
    int second = x;
    std::printf("first=%d second=%d\n", first, second);
    return 0;
}
```

它在 `-O2` 下也打印 `first=1 second=1`。咱们翻 `.s` 看,围栏之后第二次读连加载都不见了:

```bash
g++ -O2 -S e4_bug_b.cpp -o build/e4_bug_b_O2.s && grep -n -A18 'main:' build/e4_bug_b_O2.s
```

```text
21-# 6 "e4_bug_b.cpp" 1
22-	movl $99, (%rax)
23-# 0 "" 2
24-#NO_APP
25-	xorl	%eax, %eax
26-	movl	$1, %edx
27-	movl	$1, %esi
28-	leaq	.LC0(%rip), %rdi
```

咱们看到第二次读的结果被 GCC 直接折成了立即数,连内存都不碰。机制是同一个:它拿围栏之前知道的值顶上,至于是挪用寄存器还是折算成常量,随版本和现场变。

修法只有一行,咱们把空着的 clobber 段补上 `"memory"`。做法是把 `e4_bug.cpp` 原样另存为 `e4_fixed.cpp`,只把那一行的 `::` 补成 `::: "memory"`:

```cpp
    asm("movl $99, g(%%rip)" ::: "memory");
```

模板里的指令一个字没动,咱们拿同一个 `-O2` 再跑一遍,这回的输出是:

```bash
g++ -O2 e4_fixed.cpp -o build/e4_fixed_O2 && build/e4_fixed_O2; echo "exit_status=$?"
```

```text
first=1 second=99
exit_status=0
```

这一屏里两版只差一行,咱们并排看:

```text
21-	leaq	.LC0(%rip), %rdi
22-	movl	%esi, %edx
23-	xorl	%eax, %eax
24-	call	printf@PLT
```

```text
21-	movl	g(%rip), %edx
22-	leaq	.LC0(%rip), %rdi
23-	xorl	%eax, %eax
24-	call	printf@PLT
```

咱们把两版摆在一起看:错版围栏后是 `movl %esi, %edx`,复用围栏之前那次加载的结果。对版围栏后是 `movl g(%rip), %edx`,老老实实重读了一次。一行的申报,换回一次真加载。手册把这层意思写得很重:`"memory"` 告诉编译器,这块 asm 对操作数之外的内存也动过手,编译器不再假设 asm 之前读过的内存值仍然有效,需要的时候它会重新加载。手册说它“为编译器构成了一道读写的内存屏障”。

手册紧接着划了一道界:这道屏障管的是编译器,处理器自己往 asm 之后做推测读,它拦不住。处理器那边的乱序另有 fence 一类的东西管着,咱们在这儿不展开。

## volatile:让它照写的样子留下

您大概听过一句“加个 `volatile` 就稳了”。咱们现场验一验它到底保什么,来看一段有寄存器输出、可那个输出之后没人再用的 asm,存成 `e5_drop2.cpp`,编成 `-O2` 的汇编:

```cpp
#include <cstdio>

int main() {
    int x = 0;
    asm("movl $42, %0" : "=r"(x));
    std::printf("done\n");
    return 0;
}
```

```bash
g++ -O2 -S e5_drop2.cpp -o build/e5_drop2_O2.s
```

```text
14-	.cfi_def_cfa_offset 16
15-	leaq	.LC0(%rip), %rdi
16-	call	puts@PLT
17-	xorl	%eax, %eax
18-	addq	$8, %rsp
19-	.cfi_def_cfa_offset 8
20-	ret
21-	.cfi_endproc
22-.LFE15:
```

围栏一对都没有,GCC 把整段 asm 扫了出去,程序照跑,照打印 `done`,删没删从外面看不出来。咱们把 `volatile` 给同一段 asm 加上,别的都不动,另存成 `e5_keep2.cpp`,编成另一份 `-O2` 的汇编:

```cpp
#include <cstdio>

int main() {
    int x = 0;
    asm volatile("movl $42, %0" : "=r"(x));
    std::printf("done\n");
    return 0;
}
```

```bash
g++ -O2 -S e5_keep2.cpp -o build/e5_keep2_O2.s
```

围栏就回来了,还是用 `#APP` 那把尺子,这次咱们数得到:

```text
14-	.cfi_def_cfa_offset 16
15-#APP
16-# 5 "e5_keep2.cpp" 1
17-	movl $42, %eax
18-# 0 "" 2
19-#NO_APP
20-	leaq	.LC0(%rip), %rdi
21-	call	puts@PLT
```

尺子本身也就是咱们手里这两行 `grep -c`:

```text
$ grep -c 'APP' build/e5_drop2_O2.s        # → 0
$ grep -c 'APP' build/e5_keep2_O2.s        # → 2
```

两版跑起来输出一模一样,差别全在 `.s` 里。往后咱们写端口 IO 这类全靠副作用的指令,全指着它给咱们保命。

它在循环里的表现更直观。咱们把同一段 asm 放进循环体,存成 `e5_loop.cpp`:

```cpp
#include <cstdio>

int main() {
    int x = 0;
    for (int i = 0; i < 3; ++i) {
        asm("movl $42, %0" : "=r"(x));
        std::printf("done\n");
    }
    return 0;
}
```

同一段再加上 `volatile`,咱们另存成 `e5_loop_v.cpp`:

```cpp
#include <cstdio>

int main() {
    int x = 0;
    for (int i = 0; i < 3; ++i) {
        asm volatile("movl $42, %0" : "=r"(x));
        std::printf("done\n");
    }
    return 0;
}
```

咱们各编一份 `-O2` 的汇编:

```bash
g++ -O2 -S e5_loop.cpp -o build/e5_loop_O2.s
g++ -O2 -S e5_loop_v.cpp -o build/e5_loop_v_O2.s
```

不加 `volatile`,`-O2` 把整段从循环体里删了,咱们在整个 `.s` 里一对围栏都数不着。同一段加上 `volatile`,围栏就留在循环标签底下。两条尺子的读数:

```text
$ grep -c 'APP' build/e5_loop_O2.s        # → 0
$ grep -c 'APP' build/e5_loop_v_O2.s      # → 2
```

还有两处边界得记清。输出是 `"=m"` 的那段,咱们存成 `e5_drop2_m.cpp`:

```cpp
#include <cstdio>

int main() {
    int x = 0;
    asm("movl $42, %0" : "=m"(x));
    std::printf("done\n");
    return 0;
}
```

咱们看到的是它即使没人消费也没删,往内存里写被它当成副作用保守留了下来。没有输出操作数的基本 asm 存成 `e5_basic.cpp`:

```cpp
#include <cstdio>

int main() {
    asm("nop");
    std::printf("done\n");
    return 0;
}
```

它隐含 `volatile`,咱们写 `asm("nop")` 不写关键字也不会被删。两份照旧各编一份 `-O2` 的汇编:

```bash
g++ -O2 -S e5_drop2_m.cpp -o build/e5_drop2_m_O2.s
g++ -O2 -S e5_basic.cpp -o build/e5_basic_O2.s
```

咱们就用这两把尺子照样读得出:

```text
$ grep -c 'APP' build/e5_drop2_m_O2.s     # → 2:输出是 "=m"、同样没人消费,本机不删
$ grep -c 'APP' build/e5_basic_O2.s       # → 2:基本 asm 无 volatile 也不删
```

那么加上 `volatile` 能不能救前面那个内存 bug?咱们把上面那段错源码的 asm 改成 `volatile`,另存成 `e5_vol_mem.cpp`,clobber 段照旧空着:

```cpp
#include <cstdio>

int g = 1;

int main() {
    int first = g;
    asm volatile("movl $99, g(%%rip)" ::);
    int second = g;
    std::printf("first=%d second=%d\n", first, second);
    return 0;
}
```

咱们拿 `-O2` 跑出来是这样(末尾那行 `exit_status=0` 同样是 `echo "exit_status=$?"` 报的):

```bash
g++ -O2 e5_vol_mem.cpp -o build/e5_vol_mem_O2 && build/e5_vol_mem_O2; echo "exit_status=$?"
```

```text
first=1 second=1
exit_status=0
```

咱们跑出来的还是 `first=1 second=1`。翻 `.s` 看,`volatile` 只把围栏保住了,围栏之后依旧是那一行:

```text
22-	movl	%esi, %edx
```

`volatile` 留住的只有这段代码本身,周围的内存它管不着。手册还说:`volatile asm` 一样可能被编译器相对别的代码挪动,连跨跳转指令挪动都不排除。谁在管周围的内存,咱们前一段刚交代过,是 `"memory"`。

## clobber 里还能登记什么

clobber 段不止 `"memory"` 一个名字。咱们在 asm 里碰过的寄存器,得按名字如实报给编译器。名字的写法有讲究,咱们把三种拼法各写一遍,放进同一个文件存成 `e6_clobname.cpp`:

```cpp
int ClobEb() {
    int r;
    asm("movl $42, %0" : "=r"(r) : : "ebx");
    return r;
}

int BadName() {
    int r;
    asm("movl $43, %0" : "=r"(r) : : "%%ebx");
    return r;
}

int BadRsp() {
    int r;
    asm("movl $44, %0" : "=r"(r) : : "rsp");
    return r;
}
```

咱们把它递给编译器,`"%%ebx"` 吃了报错,`"rsp"` 吃了警告和一条附注:

```bash
g++ -O2 -S e6_clobname.cpp -o build/e6_clobname_O2.s; echo "exit_status=$?"
```

```text
e6_clobname.cpp: In function 'int BadName()':
e6_clobname.cpp:9:5: error: unknown register name '%%ebx' in 'asm'
    9 |     asm("movl $43, %0" : "=r"(r) : : "%%ebx");
      |     ^~~
e6_clobname.cpp: In function 'int BadRsp()':
e6_clobname.cpp:15:5: warning: listing the stack pointer register 'rsp' in a clobber list is deprecated [-Wdeprecated]
   15 |     asm("movl $44, %0" : "=r"(r) : : "rsp");
      |     ^~~
e6_clobname.cpp:15:5: note: the value of the stack pointer after an 'asm' statement must be the same as it was before the statement
exit_status=1
```

末尾那行 `exit_status=1` 也是咱们那句 `echo "exit_status=$?"` 报的,不是编译器打的。

规范写法是不带 `%` 的名字,写 `"ebx"`。咱们把同一个写法换上一对单百分号,跟 `ClobEb()` 一起存进 `e6b.cpp`:

```cpp
int ClobEbPct() {
    int r;
    asm("movl $43, %0" : "=r"(r) : : "%ebx");
    return r;
}
```

它和上面那个 `ClobEb()` 是同一个文件 `e6b.cpp` 里的两个函数,材料里本来就是这么放的。咱们看到 `"ebx"` 和 `"%ebx"` 两个在本机 GCC 16 上都编译得过,只有 `"%%ebx"` 报了 `unknown register name`。“带 `%` 必报错”这话经不起检验,报错的是双百分号。

咱们报上名字之后,GCC 会绕开它给操作数挑寄存器,该保存的时候它自己包一份保存和还原。下面这一屏取自 `e6b.cpp` 的 `_Z6ClobEbv`(文件头略去,标签到 `#APP` 之间是 GCC 自己垫的 `.cfi_*` 与一条 `pushq %rbx`):

```text
	.cfi_startproc
	pushq	%rbx
	.cfi_def_cfa_offset 16
	.cfi_offset 3, -16
#APP
# 3 "e6b.cpp" 1
	movl $42, %eax
# 0 "" 2
#NO_APP
	popq	%rbx
	.cfi_def_cfa_offset 8
	ret
```

`%rbx` 是约定里被调方必须原样还原的那一拨,咱们报了名字,GCC 就自己押上了这对 push 和 pop。调用约定把寄存器分成两拨:`%rbx`、`%rbp`、`%r12` 到 `%r15` 是被调方必须原样还原的,`%rax`、`%rcx`、`%rdx`、`%rsi`、`%rdi`、`%r8` 到 `%r11` 是临时寄存器,谁用谁负责。`%rsp` 这边只在上面留了个废弃警告,真正要守的规范是进出等值,asm 前后栈指针必须一样。

## 能用 C 写,就别劳驾汇编

手艺到手了,泼冷水的忠告也该到位。GCC 手册自己承认它不解析咱们模板里的指令,写错了没有编译期的体检,验收的路只剩看字节一条。多条独立的 asm 也不保证编译后还紧挨着,必须连续的指令请咱们写进同一个字符串。Linux 内核的代码风格文档大意说得更直白:C 能干的活就别劳驾内联汇编,常见的一小段可以包成一个 C 小函数,大块的汇编进单独的 `.S` 文件。咱们这一篇学的约束、clobber、`volatile` 申报纪律,本身说的就是这件事,每一样都在替 GCC 补它看不见的信息。内联汇编的正经用场,是 C++ 实在表达不了的那几行:端口 IO、停机、开关中断,咱们数得过来。

没铺开的,咱们照例摆在桌面上:浮点和 SIMD 的约束字母、`asm goto` 跳 C 标签、`%=` 自制不重号的局部标号、`%P` 这类操作数修饰符、双方言花括号,手册里都有,咱们用到再取。`%` 后面跟数字或以 `[` 开头的才是操作数引用,这一点两套 asm 各有一套写法。clobber 里还留了个 `"redzone"`,您在 asm 里用 call、push 的时候跟它有关系,这里只留个名字,不给行为断言。

再往前的路,是机器刚上电、内存还没铺好的地方,那里连栈都得咱们亲手摆,内联汇编的用场会比今天多得多。
