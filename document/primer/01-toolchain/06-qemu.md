---
title: 06 · QEMU 镜像与真跑
---

# 06 · QEMU 镜像与真跑:把末尾的两格交给机器

上一篇收尾的时候,咱们把话搁在那儿了:要请出一台模拟出来的机器,把这枚镜像按机器自己的方式装进去,看它能不能真的动起来。模拟出来的机器就是 QEMU,今天咱们把它请到桌上来。不过在动手以前,有一处区别得跟您交代清楚,不然您带着上一篇的印象往下走,是会绕错方向的。上一篇讲的是操作系统里的加载器,它捧着 ELF 的文件头,按文件里写着的意思,把程序摆到该在的地方。今天固化在 ROM(只读存储器)里的 BIOS 恰恰相反,它一个文件头都不看。它只认盘上第一个扇区的 512 个字节,原样搬走,末两字节得是 `55 aa`。两桩事都像是“把程序放进去”,其实根本不是一回事,今天这场实验就是要把界线跑给您看。

笔者到现在还记得那枚镜像刚躺在目录里的样子。您右键它,菜单里没有“打开方式”这一项。您给它 `chmod +x` 挂上可执行的位,系统照样只会回您一句格式错误。您双击它,那就更别提了。512 个字节的文件里,能算得上机器码的只有开头八个字节,剩下的五百多个字节里,大半也都是零,末尾的两格 `55 aa` 也不是代码,它只是一张凭条:BIOS 拿起这块盘,头一件事就是翻到最后两格去看签名,对不上就不要。这东西从头到尾就没打算让人自己点开,它得被一台机器认领,而认领它的那套说法,今天咱们一条条亲手试。

咱们今天要跑出来的东西,说穿了就这么点:一枚 512 字节的镜像,在模拟器里吐出它这辈子第一个字节。另外还有两句失败话在路上等着,以后您再在屏幕上遇上它们,能一眼分出是哪一句在跟您说话。

## 能引导的盘:不写链接脚本的那一版,和它的 1064 个字节

咱们得有能引导的盘,才有得谈。源码短得有点可怜,七行有效代码,编出来是八个字节的机器码,干的事情只有一件:往串口写一个 `K`,写完原地打转。

```asm
.code16
.globl _start
_start:
    movw $0x3f8, %dx
    movb $'K', %al
    outb %al, %dx
1:  jmp 1b
```

`0x3f8` 是串口的数据端口,`outb` 干的活就是把 `al` 里的字节送出去。`1: jmp 1b` 是往自己身上跳,CPU 从这儿起就一直在原地打转,再也不往下走了。源码不长,安排得倒是挺周到。它不依赖屏幕,不依赖键盘,也不碰任何一块内存,您只要把串口接上,它就说得出话。

咱们把这几行存成 `mbr.s`(`mbr` 就是主引导记录的缩写),交给汇编器。这一步有个开关不能漏:

```bash
as --32 -o mbr.o mbr.s
```

`--32` 是让汇编器把这几个字节编成 32 位那一套的样子。您要是漏了它,汇编器不吭声,退出码是 0,产物照样交到您手上,只是它已经是个 64 位目标文件了,752 个字节。它一直要到链接那一步才出问题。我这儿给它起名叫 `mbr64.o`,好跟带 `--32` 编出来的分开,拿 `ls -l` 一量就出来,再拿它去链:

```bash
as -o mbr64.o mbr.s
ls -l mbr64.o
```

```text
-rw-r--r-- 1 … … 752 Oct  1 17:46 mbr64.o
```

```text
$ ld -m elf_i386 -Ttext 0x7c00 --oformat binary -o mbr.bin mbr64.o
/usr/bin/ld: i386:x86-64 architecture of input file `mbr64.o' is incompatible with i386 output
```

咱们在这一步上有两个开关要对上:`--32` 说的是“汇编成哪一套”,`-m elf_i386` 说的是“按哪一套链接”,错在哪一步就在哪一步报,不是汇编期当场翻脸。

到了第一个真实的分岔口。您最省事的想法,大概是把手里的 `.o` 直接丢给链接器,连链接脚本都不写一个:

```bash
ld -m elf_i386 -Ttext 0x7c00 --oformat binary -o mbr.bin mbr.o
ls -l mbr.bin
xxd mbr.bin | tail -2
```

```text
-rwxr-xr-x ... 1064 ... mbr.bin
00000410: 0200 01c0 0400 0000 0000 0000 0100 01c0  ................
00000420: 0400 0000 0100 0000                      ........
```

1064 个字节。咱们要的是 512,它给了 1064。您再把目光挪到尾巴上,最后两格里躺着的是 `0000`。这枚镜像不能引导,病根之一就在签名上:BIOS 拿到盘,头一件事就是翻末尾两格,读不到 `55 aa`,当场就不认它。多出来的那批字节是顺带搭车的,不是它不能引导的原因。

那 1064 个字节是怎么凑出来的?咱们按节表数一遍,就是三笔:机器码 `8` 个字节,接在后面的零 `1016` 个,再贴一段 `.note.gnu.property` `40` 个字节。8 加 1016 加 40,正好 1064。您看那 1016 个零,没有人往文件里写过它们,它们只是中间空出来的地界被铺平了。

那段 40 字节的注记,就是您在 04 篇里见过的 `.note.gnu.property`,汇编器顺手塞进来的元数据。`--oformat binary` 的搬法太老实了,凡是标了“要占内存”的段,它一律按顺序铺进文件。`-Ttext 0x7c00` 在这里只给了链接器一个地址,旁的什么都没管,段怎么排、签名落在哪、文件多长,`-Ttext` 一概不操心。

正路得把这件事写进说明书,咱们换一份链接脚本上来。这一块咱们存成 `tiny-raw.ld`,别拿它去盖 04 篇的 `tiny.ld`。两份脚本不是一份,咱们眼前印出来的这一版短些,理由留在下面交代。

```ld
SECTIONS
{
  . = 0x7c00;
  .text : { *(.text) }
  . = 0x7c00 + 510;
  .sig : { SHORT(0xAA55); }
}
```

```bash
ld -m elf_i386 -T tiny-raw.ld --oformat binary -o mbr.bin mbr.o
ls -l mbr.bin
xxd mbr.bin | tail -1
```

```text
-rwxr-xr-x ... 512 ... mbr.bin
000001f0: 0000 0000 0000 0000 0000 0000 0000 55aa  ..............U.
```

512 个字节,末尾 `55aa`。咱们跟前面那版对一下,那 552 个字节的差是怎么来的。两版都占着同一个输出名 `mbr.bin`,想摆在一起比,得各自留个底:错版那枚重链一次存成 `wrong.bin`,这一枚存成 `right.bin`,下面那行 `cmp` 才对得上名字。那 40 个字节的注记,两份产物里都有,它不是这 552 的来源,差的是它落在哪儿。错版把它推到了文件最末尾:从第 8 个字节起垫了 1016 个零,一路垫到 `0x400` 才把它贴上去,注记后面一个字节都不剩。正版让它紧跟在机器码后头,占掉第 8 到第 47 个字节,再往后的零只铺到第 510 格,那是 462 个,接上末尾两格签名正好 464。拿两份产物 `cmp` 一下,第 9 个字符位(`cmp` 从 1 数起,也就是偏移 8)就开始分岔。1016 减 464,正好是那 552。您自己 `xxd right.bin | head -3`,一眼就能看见那 40 个字节跟在代码后头:

```bash
ld -m elf_i386 -Ttext 0x7c00 --oformat binary -o wrong.bin mbr.o
ld -m elf_i386 -T tiny-raw.ld --oformat binary -o right.bin mbr.o
xxd right.bin | head -3
```

```text
00000000: baf8 03b0 4bee ebfe 0400 0000 1800 0000  ....K...........
00000010: 0500 0000 474e 5500 0200 01c0 0400 0000  ....GNU.........
00000020: 0000 0000 0100 01c0 0400 0000 0100 0000  ................
```

咱们看头一行的前八个字节 `ba f8 03 b0 4b ee eb fe`,那就是那四条指令,紧跟其后的 `04 00 00 00 18 00 00 00` 起,就是那 40 个字节的注记,您一格格数下来正好对上。两份产物的首次分岔也照实摆出来:

```bash
cmp wrong.bin right.bin
```

```text
wrong.bin right.bin differ: char 9, line 1
```

游标那两行干的活也在这里,咱们看它干的是哪两件。`. = 0x7c00 + 510;` 把游标摁到第 510 格,`SHORT(0xAA55)` 占住最后两格,文件也就停在了第 512 个字节上。前一版里 `0x1fe` 的两格是 `00 00`,压根没有签名,这才是它不能引导的第一位原因。那 40 个字节的注记只是搭个顺风车。

`8 + 1016 + 40` 这三笔,咱们不靠节表,直接从字节上看一遍。签名在错版里是什么:

```bash
xxd -s 0x1fe -l 4 wrong.bin
```

```text
000001fe: 0000 0000                                ....
```

它报的是 `0000`,签名一个格都没写进去。咱们再看错版尾部那一整段:

```bash
xxd -s 0x400 -l 48 wrong.bin
```

```text
00000400: 0400 0000 1800 0000 0500 0000 474e 5500  ............GNU.
00000410: 0200 01c0 0400 0000 0000 0000 0100 01c0  ................
00000420: 0400 0000 0100 0000                      ........
```

从 `0x400` 到 `0x428` 正好 40 个字节,内容就是那 `.note.gnu.property`。三笔到这里咱们能逐字节复算:`wrong.bin` 长 1064,`[8,1024)` 那 1016 个字节全是零,`[1024,1064)` 这 40 个字节跟 `right.bin[8:48]` 逐字节相同。合起来还是那句,`8 + 1016 + 40 = 1064`。

这就是 04 篇那套手法的复用:拿游标把 `.text` 放在 `0x7c00`,再让游标跳到 510,用 `SHORT(0xAA55)` 把签名落下去。这三步一个字都没改,可脚本本身跟 04 篇的 `tiny.ld` 不是一份,我得交代一句。头一处是 `ENTRY(_start)`:产出的既然是裸二进制,没有 ELF 文件头来记入口地址,这一行在链接那一步不起作用,我拿它试过,加上这行产物一个字节都不变。第二处是 04 篇为守 510 字节立的那行 `ASSERT`,这里也没写,这一版没有这道守卫。第三处是 04 篇那块 `/DISCARD/`,这一篇的脚本里没写,所以那 40 个字节的注记留在了镜像偏移 8 到 47 的位置里(它在不在看写没写 `/DISCARD/`,落在哪则是 `--oformat binary` 按 LMA 铺字节的结果)。它跑不到(机器码最后一条就是往自己身上跳,CPU 从那儿起不会往下走),不影响引导,但 04 篇教的那一手,这一篇确实没抄。今天多出来的,是那个签名的落款终于有活干了:少了 `.sig` 的落款,后头的模拟器会对咱们说一句很难听的话,咱们到讲失败的地方再听。

`-T` 和 `-Ttext` 分属两个开关,咱们也分开摆着。本机 `ld --help` 里两行都在,只是隔得远:一行在第 64 行,一行在第 173 行,中间隔了一百多行。您拿它自己复核就知道了:

```text
  -T FILE, --script FILE      Read linker script
  -Ttext ADDRESS              Set address of .text section
```

这两行咱们摆在一起看,一个读的是整份脚本,一个只给个地址。上面那两版差的正是开关,不是“顺手多给它一个参数”就能了事的。

## QEMU 的参数,每个都在回答一个问题

镜像在手里了,咱们来点机器。命令本身很短,可是每一截都值得停一下:

```bash
qemu-system-x86_64 -fda mbr.bin -boot a -nographic -display none -no-reboot
```

`-fda mbr.bin` 是把文件挂成 0 号软盘。参数语义咱们不猜,man 页上写着 “Use file as floppy disk 0/1 image”…(底下还有半句是指路的)。那 QEMU 里跑着的那个,后文咱们就叫它客户机,跟外面这个敲命令的终端分清楚。为什么挑软盘呢?因为一张软盘最前面就是这么一个扇区,“盘上第一个扇区”在这里最不绕人。

`-boot a` 这一截,得单独说清楚它到底在干吗。man 页的原话里有一句“hard disk boot is the default”,x86 机器本来就从硬盘试起。所以 `-boot a` 的作用不是“让它能跑”,它只是把机器试硬盘那一轮省掉。您要是不写它,屏幕上就是这个样子:

```text
Booting from Hard Disk...
Boot failed: could not read the boot disk

Booting from Floppy...
K
```

这机器冲硬盘问了一句,没挂硬盘,它就回了一句 `could not read the boot disk`,然后才回头去试软盘,这才轮到咱们那个 `K`。这段不是故障,它是正常的试盘顺序,只是白占了两行版面。

`-nographic` 是不要图形窗口:它关掉图形输出,把模拟出来的串口重定向到当前终端上,跟 QEMU 自己的 monitor(可以敲它内部命令的那个控制台),混在一路。在一台连 X 都没有的机器上,也就是跑不了图形窗口、只能盯着终端的机器,客户机打出来的字总得有个去处,这个开关就是在安排这件事。`-display none` 跟它是两层,`-nographic` 已经包含了“不起图形”,您显式再写一遍会更清楚。日后想开个窗口看看,把这一截换成 `-display gtk` 就行,两处并列着写并不冲突。

`-no-reboot` 管的是客户机要求重启的时候直接退出。咱们那枚镜像自己是个死循环,可谁说它一定碰不到重启的路径呢?真碰上了,您要是不写这个开关,QEMU 就老老实实从头再来一遍,您坐在那儿等到天亮也等不来一个提示符。

还有一对开关容易漏,漏了它们,起 QEMU 那一步一声不吭,只是让您白等,那就是 `-S` 和 `-gdb`。后面讲调试的那段要拿 GDB 连上去看指令,靠的就是它们俩。man 页把话说得很直。

```text
`-S`    Do not start CPU at startup (you must type 'c' in the monitor).
```

```text
`-gdb dev`    Accept a gdb connection on device dev … Note that this option does not pause QEMU execution -- if you want QEMU to not start the guest until you connect with gdb and issue a continue command, you will need to also pass the `-S` option to QEMU.
```

man 页上取下来的原话,一个字没动,只有块首那对反引号是咱们加上去认开关的。不过我得交代一句:这里我从两处拼到一块儿,man 页上它们之间还隔着别的选项,不是挨着的两段。

`-gdb` 自己不停机。它只是给您开了个口子,让 GDB 能连进来,CPU 自己照跑不误。您要“等 GDB 连上再跑”,就得把 `-S` 和 `-gdb` 一起给它。这两样漏法,现象不一样。漏了 `-gdb`,`-S` 把机器摁在门口等着,可没人能进来,GDB 那边 `target remote` 完就回一句 `could not connect: …`,大意是连不上,您连机器的影子都碰不着。漏了 `-S`,`-gdb` 把口子开着,GDB 也真的连上了,可机器自己早跑过去了,`eip` 已经到了 `0x7c06`,您什么也没看着。

还有一条旁白:`-fda` 这类简写会带出一条 `WARNING`,大意是没指定镜像格式,探测之后猜成了 raw。它不挡事,您换成完整写法 `-drive format=raw,file=mbr.bin,if=floppy`,它就消失了。免得您照着敲的时候以为自己的机器出了毛病,这一屏也照实录贴上。实录里那一跑的文件叫 `mbrB.bin`,下面这屏我按您手上的 `mbr.bin` 改了文件名,别的字一个没动:

```text
WARNING: Image format was not specified for 'mbr.bin' and probing guessed raw.
         Automatically detecting the format is dangerous for raw images, write operations on block 0 will be restricted.
         Specify the 'raw' format explicitly to remove the restrictions.
```

## 把这枚镜像挂上去:谁在说话,谁在跑

咱们把前面那行 `qemu-system-x86_64` 敲下去,屏幕上是这么一片:

```text
SeaBIOS (version Arch Linux 1.17.0-2-2)
...
Booting from Floppy...
K
```

屏幕上那个孤零零的 `K` 一出来,从写代码到现在,咱们等的就是它。这里要分的是谁在说话:`SeaBIOS (version …)` 是固件的自我介绍,`Booting from Floppy...` 也是它的话,它在告诉咱们下一个要试软盘。这两句都属于客户机里的固件,跟咱们的代码没关系。最后那个 `K`,才是咱们自己写的八个字节发出来的。

顺着固件往下,咱们说一句 BIOS 是什么。《深入理解计算机系统》(封面上印着 CSAPP 五个字母)书页 567 上有一句:“Programs stored in ROM devices are often referred to as firmware. When a computer system is powered up, it runs firmware stored in a ROM.” 上电的时候,跑起来的是 ROM 里的固件,PC 上这块固件就叫 BIOS。QEMU 里替咱们扮这一角的,是 SeaBIOS。

话说回来,这本书也就管到这一句为止了。咱们手上这几本书翻一遍,刚说的那本 CSAPP,还有《Linux 命令行大全》(TLCL)、《UNIX 环境高级编程》(APUE)、《UNIX/Linux 系统编程手册》(TLPI),后三本对 QEMU 一个字都没有。CSAPP 呢,除了刚才那句 BIOS 的定义,既没有引导扇区,也没有 `0x7c00`,连复位向量,也就是机器上电以后取的第一条指令的地址,和 SeaBIOS 的名字都没露过面。**这一篇没有书可依,这是实情。** 那今天这些参数、这些现象打哪儿来?不懂就去翻 man 页,翻完再跑一遍,看它到底说了什么。

还有一件事让人安心,咱们顺手看一眼:同一枚 512 字节的镜像,`-hda` 把它挂成 0 号硬盘,也一样能引导。

```text
$ qemu-system-x86_64 -hda mbr.bin -nographic -display none -no-reboot
...
Booting from Hard Disk..K.
```

您看 `Hard Disk..K.` 挤在同一行里,字面上的差别就只有这么一点。SeaBIOS 认的还是末尾的 `55 aa`,挂在哪个接口上,它不太计较。咱们这一路留在软盘上,前面挂 `-fda` 的时候交代过了,图的就是“一个扇区”。

## 两句失败话,长得不一样

这两句话以后您会常遇上,今天咱们让它们各出场一次。

一句是 `not a bootable disk`。咱们把签名的两格抹成 `0000`,文件仍是 512 字节,别的地方什么都没动:

```text
$ qemu-system-x86_64 -fda nosig.bin -boot a -nographic -display none -no-reboot
WARNING: Image format was not specified for 'nosig.bin' and probing guessed raw.
         Automatically detecting the format is dangerous for raw images, write operations on block 0 will be restricted.
         Specify the 'raw' format explicitly to remove the restrictions.
SeaBIOS (version Arch Linux 1.17.0-2-2)
...
Booting from Floppy...
Boot failed: not a bootable disk
Booting from DVD/CD...
Boot failed: Could not read from CDROM (code 0003)
Booting from ROM...
iPXE (PCI 00:03.0) starting execution...ok
iPXE initialising devices...ok
```

`not a bootable disk` 就是“末两字节不是 `55 aa`”的正式说法。盘它读到了,拿起来一看凭条不对,于是放回去接着试下一件设备。底下那几行 DVD/CD、ROM,还有 iPXE 这个网络引导的固件,都是它在挨个试别的路子,跟咱们的镜像已经没关系了。您要是碰上这一句,回去敲一条 `xxd mbr.bin | tail -1`,看看最后两格。

另一句是 `could not read the boot disk`。它出现在刚才那段没挂硬盘的现场里。这句话说的是没这个设备,盘压根没递到固件手里,签名对不对都还没轮到验。碰上它,您回去查参数和文件名就是了,别去翻镜像的字节,末尾两格好着呢。

两句对上两类毛病,咱们分诊就这么简单:

| 屏上那句 | 毛病出在哪 | 回去查什么 |
|---|---|---|
| `Boot failed: not a bootable disk` | 盘在,末两字节不是 `55 aa` | `xxd mbr.bin \| tail -1` |
| `Boot failed: could not read the boot disk` | 根本没这个设备 | `-fda`/`-hda` 和文件名 |

把它们混成一句“跑不起来”最耽误事,因为您会对着镜像的字节看半天,病却在参数上。

## GDB 连上去,只做三件事

调试器第一次登场。今天咱们只让它做三件事,连上去,读 `cs:eip`,再走一步。就这三件,再多一件都不碰。

我用的配方长这样。这里说的“前端”,是 QEMU 那一串按架构分开的可执行文件:`qemu-system-i386` 和 `qemu-system-x86_64` 各是一个,挑哪个,就等于告诉 GDB 机器按哪套架构自报。这回我挑的是 `i386` 那个:

```bash
timeout 25 qemu-system-i386 -drive format=raw,file=mbr.bin,if=floppy -boot a \
  -nographic -display none -S -gdb tcp:127.0.0.1:1235 &

gdb -q -nx -batch \
  -ex 'set architecture i8086' \
  -ex 'target remote 127.0.0.1:1235' \
  -ex 'info registers cs eip' \
  -ex 'x/3i $cs*16+$eip' \
  -ex 'si' \
  -ex 'info registers cs eip' \
  -ex 'detach'
```

这里必须是 `qemu-system-i386`。您换成 `qemu-system-x86_64`,两条路都堵死。不设架构吧,GDB 回您一句 ``Invalid register `eip'``,因为在 64 位那边这个寄存器叫 `rip`,压根没有 `eip` 这一号。您把架构设成 `i8086` 吧,它又抱怨:

```text
warning: Selected architecture i8086 is not compatible with reported target architecture i386:x86-64
```

这不是 GDB 的毛病,它问的是“谁在描述机器”:`x86_64` 前端替客户机自报的是 `i386:x86-64`,GDB 就按那个来。换个前端就通了,咱们要看的东西本来就是 16 位的。

GDB 连上以后,我拿到的第一屏是这样:

```text
cs             0xf000              61440
eip            0xfff0              0xfff0
   0xffff0:	ljmp   $0x3630,$0xf000e05b
   0xffff7:	das
   0xffff8:	xor    (%ebx),%dh
```

咱们看 `cs=0xf000`、`eip=0xfff0`。`cs` 是代码段寄存器,`eip` 是段内的偏移,平常实模式下的机器就是拿“段 + 偏移”凑出一个物理地址的。眼前这一刻得例个外:**复位那一刹,`cs` 的基址不是 `0xf000` 乘十六,手册给的是 `0xffff0000`**,所以上电以后头一条指令落在 `0xfffffff0`,不是 `0xffff0`。`0xffff0` 上摆着的字节跟 `0xfffffff0` 上的一模一样,可那是同一段固件在低端的影子,不是复位的第一站。它执行的还是固件,不是咱们的程序,别记串了。`ljmp` 是主板留给 BIOS 的一块跳板,往 `0xf000:e05b` 跳过去。

咱们走一步:

```text
0x0000e05b in ?? ()
cs             0xf000              61440
eip            0xe05b              0xe05b
```

`eip` 从 `0xfff0` 变成了 `0xe05b`。就这么一下,咱们算是亲眼看见一颗 CPU 迈出去了。跳板底下那套 BIOS 自检,今天咱们不追。

## 那一行反汇编,请您别信

咱们接着往下试一次,这回让 GDB 停在咱们自己的代码上,拿到的那一屏里有一行得单独拎出来:

```text
=> 0x7c00:	mov    $0x4bb003f8,%edx
```

这是错的。咱们写的是 `.code16`,那几个字节的意思其实是 `movw $0x3f8,%dx` 加上 `movb $0x4b,%al`。GDB 把它们按 32 位的路子解了一遍,于是 `ba f8 03` 跟 `b0 4b` 五个字节,被它硬凑成了一条 `mov $0x4bb003f8,%edx`。

病根在于 `set architecture i8086` 只让寄存器按 16 位看,反汇编器没跟着切过去。您要看指令,得请 QEMU 自己开口。它有个 `-d in_asm`,把翻译过的每一块指令都记进日志,写到哪个文件由跟着的 `-D` 点名:

```bash
qemu-system-x86_64 -fda mbr.bin -boot a -nographic -display none -no-reboot \
    -d in_asm -D qemu-asm.log
```

日志两万多行,里头能捞出来的东西分几处。头一处是 BIOS 把扇区搬到 `0x7c00`,咱们看,目标地址明晃晃地写在指令里:

```text
0x000f2c39:  0f b6 d3                 movzbl   %bl, %edx
0x000f2c3c:  b8 00 7c 00 00           movl     $0x7c00, %eax
0x000f2c41:  e8 31 fe ff ff           calll    0xf2a77
```

`movl $0x7c00, %eax` 让我盯着看了好一会儿。04 篇里那个 `0x7c00` 是咱们在链接脚本里亲笔写下的,咱们只把它当成一条硬性要求。到这儿才第一次看见,搬东西的人,自己手里也攥着同一个数,而且是个立即数,明明白白写在指令里。

另一处是咱们自己写的三行。同一批字节,QEMU 按 16 位解得板板正正:

```text
0x00007c00:  ba f8 03                 movw     $0x3f8, %dx
0x00007c03:  b0 4b                    movb     $0x4b, %al
0x00007c05:  ee                       outb     %al, %dx
```

最后还有咱们写的那个死循环:

```text
0x00007c06:  eb fe                    jmp      0x7c06
```

一会儿工夫,咱们把三处凑到一块儿了:`xxd` 看到的前五字节是 `ba f8 03 b0 4b`,QEMU 把三行解成 `movw $0x3f8,%dx`、`movb $0x4b,%al`、`outb %al,%dx`,而 `0x4b` 正是 ASCII 的 `K`。方才屏幕上那个孤零零的 `K`,就是让 `outb` 送出去的。看到这三处严丝合缝地对上,我坐在那儿愣了有几秒——从 BIOS 把它搬进 `0x7c00`,到它吐出第一个字节,整条路在这里合上了,中间一处假设都没有。

## 停在自家门口,和 `ax` 里残留的那个 `0xaa55`

再往前一步,让 GDB 停在咱们自己的代码上。QEMU 那一半跟前面一模一样,端口也没换,`-S` 照样带着,整条不必重敲。GDB 这一半照着前面的配方改了改:加了 `break *0x7c00` 和 `continue`,看寄存器那两句换了名单,`x/3i` 换成了只看一条,`detach` 也去掉了。跟上面那一版比,只有这两行是新的:

```bash
  -ex 'break *0x7c00' \
  -ex 'continue' \
```

我跑出来是这样:

```text
Breakpoint 1 at 0x7c00
Breakpoint 1, 0x00007c00 in ?? ()
cs             0x0                 0
eip            0x7c00              0x7c00
ax             0xaa55              -21931
dx             0x0                 0
=> 0x7c00:	mov    $0x4bb003f8,%edx
0x00007c03 in ?? ()
eip            0x7c03              0x7c03
ax             0xaa55              -21931
```

上面那行 `=>` 后面印的是 GDB 的反汇编,咱们得自己心里有数。那是它按 32 位解出来的,错的。真身是 `movw $0x3f8,%dx` 加上 `movb $0x4b,%al`,底下讲 QEMU 日志的时候会把对的摆出来。

`cs=0x0`、`eip=0x7c00`。能说准的是**代码落在 `0x7c00` 这个物理地址上**,那是咱们让链接脚本摁下去的。至于段寄存器,今天别急着下判断:同一个物理地址,`0000:7c00` 和 `07c0:0000` 两种写法都合法,而 `cs` 在引导扇区里读出来的数**未必可信**。这件事下一篇专门说。

还有 `ax` 里的 `0xaa55`。签名是个十六位的数 `0xAA55`,按小端躺下,低字节 `55` 在前、高字节 `AA` 在后,写出来才是 `55 aa`。`ax` 里这个数正好跟它一样,所以更得说清:它不是咱们写的。咱们那七行源码里,一个字都没提过 `ax`,它进咱们代码的时候就已经是这个值了。那是搬扇区留下的痕迹,残留在寄存器里的旧值,跟咱们的代码没有关系。咱们的签名从头到尾只住在文件末尾的两格里,两者不是同一处东西。

咱们再走一步,`eip` 从 `0x7c00` 到了 `0x7c03`,第一条指令跑完了。到这儿,GDB 的事就为止了。再往下的活儿都是下一篇的正课,今天不越界。

## 排查:那串 Boot failed,哪句在说我

排障这件事,今天只做一件:告诉您碰上一串 `Boot failed` 的时候,怎么分句。刚才咱们见过的那两句,病因各自在哪,已经说过了,这里不再走一遍。真正要补的,是第三个名字。您把 `Boot failed: Could not read from CDROM (code 0003)` 读出来,它说的是光驱那边没东西,跟您一点关系都没有。所以一串滚下来,您只要认准自己认得的那两句,别的都是这机器在挨个试设备。

要是您看到的是 `K` 后面黏着一截 `qemu-system-x86_64: terminating on signal 15`,也不用急着找错。这句话不是客户机说的,它是 QEMU 被外面的 `timeout` 收走时留下的收尾通知。前面那个 `K` 已经到位了,该看到的都看到了。这件事跟 `-no-reboot` 是两码事。`-no-reboot` 管的是客户机自己要求重启的时候 QEMU 不再重来。`timeout` 管的是外层到点把 QEMU 收走。咱们那枚镜像是死循环,不靠它结束,这屏就会永远停在那儿。

还有一种情况,您会觉得机器慢得不像话:同一条命令,换个环境快了十几倍。这跟参数没关系,是加速器的事。我这儿是这样:

```text
$ qemu-system-x86_64 -accel help
Accelerators supported in QEMU binary:
tcg
mshv
nitro
kvm
$ ls /dev/kvm
ls: cannot access '/dev/kvm': No such file or directory
```

`-accel help` 列出来的 kvm,说的是 QEMU 这个程序里带没带这一类支持,它带了。真正决定能不能用的是 `/dev/kvm` 存在不存在,而本机没有。所以咱们这一趟走的是 TCG,纯软件模拟,一条指令一条指令地在软件里解释着跑。您要在自己有 `/dev/kvm` 的机器上跑同样的命令,它会快得多,可是行为与输出是一样的,今天这些现象一处都不会变。

## 债:剩下的开关、剩下的设备,和剩下的一条回路

`-d` 底下还有别的通道没有动,咱们记着一笔。QEMU 把客户机的指令一块块翻译着跑,`cpu` 是在每一块进场之前把寄存器快照吐出来,不是每条指令都记。`int` 和 `exec` 各管一头,以后真需要看细节了再请它们出场。QEMU 的其余设备也全欠着,网卡、USB、PCI 拓扑,`-M` 挑的是机型,配哪套芯片组、哪套外设归它说了算,今天一个都没碰。加速器这一块今天只说到 TCG,`kvm` 和 `mshv`、`nitro` 差在哪儿,得等真有一台能用的机器才好讲。

盘子这一层也记一笔。今天这枚镜像是 raw 的裸字节,一个扇区。多扇区引导怎么排、分区表长什么样、qcow2 和 raw 差在哪儿,都在后面。实模式到保护模式那一步(也就是从段加偏移换成另一套寻址方式),更是另开一卷的课。UEFI 引导跟 BIOS 完全是两条路,今天咱们走的小路,离它还远。

至于 GDB,今天只做到了“停在自家门口”。断点条件怎么写、寄存器怎么现场改、怎么看内存、怎么把这一串命令攒成一个顺手的小脚本,还有那个最要紧的节奏,改一行、重新看一遍,这些活儿都在前面等着咱们,下一篇就接着走。单步那对开关的差别,得等手上有带符号的东西了才说得清,也留给后面。

## 两格签名,换回来一个字母 `K`

咱们今天从头到尾,就是拿文件末尾的两格,换回来屏幕上的一个字母 `K`。1064 和 512 之间多出来的那批字节是打哪儿来的,04 篇和 05 篇里咱们都见识过它。今天真做到的,是让 GDB 连上客户机,在 `0x7c00` 上按了个断点,`continue` 过去,再用 `si` 走了一步,看着 `eip` 从 `0x7c00` 挪到 `0x7c03`。这几个动作今天只是碰了一下,下一篇咱们就正式回到调试器上,把这套回路接着搭下去。
