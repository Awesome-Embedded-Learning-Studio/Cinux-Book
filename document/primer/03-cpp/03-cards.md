---
title: 03 · 惯用法识别卡与零开销复算
---

# 十张识别卡,和一次零开销的复算

咱们翻别人的 C++ 代码,眼睛会停在几个固定的形状上:一个写着 `enum class` 的类型,一个签名尾巴上挂着 `[[nodiscard]]` 的函数,还有模板体里那句 `static_assert`。它们不解释自己,可是它们决定了这一行编不编得过。笔者把它们收成十张卡,每张卡只回答两件事:它长什么样,它当场会让编译器或工具说一句什么话。

每一张卡的现场都一样:您自己手边的机器上写一个最小片段,编一次,看回话。认脸这件事,看一遍屏比背十条定义管用。您那边跑出来会有出入,机制不变。

十张里九张是这类形状本身,还有一张落在内联汇编上。往后您读代码,认出哪一样,就按哪一样解释,别拿一张卡的口径去套另一张。

## 闸门与常量:enum class、constexpr、sizeof

第一张卡是 `enum class`。接口要的是一个 16 位端口号,咱们把一个强类型枚举的成员直接递过去。把下面这段存成 `port_bad.cpp`,咱们编一次就能看到它被拦:

```cpp
#include <cstdint>

enum class Port : std::uint16_t { kCom1 = 0x3F8, kCom2 = 0x2F8 };

extern "C" void OutPort(std::uint16_t port);

int main() {
    OutPort(Port::kCom1);
    return 0;
}
```

编译器把您写的这一行拦下来,回的是 `error: cannot convert 'Port' to 'uint16_t' {aka 'short unsigned int'}`,后面还跟一条 `note: initializing argument 1 of 'void OutPort(uint16_t)'`。它说的不是值不合法,是两边类型对不上,中间没有一条路可走。

咱们再写一版能过的,存成 `port_ok.cpp`:老式无作用域枚举、`static_cast`、C++23 的 `std::to_underlying` 三条路并排,末尾再加一个同名成员的另一个类型。

```cpp
#include <cstdint>
#include <cstdio>
#include <utility>

enum PlainPort { kPlainCom1 = 0x3F8 };              // 无作用域枚举:直接当整数用
enum class Port : std::uint16_t { kCom1 = 0x3F8 };
enum class Line : unsigned char { kCom1 = 1 };      // 同名成员,各活各的

extern "C" void OutPort(std::uint16_t port) { std::printf("OutPort(%u)\n", port); }

int main() {
    OutPort(kPlainCom1);                               // 一传就过
    OutPort(static_cast<std::uint16_t>(Port::kCom1));  // 必须写出来
    OutPort(std::to_underlying(Port::kCom1));          // C++23 的写法
    std::printf("Port::kCom1=%u Line::kCom1=%u\n",
                static_cast<unsigned>(std::to_underlying(Port::kCom1)),
                static_cast<unsigned>(std::to_underlying(Line::kCom1)));
    return 0;
}
```

咱们跑一次,屏幕上是这些:

```text
$ g++ -std=c++23 -O2 -Wall -o port_ok port_ok.cpp
$ ./port_ok
OutPort(1016)
OutPort(1016)
OutPort(1016)
Port::kCom1=1016 Line::kCom1=1
exit=0
```

三行一样的输出,分别是直接传、`static_cast` 和 C++23 的 `std::to_underlying`。末行还带出另一件事:`Port::kCom1` 与 `Line::kCom1` 同名,各活各的,谁也不会被谁顶掉。所以读到 `enum class`,您把它读成一道编译期闸门:值不会自己变成整数,要变就得显式写出来。同一个位置写着 `static_cast` 或者 `to_underlying`,那不是啰嗦,是作者在这儿按了一次手印。

第二张卡是 `constexpr`。咱们把它写出来:一个普通全局变量,一个编译期常量,再加一个既能编译期算、也能运行期算的函数。

```cpp
#include <cstdint>

std::uint16_t g_port = 0xE9;                     // 真变量:要占存储
constexpr std::uint16_t kPort = 0xE9;            // 编译期常量
constexpr std::uint64_t Mix(std::uint64_t x) { return x * 6364136223846793005ull + 1442695040888963407ull; }

static_assert(Mix(1) == 7806831264735756412ull, "编译期就把 Mix(1) 算出来了");
static_assert(kPort == 0xE9, "kPort 是编译期事实");

// B 版只多这一行:一旦有人要 kPort 的地址,它就必须有存储
extern "C" const std::uint16_t* AddressOfConstant() { return &kPort; }
extern "C" std::uint64_t MixAtRuntime(std::uint64_t x) { return Mix(x); }
```

咱们把上面这段存成 `h2b.cpp`(这段就是整个编译单元,`-c` 编译不需要 `main`)。A 版存成 `h2a.cpp`,把取地址的那个函数连同它上面那句注释一起去掉,两条 `static_assert` 和 `MixAtRuntime` 都留着。B 版的屏是这些:

```text
$ g++ -std=c++23 -O2 -c h2b.cpp -o h2b.o
$ nm -C --print-size h2b.o
0000000000000000 0000000000000008 T AddressOfConstant
0000000000000010 000000000000001c T MixAtRuntime
0000000000000000 0000000000000002 r kPort
0000000000000000 0000000000000002 D g_port
```

第二列的大小是 `--print-size` 给的。`kPort` 是 2 字节,小写 `r`,只读数据。`g_port` 也是 2 字节,大写 `D`,可写数据。A 版那一屏少了 `kPort` 与 `AddressOfConstant` 两行,其余的名字都在。您要了地址,编译期常量就被迫落进存储。同一个 `constexpr` 函数也有两副命:屏上那个 `1c` 字节的 `MixAtRuntime` 是运行期那一副,放进 `static_assert` 里求值的那一副一点代码都不产生。您读代码时把 `constexpr` 读成“这里是编译期事实”,别看成一个不能改的变量。

第三张卡是 `sizeof` / `alignof` 这一类。它向编译器问的是一个类型问题,跟值无关:您写下 `sizeof(SideEffect())`,那个函数压根不会被调用,函数体里那句 `puts` 一次都没出现,屏上只给出类型的大小。`sizeof(int&)` 和 `alignof(int&)` 量的是被引用的类型,不是引用本身。`sizeof(arr)/sizeof(arr[0])` 在真数组上能拿到元素个数,换成指针就失效。这一类跟前面讲布局的地方问的是同一批东西,只是那里认领的是数值与填充,这里只认它的身份:答案在编译期就定死了,它不求值,也没有副作用。

## 签名里挂着的那几样

第四张卡是 `[[nodiscard]]`。丢掉它的返回值,编译器会开口说话:`warning: ignoring return value of 'uint32_t ReadStatus()', declared with attribute 'nodiscard' [-Wunused-result]`,还附一条 `note: declared here`。拿指针的返回值照样报。它不强制,您确实不要,写上 `(void)ReadStatus()`,编译器就一声不吭,那一行也就成了您亲笔签的字。同一张卡上还挂着 `[[noreturn]]`,违约的两句措辞不一样:写了 `return` 的那版收到 `warning: function declared 'noreturn' has a 'return' statement`,函数体直接走到结尾的那版收到 `warning: 'noreturn' function does return`。它在这个位置的意思是“这是终点”,真返回了属于运行期未定义行为。

第五张卡是 `noexcept`。它跟前面两条不一样:它不检查任何事,它是一句递给调用方的保证,咱们这个函数不会靠异常离开。真违了约,程序不是把异常抛出去,是当场被终止:

```text
noexcept(Boom()) = 1
…
terminate called after throwing an instance of 'int'
```

退出码是 134,也就是 128 加上 SIGABRT。编译器在编译期也会提醒您一句 `warning: 'throw' will always call 'terminate' [-Wterminate]`。`noexcept` 还有一个否定答案非说不可:同一个函数体只差它,在 `-O0`、`-O1`、`-O2` 三档上两版逐字节相同,函数大小一模一样。`noexcept` 买的是那句保证,不是体积,想说零开销的时候别拿它当证据。

第六张卡是 `std::source_location` 作默认参数。参数表里有一项默认值是 `std::source_location::current()`,咱们把它存成 `where.cpp`,编一次:

```cpp
// 两条调用,各记各的位置
#include <cstdio>
#include <source_location>

void Report(const char* what,
            std::source_location loc = std::source_location::current()) {
    std::printf("%s:%u:%u  %s\n", loc.file_name(), loc.line(), loc.column(), what);
}

int main() {
    Report("第一次");
    Report("第二次");
    return 0;
}
```

咱们自己编一次,屏幕上是这些:

```text
$ g++ -std=c++23 -O2 -o where where.cpp
$ ./where
where.cpp:11:11  第一次
where.cpp:12:11  第二次
```

行号 11 和 12,正是源码里两次 `Report("...")` 调用所在的那两行(算上开头那句注释),您可以拿自己的屏对一对。列号那一截是实现给的,别对它下判断。文件名那一截是编译时给的路径,您把文件存成别的名字、放在别的目录,它就跟着变。`Report` 身体里没有 `__LINE__`,参数表里也没让调用者多写一项,可每次调用它都知道自己是从哪一行被叫起来的。运行期这边不欠东西:`nm` 上带 `source_location` 的符号只有一个,就是咱们自己写的那个 `Report`,文件名字符串只是 `.rodata` 里的一段常量,而 `.rodata` 是裸地上也保留的那一部分,所以它到了裸地上照样能用。

## 模板这一侧:准入条件、实例化、弱符号

第七张卡是模板体里的 `static_assert`。咱们看它检查的是什么:不是调用者递进来的参数,是这个模板被实例化时用的类型。把它存成 `narrow.cpp`:

```cpp
#include <cstdint>

template <typename T>
struct Header {
    static_assert(sizeof(T) >= 4, "头部类型太窄:装不下 4 字节的字段");
    std::uint8_t bytes[sizeof(T)];
};

Header<std::uint32_t> g_ok;    // 实例化:过
Header<std::uint16_t> g_bad;   // 实例化:报,而且报在实例化点上
```

您写下的那一行 `g_bad` 就编不过,报错是两条腿:

```text
$ g++ -std=c++23 -O2 -c narrow.cpp -o /dev/null
narrow.cpp: In instantiation of 'struct Header<short unsigned int>':
narrow.cpp:10:23:   required from here
…
narrow.cpp:5:29: error: static assertion failed: 头部类型太窄:装不下 4 字节的字段
…
  • the comparison reduces to '(2 >= 4)'
```

两处 `…` 省掉的是源行列示和它下面那串插入符,咱们没动报错文字本身。

上面那两行指着的是“谁把它用窄了”,下面才是断言本身。想找准入条件,咱们去模板体里看。想找违约的人,看报错最上面那一行。前面讲布局的地方也拿 `static_assert` 把常量核在了编译期,那边一屏报的是一条直线上的几个常量,这边报的是一条调用链。认出这个区别就够了,别把两处的数字混着讲。

第八张卡是模板最小形。您看见 `template <typename T>`,读成“这是一份配方,不是一个函数”。配方本身不占体积,只有拿具体类型去实例化才生成代码。下面这个探针把两处实例化显式写了出来(`template int Max<int>(int, int);` 那样两行),所以符号一定在。存成 `h7.cpp`:

```cpp
template <typename T>
T Max(T a, T b) { return a < b ? b : a; }

template int Max<int>(int, int);              // 显式实例化:这两个类型各出一份机器
template double Max<double>(double, double);

template <typename T>
T NeverUsed(T a) { return a; }                // 从不实例化:一个符号都没有
```

```text
$ g++ -std=c++23 -O2 -c h7.cpp -o h7.o
$ nm -C --size-sort --print-size h7.o
0000000000000000 0000000000000008 W int Max<int>(int, int)
0000000000000000 0000000000000009 W double Max<double>(double, double)
```

咱们看两行开头的字母,都是 `W`,弱符号,不是一个 `T`。这个探针里用了显式实例化,要求编译器为两个类型各出一份机器。一个从没被实例化的模板,`nm` 上一个符号都没有。价格跟着类型组合数走,不跟源码行数走,所以 `nm --size-sort` 读起来就是一张价格表。它默认按大小升序,想看最大的那几项得 `| tail`,想看大小列得给 `--print-size`。

上面那一屏咱们加了 `-C`,看见的是给人类读的名字。去掉 `-C`,同一个符号长这样。让两个翻译单元各自实例化一次同一个模板,每个单元里都写一个自己的函数,里面调一次 `Max`,两份都按 `-O0` 编:

```cpp
// a.cpp
template <typename T>
T Max(T a, T b) { return a < b ? b : a; }

extern "C" int UseFromA(int a, int b) { return Max(a, b); }

int main(int argc, char**) { return UseFromA(argc, 1) & 0x7F; }
```

```cpp
// b.cpp
template <typename T>
T Max(T a, T b) { return a < b ? b : a; }

extern "C" int UseFromB(int a, int b) { return Max(a, b) + 1; }
```

```text
$ g++ -std=c++23 -O0 -c a.cpp -o h7wa.o
$ g++ -std=c++23 -O0 -c b.cpp -o h7wb.o
$ nm --print-size h7wa.o | grep MaxIi
0000000000000000 000000000000001c W _Z3MaxIiET_S0_S0_
$ nm --print-size h7wb.o | grep MaxIi
0000000000000000 000000000000001c W _Z3MaxIiET_S0_S0_
```

咱们把两份目标文件一起交给链接器,可执行文件里只剩一份。这就是同一份配方到处包含却不破 ODR 的办法。那个名字笔者念不出来,可链接器只认它。还有一处要留意:小函数在 `-O2` 下会被内联,实例化符号可能不单独现身,所以命令里的优化等级,您也要一起报出来。

## 名字与汇编:链接期的名字,和一段不能被删的汇编

第九张卡是 `extern "C"` 和链接期名字。咱们把两种名字写进同一个文件,存成 `wire.cpp`:

```cpp
#include <cstdint>

namespace wire {
int Read(int x) { return x + 1; }                  // C++ 链接:名字被修饰
}

extern "C" int WireRead(int x) { return x + 2; }   // C 链接:名字原样
```

同一个“人类名字”的两种链接期名字,咱们编出来放到 `nm` 里就摆开了:

```text
$ g++ -std=c++23 -O2 -c wire.cpp -o wire.o
$ nm wire.o | grep Read
0000000000000010 T WireRead
0000000000000000 T _ZN4wire4ReadEi
```

`namespace` 里那个 `Read` 被修饰成 `_ZN4wire4ReadEi`,作用域和参数类型都编进了名字。`extern "C"` 的 `WireRead` 就叫 `WireRead`。咱们不加 `-C`,看到的才是链接器认的那个字。两边签名一样,一边少写一个 `extern "C"`,那就成了两个名字。另写两个小文件:一个把 C 名字导出去,一个少写那个 `extern "C"` 去要它:

```cpp
// wire_export.cpp
extern "C" int WireRead(int x) { return x + 2; }
```

```cpp
// wire_call.cpp
int WireRead(int x);   // 少写了 extern "C"

int main() { return WireRead(1); }
```

```text
$ g++ -std=c++23 -O2 -c wire_export.cpp -o wire_export.o
$ g++ -std=c++23 -O2 -c wire_call.cpp -o wire_call.o
$ g++ wire_call.o wire_export.o -o wire_call
/usr/bin/ld: wire_call.o: in function `main':
wire_call.cpp:(.text.startup+0x6): undefined reference to `WireRead(int)'
collect2: error: ld returned 1 exit status
```

报错里那个 `(int)` 是线索:链接器要的是带参数表的修饰名,不是人类名字。C 链接之下也没有重载,两个同名的 `extern "C"` 声明会直接冲突。这一屏 `undefined reference` 跟前面讲裸地的地方长得像。可是那里缺的是运行期供给,这里缺的是名字对不上,咱们读的时候要分得开。

第十张卡落在内联汇编上。约束那一层,前面讲内联汇编的地方已经交代过,这里咱们只认脸。探针存成 `asm_probe.cpp`:

```cpp
// 同一段汇编,两种“编译器能不能动它”
#include <cstdint>

extern "C" std::uint64_t DeadAsm(std::uint64_t x) {
    std::uint64_t y = 0;
    asm("movq %1, %0" : "=r"(y) : "r"(x));            // 输出没人用,又没有 volatile
    return 0;
}

extern "C" std::uint64_t KeptAsm(std::uint64_t x) {
    std::uint64_t y = 0;
    asm volatile("movq %1, %0" : "=r"(y) : "r"(x));   // 删不得
    return 0;
}

extern "C" std::uint64_t LoopPlainNop(std::uint64_t n) {
    std::uint64_t a = 0;
    for (std::uint64_t i = 0; i < n; ++i) { asm("nop"); a += i; }
    return a;
}

extern "C" std::uint64_t LoopVolatileNop(std::uint64_t n) {
    std::uint64_t a = 0;
    for (std::uint64_t i = 0; i < n; ++i) { asm volatile("nop"); a += i; }
    return a;
}
```

带输出操作数的扩展汇编,那个输出要是没人用,编译器有权把它当没有副作用,整条删掉。这套机制前面讲内联汇编的那一卷已经立过,咱们这里只看它在体积上是什么后果:

```text
$ g++ -std=c++23 -O2 -Wall -c asm_probe.cpp -o asm_probe.o
$ nm --size-sort --print-size asm_probe.o
0000000000000000 0000000000000003 T DeadAsm
0000000000000010 0000000000000006 T KeptAsm
0000000000000020 000000000000002e T LoopPlainNop
0000000000000050 000000000000002e T LoopVolatileNop
```

咱们看 `DeadAsm` 编出来只剩 `xor` 和 `ret`,3 字节。同一条汇编挂上 `volatile`,`KeptAsm` 留下 6 字节。所以“给这段汇编加个 `volatile`,让它别被删”在这儿是成立的。可这个经验只在扩展汇编上成立。没有操作数的 basic asm,编译器本来就按 volatile 对待:`asm("nop")` 和 `asm volatile("nop")` 编出来逐字节相同,上面那两行 `2e` 就是它们的 46 字节,循环体里都留着一条 `nop`——把 `nm` 换成 `objdump -d` 一看就知道。GCC 手册 Basic Asm 那个节点把话说得很直:“The optional `volatile` qualifier has no effect. All basic `asm` blocks are implicitly volatile.”(那个可选的 `volatile` 限定符没有效果,所有 basic asm 块都是隐式 volatile。)标准那边对 `asm` 只说它是 conditionally-supported,其余交给实现,所以它的主场在编译器手册里。

还得把两个 `volatile` 分开。前面挂在变量或者指针上的 `volatile`,管的是“编译器不能假设这块内存的内容”。这里的 `asm volatile` 管的是“这段汇编不许删、不许挪”。咱们看到哪一个,就按哪一个解释。这是另一个 `volatile`,别拿一边的口径去套另一边。

## 零开销:回收的粒度是段

卡认完了,咱们来算体积。这套现代 C++ 常说自己是零开销,意思是“不用就不付钱”。真落到体积上,这句话要由链接器兑现,它那边有两样开关能配合。`-ffunction-sections -fdata-sections` 把每个函数、每个数据项各自放进一个段,段名跟着符号走。`-Wl,--gc-sections` 让链接器从入口出发做可达性分析,把没人要的段整段拿走。一个负责把房间隔成格子,一个负责动手清空。这里的分界是:回收的粒度是段,不是函数内部的某一支。

小例子撑不起这件事:三个几乎空的死函数,`size` 的 text 列只从 1393 掉到 1218,省 175 字节,文件字节从 15912 掉到 15608(`1218` 这个数下面还会碰上一次,那是同一块收不掉的底噪)。想看比例,得把死代码造大。下面这段生成器给您造一个够大的翻译单元:8 个大函数,每个 300 条语句,每个函数各带一张只被它引用的只读表,再由一个没人调用的 `Dispatch` 用 `switch` 把它们牵住。把下面这一整段存成 `mkbig.py`,它就是咱们接下来要用的造件脚本。

```python
import sys
n, stmts = int(sys.argv[1]), int(sys.argv[2])
out = sys.stdout.write
for k in range(n):
    tab = ",".join(str((i * 2654435761 + k * 40503) & 0xFFFFFFFF) + "u" for i in range(256))
    out("static const unsigned kTable%d[256] = {%s};\n" % (k, tab))
for k in range(n):
    out('extern "C" unsigned long long Big%d(unsigned long long x) {\n' % k)
    out("  unsigned long long a = x ^ %du;\n" % (0x9E3779B9 + k))
    out("  unsigned t = kTable%d[x & 0xFFu];\n" % k)
    for i in range(stmts):
        c = (0x85EBCA6B * (i + 1) + k * 7919) & 0xFFFFFFFF
        if i % 2 == 0:
            out("  a = (a + %du) ^ 0x%08xu;\n" % (c, (c * 3) & 0xFFFFFFFF))
        else:
            out("  a = a * 0x%08xu + %du;\n" % ((c | 1), c))
    out("  t = (t ^ (unsigned)a) * 0x85EBCA6Bu;\n")
    out("  return a ^ ((unsigned long long)t << 17);\n}\n")
out('extern "C" unsigned long long Dispatch(int mode, unsigned long long x) {\n  switch (mode) {\n')
for k in range(n):
    out("    case %d: return Big%d(x);\n" % (k, k))
out("    default: return x;\n  }\n}\n")
out("int main(int argc, char**) {\n#ifdef KEEP_ONE\n  if (argc > 1) return (int)(Big0((unsigned long long)argc) & 0x7F);\n#endif\n  return argc > 0 ? 0 : 1;\n}\n")
```

笔者把原件精简成了这一段,两者不逐字相同。下面屏上的数字——包括 `k1` 少的那 32 字节和 `k3` 的 84.6%——都出自原件那一版。您拿手里这段精简版跑,绝对值会落到另一组数上,能对上的量级与机制才是要看的。造件和四个版本的编译命令是这些:

```bash
$ python3 mkbig.py 8 300 > big.cpp
$ g++ -std=c++23 -O2 -DKEEP_ONE -o k0 big.cpp
$ g++ -std=c++23 -O2 -DKEEP_ONE -Wl,--gc-sections -o k1 big.cpp
$ g++ -std=c++23 -O2 -DKEEP_ONE -ffunction-sections -fdata-sections -o k2 big.cpp
$ g++ -std=c++23 -O2 -DKEEP_ONE -ffunction-sections -fdata-sections -Wl,--gc-sections -o k3 big.cpp
$ size k0 k1 k2 k3
```

下面四格是**原件**在本机 `g++ 16.2.1` 加 `-O2` 下编出来的(您拿精简版跑会得到另一组绝对值):

```text
$ size k0 k1 k2 k3
   text	   data	    bss	    dec	    hex	filename
  37110	    560	      8	  37678	   932e	k0
  37078	    552	      8	  37638	   9306	k1
  37110	    560	      8	  37678	   932e	k2
   5697	    552	      8	   6257	   1871	k3
```

咱们这样读:`k0` 是基线,`k1` 只加了 `-Wl,--gc-sections`,`k2` 只加了 `-ffunction-sections -fdata-sections`,`k3` 两个都加。`k1` 只比基线少 32 字节,`k2` 一个字节都没少,`k3` 省下 31413 字节,也就是 84.6%。`k3` 的文件字节也从 49104 掉到 15736。

只给回收那一半,`.text` 还是一整段,里面全是活函数,链接器没有下手处。少的 32 字节来自 C 运行时启动文件 `Scrt1.o`:它有一段 4 字节的只读常量(`.rodata.cst4`),后面跟着 28 字节对齐填充,两样一起撤掉正好 32。同一轮里它的 `.data` 也一样,4 字节实体加 4 字节对齐,合起来 8 字节,落在 data 列上。这些跟咱们的函数都没关系。只给分段那一半,段确实多了,同一个目标文件里 `PROGBITS` 段从 7 个变成 24 个,`nm` 里每个符号的地址也都回到了自己那一段的开头,不加分段时它们还是顺次排开的。可没人动手回收,您一个字节都省不下来。

`k1` 那一行别读成“不加 `-ffunction-sections` 就无效”。粒度既然是段,咱们就得看死代码占在哪:当它恰好独占一段时,不加分段也能整段丢掉。同一个造件去掉 `-DKEEP_ONE`,死代码正好占满 `.text`,`main` 单独住在 `.text.startup`,您只加 `-Wl,--gc-sections`,text 就从 37094 掉到 1218,一下省掉 35876 字节。要记的是“段里只要有一个活函数,整段就得留下”,不是一句口号。

您想知道具体丢了哪些段,让链接器自己报就行:

```text
$ g++ -std=c++23 -O2 -DKEEP_ONE -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--print-gc-sections -o k3 big.cpp
/usr/bin/ld: removing unused section '.text.Dispatch' in file '/tmp/ccXXXXXX.o'
/usr/bin/ld: removing unused section '.rodata.Dispatch' in file '/tmp/ccXXXXXX.o'
…
```

这屏一共 18 行,咱们只截了这个翻译单元里最要紧的两行。省掉的里面有 `Scrt1.o` 送来的两行(`.data` 与 `.rodata.cst4`),它们下面马上就要用到。

每行都是“哪个段、来自哪个目标文件”,`/tmp` 那个临时名每次编译都换。丢掉的段里有一类名字在源码里找不到:`.rodata.Dispatch` 是 `switch` 生成跳转表落下的段。段名的形状是 `.text.<函数名>` 和 `.rodata.<符号名>`,咱们认出这个形状,就能把链接器报出来的段对回自己写的函数。把丢掉的东西加一遍:

| 丢掉的段 | 字节 |
|---|---|
| `.text.Big1` 到 `.text.Big7` | 7 × 3404 = 23828 |
| `.text.Dispatch` | 97 |
| `.rodata._ZL7kTable1` 到 `.rodata._ZL7kTable7` | 7 × 1024 = 7168 |
| `.rodata.Dispatch` | 32 |
| 小计 | 31125 |

咱们回到 `size -A` 逐段看,`.text` 从 27649 掉到 3692,`.rodata` 从 8256 掉到 1024,展开信息那一对又少了 224 字节(`.eh_frame` 160,`.eh_frame_hdr` 64),合起来是 31413。它比表里那 31125 多出 288 字节,分开看是三样:224 字节的展开信息,`.text` 里段与段之间的对齐填充 32 字节(段被撤走,这些空档也跟着消失),再加前面点过的 `Scrt1.o` 那 32 字节。表里那几行是按段名加出来的,这 288 不重复算它们。回收按可达性走,不是挑没用的删:`Dispatch` 是上游,它一被丢,下游七个大函数和七张只读表整片跟着不可达。它也不裁剪函数内部,某个 `if` 分支从没被执行,那部分代码照样留在段里。

同一份源码换链接形态,比例会不会跟着变?咱们前面看的四个格子是动态链接那一档。再把造件编成裸机近似的一档,取一次纯镜像字节:

| 形态 | 基线 text | 回收后 | 省 |
|---|---|---|---|
| `-nostdlib -static -Wl,-e,main -Wl,-N` | 35925 | 4608 | 87.2% |
| 同一镜像再 `objcopy -O binary` | 35960 | 4632 | 87.1% |

两档在一个量级。带 libc 的全静态那一档咱们不摆上来,它的基线跟着您的发行版走,绝对值不可承诺。那“电脑上省得少”的说法从哪来?它来自小例子,不来自动态链接。把能回收的全收掉,动态链接这一档还剩三样东西:`.text` 265 字节,`size` 的 text 列 1218 字节(就是前面点过的那块底噪),文件 15600 字节——那是把能回收的全收掉之后那个空壳,和上面小例子那 15608 不是同一件东西。这块底噪是个常数,跟死代码有多少无关。死代码只有几百字节时,分子小,分母里这点常数就主导了比例。换成大函数,比例立刻回到八成以上。所以要看的是死代码量与可达镜像量之比,不是“动态链接把比例稀释了”。

编译器手册自己那句话写得很克制。它说,把两样开关配上链接器的回收,`may lead to smaller statically-linked executables (after stripping)`。咱们看清楚它承诺的是什么:静态链接出来的可执行文件,别读成“动态链接上也照样省”。链接器手册把留下谁写得更直白。入口符号所在的段要留下,命令行上未定义符号所在的段也要留下,被动态对象引用到的段同样留下。宿主可执行文件里那些 `.dynsym`、`.dynstr`、`.rela.dyn`、`.dynamic`,就是这类常数,回收前后一个字节都没动。

这一档还有两个前提要报全。`-Wl,-N` 会带一条 `warning: ... has a LOAD segment with RWX permissions`,这是单 LOAD 段的必然结果,不是错误。这个镜像的入口约定也不成立,内核不会按 `main(argc, argv)` 来调它,所以它只用来比体积,不跑。回收之后还剩一小块展开信息,您把 `-fno-exceptions -fno-asynchronous-unwind-tables -fno-unwind-tables` 也加上,`.eh_frame` 整段消失,text 从 4608 掉到 4544。您在自己机器上把这套命令重复一遍,数字会和上面不同,对得上的量级与机制才是要看的。
