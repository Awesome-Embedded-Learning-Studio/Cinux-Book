---
title: 03 · 写:内联汇编
---

# 03 · 写:内联汇编,把上一篇欠下的顺手还上

上一篇收尾的时候,咱们桌上留了几张生面孔:push 和 pop 在栈上的一进一出,hlt 的那一睡,ret 凭什么认得回家的路,还有参数为什么总从 `%edi` 和 `%rsi` 进来。当时说好了,轮到写汇编的时候顺手还上。现在就是这个时候:这一篇咱们要学的就是把汇编亲手塞进 C++ 里,这门手艺的名字就叫内联汇编。咱们要塞进去的那几行,住的地方恰好就是这些老面孔的地盘,函数的门口是序言在把门,进出走的是那口栈。所以咱们从栈说起,欠下的几笔一笔一笔还上。

## push 和 pop:栈的一进一出

push 的语义,手册里就给了一句话:头一步把 `%rsp` 减下去,第二步把操作数存进新的栈顶。pop 倒是反着来:从栈顶取走数据,再把 `%rsp` 加了回来。x86-64 的一趟是 8 个字节。您别小看这对动作,咱们手搓一段编出来,它们可是汇编里最省字节的指令之一:

```bash
cat > pp.s <<'EOF'
	.text
	.globl PushPopBytes
PushPopBytes:
	pushq	%rax
	popq	%rbx
	pushq	%r12
	popq	%r12
	ret
EOF
as pp.s -o pp.o
objdump -d pp.o
```

```text
0000000000000000 <PushPopBytes>:
   0:	50                   	push   %rax
   1:	5b                   	pop    %rbx
   2:	41 54                	push   %r12
   4:	41 5c                	pop    %r12
   6:	c3                   	ret
```

咱们上一篇的 `movq` 搬一个立即数要用七个字节,咱们这里的 `pushq %rax` 只有一个字节 `50`(输出开头的两行文件头,咱们一律略去不贴)。低位的八个寄存器(rax 到 rdi)各占一个编码,`58` 起头的是 pop 家族,而 r8 到 r15 得加一颗 `41` 前缀才变成两个字节。您再看看尾巴上那个 `ret`:也才一个字节 `c3`。

光看字节不过瘾,咱们请 gdb 来单步走一遍,亲眼看 `%rsp` 怎么动。这段小程序干完活就调 Linux 的退出系统调用收工:

```bash
cat > pushwalk.s <<'EOF'
	.text
	.globl _start
_start:
	pushq	%rax
	pushq	%rbx
	popq	%rbx
	popq	%rax
	movq	$60, %rax
	xorl	%edi, %edi
	syscall
EOF
as pushwalk.s -o pushwalk.o
ld -o pushwalk pushwalk.o
gdb -q -batch -ex 'break *_start' -ex 'run' -ex 'p/x $rsp' -ex 'x/i $rip' -ex 'si' -ex 'p/x $rsp' -ex 'si' -ex 'p/x $rsp' -ex 'si' -ex 'p/x $rsp' -ex 'si' -ex 'p/x $rsp' -ex 'x/i $rip' pushwalk
```

咱们拿一条 gdb 命令在本机实测过:进 `_start` 时 `%rsp` 是 `0x7fffffffd1e0`,咱们 push 一次看到它减 8 变成 `d1d8`,再 push 一次到了 `d1d0`,pop 两次之后它又原路退回了 `d1e0`。您看这一进一出分毫不差,8 就是 x86-64 栈的粒度。gdb 默认关掉了地址随机化,所以您每次重跑看到的都是同一个数。不过正文里这串数值是本机实录,换台机器、换套环境变量数值就变了,您别拿它对表。开头那几行 debuginfod 询问是 gdb 的环境噪音,跟咱们的实验无关,您不想看见它的话,下一节 call 实验的命令开头就带了 `set debuginfod enabled off`,咱们原样搬过去用就行。

## call 与 ret:返回地址就躺在栈顶

上一篇咱们说过 `call` 替咱们把返回地址压进了栈,而 `jmp` 跳过去就不管回程。现在咱们有了 push 的知识,可以把这句话兑现成看得见的字节。您写一个 `_start` 一进门就 call、Target 里只放一句 ret 的小程序,那两行 `movq $60` 加 `syscall` 是 Linux 的 60 号退出调用,咱们借它让程序体面退场:

```bash
cat > callret.s <<'EOF'
	.text
	.globl _start
_start:
	call	Target
AfterCall:
	movq	$60, %rax
	xorl	%edi, %edi
	syscall
Target:
	ret
EOF
as callret.s -o callret.o
ld -o callret callret.o
gdb -q -batch -ex 'set debuginfod enabled off' -ex 'break *_start' -ex 'run' \
    -ex 'p/x $rsp' -ex 'x/i $rip' -ex 'si' -ex 'p/x $rsp' -ex 'x/i $rip' \
    -ex 'x/gx $rsp' -ex 'x/i *(void**)$rsp' -ex 'si' -ex 'p/x $rsp' -ex 'x/i $rip' callret
```

```text
Breakpoint 1 at 0x401000

Breakpoint 1, 0x0000000000401000 in _start ()
$1 = 0x7fffffffd1e0
=> 0x401000 <_start>:	call   0x401010 <Target>
0x0000000000401010 in Target ()
$2 = 0x7fffffffd1d8
=> 0x401010 <Target>:	ret
0x7fffffffd1d8:	0x0000000000401005
   0x401005 <AfterCall>:	mov    $0x3c,%rax
0x0000000000401005 in AfterCall ()
$3 = 0x7fffffffd1e0
=> 0x401005 <AfterCall>:	mov    $0x3c,%rax
```

咱们一步步看。咱们等 `call` 执行完,看见 `%rsp` 从 `d1e0` 变成了 `d1d8`,又一次减了 8,call 干的活就是“把返回地址 push 进栈,然后跳过去”。栈顶现在躺着的是什么?`x/gx $rsp` 替咱们答了:`0x0000000000401005`,而 `401005` 正是 `AfterCall`,call 的下一条指令的地址。`ret` 干的事就是把它弹进 `%rip`,您看单步之后咱们果然落在了 `AfterCall`,而且 `%rsp` 回到了 `d1e0`。开头咱们欠下的那句“ret 凭什么认得回家的路”,答案现在就躺在栈顶:call 出发时压下的返回地址,就是它回家的字条。

## 序言与尾声:函数门口的那两行

栈和 call 都在手上了,工具链那一卷里 g++ 亲手写下的 `pushq %rbp` 和 `movq %rsp, %rbp` 两行,现在可以逐字读了。咱们现场写一个 `Add`,拿 `-O0` 把它编成字节给您看:

```bash
printf 'int Add(int left, int right) {\n    int sum = left + right;\n    return sum;\n}\nint main() { return Add(3, 4); }\n' \
    | g++ -x c++ -O0 -c -o add.o -
objdump -d add.o
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

咱们来认序言:就是开头那两行、字节 `55 48 89 e5`。`pushq %rbp` 把调用者的 `%rbp` 存进栈,和 call 压返回地址是同一个动作,`movq %rsp, %rbp` 让 `%rbp` 指向当前的栈顶。咱们从这一行往后看,`%rbp` 在函数里就不动了,后面所有 `-0x14(%rbp)`、`-0x4(%rbp)` 这样的偏移,锚的都是它。行话管这叫帧指针:参数和局部变量在栈里的位置,咱们全靠它来定位。咱们再看尾声,干的是 `popq %rbp` 加 `ret`,把存的旧 `%rbp` 还回去,再顺着 call 留的字条回家:字节 `5d c3`。咱们看同一个输出里 main 的字节也是这套开门关门,中间它给 `%edi`、`%esi` 装了参数,又夹了一条 call 和一条 nop,您已经全读得懂了。

还有一个名叫 `leave` 的 `c9` 您迟早会见到。咱们让 `leave` 一条指令干两件事:把 `%rsp` 拉回 `%rbp`,再 pop 出旧的 `%rbp`,给动过 `%rsp` 的函数当收尾的快捷键。最简单的函数里 GCC 不用它,直接 pop 就够了。可一旦函数把 `%rsp` 挪走过了(比如开了变长数组),pop 就找不着北了,咱们就得请 `leave` 出场。咱们拿一个带变长数组的小函数实测过,它的尾声正是 `leave` 配 `ret`:字节 `c9 c3`。

那序言是不是永远都在?您把上面命令里的 `-O0` 换成 `-O1`、`-c` 换成 `-S`,重新给咱们来一遍,`_Z3Addii` 的正文(标号行略)就成了:

```text
_Z3Addii:
	leal	(%rdi,%rsi), %eax
	ret
```

咱们眼前的序言尾声双双消失,`Add` 缩成了一条 `leal`,main 更是把整个调用在编译期算成了 `movl $7, %eax` 加一条 ret,您翻自己刚编出来的 `.s` 就能看见。帧指针只是 `-O0` 的拐杖,一旦优化开了,编译器自己其实记得清清楚楚,就不劳 `%rbp` 值班了。

## hlt:一个字节的睡觉指令,用户态碰不得

上一篇 `_start` 的尾巴上躺着一个 `hlt`,咱们现在可以交代它的字节了:单个的 `f4`,您把它编成 `.s` 过一遍 as 和 objdump 就能亲手确认。它的语义是把处理器放进停机状态,等中断来了再醒,醒了从 hlt 的下一条指令继续,内核空闲时转的就是它。咱们读得到也编得出,那在咱们日常的机器上跑一下会怎样?您猜猜。咱们大胆试,反正死的是进程自己,伤不着咱们的机器:

```bash
printf 'int main() { asm volatile("hlt"); return 0; }\n' \
    | g++ -x c++ -O2 -o hlt_um -
./hlt_um; echo "exit_status=$?"
```

```text
exit_status=139
```

进程被杀了,`139 = 128 + 11`,咱们查 11 号信号就是 SIGSEGV。这里请您留意一层:它不是读了非法内存,而是 hlt 在保护模式下是特权指令,您在用户态执行它,处理器抛的是通用保护异常,Linux 内核再把这个异常翻译成了 SIGSEGV 递给进程。看系统调用的 strace 还能替咱们作证,它报的是 `SIGSEGV {si_signo=SIGSEGV, si_code=SI_KERNEL, si_addr=NULL}`,内核主动发的,没带出错地址,跟野指针的页错误长得不一样。所以同一个 `f4`,咱们在内核态跑它是正经的省电,咱们在用户态跑它就是越权,差的只是当前特权级。“特权指令”这个名字咱们在本卷第一篇见过,可咱们这还是头一回亲眼看到它发作,往后讲运行模式的篇章里咱们再收拾它。

## 参数从哪儿进,结果从哪儿回

最后一笔欠下的:参数为什么总从 `%edi` 和 `%rsi` 进来。答案其实不神秘,x86-64 的 Linux 上大家守的是同一份调用约定:头六个整数或指针参数,咱们让它们依次走 `%rdi %rsi %rdx %rcx %r8 %r9`,整数返回值走的还是 `%rax`,轮到第七个参数就只能上栈了。咱们拿一个两参函数的 `-O0` 汇编看现场:

```text
_Z4Sum2ii:
	pushq	%rbp
	movq	%rsp, %rbp
	movl	%edi, -4(%rbp)
	movl	%esi, -8(%rbp)
	movl	-4(%rbp), %edx
	movl	-8(%rbp), %eax
	addl	%edx, %eax
	popq	%rbp
	ret
```

咱们看 `a` 和 `b` 从 `%edi`、`%esi` 进场,算完的结果从 `%eax` 回家,一目了然。真正有意思的是七个参数的现场,咱们用 `-O1 -fno-inline` 把内联关掉再编(不关的话,整个调用就被编译期直接算成了一个数。咱们贴的现场截到第二条 call 为止,剩下的几行收尾您自己跑出来看):

```text
main:
	pushq	%rbx
	movl	$20, %esi
	movl	$10, %edi
	call	_Z4Sum2ii
	movl	%eax, %ebx
	pushq	$7
	movl	$6, %r9d
	movl	$5, %r8d
	movl	$4, %ecx
	movl	$3, %edx
	movl	$2, %esi
	movl	$1, %edi
	call	_Z7Sum7iiiiiii
```

咱们看前六个参数各就各位,第七个参数是一条字面意思的 `pushq $7` 压进栈的,咱们刚学的 push 在这儿就派上了用场。被调方取它的时候,`-O0` 下从 `16(%rbp)` 读(`%rbp` 往上 8 格住的是返回地址、+16 才轮到第一个栈参数),省了帧指针时从 `8(%rsp)` 读,紧贴返回地址上方。咱们到这里,上一篇留下的几张生面孔就都见过了,接下来咱们正儿八经开新手艺,从这儿起所有实验您在自己的机器上都能原样跑,需要的只有 g++ 和 objdump。

## 基本 asm:原样塞进去的第一段

C++ 里的 `asm` 是标准关键字,咱们直接写 `asm` 就好。您在别人的代码里见过的 `__asm__` 是同一件事的保险拼法,防的是个别编译选项把 `asm` 关掉,咱们平时用不上。最朴素的嵌法叫基本 asm,括号里写的只有一个指令字符串,没有别的:

```bash
cat > basic.cpp <<'EOF'
int main() {
    asm("nop");
    asm("nop\n\tnop\n\tnop");
    return 0;
}
EOF
g++ -O2 -S basic.cpp -o basic.s
grep -n -B1 -A7 '#APP' basic.s
```

```text
9-	.cfi_startproc
10:#APP
11-# 2 "basic.cpp" 1
12-	nop
13-# 0 "" 2
14-# 3 "basic.cpp" 1
15-	nop
16-	nop
17-	nop
```

(您机器上第 11、14 行里会是您自己的文件路径。)`#APP` 和 `#NO_APP` 是 GCC 在汇编输出里划的围栏,意思咱们已经能读懂:“围栏里这段是作者亲笔,我一个字没动”。咱们写进去的四个 nop 原样躺在里面,两条独立的 asm 还被并进了同一对围栏。以后您在 `.s` 里找自己嵌的那段,咱们就认这对书签。有个小提醒:老版本 GCC 写的是 `# APP`(带空格),GCC 16 写的是 `#APP`,grep 的花样请按您自己的输出来。

咱们写给它的字符串会被 GCC 一个字不改地纯拷贝进去,里面的 `%` 不当操作符用,寄存器咱们就写单个的 `%eax`。您要是手滑写成 `%%eax`,`g++ -S` 都不给您报错,错要等到汇编器那一道才炸了出来,报的是 `bad register name '%%eax'`。它也没有任何引用 C 变量的机制,顶多把全局变量的名字直接写进指令串。可 C++ 的函数名在符号表里是被修饰过的,`Foo(int)` 的真名是 `_Z3Fooi`,您在基本 asm 里 `call Foo`,链接器只当您要调一个从没定义过的 `Foo`,跟您当场翻脸。还有一层跟版本有关:很新的 GCC 16 对基本 asm 相当保守,它假定您这段指令不动任何通用寄存器,不过它可能读写任何全局变量,手册还明说了:该假定将来可能再变。所以基本 asm 在咱们这儿只当引子,真正要学的是下一位。

## 扩展 asm:让 GCC 当中间人

基本 asm 的问题在于,GCC 对围栏里的事一无所知,它既不知道您要读哪个变量,也不知道您改了哪个寄存器。扩展 asm 补的就是一条对话通道,咱们把样子摆出来:`asm(模板 : 输出 : 输入 : clobber)`,四段是用冒号隔开的,没内容的段可以空着。咱们从最小的一个开始,咱们把 42 装进变量:

```bash
cat > ext.cpp <<'EOF'
int Load42() {
    int x = 0;
    asm("movl $42, %0" : "=r"(x));
    return x;
}
int main() { return Load42(); }
EOF
g++ -O2 -S ext.cpp -o ext.s
```

咱们在 `.s` 里找 `Load42`,围栏里看到的是:

```text
_Z6Load42v:
.LFB0:
	.cfi_startproc
#APP
# 3 "ext.cpp" 1
	movl $42, %eax
# 0 "" 2
#NO_APP
	ret
```

咱们看模板里的 `%0`,它引用的就是输出 `x`。咱们来谈约束 `"+r"`:`=` 说的是“我只写它”,`r` 说的是“请给我一个通用寄存器”,落到的 `%eax` 就是 GCC 挑中的寄存器。这里有两件容易想反的事。一是扩展 asm 里 `%` 变成了操作符,写字面的寄存器名要 `%%eax`,操作数引用才有资格用单个的 `%`,跟基本 asm 正好掉了个个儿。二是咱们别指望 `%0` 就是 `%eax`,咱们只把它当成一个代号:第一个操作数落在哪儿,由 GCC 的寄存器分配说了算。咱们实测过一个热闹的版本,同一个函数里放三条各自递增一个变量的 `asm("incl %0" : "+r"(v))`,三条一模一样的模板,内联展开后分别落进了 `%eax`、`%ecx` 和 `%edx`。同一个函数留在外面没被内联的独立副本里,落到的又是另一组安排。所以咱们立一条硬规范:模板里永远用 `%0` `%1` 引用操作数,您永远别猜它是哪个寄存器。顺带您刚见的 `"+r"` 是 `=` 的亲戚,加号说的是“我又读又写”,进出的是同一个寄存器。老一点的写法是输出 `"=r"(y)` 配输入 `"0"(x)`,数字约束的意思是“这个输入跟 0 号输出住同一处”。两种都是合法的,`+` 是后来更省事的写法。还有一条 GCC 的硬规定顺便交代:纯输入的操作数不许改,编译器认定它出了 asm 还是原值,您要是动了它,没有任何 clobber 能替您申报,只能换成刚才的匹配写法绑一个输出。

咱们把多条指令写进同一个字符串,用的分隔符是 `\n\t`、每条一行,开头咱们那串 `nop` 就是这个样子。输入段照同样的写法挂变量:`asm("movl %1, %0" : "=r"(y) : "r"(x))`,编号要等输出数完了才轮到输入,所以 `%0` 是 y、`%1` 是 x。

> 约束字母里有一位容易被人念歪的:`"i"` 是立即数整数常数,而 `"N"` 不是“数字”的泛称,它是 x86 专属的机器约束,专指的是无符号 8 位常数,是 in/out 端口指令要的形状。比如咱们写 `asm volatile("outb %0, %1" : : "a"(v), "N"(0x60))`,这里的 `"a"` 是点名要操作数落进 `%eax` 系寄存器的写法,`"N"` 接的 0x60 就是那个端口号。咱们拿它去接非常数,编译器回您一句 `impossible constraint`。

## =r 与 =m:操作数住寄存器还是住内存

`r` 的兄弟 `m` 走的是另一条路:操作数不进寄存器、直接落在内存上。咱们拿同一个函数的两份写法对照着编成字节:

```bash
printf 'int BumpR(int x) { asm("incl %%0" : "+r"(x)); return x; }\n' \
    | g++ -x c++ -O2 -c -o bump_r.o -
printf 'int BumpM(int x) { asm("incl %%0" : "+m"(x)); return x; }\n' \
    | g++ -x c++ -O2 -c -o bump_m.o -
objdump -d bump_r.o
objdump -d bump_m.o
```

```text
0000000000000000 <_Z5BumpRi>:
   0:	89 f8                	mov    %edi,%eax
   2:	ff c0                	inc    %eax
   4:	c3                   	ret

0000000000000000 <_Z5BumpMi>:
   0:	89 7c 24 fc          	mov    %edi,-0x4(%rsp)
   4:	ff 44 24 fc          	incl   -0x4(%rsp)
   8:	8b 44 24 fc          	mov    -0x4(%rsp),%eax
   c:	c3                   	ret
```

(两段输出咱们各取反汇编正文,文件头就略了。)咱们看字节:`r` 版里 `x` 全程活在寄存器中,`inc` 两个字节就完事了。`m` 版 GCC 让 `%edi` 在 `-4(%rsp)` 的栈槽安家,随后的 `incl` 直接对着内存做读改写,最后再取了回来放进 `%eax` 当返回值。哪个好?咱们看指令有没有寄存器形态:`incl` 两种都行,那当然 `r` 更快。可往后咱们会碰到的 `lgdt`、`lidt` 这类加载描述表的指令(切换运行模式的时候,咱们会让 CPU 来装一张那样的大表),咱们喂给它的操作数天生只能落在内存,那时候 `m` 就是唯一的路。咱们多念一嘴,`-4(%rsp)` 这个位置落在栈指针下方的 128 字节里,咱们的行话叫它红区,是那些自己不再 call 别人的函数(叶子函数)可以白用的地盘。不过它是用户态的福利,进了内核就被关掉,到时候咱们再细说。

## 漏了 `"memory"` 的那个 bug

四段里的最后一段 clobber,咱们用一个真 bug 来认识它。下面是教学用的错误版,您抄下来跑,它真的会错:

```bash
cat > cand_c.cpp <<'EOF'
#include <cstdio>

int g = 1;

int main() {
    int first = g;
    asm("movl $99, g(%%rip)" ::);
    int second = g;
    std::printf("first=%d second=%d\n", first, second);
    return 0;
}
EOF
g++ -O2 cand_c.cpp -o cand_c
./cand_c; echo "exit_status=$?"
```

```text
first=1 second=1
exit_status=0
```

咱们在 asm 里明明白白写了把 99 存进 `g`,再读出来的 `second` 居然还是 1!您别急着看修法,咱们按老办法翻 `.s` 找机制:

```text
main:
	subq	$8, %rsp
	movl	g(%rip), %esi
#APP
# 7 "cand_c.cpp" 1
	movl $99, g(%rip)
# 0 "" 2
#NO_APP
	leaq	.LC0(%rip), %rdi
	movl	%esi, %edx
	xorl	%eax, %eax
	call	printf@PLT
```

咱们把真相找出来了:第一次读 `g` 落在 `%esi` 里,围栏之后的第二次读压根没有发生,GCC 直接把 `%esi` 顶了上去。扩展 asm 的合同是“模板是个黑盒,它跟 C 世界的全部往来,都写在操作数和 clobber 里”。咱们没写任何 clobber,GCC 就认定了内存世界没人碰过、旧值接着用,在它看来天经地义。另外两件小事咱们记一下:一是 `%%rip` 在这儿要双百分号,因为咱们写的是扩展 asm。二是 `-O0` 下这个程序打印的 `second` 是对的,`-O1` 起才翻了车。“没优化没事,一优化出事”,刚才咱们踩的就是它,往后夜里遇上的同类也是。

咱们来修,要补的只有一行:把第四段补上:

```bash
cat > cand_c_fixed.cpp <<'EOF'
#include <cstdio>

int g = 1;

int main() {
    int first = g;
    asm("movl $99, g(%%rip)" ::: "memory");
    int second = g;
    std::printf("first=%d second=%d\n", first, second);
    return 0;
}
EOF
g++ -O2 cand_c_fixed.cpp -o cand_c_fixed
./cand_c_fixed; echo "exit_status=$?"
```

```text
first=1 second=99
exit_status=0
```

`"memory"` 对 GCC 说的是“这块 asm 动过操作数之外的内存,别信你手里存的旧值”。咱们再翻 `.s`,围栏之后老老实实多了一条 `movl g(%rip), %edx`,一行 clobber 就换回了一次真加载。clobber 段还能登记寄存器的名字,咱们按名字写、不带 `%`,比如您写 `"ebx"`。做算术的 asm 常配一个 `"cc"`,申报的是标志位被动过。咱们再回到刚才那次真加载:它是 GCC 在编译期补的,所以 `"memory"` 管的也只是编译器这一层,处理器自己的乱序推测读它管不着,内存屏障的深水咱们现在不去。

## asm volatile:保住的是命,不是顺序

您可能听过“加个 volatile 就稳了”。咱们现场验一验它到底保什么。下面的 asm 有寄存器输出,可 `x` 之后就没人用了:

```bash
printf '#include <cstdio>\nint main() {\n    int x = 0;\n    asm("movl $42, %%0" : "=r"(x));\n    std::printf("done\\n");\n    return 0;\n}\n' \
    | g++ -x c++ -O2 -S -o drop.s -
grep -c 'APP' drop.s
```

```text
0
```

咱们数围栏数出来是 0,整段 asm 在 `-O2` 下被删了个干干净净、程序照跑照打印 `done`,删了您也察觉不到。咱们给 asm 加上 `volatile` 再编一次,计数就变成了 2,`movl $42, %eax` 原样回来了。这就是 volatile 的本职:照我写的样子执行,您别因为输出没人要就把整段当死代码扫地出门。往后咱们写端口 IO 这类全靠副作用的指令,到时候全指着它给咱们保命。另外有个省心的规律:没有输出操作数的 asm 天然就是 volatile,咱们的 `asm("hlt")` 不写 volatile 也不会被删。

volatile 不保的东西,咱们同样得记清。咱们看头一件:它不是内存屏障,刚那个漏 `"memory"` 的 bug 您翻回去看,修法补的才是内存申报,volatile 可救不了它。顺序咱们也别指望它保,手册明说 volatile asm 仍然可能被编译器相对别的代码挪动,咱们连“两条 volatile asm 之间保序”一类的说法在现行手册里也找不到依据,咱们不替它背书。真需要一道编译器级的屏障,惯用的写法是 `asm volatile("" : : : "memory")`,一条空指令配上 memory clobber 就成了。

## 哪些寄存器碰得,哪些碰不得

咱们嵌的 asm 住在函数里,就等于住进了调用约定的世界。约定把寄存器分成两拨:`%rbx %rbp %r12 %r13 %r14 %r15` 是被调方必须原样还原的,而 `%rsp` 也必须进出相等,剩下的 `%rax %rcx %rdx %rsi %rdi` 和 `%r8` 到 `%r11` 是临时寄存器、谁用谁负责。对咱们的意义就一条:asm 里碰了谁,咱们就在 clobber 里如实登记谁。GCC 拿到咱们的申报会做两件事,给操作数挑寄存器的时候绕开这些名字,真有必要的时候还会亲手把它们存进栈再还回来。您要是碰了必须还原的那拨寄存器又瞒报,就违背了约定,靠这些寄存器保值的代码可能被安静地破坏,而编译器不会给您任何诊断。

两位特殊的得单独说。`%rbp` 在开着帧指针的时候(比如 `-O0`)进 clobber,GCC 直接就拒绝编译了,报的是 `bp cannot be used in 'asm' here`,省了帧指针才接受,而且它会自动帮您保存。而 `%rsp` 干脆不许进 clobber,GCC 16 会警告您说这个写法已经废弃。真正的规范是 asm 进出前后 `%rsp` 必须相等,咱们的指令只要不碰栈指针,天然就满足了。

## 能用 C 写,就别劳驾汇编

手艺到手了,泼冷水的忠告也该到位。其实 GCC 手册自己承认,它不解析您模板里的指令,写错了没有编译期的体检,验收的路只有看字节一条。多条独立的 asm 也不保证编译后还紧挨着,必须连续的指令请写进同一个字符串。Linux 内核的代码风格文档说得更直白:C 能干的活就别劳驾内联汇编,常见的一小段可以包成一个 C 小函数,大块的汇编进单独的 `.S` 文件。咱们这一篇学的约束、clobber、volatile 申报纪律,本身说的就是“为什么 C 更安全”,每一样都是在替 GCC 补它看不见的信息。内联汇编的正经用场,是 C++ 实在表达不了的那几行:端口 IO、停机、开关中断,咱们数得过来。

## 没讲完的,记在明处

这一篇没铺开的,咱们照例摆在桌面上:浮点和 SIMD 的约束字母、`asm goto` 跳 C 标签、`%[名字]` 式的具名操作数、`%=` 自制不重号的局部标号、`%P` 这类操作数修饰符、双方言花括号(同一段模板给 AT&T 和 Intel 各留一份写法的机关),手册里都有、咱们用到再取。机器约束字母咱们只认了 `N`,`I` 到 `M` 那一排各有各的形状。clobber 里还留了个 `"redzone"`,您在 asm 里用 call、push 的时候跟它有关系。`ret` 带立即数弹参数的老形态、成员函数的 this 指针走 `%rdi` 的细节,也都不在这一篇的盘子里。另外上一篇搁下的 `.code16gcc`,这儿能补上一半:`-m16` 的底子是 32 位代码加 16 位打包,栈类指令(push、ret)按 32 位的默认编,所以那边的字节里常多一颗 `66` 前缀。至于寻址默认的差别,咱们还留着它,等咱们真进 16 位世界再对照。

## 搓启动代码之前

开篇认下的几张生面孔,现在都干过活了:call 压栈 ret 弹栈,咱们在 gdb 里一步步走过,漏 `"memory"` 的那个 bug,咱们亲手跑出来又亲手修好。等咱们真的动手搓启动代码,机器必须亲自动手的那几行,就是照今天这一套塞进 C++ 里的。写完了,别忘了拿 `objdump` 对一眼字节。
