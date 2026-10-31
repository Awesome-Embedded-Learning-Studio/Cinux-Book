---
title: 05 · 终端与伪终端:一门纪律,一对环,一群人的键
---

# 05 · 终端与伪终端:一门纪律,一对环,一群人的键

第 5 部走到第五章,咱们背上的欠条攒了一沓,今天开门就点名。5.4 收卷的时候,桥边留了话:键盘认字是 5.5 的戏,那一章只把 xHCI 的链路送到桥边。同章交底又记下,伪终端与 TTY 都记在 5.5 名下。

再往前,4.5 押在咱们名下的有四票:前台组的细目,TTY 行规程,^C 的来路,今天挨样还。线程与信号的交互那一票,今天还终端读的一角,线程住进同一副身子之后信号给谁的那半,记在 5.6 名下。卷尾指的两扇门,一扇内存序,4.6 已开,另一扇 TTY 等着今天。4.6 收官又点了两票:DMA 与缓存怎么打招呼,记在块设备名下,5.4 已兑,TTY 与线程的戏,今天开演。5.1 交底里那句老话:0/1/2 三座后头键盘与控制台的来路,5.5。欠条点名完毕,咱们开工。

这一章的活,咱们数下来四样,次序照着当年的脚印。头一样,把终端从全机器一台的单例分出来,长成可以成群造的类:一门行纪律,几座接线,谁家的终端谁自己接。第二样,立开关面:三位开关各管各,问话门落进 ioctl,线格式三十六字节,中间还压着一桩咱们自己量错了层的案子。第三样,铸对:伪终端一开就铸出一对,两截环夹一台真行规程,fd 头三号的假座也在这场戏里让咱们换成真座。末一样,归属:控制终端,前台组,^C 的翻译与投递,一台终端的键到这时才知道该发给谁。顺路咱们再各看一眼两件小事:isatty 真探的是哪一问,poll 的就绪脸上长什么样,都不抢正戏的台。

量法还是三条。码打头,实况锚仍是 r34_compile_self 的克隆,与 5.4 同一口锅,本章的码全按它数,行号给到文件名加冒号加行号,两章的数字您可以直接对着看。

书翻两本。TLPI 翻第 62 章 Terminals 与第 64 章 Pseudoterminals:书页 1292 与 1293 的问话门,书页 1297 的控制字符,书页 1375 与 1376 的铸对,另把书页 708 前台组那句老话请回。xv6 翻回书页 14、17、28 与 57 到 59:fd 的三号约定,最低空闲的章程,consoleintr 攒行的那一段。引文逐字核过,书里跨行断开的词拼回整词,您拿着原句去核,借得准不准,您说了算。

屏,如实交代:本章没有新起的屏。一个字三份来历的那幅画面,sleep 被 ^C 打断的那场实弹,当年的卷里都是拿串口上的话转述的,没有留下能誊的屏面,咱们不硬造,引旧话处注明出处。

> **5.1 交底时撂下过一句:0/1/2 三座后头键盘与控制台的来路,5.5。您接着往下想:shell 手里的三座,底下坐的到底是什么,是键盘,是屏幕,还是一段代码?再往深一层:您敲下一个 ^C,它起初只是一个字节,后来却成了一个信号,这一路它走过几道门,谁认得它,谁改写它,末了落在谁的头上?**
>
> 底下坐着的是一台行规程,一段管键进键出的纪律代码,键盘与屏幕只是它接的外线。^C 那个字节,从键码翻译进纪律,被开关面认出是信号字符,翻成二号,挂起,等读者上门取走,发给终端记着的前台组,读的调用答一声 EINTR。这一章四程,把这整条路一节一节点亮:纪律,开关,铸对,归属。

## 一门纪律,多台终端

进门咱们把词立住。行规程,也叫行纪律,line discipline,终端里管输入纪律的那段代码:键来了攒成行,该回显的回显,该擦的擦,攒够一整行才交给读的人。您天天面对的 shell 提示符,后头坐的就是它。

xv6 也有同一门手艺,书页 58 的原话:“The job of consoleintr is to accumulate input characters in cons.buf until a whole line arrives. consoleintr treats backspace and a few other characters specially. When a newline arrives, consoleintr wakes up a waiting consoleread (if there is one).”攒行,退格特判,唤醒等着读的人,一门不缺。差别在 xv6 把这门手艺长在 console 驱动里,咱们把它分了家。书页 59 还给了个总评:“A general pattern to note is the decoupling of device activity from process activity via buffering and interrupts.”缓冲与中断,让设备的事和进程的事互不牵扯。咱们分得更细:纪律,接线,节点,三家分住,后头一样一样见。

正文《假文件与真终端》那一卷之前,咱们全机器只有一台终端,单例。单例为什么非分不可,那一卷的原话说得直白:“伪终端成对地出生,每一对里要有一台自己的行规程,而单例连第一对都给不出。”分的思路,当年是这么定的:行纪律本身不含任何硬件,键从哪来,回显往哪去,等不着键怎么让出 CPU,全是装配的事。纪律留下,管线交出去,交给的物件叫接线。

咱们管接线的形状叫 TtyWiring,三件事四座,外加一枚上下文词,tty.hpp 的 :38 到 :53:

```cpp
struct TtyWiring {
    /// The word every seat receives back, unchecked.
    void* context = nullptr;

    /// Pulls one input byte when one waits; false when none does.
    bool (*take_key)(void* context, char* out) = nullptr;

    /// Whether the input seat holds a byte right now, non-destructive.
    bool (*has_key)(void* context) = nullptr;

    /// Where the discipline's own echo characters go.
    void (*echo)(void* context, char key) = nullptr;

    /// How a reader with no input yet lets the rest of the world run.
    void (*wait)(void* context) = nullptr;
};
```

输入的事占两座:take_key 取一个键,has_key 只看不动手。echo 送纪律自己的回显。wait 教等不着键的读者怎么把 CPU 让出去。context 是那枚上下文词,每座函数收回去的都是它,纪律不问它指向谁。

这个形状您在 5.1 见过同款:FsOps 的每枚函数指针,搭配的也都是 self 那样的上下文词。当年的原话请回:“函数指针加上下文词,与后端接口的 pair 形状、设备脸的上下文词,是同一个思想的三次落地。”一门纪律代码从此可以喂任意多台终端:控制台的实例把座位接到串口加键盘、屏幕回显与调度让出上,kConsoleWiring 就在 console_tty.cpp 的 :46。取键的次序串口在最前,键盘次之,USB 键盘 poll 殿后,console_take_key 的 :18 到 :32。串口与 PS/2 键盘是前两张进键的嘴,USB 键盘是第三张,来历咱们留到键归一组再细说。

constinit 的家法在终端身上连考两回,咱们如实记。头一考是构造:内核自己的终端要住进函数内的 constinit 存储,构造就不能带任何运行期的步骤,所以 Tty 的默认构造零动作,出生的终端安安静静,wire 才把座位与出生的开关一次性交给它,tty.hpp 的 :108。

第二考是懒挂:控制台的终端头一回被用到时,开机横幅早已把屏幕的行列定了型,窗口的形状得咱们那时再量。ConsoleTty 的落地在 console_tty.cpp 的 :68 到 :77:static constinit 的实例躺着,没接线的现接,窗口是零的现整形,运行期的 static 一个不剩。

首版在这里翻过车,学费实打实。头一版咱们在函数内写了带构造参数的 static 终端,syscall 的上下文里一声一般性保护异常。头一个判读是 freestanding 的世界里没有 __cxa_guard:编译器给函数内带构造的 static 配的运行期守卫,头一回调用把构造悄悄跑一遍,往后每回进门都得问过它。

深挖下去翻了案,当年的原话:“它其实只是第一嫌疑人,不是真凶”,那一声异常另有病根,埋在离终端很远的地方,咱们不在这里展开。判读错了,改零动作加 wire 却无论如何是对的:constinit 要的就是零动作的家底,这一考真假凶手都躲不过。

然后是纯逻辑化。console、键盘、串口、调度器这些名字,从行纪律的本体文件里全部搬了出去,搬进单独的控制台接线文件。搬完,行纪律的本体咱们不必开机就能在宿主上喂键、读行、翻开关,宿主长凳从四案起步:喂一行收一行,管行编辑的 ICANON 落下后字节裸着直通,管回显的 ECHO 落下后取键静默,攒行的池子短读留余。就绪的脸也从这里长出来,开篇答应的 poll 的就绪脸就是它,4.5 的 ppoll 问就绪,问的也是它,咱们顺着看:has_input 问的是整条输入路上有没有事,池子里有行算,攒了一半的行算,键还坐在座位上也算,tty.cpp 的 :155 到 :161。为什么半行也算,正文《亲手一玩,三件补齐》的原话:“就绪的回答只须承诺一件事:现在去 read,会有进展的。”行规程跑在 read 这一路里,原始的键还躺在驱动队列里的时候,整行的池子是空的,就绪的脸要是只看池子,答应过的进展就赖掉了,当年 ash 就卡在这扇门外。

纪律成了类,可开关一盏没立。三位开关怎么各管各,线上的记录到底几字节,里头还有一桩咱们自己量错的层,下一节当面算。

## 三位开关与线上三十六字节

开关面这个词咱们立在这:termios 的记录,几位开关加一串控制字符,一位一位管着行纪律的行为。出生三位全开,终端生下来就是最讲章法的形态,谁要裸的终端,自己来关。出生那两笔就在 wire 的手里,tty.cpp 的 :42 到 :50:

```cpp
    const std::uint32_t kBornFlags = kTermiosIsig | kTermiosIcanon | kTermiosEcho;
    base::CopyBytes(termios_ + kTermiosLflagAt, &kBornFlags, sizeof(kBornFlags));
    termios_[kTermiosCcAt + kTermiosVintr]  = kTtyIntrChar;
    termios_[kTermiosCcAt + kTermiosVquit]  = kTtyQuitChar;
    termios_[kTermiosCcAt + kTermiosVerase] = 0x7F;
    termios_[kTermiosCcAt + kTermiosVkill]  = 0x15;
    termios_[kTermiosCcAt + kTermiosVeof]   = 0x04;
    termios_[kTermiosCcAt + kTermiosVmin]   = 1;
    termios_[kTermiosCcAt + kTermiosVsusp]  = kTtySuspChar;
```

三位开关咱们一位一位过。ECHO 管回显:位落下了,echo_char 把键扣下不送,tty.cpp 的 :60 到 :64,静默地攒行。ICANON 管行编辑:位落下了,行编辑整个停摆,字节来了直通进池不等成行,回车也不当成换稿的暗号,feed 里的裸通道在 :98 到 :103。退格怎么办,您顺手看一眼。两种退格都认,`\b` 与 0x7F,is_erase 在 :14。擦一个字,回显走三笔 `\b`、空格、`\b`,:104 到 :111,把屏上的字盖掉再收回光标。

ISIG 管信号字符,可它眼下只存位不动作,当年的分界句原样请回:“ISIG 咱们存位不动作:信号字符的翻译与投递是作业控制那一篇的正菜,开关住进记录里的只有位,行为是后到的,这句分界咱们说在前面。”行为归本章末一程,咱们不提前唱。

终端的问话走 ioctl 这一扇,咱们叫它问话门,四个号一处登记:读的那一问 TCGETS,号坐 0x5401。写的三问 TCSETS、TCSETSW、TCSETSF,从 0x5402 起排,都在 tty_config.hpp 的 :74 到 :77。三问共用一份记录,分的是生效的时机,当年的口诀:“裸的 TCSETS 当场就换,W 那一问等输出排空了再换,F 那一问连攒着的输入也冲了再换。”书页 1293 把三问的章程写在一处,咱们引 F 那一问对应的 TCSAFLUSH 条目:“The change takes effect as for TCSADRAIN, but, in addition, any input that is still pending at the time the change takes effect is discarded.”连攒着的输入一并冲掉,正是 F 那一问的脾气。书里前一页还交代了这些函数出了终端就翻脸,书页 1292 的原话:“If fd doesn't refer to a terminal, these functions fail with the error ENOTTY.”

isatty 探的却不是这些号:正文《亲手一玩,三件补齐》把咱们的旧说翻了篇,musl 的 isatty 真探的是 TIOCGWINSZ,0x5413 的那一号,问窗口的大小。咱们这样答:记录八字节四格,行、列、横像素、竖像素,答的是开机横幅同款的 48 行 128 列,像素两栏填零。窗口的形状从文字屏幕现量,shape_window 在 console_tty.cpp 的 :55,门面 answer_window 在 console_node.cpp 的 :143 到 :150。

接下来是本章真正的案子:记录在线上到底几字节。量错的经过讲在前,量对的层讲在后。当年咱们决定走真 ABI 的路,拿宿主上的两套 C 库各探了一发 sizeof,两套答的都是六十,咱们照六十写了。写完自查才发觉,咱们演示开关的那个小程序,缓冲只有三十六字节,内核一次写六十,把缓冲后头的二十四字节邻家也写了,没炸靠的是运气。演示当时跑得过去,只是量错的那一层被盖在了底下。

六十是 C 库那一层的结构,咱们挨样数:四个开关字,一个行字节,三十二格控制字符,两个波特率的字段,这几样真加一遍得五十七。报出的 sizeof 却是六十,差的三格不在尾巴上:三十二格控制字符从第十七字节排到第四十八,下一个字段是四字节的速度位,要坐四的整倍数,只好跳到第五十二,四十九到五十一这三格垫了空,库量的数把它也一并算了。TCGETS 与 TCSETS 走系统调用的线,线上带的从来是内核那一层的形状:四个开关字,一个行字节,十九格控制字符,合共三十六,一格垫也找不出来。线格式这个词开篇当口语点过,这里立成术语:跨边界传的记录,线上带的那层字节形状。咱们把这一课写进了今天的码,kTermiosBytes 的座 tty_config.hpp 的 :39,头注自家记着六十字节那次读数,连写越界的二十四字节都数了进去。当年的判词请回:“库大的是库,线小的是线,各是各的。”

这一案在咱们这儿有家谱:stat 的一百四十四字节案在 5.1 交过学费,家训同一条,原话:“跨边界的记录,量的必须是线上那一层,库那一层再标准也是别人的。”

还有一截后来长出来的,咱们如实交代。线上又添了带速度字的加长版:termios2,四十四字节,配套的 TCGETS2 那一路问话,tty_config.hpp 的 :42 到 :50,号形与老问不一样,编着方向与尺寸。门咱们两代都答:console 的问话表里,三十六字节的老问与四十四字节的新问各占各的座,console_node.cpp 的 :187 到 :194,模板按记录的宽度各铸一答。wire 出生时填的控制字符表,咱们叫出生表,座位两代没挪过,本章教的线格式,以三十六字节的那一代为准。

开关立好了,咱们写一个真用它的程序,叫 ttydemo,号与布局照老章程报全:TCGETS 0x5401,记录 36 字节,lflag 坐第 12 字节,ECHO 的位值 0x8,user/programs/ttydemo.cpp 的 :7 到 :11。它的戏码是一段秘密问答:读现行的开关,把 ECHO 也关了,请您敲一行密码,读完再把开关原样恢复。把关回显的那几行抄来,:56 到 :60:

```cpp
    unsigned char quiet[kRecordBytes] = {};
    for (unsigned long index = 0; index < kRecordBytes; ++index) {
        quiet[index] = record[index];
    }
    put_word_at(quiet, kLflagAt, word_at(quiet, kLflagAt) & ~kEchoBit);
```

当年开机跑过:咱们敲进去一行秘密,屏上果然一字不出,读到的行一字不少,开关恢复之后回显照旧。开关面立住了。可一台真终端全机器只此一台,长在键盘与屏幕上,挪也挪不走。程序想要的,是一台归自己使唤的终端,这个尴尬怎么解,下一节的答案是铸一对。

## 一开就铸一对的设备

书里的定义咱们开门就请,书页 1375 的原话:“A pseudoterminal is a virtual device that provides an IPC channel. On one end of the channel is a program that expects to be connected to a terminal device. On the other end is a program that drives the terminal-oriented program by using the channel to send it input and read its output.”一头坐着指望连终端的程序,另一头坐着驱使它的程序,中间一条沟。

咱们把这门手艺叫铸对:铸出一对,中间不接任何硬件。主机那头叫 master,握在程序手里,读写走的都是普通文件的路。从机那头叫 slave,造得跟真终端一模一样,行纪律、开关面、问话门样样齐全,跑在它身上的程序察觉不出差别。

中间接的是两截环。主机写的字节进一截 raw 环,从机的纪律从这截环上取键。纪律的回显与程序的输出汇进另一截回读环,主机收的也是这截环上的话。环的预算,咱们定的是 raw 环二百五十六字节,回读环四千零九十六字节,pty_config.hpp 的 :25 与 :29。

从机的本体,咱们一件新的都不用造:一台真的 Tty 实例,座位全指着自家这一对的环。整对收成一个纯逻辑类 Pty,pty.hpp 的 :48,头注自家写着“keys from this pair's raw ring”。构造函数一手工夫全在接线上,pty.cpp 的 :7 到 :14:

```cpp
Pty::Pty(void (*wait)(void* context)) {
    wiring_.context  = this;
    wiring_.take_key = &Pty::take_raw;
    wiring_.has_key  = &Pty::peek_raw;
    wiring_.echo     = &Pty::push_echo;
    wiring_.wait     = wait;
    slave_.wire(wiring_);
}
```

context 填的是 this,咱们把取键的座接 take_raw,看键的座接 peek_raw,回显的座接 push_echo,再把 wire 喂给从机。take_raw 从 raw 环里弹一个字节,:20 到 :23。push_echo 把纪律的回显推进回读环,:30 到 :33。环上另一头的两个口子也顺手认了:主机写,就是往 raw 环里推,推满为止,master_write 在 pty.cpp 的 :35。主机读,就是从回读环里弹,弹空为止,master_read 在 :44。咱们在宿主长凳上把整对直测了六案:键盘跨对进从机成行,纪律的回显流回主机,从机自己的输出只到主机不到自己,ECHO 落下只剩静默,ICANON 落下裸字节直通,外加清对复位一案。

设备这头怎么挂进 /dev,才是真正的新词。/dev/ptmx 底下的设备,open 的语义与众不同:寻常的 open 把表里的节点原样交回,开 ptmx 却是当场铸出一对全新的伪终端,把主机那头递到您手里。当年的原话请回:“递进去的是表里静态的那一行,交回来的是新铸对的主机节点。寻常的设备把节点原样递了回去,办的是同一道手续,对谁都是不偏不倚的。”open 门口把 ptmx 的表行换成新铸对主机节点的这道手续,咱们叫换座。

铸座的正身,咱们去 PtyTable::mint_pair 里看,pty_node.cpp 的 :289。挨座找头一个空座,new 一只 Pty,主从两张 DeviceOps 脸一座一座填上,交回主机的节点,循环的头一段抄来,:290 到 :303:

```cpp
        for (unsigned long index = 0; index < driver::kPtyPairs; ++index) {
            Seat& seat = seats_[index];
            if (seat.used) {
                continue;
            }
            seat.pair                 = new driver::Pty(&pty_wait);
            seat.used                 = true;
            seat.master_ops.read      = master_read;
            seat.master_ops.write     = master_write;
            seat.master_ops.has_input = master_has_input;
            seat.master_ops.ioctl     = master_ioctl;
            seat.master_ops.release   = master_release;
            seat.master_ops.context   = &seat;
            seat.master_ops.retain    = master_retain;
```

从机那一侧的座位,咱们同一模子再铸一遍。八座铸满,第九次 open 老老实实答 ENOMEM,:319,码里的注释自家写着“the eighth pair and every one after answer ENOMEM honestly”。槽表八对,kPtyPairs 的座在 pty_config.hpp 的 :21,表本体 pty_node.cpp 的 :372。

座位的生命周期交一笔更正,教训不小。头一版咱们让主机末座关闭时还座,防悬的手续看着齐全,复核拿一个专门造的复现案一试:开第一对,关它的主机,再开第二对,拿第一对还活着的从机写字,第二对的主机收到了。当年的原话:“旧从机的字写进了新主机的环里,座位复用把两个时代的对子缝在了一起。”修法是彻底不回收:座位只铸新的,对子的完整生命周期,谁还活着,从机要不要挂断,控制终端怎么解绑,登记在后面的活里。回收的事,当年的收尾一句:“回收的事与其猜着办,咱们不如诚实地说满了。”

名字这头还有一个钩子:表里的名字写死,可 /dev/pts/3 这样的座次名,要等对子铸出来才知道,动态解析器收一条路径答一个节点。resolve 在 pty_node.cpp 的 :326:pts/ 后头那串数字,咱们叫它座次号,带着它在这里解出活的从机,座次号最多两位数字,出了八对的界答 NotFound。dev/tty 这个名字解出的是您自会话认下的终端,咱们叫它控制终端,console 顶 console 的正座,伪终端的从机顶各自的座,没认过终端的会话答 NotFound。

书页 1376 还有一句,咱们正好接上:“a terminal-oriented program must have a controlling terminal. This allows the program to obtain a file descriptor for the controlling terminal by opening /dev/tty.”每进程的别名,各回各的家。控制终端怎么认,末一程立。

钩子要通到系统调用的层,走的是 5.1 数过的那三枚脸:readable 问一句读者现在能否有进展,ioctl 把问话递给设备的脸,adopt_open 接住 open 门口的换座。5.1 立脸的时候,这三枚的名字咱们已经认过,今天它们各自的活主登场,当年的原话:“设备的问话从此不问出身,问的是节点自己的脸”,系统调用这一层从此就不用再写一个特判。

座次号那一问 TIOCGPTN,主机答的就是自家在表里的座,master_answer_number 在 pty_node.cpp 的 :375。解锁那一问 TIOCSPTLCK,咱们答一个零就算解锁。设备号照 Linux 编:身份字,咱们给 stat 答出来的整台设备号起的名,大号挑主从,主机一百二十八,从机一百三十六,小号坐对子的序号,ptmx 自己是五比二,pty_config.hpp 的 :32 到 :41。

复用的残留病也交一笔。头一版从机重新接线时只清了字节,半行,挂着的信号,旧开关,全留给了下一任:前一个会话攒了一半的行,后一个会话一开口就收到了。修法是接线这一步做全量重置,wire 自己就是范本:半行清零,暂存的信号归零,开关重建回出生的三位,池子掏空,tty.cpp 的 :36 到 :54。对子归座时 reset 再把两截环排干,pty.cpp 的 :94 到 :104。当年的话咱们原文收着:“咱们一分旧状态都不许它过夜。”

实弹咱们请 busybox 出场,它自带一个叫 script 的命令,开的正是一对伪终端,把一个交互 shell 跑在从机上,自己在主机这头当转播。咱们从键盘喂一句 echo pty-works,串口上看见的字来自三个不同的终端实例,当年的原话整句请回:“控制台的行编辑回显了一遍,从机的行规程又回显一遍,命令的输出从从机写进回读环,由 script 转回控制台的是第三份。”控制台的行编辑一份,从机的纪律一份,script 转回的命令输出一份,三层各是各的来历,一层不混。当年还有一句小字咱们不遮丑:“这时从机上的 ash 还报着 can't access tty,控制终端与前台组咱们都还没立,它降了级也要活,整台戏的锣,后面作业控制的篇里敲。”锣在末一程,咱们接着走。

这场戏里还带出一桩与伪终端无关的旧案,咱们顺路收拾,收拾的正是 5.1 那句 0/1/2 的来路。script 的孩子要在从机上安家,头一套手续是搬家:close 掉零号,再开从机节点,指望新座落在零号上。POSIX 的老章程是最低空闲,新 fd 总坐当前进程最小的空座,书页 14 的原话:“A newly allocated file descriptor is always the lowest-numbered unused descriptor of the current process.”

可咱们当时的零到二号是三张特权假座,摊在系统调用里成了九处特判,close 零号跟没关一样,内核回它 EBADF,孩子死在搬家的路上。修法三刀:找座改成最低空闲含零,空表头一回 open 拿到的就是零号。开机时给 shell 任务自己的文件表预置 console 的三座。九处特权分支尽数撤掉,头三号与三号以上的任何一个 fd 走完全相同的路。

预置这一步还翻过一版:头一版预置进了开机任务的表,可 shell 的任务是另起炉灶新建的,不继承它,改落在 shell 自己的表里才通。xv6 也是这个形态:书页 17 教 mknod 的地方,举的例子正是创建 console 这个设备文件。书页 28 的原话:“init (7764) creates a new console device file if needed and then opens it as file descriptors 0, 1, and 2. Then it starts a shell on the console. The system is up.”咱们的登录 shell 由开机预置,咱们与它站在同一个形态上。

座位多了表也要扩:ash 的行编辑开工时,会拿 F_DUPFD_CLOEXEC 从十号起找座存自己的终端描述符,八座的表它一伸手就摸到了满,咱们把表添到十六座,当年的依据一句:“扩到十六座所依据的,是客人伸的手,咱们不凭感觉定数。”

还有半句引子埋给后面:家当继承头一版走的是找座的路,父亲表里的十号被折成了孩子表里的三号,后来复盘判回保号,父亲的几号就是孩子的几号。那桩案子的正戏归 5.6,咱们在这儿只把引子放下。戏还剩最后一程,一台终端的键该发给谁,下一节当面算。

## 键是哪一组的

作业控制的整台戏,前台组与后台怎么分章法的这门学问,咱们头一步立台本,当年的原话:“一台终端的键盘,不能是谁都在读就归了谁。”三层名词的地基 4.5 立过,咱们复读一句:会话是 setsid 自立的门户,进程组是一起领信号的一伙,前台组是终端记着的那一组人。

4.5 立进程组的时候,从书页 708 请过一句英文,今天原话请回:“The controlling terminal maintains the notion of a foreground process group.”同一页还有半句把话说尽:“The foreground process group is the only process group that can freely read and write on the controlling terminal.”键产生的信号发给前台组,后台的不沾光,咱们今天就在这层关系上起楼。

这层关系记在任务的家册里:一枚控制终端的指针,fork 抄的是父亲的,setsid 把它清空,新会话落地就断旧关系。认终端走 TIOCSCTTY 那一问,条件两条:您得是会话首领,而且手上没有终端,slave_answer_ctty 在 pty_node.cpp 的 :208 到 :217,kLeader 判的就是 sid 与 pid 相等,两条不满足答 EPERM。

设备的两侧各管各的,咱们分开记:console 是一台,前台组全局记一份。伪终端是每一对自己的一座,Seat 里有一格 foreground_pgid,pty_node.cpp 的 :35。TIOCGPGRP 与 TIOCSPGRP 两问答前台组的读与写,console 侧 console_node.cpp 的 :196 与 :197,从机侧各自落在自家的座上。

信号的翻译在行纪律里做,ISIG 从存位变成动作,靠的是一张出生表。wire 的时候表就填满:INTR 座 0x03,QUIT 座 0x1C,SUSP 座 0x1A,这三座在 wire 的引码里填的是常量名,数字今天报。擦除 0x7F,行抹除 0x15,EOF 0x04,VMIN 一,后四座同一段引码里见的就是值。ISIG 立着的时候,纪律收的每一个键都对照出生表:对上 INTR 座的是 ^C,翻成二号信号。QUIT 座上的翻三号。SUSP 座上的 ^Z 翻二十号,signal_for 在 tty.cpp 的 :141 到 :153。哪一格填了零,咱们就把哪一位当禁用。

书页 1297 把 ^C 的整句章程写着,咱们引中段:“typing this character causes an interrupt signal (SIGINT) to be sent to the terminal's foreground process group (Section 34.2). The INTR character itself is not passed to the reading process.”信号字符自己不进读的数据,翻完就吃掉。翻译的那段码,feed 的开头,tty.cpp 的 :83 到 :97:

```cpp
    if ((lflag_of(termios_) & kTermiosIsig) != 0) {
        const int kSignal = signal_for(key);
        if (kSignal != 0) {
            if (pending_signal_ == 0) {
                pending_signal_ = kSignal;
            }
            if ((lflag_of(termios_) & kTermiosNoflsh) == 0) {
                used_                   = 0;
                unsigned char discarded = 0;
                while (pool_.pop(discarded)) {
                }
            }
            return false;
        }
    }
```

信号一来,咱们若没立 NOFLSH,纪律还会把攒了一半的行与池里待取的行都清了:被打断的输入不该留给下一位,feed 里那段在 :89 到 :94。同一时刻只记头一个信号,后到的不覆盖早到的,翻译的结果暂挂着等读的人来取,暂存的号咱们叫它挂起信号,取走即清,:86 到 :88。

投递的路径接在读的门口,咱们看从机侧的码,pty_node.cpp 的 :82 到 :90:

```cpp
    driver::Tty&        tty     = kSeat->pair->slave();
    const unsigned long kGot    = tty.read_line(static_cast<unsigned char*>(sink), count, false);
    const int           kSignal = tty.take_pending_signal();
    if (kSignal != 0 && kSeat->foreground_pgid != 0) {
        proc::SendToGroup(kSeat->foreground_pgid, kSignal);
    }
    if (kSignal != 0) {
        return base::KernelError::kInterrupted;
    }
```

read_line 交零让了路,咱们在设备这一层取出挂起的信号,发给终端记着的前台组,然后给读的调用答 EINTR,errno 名册的新词 kInterrupted 映的就是它。行纪律里取键的循环,挂起信号为零才肯继续,tty.cpp 的 :132。read_line 一见挂起信号扭头就交零,:165 与 :182 到 :184。

EINTR 这笔欠的名册上还记着别的角:信号打断睡眠,ppoll 的门上还过一角,终端读的门上今天又还一角,当年的原话咱们照挂:“两角加起来仍不是完整的一片”。这是当年的实况,后来的事 4.5 已刊:那一角长成了一排门,ppoll、read、sendfile、nanosleep、socket 的等待圈里,终态都挂了同一问。

可 ^C 这个键,当年时灵时不灵,案卷送出去,回来的判决一案两病,咱们把正戏给它。头一病在键盘:咱们的键码翻译只认 Shift 修饰,Ctrl 按着的时候查表查出来的还是个普通的 c,^C 的三号字节根本到不了行纪律。

修法是咱们让左右 Ctrl 各自独立地跟踪,TranslatorState 里两枚布尔,scancode.hpp 的 :39 与 :40。0x1D 是 Ctrl 的键码,前缀 0xE0 一到,同一枚键码选的是右手,scancode.cpp 的 :11 到 :23。修饰在的时候按控制字节映射,折法的那几行抄来,:32 到 :41:

```cpp
    const char kGlyph = state.shift_held ? kScancodeUpper[kMake] : kScancodeLower[kMake];
    if (state.control_left || state.control_right) {
        if (kGlyph >= 'a' && kGlyph <= 'z') {
            return static_cast<char>(kGlyph - 'a' + 1);
        }
        if (kGlyph >= '@' && kGlyph <= '_') {
            return static_cast<char>(kGlyph - '@');
        }
    }
    return kGlyph;
```

您核一遍:字母折成一到二十六,@ 到 _ 折成零到三十一,c 减 a 加一,正好三号。第二病在出生表:ash 的行编辑开工,头一件事拿 TCSETS 把 ISIG 关了打算自己管键,可它挑终端的时候,要看的就是出生表(c_cc)里 VINTR 是不是非零,头一版的表生来全零,它一看这终端没有信号字符,整套行编辑就缩了。

判词把章法也理清了,咱们请回:“ISIG、ICANON、ECHO 三位各管各的,回显只听 ECHO 的,关掉了 ISIG 也不等于关掉了回显。”这一案的卷宗合上,病根一条,当年的合卷一句:“咱们的测试造不出真客人的形态。”

键盘的两代,咱们顺路各看一眼。PS/2 这一代,键盘控制器自己得站住,init 一口气办下来,keyboard.cpp 的 :60 到 :76:

```cpp
bool Keyboard::init() {
    send_command(kCommandDisableFirst);
    send_command(kCommandDisableSecond);
    flush_output();
    send_command(kCommandReadConfig);
    auto const kConfig = read_data();
    auto const kWanted = static_cast<uint8_t>((kConfig | kConfigIrqFirst | kConfigTranslation) &
                                              static_cast<uint8_t>(~kConfigIrqSecond));
    send_command(kCommandWriteConfig);
    write_data(kWanted);
    send_command(kCommandSelfTest);
    if (read_data() != kCommandExpectHealthy) {
        return false;
    }
    send_command(kCommandEnableFirst);
    return true;
}
```

咱们挨条对:禁两口,冲掉输出缓冲里攒的旧字节,读写配置把一号线的中断位与翻译位置上,发自检命令 0xAA 等一个 0x55,等到了才放行第一口。attach 把处理函数挂上 1 号线,:78 到 :83。键来了,on_byte 拿 TranslateScancode 翻字,翻出非零的字就推进事件队列,给就绪的脸报一声,:85 到 :91。

USB 这一代是咱们说的第三张嘴:5.4 把 xHCI 的链路送到了桥边,这边咱们接的是报告的翻译。usb_keyboard.cpp 的 :90 把中断读挂上。报告落地,report_arrived 拿 HidReportKey 按出字,比对上一份报告,有新字就推进队列,末了把中断读重新挂上,:100 到 :109,一次性的回调,消费完了才谈得上重挂。

这一卷的方法论咱们也请回一句,正文《门房、册子与门铃》那一卷《四道坎与一块像素》一篇的原话:“串口的证据在前,屏幕的热闹在后。”当年像素判读把一行 panic 错认成了打字的回显,这句是那次复盘立下的。

实弹挑最硬的一场打:sleep 10 敲下去,趁它睡着的当口补一个 ^C。纪律把它翻了出来,投给前台组,sleep 被 EINTR 打断退了场,ash 收了它的尾,提示符干干净净地回来,当年咱们在串口上,一声未接的报数都没有,每一问都有人接。

然后是全卷的主画面,咱们看得真切:ash 从 /dev/console 起整套会话,setsid,认控制终端,立前台组,走了一遍,降级一次都没有,前头那句 can't access tty 的告白从此听不见了。双重回显也随手销案:ash 的行编辑一开工就拿 TCSETS 把 ECHO 关了,自己接管整行,从机里内核的回显与 ash 自己的回显不再叠加,echo 一句,屏上出的是一份字。

链子到这儿全通了,咱们把开篇点名的欠条挨张清点。4.5 的四票:前台组的细目当面讲完,行规程这门课今天立了,^C 的来路一整条走通,线程与信号的交互,今天还上终端读这一角,线程住进同一副身子之后信号给谁的那半,归 5.6。4.6 的两票:DMA 一票 5.4 已兑,TTY 与线程的戏今天开了场,线程的后半 5.6 接。5.1 那句 0/1/2 三座的来路,真座,行规程,问话门,都在这门里。5.4 桥边的三句:键盘认字从键码表到 Ctrl 折法今天开课,USB 类驱动的报告翻译接上了桥,伪终端与 TTY 名下兑现。

往后的门也指给您:/dev 与 devtmpfs 的内里,procfs 的门面,网络栈,都归 5.7。家当怎么继承,归 5.6。远期的欠条照挂:往断了的管道写字该响的 SIGPIPE 与对应的错误码 EPIPE,挂点还空着,伪终端 PTY 的座位只铸不还,行池还认不出 EOF 的边界,script 会话收尾的疑点还悬着,一样一样记在明处,咱们心里有数再往前走。

## 本章留下了什么

终端与伪终端这一门,咱们今天走了四程:纪律,开关,铸对,归属。新词入册:

| 术语 | 行话 | 一句话口径 |
| --- | --- | --- |
| 行规程 | line discipline | 管键进键出的纪律代码:攒行、回显、翻信号,一台终端一台 |
| 接线 | TtyWiring | 三件事四座函数指针,外加一枚上下文词,纪律靠它认输入、回显与等待 |
| 开关面 | termios | 几位开关加一串控制字符的记录,谁改谁的,答回原样 |
| 线格式 | wire format | 跨边界传的记录在线上的字节形状,量的必须是线上那一层 |
| 铸对 | PTY pair | 主从两截环夹一台真行规程,开 ptmx 当场铸出 |
| 换座 | adopt_open | open 门口的手续,ptmx 的表行换成新铸对的主机节点 |
| 座次号 | TIOCGPTN | 问主机自家是第几对的那一问 |
| 身份字 | device id | stat 答出的整台设备号,大号挑主从,小号坐对子序号 |
| 控制终端 | controlling terminal | 会话认下的那台终端,记在任务家册,fork 抄、setsid 清 |
| 前台组 | foreground process group | 终端记着的那一组人,键产生的信号发给它 |
| 出生表 | c_cc | 控制字符的家,^C、^Z、擦除、抹行、EOF、VMIN 各坐各座 |
| 挂起信号 | pending signal | 纪律翻完暂存的号,取走即清,同刻只记头一个 |
| 第三张嘴 | USB keyboard | 串口与 PS/2 键盘之后,USB 键盘这第三处进键的口 |
| 最低空闲 | lowest-numbered unused | 找座的章程,新 fd 总坐最小的空座,零号不豁免 |

没讲的如实交底:devtmpfs 与 procfs 的内里,5.7,本章的设备只到脸面为止。ps 列不出进程那一案,病根在 procfs 侧 stat 字段的细目,5.7 再当面查。开机名单里那些查不到父亲的条目怎么来的,归 5.6。会话与进程组的本体,4.5 已刊。窗口的像素与字模,咱们一格没碰。动态链接,一步没越。fork 与 exec 怎么把家当传下去,5.6 的正戏,本章只放了保号的引子。

## 练一练

咱们留三道题,一道量字节,一道判开关,一道追字节。

**题一,量字节。** 库那一层的 termios:四个开关字,一个行字节,三十二格控制字符,两个波特率字段。您挨样数出字段实加几字节,对齐垫几格,sizeof 报几。再算线上那一层:四个开关字,一个行字节,十九格,合共几字节。末了答一句:当年按六十写 TCGETS,砸到的是谁家的内存,凭什么砸得着?

**题二,判开关。** ISIG 开着、ECHO 关着,您敲一个 ^C,屏上出什么,这个字节进不进读的数据,读的调用答什么?ISIG 关着、ECHO 开着,您再敲一个 ^C,这一回它算什么?拿“回显只听 ECHO 的,关掉了 ISIG 也不等于关掉了回显”那一句答。

**题三,追字节。** 从 USB 键盘到信号,您把 ^C 这个字节的路挨门数:HID 报告进来,谁翻的字?进了行纪律,谁认出它是信号字符,翻成几号?挂起之后,谁在哪个门口取走它,发给谁?末了一问:伪终端的前台组还是零,信号发不出去,读的调用答什么?

### 参考答案

题一:咱们量。库层字段实加五十七:开关字十六,行字节一,控制字符三十二,波特率两个四字节的字段八。对齐垫三格,垫在控制字符与速度字段之间,sizeof 报六十。线上三十六:十六加一加十九。按六十写 TCGETS,内核往调用方的记录里一次写六十字节,真记录只有三十六,砸的是记录后头那二十四字节的邻家,当年砸的就是 ttydemo 缓冲后头的栈。

题二:咱们判。ISIG 开、ECHO 关:屏上什么都不出,^C 被纪律翻成二号信号吃掉,不进读的数据,read 答 EINTR。ISIG 关、ECHO 开:^C 就是一个普通的 0x03 字节,照常当普通键攒行,能被读走。回显归 ECHO 管,信号的事归 ISIG 管,两位各管各,谁也不替谁关门。

题三:咱们追。HidReportKey 按出字,报告里的键进了事件队列。行纪律的 feed 对照出生表,signal_for 认出 0x03 坐 INTR 座,翻成二号,挂起。读者上门,从机读的门口 take_pending_signal 取走它,SendToGroup 发给终端记着的前台组,read 答 EINTR。前台组是零,信号不发,可 EINTR 照答:kSignal 非零就打断的分支写在发信号的 if 外头,挂起的号取走即清,谁也不欠谁。

## 延伸

- TLPI 第 62 章 Terminals,书页 1292 起:tcgetattr 与 tcsetattr 的章程、stty、控制字符逐条与 canonical 模式,本章请过的书页您再自读一遍。
- TLPI 第 64 章 Pseudoterminals,书页 1375 起:铸对的定义、ptmx 与 posix_openpt 的各小节,随用随抽。
- xv6 书第 1 章书页 14、17、28 与第 6 章书页 57 到 59:fd 的三号约定与最低空闲、init 开 console、consoleintr 攒行的那一段,咱们引的原句都在这几页。
- 正文《假文件与真终端》那一卷:分单例、三十六字节、铸对、真座位、键归一组与外面回来的判决,咱们引的旧话原文都在那一卷。
- 正文《亲手一玩,三件补齐》一篇:isatty 真探 TIOCGWINSZ 与半行也算就绪的出处。
- 正文《四道坎与一块像素》一篇:USB 键盘从报告到键码的那条路,与方法论那句的上下文。
