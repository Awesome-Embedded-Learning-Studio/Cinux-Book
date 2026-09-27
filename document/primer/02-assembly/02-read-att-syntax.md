---
title: 02 · 读:AT&T 语法最小集
---

# 02 · 读:AT&T 语法最小集,把一段 AT&T 汇编从头读到尾

您机器上正常编链接出来的每一个 C++ 程序,真正的入口都不在您写的 `main` 里,而在系统自带的启动文件里,那儿住着一段名叫 `_start` 的代码。咱们拿 `Scrt1.o` 开刀反汇编出来(PIE 的意思是“位置无关可执行”,链接器如今的默认出品。不开 PIE 的链接,用的几乎是它双生的 `crt1.o`,咱们看哪一份都行):

```bash
objdump -d "$(g++ -print-file-name=Scrt1.o)"
```

输出里 `_start` 的那部分,咱们原样截下来(不同版本的 glibc,也就是 GNU 的 C 标准库,这几行的长相也许有一两处不一样,认得出它的模样就没问题):

```text
0000000000000000 <_start>:
   0:	f3 0f 1e fa          	endbr64
   4:	31 ed                	xor    %ebp,%ebp
   6:	49 89 d1             	mov    %rdx,%r9
   9:	5e                   	pop    %rsi
   a:	48 89 e2             	mov    %rsp,%rdx
   d:	48 83 e4 f0          	and    $0xfffffffffffffff0,%rsp
  11:	50                   	push   %rax
  12:	54                   	push   %rsp
  13:	45 31 c0             	xor    %r8d,%r8d
  16:	31 c9                	xor    %ecx,%ecx
  18:	48 8b 3d 00 00 00 00 	mov    0x0(%rip),%rdi        # 1f <_start+0x1f>
  1f:	ff 15 00 00 00 00    	call   *0x0(%rip)        # 25 <_start+0x25>
  25:	f4                   	hlt
```

`$0xfffffffffffffff0` 念都念不顺的这一长串,它是什么意思呢?`0x0(%rip)` 的括号里住的又是谁?另外两桩小困惑咱们当场就能打发掉:“endbr64” 是新一点 CPU 在开头盖的安全记号,咱们读码的时候跳过它就好,还有 `call` 前面的那颗星,它说的是“目标地址装在括号那块内存里”。余下的疑问,咱们从后缀说起,一路拆到收尾的验收:那一段编译器亲手编出来的函数,正等着您把它整段读穿。

## 后缀 b/w/l/q:给指令报尺码

`mov` 干的是搬运的活,一趟搬几个字节的事,得您用后缀告诉它:

```text
b = byte     1 字节
w = word     2 字节(x86 的 word 就是 2 字节)
l = long     4 字节
q = quad     8 字节
```

您猜一猜,四条只差后缀的 `mov`,编出来的字节会差在哪儿?咱们当场编出来看:

```bash
cat > suf.s <<'EOF'
.text
movb $0x12, %al
movw $0x1234, %ax
movl $0x12345678, %eax
movq $0x12345678, %rax
EOF
as suf.s -o suf.o
objdump -d suf.o
```

```text
0000000000000000 <.text>:
   0:	b0 12                	mov    $0x12,%al
   2:	66 b8 34 12          	mov    $0x1234,%ax
   6:	b8 78 56 34 12       	mov    $0x12345678,%eax
   b:	48 c7 c0 78 56 34 12 	mov    $0x12345678,%rax
```

`movb` 只用了两个字节,`movq` 却撑到了七个,这四趟搬运里尺码越大的,字节也就跟着多了。您要是猜的是“每条多一个字节”,看到 `movw` 只多出了两个,答案也就露面了:管宽度的原来是前缀,`movw` 开头的 `66`、`movq` 开头的 `48`,都是一路的。还有 `$0x1234` 躺成的 `34 12`,还是那个小端的姿势,上一篇咱们也见过它了。

寄存器在场的时候,后缀其实可以省,`as` 从 `%ax` 的尺码就能推出宽度,您看 `_start` 里那行 `xor %ecx,%ecx` 连后缀都没带。反汇编的文本常常把后缀省掉,而 GCC 写 `.s` 源码的时候,后缀是一行行全带着的,`g++ -S` 的产出就是这样,咱们随手截四行:

```text
	pushq	%rbp
	movq	%rsp, %rbp
	movl	%edi, -4(%rbp)
	movl	%esi, -8(%rbp)
```

## 源在左,目的在右

排在头一个的操作数是源,排在第二个的才是目的地。Intel 语法正好把顺序倒了个个儿,两副面孔的迷惑,咱们上一篇已经挨过一回了。咱们编一对出来,亲手感受一下源的方向:

```bash
cat > ord.s <<'EOF'
.text
movq %rax, %rbx
movq %rbx, %rax
EOF
as ord.s -o ord.o
objdump -d ord.o
```

```text
0000000000000000 <.text>:
   0:	48 89 c3             	mov    %rax,%rbx
   3:	48 89 d8             	mov    %rbx,%rax
```

您写的是 `%rax` 给 `%rbx`,读回来的 `mov %rax,%rbx` 头一个还是源,`as` 的读法咱们可以放心信。字节里 `c3` 和 `d8` 换了个位置,靠的也只是源与目的的对调。真码(系统里真跑着的那些代码)里的例子随手就能捞到:`_start` 的 `mov %rdx,%r9`,读法就是把 `rdx` 的内容交给 `r9`。

## `$` 与 `%`,还有不打记号的裸名字

`$` 和 `%` 咱们在上一篇就认下了:`$` 打头的是数字本身,`%` 打头的是寄存器。忘记它们的下场咱们也领教过,连一个错都不给您报,编出来的意思却能差出十万八千里。这里只补一件新东西:什么都不带的裸名字,指的是内存里那个位置的东西。上一篇排障里的 `movq 0x7c0, %rax` 就是它:`0x7c0` 不带 `$`,读的就成了那个地址上的内存。真码里的 `$` 满屏都是。`_start` 里的 `and $0xfffffffffffffff0,%rsp` 干的是“把栈地址对齐到 16 字节”的活,而那个吓人的长串其实是 -16:您拿它加个 16,末尾正好翻成了 0。至于裸名字的位置具体落在哪儿,那是链接器的活,这一篇咱们只管认出它是内存。

## 括号寻址:disp(base,index,scale)

内存操作数的全形态长成 `disp(base, index, scale)` 的样子,地址的算法是 base 加上 index 乘 scale,再往上添 disp 的偏移。四样里不用的都可以空着,`(%rsi)` 是光杆的括号,`8(%rsi)` 只留了一格偏移。咱们挨个编出来看:

```bash
cat > addr.s <<'EOF'
.text
movl (%rsi), %eax
movl 8(%rsi), %eax
movl 8(%rsi,%rdi), %eax
movl 8(%rsi,%rdi,4), %eax
EOF
as addr.s -o addr.o
objdump -d addr.o
```

```text
0000000000000000 <.text>:
   0:	8b 06                	mov    (%rsi),%eax
   2:	8b 46 08             	mov    0x8(%rsi),%eax
   5:	8b 44 3e 08          	mov    0x8(%rsi,%rdi,1),%eax
   9:	8b 44 be 08          	mov    0x8(%rsi,%rdi,4),%eax
```

您看字节那一列,光是 `(%rsi)` 就编出了两个字节,添上偏移就变成了三个,添上 `index` 又多出了一个。您把 `scale` 从 1 换成 4,字节的总数没变,变的是中间那颗字节的值,`3e` 换成了 `be`。咱们没写 scale 的那行,`objdump` 还给补成了 `,1`,这是 `objdump` 的好心。上一篇输出里的 `mov %edi,-0x4(%rbp)`,当时咱们整行跳过去了,现在您能把它读全了,把 `edi` 里的 4 个字节,存进 `rbp` 往回 4 格的内存里。Intel 那边的 `[rbp-0x4]`,说的也是同一个地方。`_start` 里的 `mov 0x0(%rip),%rdi` 也是同一个模子,括号里的 `%rip` 是个特殊基址,指的是“正在执行的下一条指令”,行尾 `#` 号的后面,是 `objdump` 替您算好的这副括号自己住的地址。`scale` 咱们就不硬凑例子了,到了验收那一段,编译器编出来的真身就会冒出来给您看。

## 标号与伪指令:写给汇编器看的行

以 `.` 开头的行跟机器码不沾边,它们是写给汇编器看的吩咐,咱们行话里管它叫伪指令。标号是咱们要认识的另一种小机关,`名字:` 记录的是“下一个字节的地址”,占用的字节数是零。咱们拿 `.global` 做个最见效的实验:

```bash
cat > lbl.s <<'EOF'
.text
.global visible
visible:
    inc %eax
hidden:
    inc %ebx
EOF
as lbl.s -o lbl.o
nm lbl.o
```

```text
0000000000000002 t hidden
0000000000000000 T visible
```

您看专门看符号的 `nm` 交回来的两行,里面藏着区别:大写 `T` 给了 `visible`,小写 `t` 归了 `hidden`。大写的 `T` 表示链接器也找得到,小写的 `t` 表示只有本文件找得到,差别就落在了这里。`.global` 干的就是这件事,把 `visible` 的名字递出门去。`.section` 定的是“接下来的东西住进哪个抽屉”,咱们拿 `objdump -h` 看 `lbl.o`,抽屉全都列在了表里:

```text
Idx Name          Size      VMA               LMA               File off  Algn
  0 .text         00000004  0000000000000000  0000000000000000  00000040  2**0
                  CONTENTS, ALLOC, LOAD, READONLY, CODE
  1 .data         00000000  0000000000000000  0000000000000000  00000044  2**0
                  CONTENTS, ALLOC, LOAD, DATA
  2 .bss          00000000  0000000000000000  0000000000000000  00000044  2**0
                  ALLOC
  3 .note.gnu.property 00000030  0000000000000000  0000000000000000  00000048  2**3
                  CONTENTS, ALLOC, LOAD, READONLY, DATA
```

`.text` 里住的是代码,`.data` 里住的是数据,各归各的抽屉。空着的 `.bss` 和编译器捎带的 `.note.gnu.property` 咱们眼下都用不上,但咱们贴的表是全的。数字标号 `0:` 和 `1:` 允许咱们在一个文件里反复定义,咱们回引的时候,`0b` 说的是“往回找最近的 `0:`”,`0f` 说的是“往前找最近的 `0:`”,到了跳转的环节,咱们会让它们真跑起来。

`.code16` 是这几条里最值得咱们当场验的,它跟汇编器说的是“接下来的指令按 16 位编”:

```bash
cat > plain.s <<'EOF'
.text
mov %ax, %bx
EOF
cat > sixteen.s <<'EOF'
.code16
mov %ax, %bx
EOF
as plain.s -o plain.o
as sixteen.s -o sixteen.o
objdump -d plain.o
objdump -d -M i8086 sixteen.o
```

```text
0000000000000000 <.text>:
   0:	66 89 c3             	mov    %ax,%bx

0000000000000000 <.text>:
   0:	89 c3                	mov    %ax,%bx
```

咱们对比着看:64 位的默认下,`mov %ax, %bx` 编出的是 `66 89 c3`,带上 `.code16` 就缩成了 `89 c3`,`66` 前缀也就省下了。十六位的输出读得正常,靠的是咱们多递了 `-M i8086`,给 `objdump` 换上 16 位的眼睛。

清零在 16 位的底下有两种写法,咱们各编一条:

```bash
cat > zero.s <<'EOF'
.code16
xor %ax, %ax
mov $0, %ax
EOF
as zero.s -o zero.o
objdump -d -M i8086 zero.o
```

```text
0000000000000000 <.text>:
   0:	31 c0                	xor    %ax,%ax
   2:	b8 00 00             	mov    $0x0,%ax
```

省出一个字节的是 `xor`,它也成了老手们的心头好。您回头看 `_start` 里那几行 `xor`,清零用的全是这一招。咱们自己也能让 GCC 说起 16 位的话:把 `-m16` 递给它,实测它吐出来的汇编,开头躺着的就是一行 `.code16gcc`。它跟 `.code16` 干的“按 16 位编”是同一件事,寻址方面的默认写法另有差别,那一步咱们暂时用不到,也就不往深处追了。

## `jmp` 与 `call`:装的是距离

近处的 `jmp` 和 `call` 装进字节的是距离,说的就是“往下一条指令之后再数多少格”。咱们让三种近形态出场:

```bash
cat > jmp.s <<'EOF'
.text
top:
    inc %eax
    jmp top
    call top
    jmp 1f
    inc %eax
1:  ret
EOF
as jmp.s -o jmp.o
objdump -d jmp.o
```

```text
0000000000000000 <top>:
   0:	ff c0                	inc    %eax
   2:	eb fc                	jmp    0 <top>
   4:	e8 f7 ff ff ff       	call   0 <top>
   9:	eb 02                	jmp    d <top+0xd>
   b:	ff c0                	inc    %eax
   d:	c3                   	ret
```

您把 `fc` 当普通数字读,算出来的是 252,可 `jmp top` 要跳回去的地方,离它不过 4 个字节的距离,哪里用得着您跳 252 个字节?答案就在距离里:数数从 `eb fc` 的下一个字节起,`top` 的位置在 4 个字节之前,距离就成了 -4,按带符号的读法读回去,`fc` 对上的正是它,一个普普通通的 -4。`e8 f7 ff ff ff` 编的是 `call`,装的同样是距离,五个字节里塞了个 32 位的。`call` 替咱们把返回地址压进了栈,`jmp` 跳过去就不管回程了。往前找的 `jmp 1f` 编出 `eb 02`,正跨过挡路的 `inc %eax`,目标还被 `objdump` 标在了尖括号里。

远形态的 `ljmp` 咱们放到最后讲,它长的是另一副长相,要的是段和偏移两个立即数(段是 16 位时代寻址的另一半,细节留到正文讲实模式的那一篇再还):

```bash
cat > far.s <<'EOF'
.code16
begin:
    ljmp $0x0000, $begin
EOF
as far.s -o far.o
objdump -d -M i8086 far.o
```

```text
0000000000000000 <begin>:
   0:	ea 00 00 00 00       	ljmp   $0x0,$0x0
```

`ea` 打头的字节一共五个,偏移和段各自占了 16 位。带条件跳转的那些指令,行话里咱们统称它们 `jxx`,`jne` 和 `je` 还有 `jle` 都在它的名下。它们的真身咱们不用等太久,验收那一段的循环里就躺着一枚。

## Warning、`(bad)`,和一片空白的反汇编

您顺手写下一行 `mov $1, (%rsi)`,立即数直接进了内存,`as` 对它的反应只有这么半句:

```text
t1.s: Assembler messages:
t1.s:2: Warning: no instruction mnemonic suffix given and no register operands; using default for `mov'
```

宽度的事,这回 `as` 只吱了半声:它警告完了,转头就自作主张地猜,咱们实测它猜成了 `movl`,编出来的是 `c7 06 01 00 00 00`。猜错了可是字节级的错,所以遇上立即数进内存的行,咱们的后缀永远写全。

您要是写了 `jmp 1b`,而它要找的 `1:` 并不存在,`as` 就会回您:

```text
t2.s: Assembler messages:
t2.s:2: Error: backward ref to unknown label "1:"
```

您要么补上标号,要么把方向从 `b` 换成 `f`。

16 位的码,默认 64 位的 `objdump -d` 读起来会出两种岔子。`sixteen.o` 那样的,`89 c3` 被读成了另一条合法指令 `mov %eax,%ebx`,您被骗了都不知道。`far.s` 编出来的码,开头的 `ea` 在 64 位里根本不成指令,`objdump` 直接打出了 `(bad)`。两种岔子您都能当场验,`objdump -d sixteen.o` 读回来的是像模像样的 `mov %eax,%ebx`,`objdump -d far.o` 的头一行就是 `ea (bad)`。您把 `-M i8086` 递给它们,两种就都正常了。

您要是看见 `objdump -d` 只吐了个文件头,说明的是文件里根本没有代码,咱们的东西多半进了 `.data`,没有 `.text` 就没有可反汇编的。

## 验收:把咱们自己编的 `Sum` 读穿

手练过了,咱们就编一份真码,亲眼看看咱们的读法顶不顶用。您抄起编辑器,写一个五行的小函数,`-O1` 是轻度优化的档位,开它图的是代码短,您读起来也省劲:

```bash
cat > sum.cpp <<'EOF'
int Sum(int const* w, int n) {
    int s = 0;
    for (int k = 0; k < n; ++k) { s += w[k]; }
    return s;
}
EOF
g++ -O1 -c sum.cpp -o sum.o
objdump -d sum.o
```

```text
0000000000000000 <_Z3SumPKii>:
   0:	85 f6                	test   %esi,%esi
   2:	7e 2a                	jle    2e <_Z3SumPKii+0x2e>
   4:	48 89 f8             	mov    %rdi,%rax
   7:	48 63 f6             	movslq %esi,%rsi
   a:	48 8d 0c b7          	lea    (%rdi,%rsi,4),%rcx
   e:	ba 00 00 00 00       	mov    $0x0,%edx
  13:	66 90                	xchg   %ax,%ax
  15:	66 66 2e 0f 1f 84 00 	data16 cs nopw 0x0(%rax,%rax,1)
  1c:	00 00 00 00 
  20:	03 10                	add    (%rax),%edx
  22:	48 83 c0 04          	add    $0x4,%rax
  26:	48 39 c8             	cmp    %rcx,%rax
  29:	75 f5                	jne    20 <_Z3SumPKii+0x20>
  2b:	89 d0                	mov    %edx,%eax
  2d:	c3                   	ret
  2e:	ba 00 00 00 00       	mov    $0x0,%edx
  33:	eb f6                	jmp    2b <_Z3SumPKii+0x2b>
```

编译器的版本不同,这几行的长相也许有一两处不一样,但读法是一样的,您照样往下读就行。

开头那串 `_Z3SumPKii` 是 C++ 给 `Sum` 起的内部名,您认得它是个名字就行。`test %esi,%esi` 是给 `n` 做的体检,`jle 2e` 专门接体检不合格的情况,一跳就到了 `2e` 那边,一个 0 已经在那边等着它了。`mov %rdi,%rax` 把数组指针请进了 `rax`,给后面的巡回做准备。

`movslq %esi,%rsi` 是个咱们没讲过的脸,它把 32 位带符号的 `n` 拉宽成 64 位,细节以后碰多了自然就清了。咱们的高光在下一行:`lea (%rdi,%rsi,4),%rcx`,括号里三样全齐了,`base` 是装着数组指针的 `%rdi`,`index` 是拉宽后的 `n`,乘的 `scale` 是个 4,`disp` 这回轮空成了 0,所以没露面,算出来的正是“越过第 n 个整数之后的位置”。`lea` 您就当它是“会算地址但不搬内存的 mov”。前面括号寻址的小节里,咱们还在一笔一笔手搓这个形态,一转眼它就从编译器手里冒了出来,欸?三样配料一样不缺,连 `scale` 都原样用上了。

`mov $0x0,%edx` 把源码里的 `s` 归了零,后面那两行 `xchg %ax,%ax` 和长尾巴的 `data16 cs nopw`,都是编译器垫的对齐填充,本质是几种花式的“什么也不做”,咱们不逐字节拆。

真正的循环从 `20` 那里开始。`add (%rax),%edx` 把指针眼前的整数收进 `s` 里,`add $0x4,%rax` 让指针往前挪了一格,`cmp %rcx,%rax` 问的就是到头了没有,`jne 20 <_Z3SumPKii+0x20>` 一看没到头就跳回了 `20`:短距离跳转的真身就在这儿,连 `objdump` 的尖括号注释都原样带着。循环走完了,`mov %edx,%eax` 把结果挪进返回值坐的 `%eax`,一句 `ret` 就收了工。`n` 不大于 0 的岔路,您也读得下来:那边的手续是领一个 0,`jmp 2b` 往前找到了汇合点,和咱们手搓的 `jmp 1f` 一个脾气。

您顺手还能挑出几张咱们没讲过的脸:比如 `push` 和 `pop` 在栈上的那一进一出,比如 `hlt` 让 CPU 停下来的那一睡,还有 `ret` 凭什么认得回家的路,比如参数为什么总从 `%edi` 和 `%rsi` 进来。它们不是这一篇欠的,轮到写汇编的时候,顺手就顺下来了。不信您再翻回开头看看那段 `_start`,刨掉这些生面孔和被打发掉的 `endbr64` 还有 `call` 前面那颗星,剩下的那些行,您应该都能读出个大概了。

## 指令全集、宏,还有 32/64 位模式

读法到了手之后,咱们不背指令的全集:一条指令具体干什么的事,咱们靠查手册,也靠用着攒着的笨办法。`.code32` 和 `.code64` 还有保护模式的系统指令,各自的用场都在保护模式的那边,到了那边咱们再学。写汇编宏的家当咱们也还没攒下,真要用的时候再来取。至于把这些汇编行亲手塞进 C++ 里的那门手艺,行话里的名字就叫“内联汇编”。轮到它的时候,咱们再一门心思学它。
