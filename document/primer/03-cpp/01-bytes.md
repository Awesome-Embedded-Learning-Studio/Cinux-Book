---
title: 01 · 字节得自己数
---

# 01 · 字节得自己数:布局、对齐和那点填充

咱们要递给硬件的东西,几乎都是结构体。磁盘地址包、GDT 描述符、页表项,对端只认字节序列,它压根不知道您那个类型叫什么名字。编译器答应咱们的却只有“成员按名字能取到”这么一件事,它不替咱们保证字段之间一个空隙都没有。

咱们随手把这个结构体的字段换个顺序,数出来的字节数立刻就不一样了。当初照着它跟硬件对齐定下的约定,一声不响地全部作废,程序照样编过,错要等到设备那边才发作。这一篇要干的,就是把那些字节自己数清楚,再让编译器替咱们盯着它们。

## 它占几格、住在哪

咱们借 `sizeof`、`alignof`、`offsetof` 来给同一个结构体量身材,看看差出来的那些字节究竟藏在哪儿。拿来当标本的探针,存成 `layout.cpp`,长这样:

```cpp
// 同一组字段的两种排列 —— 自然对齐 vs [[gnu::packed]]
#include <cstddef>
#include <cstdint>
#include <cstdio>

struct [[gnu::packed]] WirePacked {
    std::uint8_t  tag;
    std::uint64_t value;
    std::uint16_t crc;
};

struct WireNatural {
    std::uint8_t  tag;
    std::uint64_t value;
    std::uint16_t crc;
};

int main() {
    std::printf("WirePacked  sizeof=%2zu alignof=%zu offsetof(tag)=%zu offsetof(value)=%zu offsetof(crc)=%zu\n",
                sizeof(WirePacked), alignof(WirePacked),
                offsetof(WirePacked, tag), offsetof(WirePacked, value), offsetof(WirePacked, crc));
    std::printf("WireNatural sizeof=%2zu alignof=%zu offsetof(tag)=%zu offsetof(value)=%zu offsetof(crc)=%zu\n",
                sizeof(WireNatural), alignof(WireNatural),
                offsetof(WireNatural, tag), offsetof(WireNatural, value), offsetof(WireNatural, crc));
    return 0;
}
```

两边都是三个字段,笔者写的时候连顺序都没动过,差别只在其中一份头上挂了 `[[gnu::packed]]`。

```text
$ g++ -std=c++23 -O0 -Wall -o layout layout.cpp
$ ./layout
```

咱们跑一遍,它打出来的头两行就是 `WirePacked` 与 `WireNatural` 的读数:

```text
WirePacked  sizeof=11 alignof=1 offsetof(tag)=0 offsetof(value)=1 offsetof(crc)=9
WireNatural sizeof=24 alignof=8 offsetof(tag)=0 offsetof(value)=8 offsetof(crc)=16
```

咱们把 `WireNatural` 一个个排开看。`tag` 占住偏移 0 上那 1 个字节,轮到 `value`,它自己要求起始地址是 8 的倍数,偏移 1 到 7 这个位置不合格,只能被安排到偏移 8 上去,夹在中间的 7 个字节就空着了。这就是填充。`crc` 紧接着落在偏移 16 和 17,收在 18 上,一路都没有再垫,可整个结构体的大小还得是它最大成员对齐值的整数倍,尾巴上再补 6 个字节,凑成 24。24 减去 11,差的 13 个字节全是编译器垫进去的,里面一个字段的数据都没有。

咱们手里这段探针还排了另一组字段,`char` 打头、`int` 居中、`char` 收尾。把 `TriPacked` 与 `TriNatural` 接进 `layout.cpp`、摆在 `main` 之前,`printf` 照前面两条的样子再添两条:

```cpp
// 同一条探针里第二个结构体
struct [[gnu::packed]] TriPacked {
    char a;
    int  b;
    char c;
};

struct TriNatural {
    char a;
    int  b;
    char c;
};
```

咱们把同一条探针再编一遍跑起来,末尾又多两行:

```text
TriPacked   sizeof= 6 alignof=1 offsetof(a)=0 offsetof(b)=1 offsetof(c)=5
TriNatural  sizeof=12 alignof=4 offsetof(a)=0 offsetof(b)=4 offsetof(c)=8
```

`WirePacked` 那边,属性一挂,`alignof` 从 8 掉到 1,这才是 `packed` 真正动过的地方:它改的是**对齐假设**,11 这个尺寸只是后果。换一组字段类型,数出来的规律一样:咱们盯着的是同一处限制,动手的一直在对齐上。

## 填充动了以后,指令长什么样

数字看完了,咱们把镜头切到反汇编上,看看从同一个 `value` 取数,两版编出来差在哪儿。下面这些片段里的指令统一按 AT&T 语法写,而且都取自 `-O2` 那一档编译出来的目标文件。同一份源码换一版编译器、换一个 `-march`,编出来的指令和偏移都可能变,您那边对得上的是形态,不是这几个数。

咱们要编的源码存成 `access.cpp`,结构体沿用前面给出过的 `WirePacked` 与 `WireNatural`:

```cpp
// 结构体跟上面那份一样,再补上六个函数
#include <cstdint>

struct [[gnu::packed]] WirePacked {
    std::uint8_t  tag;
    std::uint64_t value;
    std::uint16_t crc;
};

struct WireNatural {
    std::uint8_t  tag;
    std::uint64_t value;
    std::uint16_t crc;
};

extern "C" std::uint64_t ReadPacked(const WirePacked* w)  { return w->value; }
extern "C" std::uint64_t ReadNatural(const WireNatural* w) { return w->value; }
extern "C" void WritePacked(WirePacked* w, std::uint64_t v)   { w->value = v; }
extern "C" void WriteNatural(WireNatural* w, std::uint64_t v) { w->value = v; }
extern "C" void CopyPacked(WirePacked* d, const WirePacked* s)   { *d = *s; }
extern "C" void CopyNatural(WireNatural* d, const WireNatural* s) { *d = *s; }
```

```text
$ g++ -std=c++23 -O2 -c access.cpp -o access.o
$ objdump -d access.o
```

下面两屏是同一份反汇编里的两段,中间隔着两个写版本,各函数后面那些对齐填充(`nopw`/`nopl` 那几行)咱们也没贴。咱们从两个读函数看起:

```text
0000000000000000 <ReadPacked>:
   0:	48 8b 47 01          	mov    0x1(%rdi),%rax
   4:	c3                   	ret

0000000000000010 <ReadNatural>:
  10:	48 8b 47 08          	mov    0x8(%rdi),%rax
  14:	c3                   	ret
```

咱们把这两段摆在一起看,指令形态一模一样,一条 8 字节的 `mov`,只有后面那个立即数换了位置,一边是 `0x1`,一边是 `0x8`。

笔者知道很多讲对齐的材料会在这一步带上一句“未对齐的访问会被拆成好几次字节拼接”。咱们手头放着 `mov 0x1(%rdi),%rax`,它说明这个说法在 x86-64 上不成立:它就是一条普普通通的 8 字节读,硬件本来就容忍地址不对齐,编译器用不着替咱们补什么。往后您真正要留意的是**偏移量的变化**:属性一挂,编译器不再假设那个地址是齐的,它只保证按您声明的偏移去取,再也不敢拿“对齐”这个前提去安排别的事情。

咱们再来看整体拷贝。一句 `*d = *s`,两份拷贝函数各只有四条搬运指令再加一条 `ret`,没有别的:

```text
0000000000000040 <CopyPacked>:
  40:	48 8b 06             	mov    (%rsi),%rax
  43:	48 89 07             	mov    %rax,(%rdi)
  46:	8b 46 07             	mov    0x7(%rsi),%eax
  49:	89 47 07             	mov    %eax,0x7(%rdi)
  4c:	c3                   	ret

0000000000000050 <CopyNatural>:
  50:	f3 0f 6f 06          	movdqu (%rsi),%xmm0
  54:	0f 11 07             	movups %xmm0,(%rdi)
  57:	48 8b 46 10          	mov    0x10(%rsi),%rax
  5b:	48 89 47 10          	mov    %rax,0x10(%rdi)
  5f:	c3                   	ret
```

`packed` 版拿到的是“偏移 0 起 8 个字节”加“偏移 7 起 4 个字节”,两条互相重叠的搬运,合起来盖满 11 个字节。自然版拿到的是一条 16 字节的 `movdqu` 配 `movups`,再补一条 8 字节。原本一条 `movdqu` 配一条 `movups` 就能搬完的,现在退化成了 `mov` 加 `mov 0x7` 两条窄搬运,还互相重叠。咱们到这一步才看见属性收走的代价落在哪儿。

单个成员的读写倒是不吃亏,两版都是一条指令加一条 `ret`。所以咱们以后要留意的地方很具体:凡是对 packed 结构体做整体拷贝,搬运用的是窄的、重叠的两条。

## 取成员的地址,编译器会提醒您一句

想把 `&w->value` 交给别人,编译器会抢在前面开口。笔者把那个结构体的定义复制过来,单写两个取地址函数放进去,存成当前目录下的 `packed_addr.cpp`。下面几屏里的文件名都省了路径前缀。这回咱们连 `-Wall` 都不加:

```cpp
// 两个取地址函数:一个取 8 字节的 value,一个取 1 字节的 tag
#include <cstdint>

struct [[gnu::packed]] WirePacked {
    std::uint8_t  tag;
    std::uint64_t value;
    std::uint16_t crc;
};

extern "C" std::uint64_t* AddrOfValue(WirePacked* w) { return &w->value; }
extern "C" std::uint8_t*  AddrOfTag(WirePacked* w)   { return &w->tag; }
```

```text
$ g++ -std=c++23 -O0 -c packed_addr.cpp -o packed_addr.o
packed_addr.cpp: In function 'uint64_t* AddrOfValue(WirePacked*)':
packed_addr.cpp:10:63: warning: taking address of packed member of 'WirePacked' may result in an unaligned pointer value [-Waddress-of-packed-member]
   10 | extern "C" std::uint64_t* AddrOfValue(WirePacked* w) { return &w->value; }
      |                                                               ^~~~~~~~~
```

咱们没加 `-Wall`,它**默认就开着**。它挑的也准:同一个结构体里取 1 字节的 `tag` 一声不响,取偏移 1 的 8 字节 `value` 才会触发。它拦下的,正是指针连对齐都不保证的情况。真想让构建在这里停下来,加 `-Werror=address-of-packed-member` 就够了。

同一个结构体里取 1 字节的 `tag`,这一屏里没有出现对应的告警行,咱们往后还会用到这个差别。缺失也是一种证据:两个函数就并排写在刚才写下的 `packed_addr.cpp` 里,编译器只冲着 `value` 开了口,咱们知道 `tag` 不告警就够了。

## 把限制写成编译器看得懂的那种

前面那些数都是笔者在终端里量出来的。咱们哪天顺手把字段顺序调一下,布局就跟着变了,当初量出来的那几个数字就跟现在的类型对不上。这些数字靠手工记着,迟早有一天会记岔,程序却照样编过。咱们要的是让编译器替咱们盯着,`offsetof`、`sizeof`、`alignof` 都是编译期常量表达式,能直接进 `static_assert`。把它们连同结构体存成 `contract_ok.cpp`:

```cpp
// 把字节合同钉进编译期:合同成立的那一版
#include <cstddef>
#include <cstdint>

struct [[gnu::packed]] WirePacked {
    std::uint8_t  tag;
    std::uint64_t value;
    std::uint16_t crc;
};

static_assert(sizeof(WirePacked) == 11, "WirePacked 必须是 11 字节:tag/8 字节 value/crc 紧贴");
static_assert(offsetof(WirePacked, value) == 1, "value 必须紧贴 tag");
static_assert(offsetof(WirePacked, crc) == 9, "crc 必须紧贴 value");
static_assert(alignof(WirePacked) == 1, "packed 结构体自身不对齐");

int main() { return 0; }
```

限制守得住的时候,编译器一个字节都不多说,连一行输出都没有(合同成立的那一版照同样两条命令编,只把文件名换成 `contract_ok.cpp`,它一声不吭、退出码 0)。咱们现在把 `[[gnu::packed]]` 从定义头上摘掉,四条断言一字不改,另存成 `contract_broken.cpp`,再看它交回来什么:

```text
$ g++ -std=c++23 -O0 -c contract_broken.cpp -o contract_broken.o
contract_broken.cpp:11:34: error: static assertion failed: WirePacked 必须是 11 字节:tag/8 字节 value/crc 紧贴
   11 | static_assert(sizeof(WirePacked) == 11, "WirePacked 必须是 11 字节:tag/8 字节 value/crc 紧贴");
      |               ~~~~~~~~~~~~~~~~~~~^~~~~
  • the comparison reduces to '(24 == 11)'
contract_broken.cpp:12:43: error: static assertion failed: value 必须紧贴 tag
   12 | static_assert(offsetof(WirePacked, value) == 1, "value 必须紧贴 tag");
      |                                           ^
  • the comparison reduces to '(8 == 1)'
contract_broken.cpp:13:41: error: static assertion failed: crc 必须紧贴 value
   13 | static_assert(offsetof(WirePacked, crc) == 9, "crc 必须紧贴 value");
      |                                         ^
  • the comparison reduces to '(16 == 9)'
contract_broken.cpp:14:35: error: static assertion failed: packed 结构体自身不对齐
   14 | static_assert(alignof(WirePacked) == 1, "packed 结构体自身不对齐");
      |               ~~~~~~~~~~~~~~~~~~~~^~~~
  • the comparison reduces to '(8 == 1)'
```

四条报错一起出来,每条后面还跟着一句 `• the comparison reduces to …`,把常量折叠的结果当场算给您看。咱们的眼睛不用离开屏幕,就能看见它算出来的 24 跟您以为的 11 差在哪儿。写编译期断言的收益还不止于此:四条和四十条,编译期要花的功夫差不多,**多写几条几乎不花什么代价**。把口头约定变成编译器看得懂的限制,靠的就是这一手。

## 谁在改那块内存,编译器看不见

咱们已经签完摆放,还有一条边界得自己画:内存里有些地方,**除了您眼前这几行代码,别人也会动**。编译器看不见那个“别人”,`volatile` 就是用来把这件事告诉它的。

咱们看一组对照。两个轮询函数,一个读 `volatile int`,一个读普通 `int`,同一份源码、同一个 `-O2`。存成 `wait.cpp`:

```cpp
volatile int vflag;
int nflag;

extern "C" void WaitVolatile() { while (vflag == 0) {} }
extern "C" void WaitPlain()    { while (nflag == 0) {} }
```

咱们编出来的结果是这样:

```text
$ g++ -std=c++23 -O2 -c wait.cpp -o wait.o
$ objdump -d wait.o
0000000000000000 <WaitVolatile>:
   0:	8b 05 00 00 00 00    	mov    0x0(%rip),%eax        # 6 <WaitVolatile+0x6>
   6:	85 c0                	test   %eax,%eax
   8:	74 f6                	je     0 <WaitVolatile>
   a:	c3                   	ret

0000000000000010 <WaitPlain>:
  10:	c3                   	ret
```

两个函数后面那行 `nopl` 对齐填充咱们没贴。

咱们数一下 `WaitVolatile` 保住了哪几条:`mov`、`test`、`je`,三条循环本体指令一条不少。`WaitPlain` 那边**整个函数体都没了,只剩一条 `ret`**,一次内存访问都没有。

这里发生的事,咱们对照源码看一遍就清楚了。`nflag` 是全局变量,初值是 0,整个程序里没有任何一处写它,`-O2` 下的编译器据此证明那个条件恒真,把循环体当成死代码删掉了。咱们别把它读成“变量被缓存进了寄存器”,也别读成“循环被改成了死循环”。标准那边授权它这么做:实现有权假定线程最终会终止、会调用库的 IO 函数、会做同步操作,也有权假定它会走一次 volatile 左值的访问——这类许可是好几项,不止这里点的这几下。所以对空循环做删除、合并和重排都是允许的,哪怕没谁证明得了它一定会结束。

咱们手里这个关键字,要交代的只有一件事:`volatile` 划出的是**编译器的知情权边界**。它告诉编译器,这个地址的内容可能被代码流之外的东西改过,您不能只按眼前这几行去推断。至于线程之间谁排在前、谁排在后,要不要加屏障,那是另一套东西,`volatile` 不负责那些。

### 运行起来真能看出来的那一版

光看反汇编还不够,咱们让“设备”在程序跑起来以后再动一次。`mmap` 咱们在这卷里是头一回用,它做的事很简单:向系统要一段内存,拿回一个起始地址,往后的读写就当普通指针用。下面这段探针把状态位放在 `mmap` 拿到的内存里,靠一个定时器信号处理函数,从代码流之外把它置成 1,再让两版轮询函数各自去等。把这段存成 `mmio_runtime.cpp`,后面几种跑法都编自它:

```cpp
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>

static std::uint32_t* g_reg = nullptr;

extern "C" void OnAlarm(int) { *g_reg = 1; }   // 相当于设备把状态位拉高

extern "C" __attribute__((noinline)) int PollPlain()    { while (*g_reg == 0) {} return 0; }
extern "C" __attribute__((noinline)) int PollVolatile() { volatile std::uint32_t* r = g_reg; while (*r == 0) {} return 0; }

int main(int argc, char** argv) {
    void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { std::perror("mmap"); return 2; }
    g_reg = static_cast<std::uint32_t*>(p);
    std::signal(SIGALRM, OnAlarm);
    ::alarm(1);
    if (argc > 1 && argv[1][0] == 'v') { PollVolatile(); } else { PollPlain(); }
    std::printf("循环退出,状态位=%u\n", (unsigned)*g_reg);
    return 0;
}
```

```text
$ g++ -std=c++23 -O2 -Wall -o mmio_runtime mmio_runtime.cpp
$ g++ -std=c++23 -O0 -Wall -o mmio_runtime_O0 mmio_runtime.cpp
$ g++ -std=c++23 -O1 -Wall -o mmio_runtime_O1 mmio_runtime.cpp
```

`timeout 5` 是给每次运行设一个 5 秒的上限,超了就把程序掐掉,咱们不用干等。同一份源码按三个优化等级各编一次,文件名就带上了档次。

```text
$ timeout 5 ./mmio_runtime ; echo "exit=$?"   # 不写 volatile
循环退出,状态位=0
exit=0

$ timeout 5 ./mmio_runtime v ; echo "exit=$?"   # 写了 volatile
循环退出,状态位=1
exit=0
```

咱们用同一条命令、同一份源码,只多一个关键字,一边打印 `状态位=0`,一边打印 `状态位=1`,两次都是退出码 0,**都不报错**。没写 `volatile` 的那次压根没等,它以为是 0,抬腿就走。汇编里更直白,咱们看到的是 `xor %eax,%eax` 加 `ret`,连那个地址都没碰(下面这一屏是运行版 `PollPlain` 的全文,它短得没什么可截。这个地址随链接布局变,您那边印出来不会一样,看指令形态就行):

```text
0000000000001240 <PollPlain>:
    1240:	31 c0                	xor    %eax,%eax
    1242:	c3                   	ret
```

咱们这一版走的是标准给的另一条许可,底子跟前面讲过的同一条一样。编译器证明不了 `g_reg` 指向的值恒为 0,可循环体里也没有任何可观测的动作,于是实现有权假定这个循环会终止,把它整个拿掉。这就是那个关键字要拦住的地方。

### 地址是编译期常量时,它怎么想

咱们再把“设备寄存器”换成一个编译期常量地址,编译器下手的地方就更清楚了。下面几屏都来自这一段源码:它和前面运行版的函数**重名,源码却不是同一份**——地址是编译期常量,像对准了一块设备寄存器,还请您留意这个分别。存成 `mmio_const.cpp`:

```cpp
// 地址是编译期常量:像对准了一块设备寄存器
#include <cstdint>

constexpr std::uintptr_t kReg = 0x40011004;

extern "C" void KickPlain() {
    *reinterpret_cast<std::uint32_t*>(kReg) = 0;
    *reinterpret_cast<std::uint32_t*>(kReg) = 1;
}

extern "C" void KickVolatile() {
    *reinterpret_cast<volatile std::uint32_t*>(kReg) = 0;
    *reinterpret_cast<volatile std::uint32_t*>(kReg) = 1;
}

extern "C" std::uint32_t PollPlain() {
    while (*reinterpret_cast<std::uint32_t*>(kReg) == 0) {}
    return *reinterpret_cast<std::uint32_t*>(kReg);
}

extern "C" std::uint32_t PollVolatile() {
    volatile std::uint32_t* r = reinterpret_cast<volatile std::uint32_t*>(kReg);
    while (*r == 0) {}
    return *r;
}

extern "C" std::uint32_t WriteThenReadPlain() {
    *reinterpret_cast<std::uint32_t*>(kReg) = 0x10;
    return *reinterpret_cast<std::uint32_t*>(kReg);
}

extern "C" std::uint32_t WriteThenReadVolatile() {
    *reinterpret_cast<volatile std::uint32_t*>(kReg) = 0x10;
    return *reinterpret_cast<volatile std::uint32_t*>(kReg);
}
```

```text
$ g++ -std=c++23 -O2 -c mmio_const.cpp -o mmio_const.o
$ objdump -d mmio_const.o
```

-O2 下,连着写两次同一个地址。为省屏,咱们只留要对照的那几个函数,函数之间那些对齐填充(`nopl`/`nopw` 那几行)不贴。后面几屏开头那个 `...`,表示那里跳过了别的函数:

```text
0000000000000000 <KickPlain>:
   0:	c7 04 25 04 10 01 40 	movl   $0x1,0x40011004
   7:	01 00 00 00 
   b:	c3                   	ret

0000000000000010 <KickVolatile>:
  10:	c7 04 25 04 10 01 40 	movl   $0x0,0x40011004
  17:	00 00 00 00 
  1b:	c7 04 25 04 10 01 40 	movl   $0x1,0x40011004
  22:	01 00 00 00 
  26:	c3                   	ret
```

咱们看到非 volatile 那一版只留下后面的 `movl $0x1`,写 0 的那一次整个消失。对设备寄存器来说,写就是下令,少下一次令,现象就是灯不闪。`volatile` 那一版两条都还在。

咱们写完立刻回读,它又换了个手法。同一份库里余下的函数都省略了,只把它们俩留下对着看:

```text
...
0000000000000060 <WriteThenReadPlain>:
  60:	c7 04 25 04 10 01 40 	movl   $0x10,0x40011004
  67:	10 00 00 00 
  6b:	b8 10 00 00 00       	mov    $0x10,%eax
  70:	c3                   	ret

0000000000000080 <WriteThenReadVolatile>:
  80:	c7 04 25 04 10 01 40 	movl   $0x10,0x40011004
  87:	10 00 00 00 
  8b:	8b 04 25 04 10 01 40 	mov    0x40011004,%eax
  92:	c3                   	ret
```

咱们看到非 volatile 那一版把您刚写下去的那个常数直接还了回来,**压根没去读那个地址**。`volatile` 那一版老老实实回读。

再换成轮询,咱们看到它还是一样的做法。还是各留一个,其余省略:

```text
...
0000000000000030 <PollPlain>:
  30:	8b 04 25 04 10 01 40 	mov    0x40011004,%eax
  37:	c3                   	ret

0000000000000040 <PollVolatile>:
  40:	8b 04 25 04 10 01 40 	mov    0x40011004,%eax
  47:	85 c0                	test   %eax,%eax
  49:	74 f5                	je     40 <PollVolatile>
  4b:	8b 04 25 04 10 01 40 	mov    0x40011004,%eax
  52:	c3                   	ret
```

咱们看到非 volatile 那一版只有一条 `mov` 加一条 `ret`,不再循环。**读一次之后,它就有权不再重读**。这些做法背后是同一件事,**编译器有权认为一个地址的内容只由您眼前的代码改**。您不写 `volatile`,就等于替它签了一份默认的声明。

### `-O0`、`-O1`、`-O2`,三种命运

咱们把同一份运行版源码换个优化等级再编,答案完全不一样:

```text
$ timeout 5 ./mmio_runtime_O0 ; echo "exit=$?"   # -O0,不写 volatile
循环退出,状态位=1
exit=0
$ timeout 5 ./mmio_runtime_O0 v ; echo "exit=$?"   # -O0,写了 volatile
循环退出,状态位=1
exit=0
$ timeout 5 ./mmio_runtime_O1 ; echo "exit=$?"   # -O1,不写 volatile
exit=124
$ timeout 5 ./mmio_runtime_O1 v ; echo "exit=$?"   # -O1,写了 volatile
循环退出,状态位=1
exit=0
```

咱们在 `-O0` 那一档看到,写不写 `volatile` 都能等到 1,每圈重读,结果正确。到了 `-O1`,不写 `volatile` 的版本把值读进 `%eax` 一次,往后只对 `%eax` 做 `test`,成了真正的死循环,`timeout` 等满 5 秒把它掐掉,退出码 `124`。下面这一屏是它那个 `PollPlain` 的全文(地址随链接布局变,您那边印出来不会是这几个数,看指令形态就行):

```text
000000000000118e <PollPlain>:
    118e:	48 8b 05 ab 2e 00 00 	mov    0x2eab(%rip),%rax        # 4040 <_ZL5g_reg>
    1195:	8b 00                	mov    (%rax),%eax
    1197:	90                   	nop
    1198:	85 c0                	test   %eax,%eax
    119a:	74 fc                	je     1198 <PollPlain+0xa>
    119c:	b8 00 00 00 00       	mov    $0x0,%eax
    11a1:	c3                   	ret
```

`-O2` 那一档更省事,循环整个被删掉,程序秒退,咱们拿到的是前面那个 `状态位=0`。每一条命令后面都带着 `timeout 5`,三档摆在一起,结果是这样:

- `-O0`:每圈重读,正确。
- `-O1`:值被缓存,卡死,退出码 `124`。
- `-O2`:循环被删,秒退,打印 0。

教材里常说的那句“编译器把变量缓存进寄存器,循环跳不出去了”,**只在 `-O1` 这一档成立**。换个等级就换了症状,三种症状还都不报错。所以咱们以后引用这一组现场,命令后面那几级优化必须一起写出来,否则您把源码拿回去复现,看到的会是另一种表现。

## 排障:那个 8 字节字段横跨边界会怎样

咱们已经知道顺序、宽度、偏移都能自己数清,排障这里再收一条常见的翻车姿势。咱们得接受这个前提:**内存是按页给出来的**,一条 8 字节的访问如果不巧横跨了页边界,踩进不许碰的那一页里的就不止一半。

同一个 8 字节的 `value`,同一组字段,同一台机器,咱们只换对象摆放的位置。咱们把探针存成 `page_probe.cpp`。它要两页内存,把页边界后面那一页用 `mprotect` 标成 `PROT_NONE`,再把对象摆到边界跟前。不带参数走 packed 那一摆,带一个 `n` 走自然那一摆。

```cpp
// 一个 8 字节字段横跨页边界时会怎样
#include <cstdint>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>

struct [[gnu::packed]] Packed { std::uint8_t tag; std::uint64_t value; };
struct Natural                { std::uint8_t tag; std::uint64_t value; };

int main(int argc, char** argv) {
    const long ps = sysconf(_SC_PAGESIZE);
    char* base = static_cast<char*>(mmap(nullptr, 2 * ps, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (base == MAP_FAILED) { std::perror("mmap"); return 2; }
    if (mprotect(base + ps, ps, PROT_NONE) != 0) { std::perror("mprotect"); return 2; }
    char* edge = base + ps;

    if (argc > 1 && argv[1][0] == 'n') {
        Natural* p = reinterpret_cast<Natural*>(edge - sizeof(Natural));
        p->value = 0x1122334455667788ull;
        std::printf("natural: &value=%p 8 字节落在 [%p,%p),页边界=%p -> 全在映射页内\n",
                    (void*)&p->value, (void*)&p->value, (void*)((char*)&p->value + 8), (void*)edge);
        std::printf("natural: 读到 value=0x%llx  tag=%u\n",
                    (unsigned long long)p->value, (unsigned)p->tag);
        return 0;
    }

    Packed* p = reinterpret_cast<Packed*>(edge - 4);
    p->tag = 0xAB;
    std::printf("packed: &tag=%p 读到 tag=0x%02x(1 字节,还在映射页里)\n", (void*)&p->tag, (unsigned)p->tag);
    std::printf("packed: &value=%p 8 字节落在 [%p,%p),页边界=%p -> 后 %ld 字节在 PROT_NONE 页里\n",
                (void*)&p->value, (void*)&p->value, (void*)((char*)&p->value + 8), (void*)edge,
                (long)((char*)&p->value + 8 - edge));
    std::fflush(stdout);
    std::uint64_t v = p->value;
    std::printf("不该走到这里:value=0x%llx\n", (unsigned long long)v);
    return 0;
}
```

咱们把它摆得对齐,8 个字节就整整齐齐落在映射页里面:

```text
$ g++ -std=c++23 -O0 -Wall -o page_probe page_probe.cpp
$ ./page_probe n ; echo "exit=$?"
natural: &value=0x78a880d51ff8 8 字节落在 [0x78a880d51ff8,0x78a880d52000),页边界=0x78a880d52000 -> 全在映射页内
natural: 读到 value=0x1122334455667788  tag=0
exit=0
```

咱们再往页边界那边挪一挪,那个 8 字节访问的后 5 个字节就落进了一张标着 `PROT_NONE` 的页:

```text
$ ./page_probe ; echo "exit=$?"
packed: &tag=0x72686a561ffc 读到 tag=0xab(1 字节,还在映射页里)
packed: &value=0x72686a561ffd 8 字节落在 [0x72686a561ffd,0x72686a562005),页边界=0x72686a562000 -> 后 5 字节在 PROT_NONE 页里
bash: line 1:     5 Segmentation fault         ./page_probe
exit=139
```

咱们数一数它踩到了哪儿,**一个 N 字节的访问若地址不对齐,就可能跨过 N 字节边界,进而跨过页边界**。对齐上的限制替咱们保证的,正是“N 字节的访问不会跨过 N 字节的边界”,于是它一定落在同一页里。1 字节的 `tag` 摆在同样的位置,读到 `0xab`,一声不响。`139` 是 `128` 加上 `11`,11 号信号叫 SIGSEGV。这个退出码本身不随机器漂,您在哪台 x86-64 Linux 上重跑,它都会落到同一个位置上。

您机器上的地址跟上面那串不会一样,末尾那行 `Segmentation fault` 是 shell 自己报的,它前头的行号和数字每次也换。对得上的是跨过去几个字节、以及退出码 `139`。

页这个东西不是咱们这一篇的课题,这里只借它的现象说一句话。至于 `alignas`,笔者的现场里一处都还没有,等到真需要它的时候,咱们再回头把它和 `packed` 摆在一起看:两个都只是同一个旋钮上的方向。
