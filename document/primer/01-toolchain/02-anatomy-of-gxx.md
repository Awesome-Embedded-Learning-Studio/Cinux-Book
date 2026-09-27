---
title: 02 · 编译驱动解剖
---

# 02 · 编译驱动解剖:g++ 背后指挥了谁

咱们上一章收尾的时候,给 `g++` 留了一句话:您看着只有几个字母,背地里却指挥着一串人马。这一章咱们就把这串人马点齐。点名的由头也很现成:您装的明明是 `g++`,可报错里跳出来的名字,常常是您从来没装过的 `cc1plus` 和 `collect2`。您拿 `command -v` 去找 `cc1plus`,它连影子都不给您。它坏了吗?没有的,它只是不肯搬进 PATH 而已,而咱们这一章要拆的也正是这个谜面。

这一章的战果,您当场就能数清:一条 `g++` 的背后站着 `cc1plus`、`as`、`collect2` 三位工人,每一位都会当着您的面干活,四道工序(预处理、编译、汇编、链接)的中间产物,您也都会拿到手里、看到眼里。咱们把四道工序分别跑一遍,程序照样是活的。往后遇上了编译报错,您看一眼报错的口气,也就知道该去哪道工序里抓凶手了。

## 让 g++ 自己交代:-v 里藏着谁

咱们逼司机交底的开关是 `-v`。咱们还是拿上一章的 `hello.cpp` 当标本:

```bash
g++ -v hello.cpp -o hello
```

它交出来的东西有三四十行,咱们截了长的、拿省略号盖掉了临时文件名,骨架是这样的:

```text
Using built-in specs.
COLLECT_GCC=g++
Target: x86_64-pc-linux-gnu
gcc version 16.2.1 20260810 (GCC)
 /usr/lib/gcc/x86_64-pc-linux-gnu/16/cc1plus -quiet -v -D_GNU_SOURCE hello.cpp …
 as -v --64 -o /tmp/cc….o /tmp/cc….s
GNU assembler version 2.47 (x86_64-pc-linux-gnu) using BFD version (GNU Binutils) 2.47
 /usr/lib/gcc/x86_64-pc-linux-gnu/16/collect2 -plugin …
```

您数一数,里面就藏着三位干活的。头一位就是真正的编译器 `cc1plus`:您写 `g++`,收钱办事的是它,报语法错误的也是它,只是它住在 `/usr/lib/gcc/` 的深处,不肯搬进 PATH 而已。第二位是咱们的老熟人 `as`,也就是咱们装机那天点过灯的 binutils 一家里管汇编的。第三位是管收尾的 `collect2`,链接的收尾由它出面张罗,真正抬材料的,是它请出来的链接器 `ld`。

所以 `g++` 是什么?它干的其实是司机的活,行话里的名字叫编译驱动(driver)。您报上目的地,点名的事它按顺序来,四道工序被它点着名一路走完了,中间落下的文件,就是咱们接下来要挨个拿到手里的东西。

## 头一道工序:-E,把头文件揉成一大团

咱们让司机在头一道停车的开关是 `-E`,它只干预处理的活,把结果原样吐给咱们:

```bash
g++ -E hello.cpp -o hello.i
wc -l hello.i
head -4 hello.i
tail -6 hello.i
```

```text
1097 hello.i
# 0 "hello.cpp"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3

# 3 "hello.cpp"
int main() {
    std::printf("the toolchain is alive\n");
    return 0;
}
```

咱们只写了几行的程序,预处理交回来的是一千多行。欸就有人说了：咋多出来的？

答案是全是 `#include` 带进门的:`<cstdio>` 的背后,是整套 C 的标准头文件,这道工序把它们原样揉了进来。那些 `#` 打头的行是行号注释,意思读出来是“接下来的内容,来自某个文件的第几行”。所以您在尾巴上又见到了 `# 3 “hello.cpp”`,咱们亲手写的 `main`,连一个字都还是原样的。宏的展开、条件编译的取舍,也都是这道工序的活,咱们一句话记下就好。

## 第二道工序:-S,拿到亲笔写的原稿

```bash
g++ -S hello.i -o hello.s
wc -l hello.s
grep -n -A9 'main:' hello.s | head -14
```

```text
28 hello.s
9:main:
10-.LFB2:
11-	.cfi_startproc
12-	pushq	%rbp
13-	.cfi_def_cfa_offset 16
14-	.cfi_offset 6, -16
15-	movq	%rsp, %rbp
16-	.cfi_def_cfa_register 6
17-	leaq	.LC0(%rip), %rax
18-	movq	%rax, %rdi
```

咱们跟两张新面孔提前混个脸熟:`pushq %rbp`、`movq %rsp, %rbp`,到了汇编那一卷,咱们再把它们逐字说明,而您眼下看到的,就是人家亲笔写下的原稿。`.s` 文件就是 `cc1plus` 一行一行写出来的。

原稿里还藏着一桩小小的案子。咱们源码里写的明明是 `printf`,可您往后翻几行,调用的是 `call puts@PLT`(`@PLT` 是动态链接的中转记号,这一卷咱们不用管它)——欸?咱们的 `printf` 呢?这是 `cc1plus` 干的好事:它看您只打一个字符串加换行,自作主张换成了更省事的 `puts`。破案的位置就在第二道工序里。您留意这个手法,到了下一道,证据马上就露头了。

## 第三道工序:-c,半成品 hello.o

```bash
g++ -c hello.s -o hello.o
file hello.o
nm hello.o
```

```text
hello.o: ELF 64-bit LSB relocatable, x86-64, version 1 (SYSV), not stripped
0000000000000000 T main
                 U puts
```

`file` 交回来的那个词,您在上一章就见过:relocatable,说的就是可以重新安家的事。现在您能对上号了,它就是汇编这道工序交出来的半成品。

真正的新证据,在 `nm` 的输出里。`T` 的意思是“在本地安家”,`main` 落了户。`U` 的意思是“还没着落”,`puts` 还悬着呢:它的真身在 libc(C 标准库)里,咱们只是喊了一声,该到的人还没到。欠下的名额该谁来补呢?补上的活,归了链接这一道。您看,上一节的 `puts` 案到这里对上了:第二道工序换了人,第三道工序留下了欠条,把人找到的,是最后一道的链接。

## 第四道工序:链接,collect2 张罗收尾

```bash
g++ hello.o -o hello2
./hello2
```

```text
the toolchain is alive
```

活了,而且它跟司机全程开下来的那个,是一模一样的。咱们这一趟,把四道工序分别跑了一遍,走到这个地步它还活着,四道工序谁离了司机都能单独接活。

回看 `-v` 里被截断的 `collect2` 长命令,现在咱们能读懂它的货单了:`crt` 打头的这一类文件是启动文件,crt 读的是 C runtime、C 运行时。PIE 说的就是位置无关的可执行文件,如今链接器的默认出品,这一类里头一个叫的就是 `Scrt1.o`,`-l` 开头的是库,咱们那句没人补的 `puts`,就是在 `-lc` 那里找到的人。链接的门道,这一卷第 04 篇咱们就亲手写链接脚本。启动文件和库搜索的深水,咱们记在债上,更后面的课再还。

## 四道工序,一张图

```text
hello.cpp -- 预处理(-E) --> hello.i
hello.i   -- 编译(-S)  --> hello.s
hello.s   -- 汇编(-c)  --> hello.o
hello.o   -- 链接       --> hello(可执行)
```

等报错冒了头,咱们就按图上标好的工序找。

## 报错来了,挨着工序问

头一桩案子说的就是头文件找不到。咱们现场造一桩:

```bash
printf '#include "no-such.h"\nint main() { return 0; }\n' > badinc.cpp
g++ -c badinc.cpp
```

```text
badinc.cpp:1:10: fatal error: no-such.h: No such file or directory
    1 | #include "no-such.h"
      |          ^~~~~~~~~~~
compilation terminated.
```

喊冤的是预处理——“您要的门牌,我挨家挨户找了个遍。”它的找法是:双引号包的头文件,它会在源文件自家的目录里找一遍,找不到的话,再去系统的目录。尖括号包的,一出门就是奔系统目录去的。您自己写的头文件放歪了,您就用 `-I` 把目录递给它。

另一桩常见的是语法报错,咱们现场造一桩少个分号的案子:

```bash
printf 'int main() { return 0 }\n' > badsyn.cpp
g++ -c badsyn.cpp
```

```text
badsyn.cpp: In function ‘int main()’:
badsyn.cpp:1:22: error: expected ‘;’ before ‘}’ token
    1 | int main() { return 0 }
      |                      ^~
      |                      ;
```

说话的就是 `cc1plus` 本人,行号、位置、连该怎么补的分号,都画给您了。报错的抬头没有别人,正是编译工序的本人。

最后一桩是链接的 undefined reference。咱们声明一个函数,身体却是一直欠着的:

```bash
printf 'int missing();\nint main() { return missing(); }\n' > badlink.cpp
g++ badlink.cpp -o badlink
```

```text
/usr/bin/ld: /tmp/cc….o: in function `main':
badlink.cpp:(.text+0x5): undefined reference to `missing()'
collect2: error: ld returned 1 exit status
```

这一回说话的是链接器 `ld`:您喊了 `missing`,它把该找的地方都找了,也确实没有它的下落。第三行的 `collect2` 最容易被冤枉,您翻遍论坛,到处都是骂它的声音,可它其实只是个传话的,翻过来的意思,无非是 ld 退场的时候带了个错误码回来。真正的病因在它上面那两行,您别去动 collect2,它送的是信。

## 债:specs、库搜索与优化

`-v` 的头一行写着 “Using built-in specs.”。specs 是司机随身的行车手册,里面写着各式默认的动作,改了它,司机走的路线就会变,咱们不碰,咱们知道有它就行。`-l` 背后的库搜索顺序、启动文件的分工,都压在咱们的债上,到了更后面的课再收。`-O` 一家的优化开关,属于编译工序里的另一套旋钮,眼下是用不上的,留给以后省字节的场合。

## 司机点完名

咱们拆到最后一道,谜底的边上还挂着新问题:链接这道工序去哪里找人,看的是“给谁编”。给咱们自己的电脑编,libc 是现成就有的。可要给一台没有操作系统的机器编,咱们要的人,又该从哪里来?下一章的 host、target、freestanding 三个词,接的就是这一问。
