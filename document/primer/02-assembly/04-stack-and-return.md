---
title: 04 · 栈与返回地址
---

# 04 · 栈与返回地址,那口栈自己会说话

上一篇讲 `call` 的时候,咱们挂起过一个疑问:它跳出去之前,顺手把回家的地址放进了栈。可当时咱们对那口栈一无所知,它是圆是扁都没见过,这句话就只能当个承诺搁着。这一趟咱们不再往前跑,调头把那口栈看清楚,再回来把字条捡起来。

咱们平时写函数,写得相当随意:这个函数喊那个函数,那个函数又喊别人,喊完各自回家,一路上谁也不迷路。您想过没有,机器凭什么知道该回哪儿?函数散在内存各处,地址是链接的时候才定下来的,`_start` 根本不可能提前把每个被喊的函数的门牌号背在脑子里。答案是一张一张的字条,出去的时候留下,回家的时候照字条走。放字条的那个地方,就是栈。

咱们机器的栈住在地址空间的最高处,越用越往低处走,`%rsp` 这根栈指针就是它的水位线。咱们这就动手,看栈上的字条和那几个字节。

## push 和 pop:栈的一进一出

手册里对 `push` 只给了一句话:头一步把 `%rsp` 减下去,第二步把操作数写进新的栈顶,`pop` 反着来。您别小看这对动作,它们可是汇编里最省字节的指令之一。咱们编一个小函数,专门量一量它们在字节上占多宽,存成 `pp.s`:

```asm
	.text
	.globl PushPopBytes
PushPopBytes:
	pushq	%rax
	popq	%rbx
	pushq	%r12
	popq	%r12
	ret
```

```bash
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

咱们读法那一篇里量过,搬一个立即数用掉七个字节的是 `movq`。您再对照这里的 `pushq %rax`,它只有一个字节 `50`(输出开头那几行 `file format` 和 `Disassembly of section` 的表头,咱们一律略去不贴)。低位的八个寄存器,`%rax %rbx %rcx %rdx %rsi %rdi %rbp %rsp` 这八位,各占一个编码,`58` 起头的是 pop 家族,而 `r8` 到 `r15` 得加一颗 `41` 前缀才变成两个字节。您再看看尾巴上那个 `ret`:也才一个字节 `c3`。`PushPopBytes` 这个名字只是给这段代码起个名,咱们量的是它的字节,不喊它。

字节看过了,咱们再请 gdb 来走两步,亲眼看 `%rsp` 怎么动。这一段小程序干完活就调 Linux 的 60 号退出系统调用收工,所以咱们单步到哪儿它都不会跑飞,把它存成 `pushwalk.s`:

```asm
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
```

```bash
as pushwalk.s -o pushwalk.o
ld -o pushwalk pushwalk.o
gdb -q -batch -ex 'set debuginfod enabled off' -ex 'break *_start' -ex 'run' -ex 'p/x $rsp' -ex 'x/i $rip' \
    -ex 'si' -ex 'p/x $rsp' -ex 'si' -ex 'p/x $rsp' -ex 'si' -ex 'p/x $rsp' -ex 'si' -ex 'p/x $rsp' \
    -ex 'x/i $rip' pushwalk
```

```text
Breakpoint 1 at 0x401000

Breakpoint 1, 0x0000000000401000 in _start ()
$1 = 0x7fffffffda30
=> 0x401000 <_start>:	push   %rax
0x0000000000401001 in _start ()
$2 = 0x7fffffffda28
0x0000000000401002 in _start ()
$3 = 0x7fffffffda20
0x0000000000401003 in _start ()
$4 = 0x7fffffffda28
0x0000000000401004 in _start ()
$5 = 0x7fffffffda30
=> 0x401004 <_start+4>:	mov    $0x3c,%rax
```

`$1` 到 `$5` 是 gdb 给每次打印编的号,您别把它跟代码里 `$60` 的立即数前缀弄混了。咱们照着这几行念一遍:进 `_start` 时 `$1` 报的是 `0x7fffffffda30`,`$2` 报的是 `da28`,少了 8,那会儿咱们刚跨过 `push %rax` 这一步。再 push 一次,`$3` 报 `da20`,还是降了 8。后面两次 pop,水位线原路退了回来,`$4` 是 `da28`,`$5` 回到 `da30`。您跟着数这一进一出,一步都没错开,8 就是 x86-64 栈的粒度。

单步停在哪儿,`%rsp` 报的就是刚过去的那一步做完之后的样子,咱们读到的每一次减少,都来自上一步。那两行 `=>` 是 gdb 打的标记,指着当前要执行的指令,`objdump` 那边管同一个地址叫 `401004`,gdb 这边顺手标成了 `_start+4`,写法不一样,指的是同一条 `mov`。

上面这段读数是笔者这边跑工具的开发机上录下来的,下一节那段也一样。命令里带了 `set debuginfod enabled off`,gdb 开头那几行 debuginfod 询问就不会出现了,那本来就是它自家的环境噪音,跟咱们的实验无关。至于那串栈地址,换台机器、换套环境变量数值就变了,您别拿它对表。

## call 与 ret:返回地址就躺在栈顶

push 和 pop 的手感有了,咱们回来看 `call`。上一篇说它顺手把回家的地址记在了栈上,这句话现在能拆成两半了:`call` 干的第一件事就是一次 push,把返回地址压进栈,第二件事才是跳到目标去,跳法和 `jmp` 一样。咱们写一个最小的现场,存成 `callret.s`:`_start` 进门就 call,`Target` 里只放一句 `ret`,`AfterCall` 是返回地址落回来之后接着跑的那一段。那三行 `movq $60`、`xorl %edi,%edi` 加 `syscall` 是 Linux 的 60 号退出调用,咱们借它让程序体面退场。

```asm
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
```

```bash
as callret.s -o callret.o
ld -o callret callret.o
gdb -q -batch -ex 'set debuginfod enabled off' -ex 'break *_start' -ex 'run' \
    -ex 'p/x $rsp' -ex 'x/i $rip' -ex 'si' -ex 'p/x $rsp' -ex 'x/i $rip' \
    -ex 'x/gx $rsp' -ex 'x/i *(void**)$rsp' -ex 'si' -ex 'p/x $rsp' -ex 'x/i $rip' callret
```

```text
Breakpoint 1 at 0x401000

Breakpoint 1, 0x0000000000401000 in _start ()
$1 = 0x7fffffffda40
=> 0x401000 <_start>:	call   0x401010 <Target>
0x0000000000401010 in Target ()
$2 = 0x7fffffffda38
=> 0x401010 <Target>:	ret
0x7fffffffda38:	0x0000000000401005
   0x401005 <AfterCall>:	mov    $0x3c,%rax
0x0000000000401005 in AfterCall ()
$3 = 0x7fffffffda40
=> 0x401005 <AfterCall>:	mov    $0x3c,%rax
```

咱们一步步看。`call` 执行完,`$2` 报的 `%rsp` 从 `da40` 变成了 `da38`,又减了 8,扣掉的正是它压返回地址用掉的那 8 个字节。

这里连着两行不一样的输出,咱们别读岔了:`x/gx $rsp` 把栈顶那 8 个字节当成一个数读出来,交回来的是 `0x0000000000401005`。跟着的 `x/i *(void**)$rsp` 要绕一道:`(void**)` 说的是把这个地址上的 8 个字节当成一个指针再取出来,`*` 取到指针指的值,最后 `x/i` 拿这个值当指令地址去反汇编,于是印出了 `0x401005 <AfterCall>: mov $0x3c,%rax`。认出了 `AfterCall` 的是后一条命令,前一条只负责把栈顶那个数端出来。而 `401005` 正是 `AfterCall`,也就是 call 的下一条指令,gdb 顺手替咱们标上了名字。

`ret` 干的事,就是把它弹进 `%rip`:您看单步之后咱们果然落在了 `AfterCall`,`$3` 报的 `%rsp` 又回到了 `da40`。到这里,上一篇挂着的那个疑问就算还清了:ret 凭什么认得回家的路?它不是认得路,它是照着栈顶的字条走。

咱们补一块字节,objdump 那边留着同一份证据。callret 咱们前面已经 `as` 过、`ld` 过了,直接看链接后的它:

```bash
objdump -d callret
```

```text
0000000000401000 <_start>:
  401000:	e8 0b 00 00 00       	call   401010 <Target>

0000000000401005 <AfterCall>:
  401005:	48 c7 c0 3c 00 00 00 	mov    $0x3c,%rax
  40100c:	31 ff                	xor    %edi,%edi
  40100e:	0f 05                	syscall

0000000000401010 <Target>:
  401010:	c3                   	ret
```

咱们边指边算:call 的字节是 `e8` 打头,后面跟一个 32 位的相对距离,`0b 00 00 00` 小端读出来就是 `0x0b`。距离从 `call` 的下一条指令起算,`401005` 加上 `0x0b` 落在 `Target` 的 `401010`,而栈顶字条上的地址恰好就是这段距离的起点。同一屏里还能对上几处:`movq $60, %rax` 编成了 `48 c7 c0 3c 00 00 00`,那个 `3c` 就是十进制的 60,`xorl %edi,%edi` 编成 `31 ff`,最后 `Target` 里的 `ret` 只有一个字节 `c3`。

咱们看 `0x401000` 这个位置,它是 `ld` 默认摆下 `_start` 的地方,换一份链接脚本它就会搬家。

## 没铺开的,记在明处

栈上还有两笔,咱们这一篇不去深挖,只把名字认下来。一笔是调用约定要求的 16 字节对齐:调用发生的那一刻,`%rsp` 得落在 16 的倍数上,返回地址一压进去就低 8,所以函数体里常常不是整倍数。对齐要求在读法那一篇那段 `_start` 里能看见半截,`and $0xfffffffffffffff0,%rsp` 拿的正是那个 -16,把栈拉回 16 字节的倍数上。咱们量到的 8 是 push 一步的宽度,16 是对齐要求的刻度,两个数各管各的。

另一笔是栈顶下面那 128 个字节,行话叫红区,它只有叶子函数才用得上,也就是自己不再往下 call 谁的函数才能白用。栈帧能变长的函数怎么收场,浮点和 SIMD 的值怎么在栈上安排,咱们也都留着,等哪天用得上了再看。

## 还欠着的几笔

咱们亲手量过的东西就摆在上面:push 和 pop 一进一出各占一个字节,`%rsp` 在 `da30` 与 `da20` 之间一来一回,还有 `call` 压下的字条上写着 `0x401005`。栈上还没交代的有几笔:每次进函数时门口那两行 `pushq %rbp` 和 `movq %rsp, %rbp` 是什么名堂,函数跑完又怎么收场,参数从哪条路进来,`hlt` 那一睡也还没交代。下一篇咱们一起把它们说清楚。
