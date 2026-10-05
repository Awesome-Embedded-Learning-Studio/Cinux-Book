---
title: 05 · ELF 深读
---

# 05 · ELF 深读:三张纸,把一个文件从头读到脚

04 篇补上那块 `/DISCARD/` 以前,`objcopy -O binary` 抽出来的字节里,`hi` 后面还跟进过这么一串 `04 00 00 00 20 00 00 00…`,它来自汇编器塞进去的 `.note.gnu.property`。现在咱们换个方向,要把文件头、节表、程序头表从头读一遍了,读完您至少能说清它们各管什么。读它们用的家伙什是 `readelf`,它装机那天跟着 binutils 进了门,一直没派上过正经的用场。实验对象是两样公开的东西。一样是系统里的 `/bin/true`,您不用编就有。另一样是咱们当场编的三个十行以内的小程序:`tiny.c` 这个程序里只有一个 `main` 函数,`greet.c` 打了一行字,`bss.c` 挂了一个 400 KB 的全局数组。添上 `-no-pie` 是为了把住址固定下来,地址固定了读起来才清爽。三份源码就这么长,咱们把它们摆在桌上:

```c
int main(void){return 0;}
```

```c
#include <stdio.h>
int main(void) {
    printf("hi from ELF\n");
    return 0;
}
```

```c
int big[100000];
int main(void){ return big[0]; }
```

它们各自存成 `tiny.c`、`greet.c`、`bss.c`。您照着数一眼:`tiny.c` 一行的那个只有一个 `main`,`greet.c` 五行,`bss.c` 两行的那个挂着一个 `int big[100000]`,就是那 400 KB 的全局数组。三条命令摆开:
```bash
gcc -no-pie -o tiny tiny.c
gcc -no-pie -g -o greet greet.c
gcc -no-pie -o bssdemo bss.c
```

## 一个文件头,它交代了整份文件怎么读

`readelf -h` 问的就是文件头,咱们拿 `tiny` 看一枪:
```text
$ readelf -h tiny
ELF Header:
  Magic:   7f 45 4c 46 02 01 01 00 00 00 00 00 00 00 00 00 
  Class:                             ELF64
  Data:                              2's complement, little endian
  Version:                           1 (current)
  OS/ABI:                            UNIX - System V
  ABI Version:                       0
  Type:                              EXEC (Executable file)
  Machine:                           Advanced Micro Devices X86-64
  Version:                           0x1
  Entry point address:               0x401020
  Start of program headers:          64 (bytes into file)
  Start of section headers:          13968 (bytes into file)
  Flags:                             0x0
  Size of this header:               64 (bytes)
  Size of program headers:           56 (bytes)
  Number of program headers:         15
  Size of section headers:           64 (bytes)
  Number of section headers:         29
  Section header string table index: 28
```

这一屏说的全是文件该怎么读。头 16 个字节里就躺着魔数和两个开关了,咱们用 `xxd -l 64` 逐格对了一遍:

```text
$ xxd -l 64 tiny
00000000: 7f45 4c46 0201 0100 0000 0000 0000 0000  .ELF............
00000010: 0200 3e00 0100 0000 2010 4000 0000 0000  ..>..... .@.....
00000020: 4000 0000 0000 0000 9036 0000 0000 0000  @........6......
00000030: 0000 0000 4000 3800 0f00 4000 1d00 1c00  ....@.8...@.....
```

咱们看头一行开头的 `7f45 4c46`,它就是魔数,后面每一格都能在表格里找到归处:
| 偏移 | 字节 | 在文件头里是谁 |
|---|---|---|
| `0x00` | `7f 45 4c 46` | 魔数 `\x7fELF` |
| `0x04` | `02` | `Class: ELF64`,2 就是 64 位 |
| `0x05` | `01` | `Data:` 的 1,是小端 |
| `0x10` | `02 00` | `Type: EXEC` 的 2,代表可执行 |
| `0x12` | `3e 00` | `Machine:` 的 0x3e,就是 x86-64 |
| `0x18` | `20 10 40 00` | 入口 `0x401020` |
| `0x20` | `40 00` | 程序头表的门牌 64 |
| `0x28` | `90 36` | 节表的门牌 13968 |

这两颗开关能说明的是,`Class` 和 `Data` 管的就是怎么读,`02` 说的是 64 位,`01` 说的是小端。入口位置躺着的 `20 10 40 00` 得倒过来念,低位字节排在了最前,咱们拼起来就得到了 `0x401020`。程序头表的门牌紧跟在 64 字节的头后面,节表远在 `13968` 的位置。
类型字段里写着的会说话。咱们编的 `tiny` 报的是 `EXEC (Executable file)`,住址是固定的,系统自带的 `/bin/true` 报的却是 `DYN (Position-Independent Executable file)`,入口写的就是 `0x2590` 这么个数。`DYN` 其实就是 PIE,地址是相对的,您可别拿它的入口跟着咱们的 `EXEC` 并在一起念了。

## 节表:文件被切成了哪些块

要看的东西都记在节表里,咱们把它一行一行数出来:
```text
$ readelf -S tiny
There are 29 section headers, starting at offset 0x3690:
Section Headers:
  [Nr] Name              Type             Address           Offset
       Size              EntSize          Flags  Link  Info  Align
  …
  [10] .text             PROGBITS         0000000000401020  00001020
       0000000000000101  0000000000000000  AX       0     0     16
  [23] .data             PROGBITS         0000000000404000  00003000
       0000000000000010  0000000000000000  WA       0     0     8
  [24] .bss              NOBITS           0000000000404010  00003010
       0000000000000008  0000000000000000  WA       0     0     1
  …
Key to Flags:
  W (write), A (alloc), X (execute), M (merge), S (strings), I (info),
  L (link order), O (extra OS processing required), G (group), T (TLS),
  C (compressed), x (unknown), o (OS specific), E (exclude),
  D (mbind), l (large), p (processor specific)
```

`.text` 和 `.data` 报的都是 `0x40…`,那些是它们在内存里的真住址。后面的 `.symtab`、`.strtab`、`.shstrtab` 报的全是 `0`,它们都进不了内存,是给链接器和调试器留的登记册。
最左边的 `Type` 说的是这一块的性质,`PROGBITS` 说的是这一块有位,而 `NOBITS` 说的是它没有位,后面那个 `.bss` 会当场演给您看。`Flags` 那一列是最容易读错的一列,所以咱们请工具自己来给权威解释。`readelf -S` 在输出末尾自带了 key,那里逐字写着的是 `W`、`A`、`X`、`M`、`S`、`I` 这几个字母的全名。
咱们看 `A` 字母,它其实是 alloc,要占运行时的内存,它说的既不是读写权限,也不是可执行的权限。
`tiny` 身上最漂亮的一处,是 `.text` 的地址 `0x401020` 跟文件头里那个入口一模一样。
```text
$ objdump -d tiny
0000000000401020 <_start>:
  401020:	f3 0f 1e fa          	endbr64
  401024:	31 ed                	xor    %ebp,%ebp
```
不过这是本例的性质,咱们不能当规律背。咱们编 `tiny` 的时候加了 `-no-pie`,`_start` 又正好被摆在 `.text` 的开头,才有这么一处巧合。`/bin/true` 这类 `DYN` 文件就不一定了,它的入口并不落在 `.text` 的第一格上。

## 一个 400 KB 的数组,装进 15 KB 的文件

`.bss` 是登记册里奇怪的一位。咱们跑一次 `size bssdemo`,它报的 `bss` 是 400032 个字节,文件自己才占了 15856 个字节,这事有点不对了。两个数都摆在同一屏里:

```text
$ size bssdemo
   text	   data	    bss	    dec	    hex	filename
   1127	    488	 400032	 401647	  620ef	bssdemo
$ ls -l bssdemo
-rwxr-xr-x 1 … … 15856 Sep 30 14:34 bssdemo
```

`size` 报的是三段各占多少字节,那个数组自己占 `100000 × 4`,就是 400000 个字节,`bss` 那一栏多出来的 400032 里,还含着别的 `.bss` 内容 32 个字节。`ls -l` 量的是文件自己躺在地上的长度,15856 字节,换算过来是 15.48 KiB,咱们嘴上说的 15 KB 是往下截了一档。一个 400 KB 的数组,文件里占的却是 15 KB 这个量级,节表里写的这个正是:

```text
$ readelf -S bssdemo
  …
  [23] .data             PROGBITS         0000000000404000  00003000
       0000000000000010  0000000000000000  WA       0     0     8
  [24] .bss              NOBITS           0000000000404020  00003010
       0000000000061aa0  0000000000000000  WA       0     0     32
  …
```

`Type` 里写着的 `NOBITS` 就是没有位的一段,这个名字摆在了那里,咱们到现在才把它说清。文件里却没给它留地方,加载器照着程序头表的 `MemSiz` 划好地盘,把缺的字节填成零。
`FileSiz` 说的是文件里的长度,`MemSiz` 说的是内存里的长度。咱们回 `tiny` 的程序头表上验这个说法,那个 `RW` 的 `LOAD` 的两个数是 `0x1e8` 和 `0x1f0`,差了 8 个字节,`tiny` 里 `.bss` 的大小正好是 `0x8`。
咱们把这件事跟 04 篇接上:`objcopy -O binary` 会把占内存、要装进镜像的段按 LMA 铺进文件,那批 `.note` 就是这么溜进文件的,`.bss` 的 `NOBITS` 是它的反面,它们本来就不是一回事了。

## 程序头表:装载的人只看这一张

节表给链接器看的是怎么切,程序头表给加载器看的是怎么搬。咱们看的就是同一个文件,它里面节表有 29 个了,程序头表只有 15 个了,数目还不一样了。

```text
$ readelf -l tiny
Elf file type is EXEC (Executable file)
Entry point 0x401020
There are 15 program headers, starting at offset 64
Program Headers:
  Type           Offset             VirtAddr           PhysAddr
                 FileSiz            MemSiz              Flags  Align
  PHDR           0x0000000000000040 0x0000000000400040 0x0000000000400040
                 0x0000000000000348 0x0000000000000348  R      0x8
  INTERP         0x00000000000003ac 0x00000000004003ac 0x00000000004003ac
                 0x000000000000001c 0x000000000000001c  R      0x1
      [Requesting program interpreter: /lib64/ld-linux-x86-64.so.2]
  LOAD           0x0000000000000000 0x0000000000400000 0x0000000000400000
                 0x0000000000000558 0x0000000000000558  R      0x1000
  LOAD           0x0000000000001000 0x0000000000401000 0x0000000000401000
                 0x0000000000000131 0x0000000000000131  R E    0x1000
  LOAD           0x0000000000002000 0x0000000000402000 0x0000000000402000
                 0x0000000000000178 0x0000000000000178  R      0x1000
  LOAD           0x0000000000002e28 0x0000000000403e28 0x0000000000403e28
                 0x00000000000001e8 0x00000000000001f0  RW     0x1000
  …
```

那 15 个里有一类的名字叫 `LOAD`,加载器看到它就明白了,这一段要从文件里搬到内存里去了。这样的条目只有 4 个,那 29 个节里进了段的是 24 个,剩下的 `[0]` NULL、`.comment`、`.symtab`、`.strtab`、`.shstrtab` 五个不属于任何一段,这就是两张表分工的最硬证据。它们怎么缝上,工具把它直接印在了末尾(带上 `readelf -S` 管节表、`readelf -s` 管符号表,两个开关只差一个字母的大写小写),咱们只取其中两行:

```text
 Section to Segment mapping:
  Segment Sections...
  …
   03     .init .text .fini 
   05     .init_array .fini_array .dynamic .got .got.plt .data .bss 
  …
```

段 3 里装着的 `.init .text .fini` 就是这三块,段 5 里装着的数据是那一摊,段 5 里连 `.bss` 也装了进去。`Flags` 这一列在两张表里说的并不是一回事。节表的 `Flags` 是 `A/W/X/M/S/I` 这一套字母,程序头表的 `Flags` 说的是内存页的权限,段 3 报的 `R E` 是可读可执行的代码段,`E` 没出现的段不可执行,这是内存保护的现场,底下那层页表的事留给后面。
再往下的那个 `LOAD` 一口气搬走了 `0x558` 个字节,连 ELF 头自己和程序头表都一起装了。书里点过这件事的,不过书上举的是 32 位那版,它那个例子的第一段从 `0x08048000` 起,咱们这儿是 64 位,数字换成了 `0` 和 `0x558`。`INTERP` 那一行请出来的解释器是 `/lib64/ld-linux-x86-64.so.2`,把 libc 挂上的事就归它管了。

## 名字那一摊:两张符号表

咱们看 `tiny` 身上的两张表,它其实分成上下两段,上段的只有 5 条,下段的却有 36 条。把它们读出来的开关就是 `readelf -s`:
```text
$ readelf -s tiny
Symbol table '.dynsym' contains 5 entries:
   Num:    Value          Size Type    Bind   Vis      Ndx Name
     0: 0000000000000000     0 NOTYPE  LOCAL  DEFAULT  UND 
     1: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND _[...]@GLIBC_2.34 (2)
     2: 0000000000000000     0 NOTYPE  WEAK   DEFAULT  UND _ITM_deregisterT[...]
     3: 0000000000000000     0 NOTYPE  WEAK   DEFAULT  UND __gmon_start__
     4: 0000000000000000     0 NOTYPE  WEAK   DEFAULT  UND _ITM_registerTMC[...]
Symbol table '.symtab' contains 36 entries:
   Num:    Value          Size Type    Bind   Vis      Ndx Name
  …
     4: 0000000000401070     0 FUNC    LOCAL  DEFAULT   10 deregister_tm_clones
    29: 0000000000401020    38 FUNC    GLOBAL DEFAULT   10 _start
```

`.dynsym` 是动态的一张,`.symtab` 是全的,36 条名字都记在里面的,所以 `main` 和 `_start` 都上了这本登记册,符号表里写着的还有 `Ndx`,那是这个符号住在第几号节的意思。`.dynsym` 里那些名字挂着的 `UND` 其实是 undefined,它的意思很直白,这个名字根本不在本文件里面的,它得靠别人来补上的,libc 就是那个别人了。
`stripped` 剥掉的正是 `.symtab` 里面的登记。`/bin/true` 是被剥过的,所以您拿 `readelf -s /bin/true` 去找 `main`,怎么都找不到。`tiny` 是咱们自己编的,咱们没剥它,`main` 和 `_start` 都在这本登记册上了。stripped 不等于什么名字都没有了,它剥掉的是完整的一本,动态的一本还在。

## 同一批字节,两副面孔

`readelf` 管的是文件的结构,`objdump` 管的是字节怎么读。咱们请 `objdump` 来收个尾,它能给您两种看法:

```text
$ objdump -S greet
0000000000401136 <main>:
#include <stdio.h>
int main(void) {
  401136:	55                   	push   %rbp
  401137:	48 89 e5             	mov    %rsp,%rbp
    printf("hi from ELF\n");
  40113a:	48 8d 05 c3 0e 00 00 	lea    0xec3(%rip),%rax        # 402004 <_IO_stdin_used+0x4>
  401141:	48 89 c7             	mov    %rax,%rdi
  401144:	e8 e7 fe ff ff       	call   401030 <puts@plt>
  …
  40114f:	c3                   	ret
```
再来看 `tiny` 那边的 `objdump -d`,它只用汇编的,`greet` 身上加了 `-S`,所以把源码和字节并排放了,代价是编译时会多留一份调试信息的,所以它的编译命令上添了 `-g`。再说这里的 `-S`,咱们接着看地址,`<_start>` 在 `.text` 家里报的地址是 `0x401020`,`main` 落在了 `0x401116`,两个地址一比,`main` 就在 `.text` 的范围内。
咱们上面这一块换成了 `greet`,它的 `main` 住在 `0x401136`,跟 `tiny` 的 `0x401116` 差了 32 个字节,换个文件 `main` 就换了地方。

## 读出来一大屏、找不到 main、数组不见踪影

头一个症状是 `readelf -l` 甩了一大屏,咱们不知道从哪一行下眼。咱们按三张纸的顺序走,它们指的就是文件头、节表、程序头表,文件头里要读的是 `Entry point address`,它就是咱们的起跑线,咱们再把两个表的门牌顺手记下来。真的入口要去符号表里找,咱们在 `tiny` 的 `.symtab` 里能读到一条 `_start`,它的 `Value` 就是 `0x401020`,这个数跟文件头里的入口对得上,咱们的 `main` 只是被它回头叫到的一个普通函数。
咱们敲 `objdump -d tiny | grep -A3 '<_start>'` 这行命令,`grep -A3` 的意思是连标号下面三行一起印出来,地址和它身边的指令都在里面。如果咱们连 `_start` 都查不着,那多半是文件被剥过了,咱们只能从文件头记下的入口去猜。
第二个症状就是 `readelf -s` 找不到 `main` 的,它的来源多半在剥符号表这一步。剥掉的是 `.symtab`,`.dynsym` 只收动态链接要用的那几个名字。您在自己的程序上跑一眼 `file`,报 not stripped 的就带着完整的一层,报 stripped 的,就是被剥过一遍的,符号表跟着短了一截。
第三个症状是那个 400 KB 的数组。`size bssdemo` 报出的 `bss 400032` 就是这么个数,回头 `ls -l` 只剩了 15 KB,您难免怀疑编译器把数组吃掉了。数组并没有丢了,它就是 `.bss` 里的 `NOBITS`,文件里就没给它留地方。

## 债:重定位、动态链接器与权限下的页表

这篇欠下的债不少。`.rela.dyn` 那批重定位、`puts@plt` 为什么绕这么大一圈、PLT 与 GOT 那一对表怎么配合作业,都是链接与装载后面的正课。`INTERP` 请出来的 `ld-linux` 怎么把 libc 挂上,咱们记在这儿。`.eh_frame` 和 `.sframe` 是异常与栈回溯要用的表,`.gnu.hash`、`.gnu.version` 归动态链接器管了,这些债也留到以后再还了。`.init_array` 加上 `__libc_start_main` 的启动链,欠的是入口约定和运行时那一笔,今天咱们只认了脸。段权限那一列 `R E` 与 `RW` 的背后是页表,那是操作系统那层的活。`Type` 的全貌不止 `EXEC` 与 `DYN`,32 位的 ELF 另有一套数字,咱们今天都没碰,`readelf` 还有别的开关,留到要用的时候再认。
还有 `.note.gnu.property` 的旧债,咱们一直没有结,它这一回也还躺在文件里面。偏偏是 `objcopy -O binary` 这一类不问名字的搬法,才会把它一起装进裸镜像里去,这一类顺手牵羊的搬法,咱们在 04 篇那块 `/DISCARD/` 上已经吃过它一回亏了。
下一篇里咱们要请出一台模拟出来的机器,把它按自身的方式装进去,看它能不能从起跑线上真的迈出第一步。
