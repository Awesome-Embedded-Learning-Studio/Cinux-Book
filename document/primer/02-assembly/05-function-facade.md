---
title: 05 · 函数的门面
---

# 05 · 函数的门面,从门口那两行读到参数的来路

编译驱动那一趟,咱们跟两行代码只混了个脸熟:`pushq %rbp` 和 `movq %rsp, %rbp`。当时说好,等真读起汇编再逐字说明。那两行还在原地立着,今天轮到它们了。

函数的门口立着这两行,咱们天天在 `objdump` 的输出里见到它们,却还答不上来:调用者的 `%rbp` 被存去了哪里,函数跑完又由谁来还回来。还是在这个门口,一个字节的 `hlt` 能让咱们的进程当场倒下,理由也一直挂着没交代。再往里走,参数从哪条路进来,结果从哪条路回去,同样只有现象,还没个统一的说法。

## 序言与尾声:函数门口的那两行

栈和 call 都在手上了,g++ 亲手写下的 `pushq %rbp` 和 `movq %rsp, %rbp` 两行,现在可以逐字读了。咱们现场写一个 `Add`,写进 `add.cpp`,拿 `-O0` 把它编成字节给您看:

```cpp
int Add(int left, int right) {
    int sum = left + right;
    return sum;
}

int SumVla(int n) {
    int buf[n];
    buf[0] = n;
    return buf[0];
}

int main() {
    return Add(3, 4);
}
```

```bash
g++ -O0 -c add.cpp -o add_O0.o
objdump -d add_O0.o
```

```text
0000000000000000 <_Z3Addii>:
   0:	55                   	push   %rbp
   1:	48 89 e5             	mov    %rsp,%rbp
   4:	89 7d ec             	mov    %edi,-0x14(%rbp)
   7:	89 75 e8             	mov    %esi,-0x18(%rbp)
   a:	8b 55 ec             	mov    -0x14(%rbp),%edx
   d:	8b 45 e8             	mov    -0x18(%rbp),%eax
  10:	01 d0                	add    %edx,%eax
  12:	89 45 fc             	mov    %eax,-0x4(%rbp)
  15:	8b 45 fc             	mov    -0x4(%rbp),%eax
  18:	5d                   	pop    %rbp
  19:	c3                   	ret
```

这一段咱们贴的是 `_Z3Addii`,同一个 `add_O0.o` 里还躺着 `_Z6SumVlai` 和 `main`,那两段没搬上来,明说一声这是节选。同一个符号在讲两副面孔的那一篇里也露过一次脸,那边写的 `add.cpp` 只装一个 `Add`,栈槽偏移跟这里不是一组数。这一篇的现场多了个局部变量 `sum`,还捎带上另外两个函数。

咱们来认序言:就是开头那两行、字节 `55 48 89 e5`。`pushq %rbp` 把调用者的 `%rbp` 存进栈,和 call 压返回地址是同一个动作,`movq %rsp, %rbp` 让 `%rbp` 指向当前的栈顶。咱们从这一行往后看,`%rbp` 在函数里就不动了,后面所有 `-0x14(%rbp)`、`-0x4(%rbp)` 这样的偏移,锚的都是它。行话管这叫帧指针:参数和局部变量在栈里的位置,咱们全靠它来定位。咱们再看尾声,干的是 `popq %rbp` 加 `ret`,把存的旧 `%rbp` 还回去,再顺着 call 留的字条回家:字节 `5d c3`。同一个文件里 main 的字节也是这套开门关门,中间它给 `%edi`、`%esi` 装了参数,再夹一条 call,您把 `add_O0.o` 整个倒出来就看得见,那些行您已经全读得懂了。您自己跑出来的栈槽偏移也许跟这里差几格,读法是一样的。

还有一个名叫 `leave` 的 `c9` 您迟早会见到。咱们让 `leave` 一条指令干两件事:把 `%rsp` 拉回 `%rbp`,再 pop 出旧的 `%rbp`,给动过 `%rsp` 的函数当收尾的快捷键。最简单的函数里 GCC 不用它,直接 pop 就够了。可一旦函数把 `%rsp` 挪走过了(比如开了变长数组),pop 就找不着北了,咱们就得请 `leave` 出场。上面那个 `SumVla` 就属于这一档,咱们只看它的尾巴(`sed -n '/甲/,/乙/p'` 这个写法只印落在甲、乙两段之间的行,底下就靠它把函数体挑出来):

```bash
objdump -d add_O0.o | sed -n '/<_Z6SumVlai>:/,/^$/p' | tail -4
```

```text
  a6:	e8 00 00 00 00       	call   ab <_Z6SumVlai+0x91>
  ab:	c9                   	leave
  ac:	c3                   	ret
```

咱们看它的尾声,正是 `leave` 配 `ret`:字节 `c9 c3`。头一行里的 `call` 是还没链接时的栈保护桩,四个字节的位移全是 0,所以它落在紧挨着的下一条指令上。这一行眼下不用管,等链接器接手,它才会被填上真地址。

那序言是不是永远都在?咱们把优化开到 `-O1`,重新编一遍:

```bash
g++ -O1 -S add.cpp -o add_O1.s
grep -n -A5 '_Z3Addii:' add_O1.s
```

```text
5:_Z3Addii:
6-.LFB0:
7-	.cfi_startproc
8-	leal	(%rdi,%rsi), %eax
9-	ret
10-	.cfi_endproc
```

```bash
grep -n -A5 'main:' add_O1.s
```

```text
25:main:
26-.LFB2:
27-	.cfi_startproc
28-	movl	$7, %eax
29-	ret
30-	.cfi_endproc
```

行号和 `.cfi_*` 那些行是 grep 与 GCC 的调试信息捎带的,咱们只看中间那两行。咱们眼前的序言尾声双双消失,`Add` 缩成了一条 `leal`,main 更是把整个调用在编译期算成了 `movl $7, %eax` 加一条 ret,您翻自己刚编出来的 `.s` 就能看见。帧指针只是 `-O0` 的拐杖,一旦优化开了,编译器自己其实记得清清楚楚,就不劳 `%rbp` 值班了。

## hlt:一个字节的睡觉指令,用户态碰不得

读语法那一篇开头那段 `_start` 的尾巴上躺着一个 `hlt`,咱们现在可以交代它的字节了:单个的 `f4`,您把它编成 `.s` 过一遍 as 和 objdump 就能亲手确认:

```bash
printf '.text\nhlt\n' > hlt.s
as hlt.s -o hlt.o
objdump -d hlt.o
```

```text
0000000000000000 <.text>:
   0:	f4                   	hlt
```

它的语义是把处理器放进停机状态,等中断来了再醒,醒了从 hlt 的下一条指令继续,内核空闲时转的就是它。咱们读得到也编得出,那在咱们日常的机器上跑一下会怎样?您猜猜。咱们大胆试,反正死的是进程自己,伤不着咱们的机器:

```bash
printf 'int main() { asm volatile("hlt"); return 0; }\n' | g++ -x c++ -O2 -o hlt_um -
./hlt_um; echo "exit_status=$?"
```

```text
exit_status=139
```

进程被杀了,`$?` 报出来的 139 不是程序自己退的码,而是 shell 对信号死亡的记法:`139 = 128 + 11`,咱们查 11 号信号就是 SIGSEGV。这里请您留意一层:它不是读了非法内存。

那它到底犯了哪一条?Intel 的手册里写得没有余地:处理器跑在保护模式或 virtual-8086 模式下的时候,要执行 `hlt`,程序或过程的特权级必须是 0。手册紧接着的异常表还写着:当前特权级不是 0,处理器抛 `#GP(0)`。您翻开手册的 HLT 词条,这两句都印在上面。咱们的进程跑在用户态,当前特权级不是 0,几句话就凑成了一条完整的因果:`hlt` 是特权指令,处理器当场抛 `#GP(0)`,Linux 接过这个异常,再把它翻译成 SIGSEGV 递给进程。看系统调用的 `strace` 还能替咱们作证,它把进程和内核之间的来回一条条印出来,`-f` 是让它连子进程一起跟(这个例子只有单进程,加不加都一样,咱们跑的时候带上也无妨):

```bash
strace -f ./hlt_um
```

```text
--- SIGSEGV {si_signo=SIGSEGV, si_code=SI_KERNEL, si_addr=NULL} ---
+++ killed by SIGSEGV +++
```

它报的正是 `si_code=SI_KERNEL`、`si_addr=NULL`,内核主动发的,没带出错地址,跟野指针的页错误长得不一样。所以同一个 `f4`,咱们在内核态跑它是正经的省电,咱们在用户态跑它就是越权,差的只是当前特权级。特权指令这个名字咱们不是头一回听见,可亲眼看到它发作,这是头一回。

## 参数从哪儿进,结果从哪儿回

最后一笔欠下的:参数为什么总从 `%edi` 和 `%rsi` 进来。答案其实不神秘,x86-64 的 Linux 上大家守的是同一份调用约定:头六个整数或指针参数,咱们让它们依次走 `%rdi %rsi %rdx %rcx %r8 %r9`,整数返回值走的还是 `%rax`,轮到第七个参数就只能上栈了。咱们拿一个两参函数的 `-O0` 汇编看现场,写进 `args.cpp`:

```cpp
int Sum2(int a, int b) {
    return a + b;
}

int Sum7(int a, int b, int c, int d, int e, int f, int g) {
    return a + b + c + d + e + f + g;
}

int main() {
    int r2 = Sum2(10, 20);
    int r7 = Sum7(1, 2, 3, 4, 5, 6, 7);
    return r2 + r7;
}
```

```bash
g++ -O0 -S args.cpp -o args_O0.s
grep -A16 '_Z4Sum2ii:' args_O0.s | head -18
```

```text
_Z4Sum2ii:
.LFB0:
	.cfi_startproc
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset 6, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register 6
	movl	%edi, -4(%rbp)
	movl	%esi, -8(%rbp)
	movl	-4(%rbp), %edx
	movl	-8(%rbp), %eax
	addl	%edx, %eax
	popq	%rbp
	.cfi_def_cfa 7, 8
	ret
	.cfi_endproc
```

咱们看开头的序言那几行:除了 `pushq %rbp` 和 `movq %rsp, %rbp` 这两行,`movl %edi, -4(%rbp)` 与 `movl %esi, -8(%rbp)` 也在讲读法的那一篇里原样见过。这里要认的是 `a` 和 `b` 从 `%edi`、`%esi` 进场,算完的结果从 `%eax` 回家,一目了然。七个参数的现场里,前四个也是各就各位,这一屏就截到这儿(开头那几行序言跟 `Sum2` 那屏逐行一样,咱们不重复认)。凑够六个之后,第七个只能走栈:

```bash
grep -A12 '_Z4Sum7iiiiiii:' args_O0.s | head -12
```

```text
_Z4Sum7iiiiiii:
.LFB1:
	.cfi_startproc
	pushq	%rbp
	.cfi_def_cfa_offset 16
	.cfi_offset 6, -16
	movq	%rsp, %rbp
	.cfi_def_cfa_register 6
	movl	%edi, -4(%rbp)
	movl	%esi, -8(%rbp)
	movl	%edx, -12(%rbp)
	movl	%ecx, -16(%rbp)
```

```bash
sed -n '51,56p' args_O0.s
```

```text
	movl	16(%rbp), %eax
	addl	%edx, %eax
	popq	%rbp
	.cfi_def_cfa 7, 8
	ret
	.cfi_endproc
```

被调方读第七个参数,走的就是 `16(%rbp)`,咱们把 `args_O0.s` 的第 51 到 56 行抓出来(`sed -n '51,56p'` 里的数字就是行号),从上面那行 `movl 16(%rbp), %eax` 里看得到:`%rbp` 往上 8 格住的是返回地址,+16 才轮到第一个栈参数。

不过 `-O1` 一开,整个调用在编译期就算完了,咱们得把内联关掉才看得到真现场。不关的时候,main 里只剩两行:

```bash
g++ -O1 -S args.cpp -o args_O1.s
grep -A5 'main:' args_O1.s
```

```text
main:
.LFB2:
	.cfi_startproc
	movl	$58, %eax
	ret
	.cfi_endproc
```

您把这一串加一遍,10+20+1+2+3+4+5+6+7 正好 58,七个参数的和在编译期就定死了。咱们加上 `-fno-inline`,调用现场才露出来:

```bash
g++ -O1 -fno-inline -S args.cpp -o args_O1_noinline.s
grep -A22 'main:' args_O1_noinline.s
```

```text
main:
.LFB2:
	.cfi_startproc
	pushq	%rbx
	.cfi_def_cfa_offset 16
	.cfi_offset 3, -16
	movl	$20, %esi
	movl	$10, %edi
	call	_Z4Sum2ii
	movl	%eax, %ebx
	pushq	$7
	.cfi_def_cfa_offset 24
	movl	$6, %r9d
	movl	$5, %r8d
	movl	$4, %ecx
	movl	$3, %edx
	movl	$2, %esi
	movl	$1, %edi
	call	_Z4Sum7iiiiiii
```

咱们看前六个参数各就各位,轮到第七个的时候,来的是一条字面意思的 `pushq $7`,栈上那一进一出在这儿又派上了用场。贴出来的现场就截到 `call _Z4Sum7iiiiiii` 为止,后面 `addq $8, %rsp` 那几行收尾,您自己跑出来看。

轮到被调方取这个参数,若没有帧指针,咱们就从 `8(%rsp)` 读,紧贴返回地址上方:

```bash
grep -A10 '_Z4Sum7iiiiiii:' args_O1_noinline.s
```

```text
_Z4Sum7iiiiiii:
.LFB1:
	.cfi_startproc
	addl	%esi, %edi
	addl	%edx, %edi
	addl	%ecx, %edi
	addl	%r8d, %edi
	leal	(%rdi,%r9), %eax
	addl	8(%rsp), %eax
	ret
	.cfi_endproc
```

还有一门手艺没动:这些汇编行眼下都得喂给汇编器过一遍,想让它们直接住进 C++ 里,行话叫内联汇编,咱们下一篇就动手。
