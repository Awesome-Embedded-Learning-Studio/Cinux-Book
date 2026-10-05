---
title: 01 · 上电与取指
---

# 01 · 上电与取指:第一条指令是一条远跳

咱们按下电源键的那一刻,机器眼里没有程序、没有函数、没有类型,只有一大片可以按字节寻址的数组。这句话不是笔者编的,它出自一本讲系统的书(CS:APP 2e,书页 161):机器码看待内存的方式,就是“a large, byte-addressable array”。书里的数组和结构体,到了机器码这一层,只是一串挨在一起的字节。

可咱们接下来要问的是:这串字节算什么呢?同一串字节放在那儿,换个谁来读,答案就换一个。这一篇就从这儿进,一路走到机器吐出它的第一条指令。

咱们这一路从机器里读出来的每一屏,都是模拟器里的 QEMU 加上它自带的 SeaBIOS 给出的,不是哪一台真正的机器上的样子。至于 `od`、`objdump`、`readelf`、`gcc` 那些屏,它们是咱们开发它的电脑上的工具给出的,跟模拟器无关。

## 五个字节,三种读法

咱们自己捏五个字节,拿来当题目:

```bash
printf '\xb8\x00\x7c\xcd\x10' > five.bin   # 读者自己捏的 5 个字节
```

咱们拿 `od -A x -t x1 -t x2` 按两种宽度看一眼,`b8 00` 这几个字节,按字节读是两格,按十六位小端读出来是 `0x00b8`:

```text
000000 b8 00 7c cd 10
        00b8  cd7c  0010
000005
```

咱们再把同一份文件交给 `objdump`,只换一下“按哪一代读”的眼睛:

```text
$ objdump -D -b binary -m i386:x86-64 -Mintel five.bin
0000000000000000 <.data>:
   0:	b8 00 7c cd 10       	mov    eax,0x10cd7c00

$ objdump -D -b binary -m i8086 -Mintel five.bin
00000000 <.data>:
   0:	b8 00 7c             	mov    ax,0x7c00
   3:	cd 10                	int    0x10
```

咱们看这串字节,它一格都没动,同一个 `objdump` 换一副眼睛,一种读法得到一条指令,另一种读法得到两条。`mov eax,0x10cd7c00` 和 `mov ax,0x7c00` 后面跟着 `int 0x10`,说的是同一串字节。

## 边界一错,后面全线错位

五个字节短,够不上真正要命的地方。咱们把题目换成系统自带的真码,从 `/bin/true` 入口处取 32 个字节。入口地址是 `readelf -h` 报的,把它交给 `dd` 就能截出这一段。入口地址和开头这几个字节都随发行版与 glibc 版本变,下面这一屏是本机 `/bin/true` 的样子:

```bash
ENTRY=$(readelf -h /bin/true | sed -n 's/.*Entry point address: *//p')   # 本机 0x2590
dd if=/bin/true of=true-entry.bin bs=1 skip=$((ENTRY)) count=32 status=none
```

截出来的东西,咱们就靠 `xxd` 看一眼形状:

```text
$ xxd true-entry.bin
00000000: f30f 1efa 31ed 4989 d15e 4889 e248 83e4  ....1.I..^H..H..
00000010: f050 5445 31c0 31c9 488d 3dd1 feff ffff  .PTE1.1.H.=.....
```

咱们按六十四位读,它从 `endbr64` 起一路顺下去,十一条指令各归各位。屏上的原文咱们只引十行,略去的是原文 `6:` 那一行,字节是 `49 89 d1`,它把 `rdx` 的值搬到另一个寄存器:

```text
$ objdump -D -b binary -m i386:x86-64 -Mintel true-entry.bin
0000000000000000 <.data>:
   0:	f3 0f 1e fa          	endbr64
   4:	31 ed                	xor    ebp,ebp
   9:	5e                   	pop    rsi
   a:	48 89 e2             	mov    rdx,rsp
   d:	48 83 e4 f0          	and    rsp,0xfffffffffffffff0
  11:	50                   	push   rax
  12:	54                   	push   rsp
  13:	45 31 c0             	xor    r8d,r8d
  16:	31 c9                	xor    ecx,ecx
  18:	48 8d 3d d1 fe ff ff 	lea    rdi,[rip+0xfffffffffffffed1]        # 0xfffffffffffffef0
```

咱们再换十六位那副眼睛读同一串字节,`50 54 45 31 c0` 一下子变成四条:

```text
  11:	50                   	push   ax
  12:	54                   	push   sp
  13:	45                   	inc    bp
  14:	31 c0                	xor    ax,ax
  16:	31 c9                	xor    cx,cx
  18:	48                   	dec    ax
  19:	8d 3d                	lea    di,[di]
  1b:	d1 fe                	sar    si,1
```

六十四位那边 `50` 是 `push rax`、`54` 是 `push rsp`、`45 31 c0` 是一条 `xor r8d,r8d`,三条。十六位这边:`50 54 45 31 c0` 五个字节切成四条,块里从 `18` 往后接着往下列的是 `dec ax`、`lea di,[di]`、`sar si,1`,再往后的字节就念不出完整指令了。助记符的名字也换了,可咱们真正要打起精神的地方是指令的边界。边界一错,后面全线错位。

## 字节是什么,取决于谁在解释它

`/bin/true` 的开头四个字节也是文件身份。咱们从它头上截 32 个字节另存一份,单看开头:

```bash
head -c 32 /bin/true > head32.bin          # 系统自带真码的头 32 字节
```

```text
$ head -c 32 /bin/true | xxd
00000000: 7f45 4c46 0201 0100 0000 0000 0000 0000  .ELF............
00000010: 0300 3e00 0100 0000 9025 0000 0000 0000  ..>......%......
```

咱们看它开头这一串,`7f 45 4c 46`,人人认得那是 `\x7fELF`。现在把这一小截当码看,六十四位那副眼睛念出来的是这些:

```text
$ objdump -D -b binary -m i386:x86-64 -Mintel head32.bin   # /bin/true 的头 32 字节
   0:	7f 45                	jg     0x47
   2:	4c                   	rex.WR
   3:	46 02 01             	rex.RX add r8b,BYTE PTR [rcx]
   6:	01 00                	add    DWORD PTR [rax],eax
	...
  10:	03 00                	add    eax,DWORD PTR [rax]
  12:	3e 00 01             	ds add BYTE PTR [rcx],al
  15:	00 00                	add    BYTE PTR [rax],al
  17:	00 90 25 00 00 00    	add    BYTE PTR [rax+0x25],dl
  1d:	00 00                	add    BYTE PTR [rax],al
	...
```

咱们再换十六位那副眼睛读同一小截:

```text
$ objdump -D -b binary -m i8086 -Mintel head32.bin
   0:	7f 45                	jg     0x47
   2:	4c                   	dec    sp
   3:	46                   	inc    si
   4:	02 01                	add    al,BYTE PTR [bx+di]
   6:	01 00                	add    WORD PTR [bx+si],ax
	...
  10:	03 00                	add    ax,WORD PTR [bx+si]
  12:	3e 00 01             	add    BYTE PTR ds:[bx+di],al
  15:	00 00                	add    BYTE PTR [bx+si],al
  17:	00 90 25 00          	add    BYTE PTR [bx+si+0x25],dl
  1b:	00 00                	add    BYTE PTR [bx+si],al
  1d:	00 00                	add    BYTE PTR [bx+si],al
	...
```

文件解析器读它,得到“这是一个 ELF 文件”,反汇编器读它,同一串字节就成了一堆谁也用不上的算术。机器自己不认识文件头,它只认识字节。这串字节是数据还是指令、属于哪一代的那一套,全都是解释的人加上去的:CPU 里的译码器算一个解释器,`objdump` 算一个,咱们写代码时心里那套类型也算一个。

## `eip` 那几个数,是从哪儿读来的

复位是一个有确切数值的状态。咱们让模拟器跑起来,用 GDB 连上去,在复位那一刻读三个寄存器:

```text
cs             0xf000              61440
eip            0xfff0              0xfff0
cr0            0x60000010          [ CD NW ET ]
cs:ip 折成的线性地址(cs*16+ip)= 0xffff0
```

`cr0` 的最低位是 0,也就是保护模式没开,咱们看到机器确实停在实模式里。咱们拿它报出的三个数去对 Intel 手册 Vol 3A 的 Table 9-1,一格不差:那页写着 `EIP=0000FFF0H`、`CR0=60000010H`,还写着处理器停在“real-address mode with paging disabled”。手册 §9.1.4 接着说,复位后头一条被取到并执行的指令落在物理地址 `FFFFFFF0H`,在处理器最高物理地址往下 16 个字节的位置。

## GDB 的 `$pc` 是线性地址

在复位那一刻,笔者拿 `x/8i $pc` 去看指令,屏幕上排出来八行一模一样的 `add %al,(%eax)`,像内存里全是零。同一次会话里,咱们把段也折进去再读一次,读到的才是真东西。下面两行是同一处内存、两个读法:

```text
0xfff0:	0x00	0x00	0x00	0x00	0x00	0x00	0x00	0x00
0xffff0:	0xea	0x5b	0xe0	0x00	0xf0	0x30	0x36	0x2f
```

`$pc` 在这里是线性地址的口径,它只拿了 `eip` 那个 `0xfff0` 去读,而 `cs` 是 `0xf000`,真正要读的地方得把段折进去(`x/8i $cs*16+$eip`)。咱们在模拟器上想读 CS:IP 指着的那块,得自己把段补上。这次会话里 GDB 给出的指令原文不可信:`0xffff0` 的五个字节 `ea 5b e0 00 f0` 是一条远跳,它却解成了七个字节的 `ljmp $0x3630,$0xf000e05b`。要引指令原文,咱们引 QEMU 自己的日志,它的 `-d in_asm` 才是模拟器自己的说法。

## 机器自己报出来的复位态:选择子与隐藏基址

咱们要知道,QEMU 的 monitor 是它自带的那个控制台,不是新工具,顺手教两手就够:`info registers` 打寄存器,`xp /8xb <地址>` 按字节看物理内存。冻在复位那一点上,它报出来的头几行是这样的:

```text
EIP=0000fff0 EFL=00000002 [-------] CPL=0 II=0 A20=1 SMM=0 HLT=0
ES =0000 00000000 0000ffff 00009300
CS =f000 ffff0000 0000ffff 00009b00
```

`CS` 后面跟着四格:选择子 `f000`、隐藏基址 `ffff0000`、限长 `ffff`,最后一格把访问权限的几个字节打包在一起。选择子乘十六只有 `0x000f0000`,而它此刻的基址写的是 `0xffff0000`。咱们看看手册那两段是怎么交代这个特例的:CS 分成可见的选择子和隐藏的基址两部分,实模式里基址通常由选择子左移四位得来,可复位那一次选择子装的是 `F000H`、基址装的是 `FFFF0000H`,起点就这么加出来(`FFFF0000 + FFF0H = FFFFFFF0H`)。模拟器报的这一行,跟手册一个字一个字对得上。

同一屏还白送咱们一件事,模拟器把固件映像映在了两个地方:

```text
fffffff0: 0xea 0x5b 0xe0 0x00 0xf0 0x30 0x36 0x2f
000ffff0: 0xea 0x5b 0xe0 0x00 0xf0 0x30 0x36 0x2f
```

上面那行原样是 `fffffff0: 0xea 0x5b 0xe0 0x00 0xf0 0x30 0x36 0x2f`(这里省掉的是原行末尾的回车)。`0xfffffff0` 与 `0x000ffff0` 两个物理地址读出八个一模一样的字节,这既回答了“为什么手册把第一条指令记在 `FFFFFFF0`”,也告诉咱们这串字节在低端另有一份。在模拟器这边,低端映出来的就是它自带的 SeaBIOS。

## 取指循环,落到肉眼上是一串数在动

咱们看书上把取指循环说得多直白(CS:APP 2e,书页 9):从上电到下电,处理器反复执行程序计数器指着的指令,再把程序计数器更新到下一条。“读取、解释、执行”这个模型,拼的是书页 9 上的原话。

咱们造一个 512 字节的裸扇区,让它进模拟器里跑,自己拿 GDB 单步十次,每一步都看 `eip` 落在哪儿:

```text
si0 : cs=0000 eip=7c00 线性=0x07c00  => 0x7c00:	ljmp   $0xb8,$0x7c05
si1 : cs=0000 eip=7c05 线性=0x07c05  => 0x7c05:	mov    $0xd88e0000,%eax
si2 : cs=0000 eip=7c08 线性=0x07c08  => 0x7c08:	mov    %eax,%ds
si3 : cs=0000 eip=7c0a 线性=0x07c0a  => 0x7c0a:	mov    %eax,%ss
si4 : cs=0000 eip=7c0c 线性=0x07c0c  => 0x7c0c:	mov    $0xbb7c00,%esp
si5 : cs=0000 eip=7c0f 线性=0x07c0f  => 0x7c0f:	mov    $0x1b97c00,%ebx
si6 : cs=0000 eip=7c12 线性=0x07c12  => 0x7c12:	mov    $0x2ba0001,%ecx
si7 : cs=0000 eip=7c15 线性=0x07c15  => 0x7c15:	mov    $0xca010002,%edx
si8 : cs=0000 eip=7c18 线性=0x07c18  => 0x7c18:	add    %ecx,%edx
si9 : cs=0000 eip=7c1a 线性=0x07c1a  => 0x7c1a:	mov    $0x4b,%al
si10: cs=0000 eip=7c1c 线性=0x07c1c  => 0x7c1c:	mov    $0xf4ee00e9,%edx
```

这一屏咱们只看左半边的偏移:`7c00` 到 `7c05` 跨了五格,`7c05` 到 `7c08` 跨三格,再往后是两格、两格、三格,剩下几步也都在两格到三格之间。每一步的跨度,正好是上一条指令的长度。取指循环这件事,落到能看见的层面上,就是这串数在一格一格往前挪。右边那半截指令原文别信,理由上一节交代过了。

那段 512 字节的源码,咱们自己写起来也就这么长:

```asm
.code16
.global _start
_start:
    ljmp $0x0000, $flat
flat:
    movw $0x0000, %ax
    movw %ax, %ds
    movw %ax, %ss
    movw $0x7c00, %sp
    movw $0x7c00, %bx
    movw $0x0001, %cx
    movw $0x0002, %dx
    addw %cx, %dx
    movb $0x4b, %al
    movw $0x00e9, %dx
    outb %al, %dx
1:  hlt
    jmp 1b
```

入口处 `_start` 下面那行 `ljmp` 一次把段和偏移一起换掉,是条远跳。段寄存器咱们这一篇只认到这儿:`cs` 就是那个指着代码所在段起点的寄存器,它换的时候,取指的地方跟着换。段:偏移那一套怎么在别处用起来,下一篇接着看。块里末尾那句 `outb %al, %dx` 送去的是端口 `0x00e9`。那是模拟器自己开的一个调试口,写进去的字节落进它的终端,或者 `-debugcon file:…` 指的那个文件。

## 第一条指令是远跳,而且它跨得很远

咱们把镜头挪回上电那一刻。模拟器带上 `-d in_asm,cpu` 跑起来,复位后取到的第一条指令逐字是这样:

```text
IN: 
0xfffffff0:  ea 5b e0 00 f0           ljmpw    $0xf000:$0xe05b
```

这行是模拟器自己写的,它记下那次远跳的方式就是“段:偏移”:段 `0xf000`、偏移 `0xe05b`。咱们再看它下面的寄存器块,`CS` 那一行逐字是 `CS =f000 ffff0000 0000ffff 00009b00`,也就是选择子 `f000`、隐藏基址 `ffff0000`。

跳过去之后咱们再看下一块,`CS` 那一行变了:

```text
EIP=0000e05b EFL=00000002 [-------] CPL=0 II=0 A20=1 SMM=0 HLT=0
CS =f000 000f0000 0000ffff 00009b00
```

选择子还是 `f000`,基址却从 `ffff0000` 变成了 `000f0000`。手册 §9.1.4 第二段把这件事写死了。硬件复位之后,CS 头一次装进新值的那一刻,处理器就回到实模式那套寻常算法上,手册给出的原话是“CS base address = CS segment selector * 16”。同一条 ×16 的老算术,咱们看着它复位那一刻不成立、跳一下之后就成立。代码就落在低端的 `F000:E05B`,不重装 CS 到不了那里,远跳干的正是这件事,而能改动 CS 的只有段间转移这类指令,以及中断。手册那一段还留了另一条路:固件要是打算一直待在复位基址上,它的代码里就不能出现远跳、远调用,也不能让中断发生。模拟器带的固件选了前者,于是咱们在上电之后很快就遇到了一次远跳。

在跳过去的那些块里,咱们还能捞到一条:QEMU 一次翻译的不止一条指令,它是成块翻的。不带调试器的同一趟运行,日志长这样:

```text
0x00007c05:  b8 00 00                 movw     $0, %ax
0x00007c08:  8e d8                    movw     %ax, %ds
0x00007c0a:  8e d0                    movw     %ax, %ss

----------------
IN: 
0x00007c0c:  bc 00 7c                 movw     $0x7c00, %sp

----------------
IN: 
0x00007c0f:  bb 00 7c                 movw     $0x7c00, %bx
0x00007c12:  b9 01 00                 movw     $1, %cx
0x00007c15:  ba 02 00                 movw     $2, %dx
0x00007c18:  01 ca                    addw     %cx, %dx
0x00007c1a:  b0 4b                    movb     $0x4b, %al
0x00007c1c:  ba e9 00                 movw     $0xe9, %dx
0x00007c1f:  ee                       outb     %al, %dx
```

头一块并了三条,后面一块并了七条,两边还正好在 `mov %ax,%ss` 之后断开。这里断开的理由在手册 §6.8.3:写 `SS` 的指令连同它后面一条,中间要把中断(包括 NMI)、数据断点和单步陷阱一并推迟到下一条指令的边界之后,实现只好在这里断块,才守得住。两件事咱们分开看:推迟中断是架构定下的,在哪儿断块是模拟器自己的做法。同一枚镜像、同一次运行,挂了调试器就变成一条一块,所以引这一屏的时候得说清这是没挂调试器的那一趟。也请别把模拟器的块说成 CPU 的流水线,它是 QEMU 翻译指令的单位,再往下编就不在这几篇的范围里了。

## 指令集和实现,是两层

书上把这两层分开说(CS:APP 2e,书页 10):描述每条机器码指令效果的那一层叫指令集架构(ISA),描述处理器实际怎么实现的那一层叫微架构。书页 160 还给了个说法:大多数 ISA 描述程序行为的方式,是“as if each instruction is executed in sequence […]”,像是一条做完才轮到下一条。咱们把“像”这个字眼看住,它就是上一段那几块日志的注脚。

咱们拿同一份源码编给两个目标,最能看清变的是什么。源码就三个函数,咱们把它存成 `isa.c`:

```c
long Add(long a, long b) { return a + b; }

long Sum3(long a, long b, long c) { return a + b + c; }

int Widen(short s) { return s * 2; }
```

咱们手上还是同一个 `isa.c`,换一个目标就换一副指令。下面两半都只截了其中几段:

```text
$ gcc -m32 -O2 -S -o isa32.s isa.c
Add:
	movl	8(%esp), %eax
	addl	4(%esp), %eax
	ret
...

$ gcc -m64 -O2 -S -o isa64.s isa.c
Add:
	leaq	(%rdi,%rsi), %rax
	ret
Widen:
	movswl	%di, %eax
...
```

咱们看源码,一个字都没改。变的头一件是寄存器名(`%esp` 换成了 `%rdi`、`%rsi`),第二件是参数从哪儿取(栈上取变成了寄存器里取),`Widen` 里的 `movswl`,两个目标编出来都有,变的只是源操作数从栈上换成了 `%di`。`gcc` 那个 `-m32` 与 `-m64` 的差别到底涉及哪几层,这一篇只用到目标决定指令这一层,两套调用约定的细活留给往后。

`readelf -h` 里那一行 `Machine`,记录的就是咱们这枚产物编给哪一代机器。目标文件是编出来的:

```bash
gcc -m32 -O2 -S -o isa32.s isa.c ; gcc -m64 -O2 -S -o isa64.s isa.c
gcc -m32 -O2 -c -o isa32.o isa.c ; gcc -m64 -O2 -c -o isa64.o isa.c
readelf -h isa32.o | grep Machine ; readelf -h isa64.o | grep Machine
```

`-c` 出的是可重定位的目标文件,咱们编完再问它们的 `Machine` 字段:

```text
  Machine:                           Intel 80386
  Machine:                           Advanced Micro Devices X86-64
```

更能说明同一个指令集、机器身份却可以不同的,是咱们把同一份 512 字节镜像交给四种型号的模拟 CPU 去跑。镜像里只做一件事:念 CPUID,把结果吐出来。

```text
-cpu qemu64    -> AuthenticAMD 00060fb1 QEMU Virtual CPU version 2.5+
-cpu max       -> AuthenticAMD 00000663 QEMU TCG CPU version 2.5+
-cpu 486       -> GenuineIntel 00000480 
-cpu pentium3  -> GenuineIntel 00000673 
```

镜像一个字节没改,四台“机器”都跑得动,各自报出自己是谁,厂商串、型号号和品牌串都是 CPUID 指出来的一串字节。四张脸各不相同,`486` 和 `pentium3` 报出来的只有厂商串和型号号,品牌串一栏空着,那是它们的 CPUID 扩展叶还没到那么远,能问的问题本身也随机器代际变。这几串具体的值都是模拟器这一趟的偶然结果,随 QEMU 版本就变,咱们别把它们当成 x86 的什么常数。

## 双视角锚点:同一片内存的两种读法

**内存是一大串字节。**应用程序员看过去,内存里是有类型的:数组、结构体,他关心 `sizeof` 是多少、字段有没有对齐,写下一个 `for` 循环,心里知道机器会照着他的意思跑完。这是书里的散文口径,到了内核作者这边,复位向量 `0xFFFFFFF0` 那里就是一串字节,咱们前面读出来的 `ea 5b e0 00 f0` 里没有任何类型,它是条指令还是个地址,得看谁在读。往内存里写之前,他得自己担着“我按几格写进去、就按几格读回来”这件事。

**剩下的事,机器只做一件。**应用程序员写完 `while`,心安理得地相信 CPU 在执行他的语句,书页 9 也是这个口径:PC 指着下一条,从上电到下电反复执行。内核作者看到的是另一幕:他在模拟器里盯着 `eip` 停在 `0xfff0`,第一条指令是一条 `ljmpw $0xf000:$0xe05b`,除了“下一条在哪儿、那儿的字节是什么”,机器不欠他任何解释。咱们内核作者这边还得心里有数:“看起来一条一条”是架构给的承诺,实现有自己的切法。

## 哪些是手册定的,哪些只是 QEMU 的做法

这一篇里的机器行为,咱们都是在模拟器上看的。其中复位基址 `FFFF0000`、第一条指令落在 `FFFFFFF0`,这是 Intel 手册写明的内容,模拟器照着它们兑现,所以您在一台真正的机器上也应当见到同样的数。固件映像另在低端映一份、SeaBIOS 按 `0000:7C00` 交棒,这两件事就属于 QEMU 配 SeaBIOS 自己的做法了,别的固件未必这么干,咱们手上也没有一台真正的机器去对照。

保护模式和长模式这一篇一步都没进,`cr0` 里的 PE 位只是读到、没有写过。真正的机器上,物理内存的映射长什么样,这一篇也没法给您看,咱们手上的模拟器给不了那一层底细。真处理器之间的微架构差异(计时、流水线、缓存),这篇同样做不了,CPUID 那一屏证的是机器自报的身份不同,它证不了谁快谁慢。CS:IP 折出来的线性地址怎么落到物理内存上,咱们下一篇接着看。
