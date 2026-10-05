---
title: 02 · 裸地上 C++ 还剩什么
---

# 语言还在,替它干活的那一层没了

咱们手边随手就能写出这么几份最正常不过的 C++ 源码。一份叫 `guard.cpp`,里面只有一个函数局部静态对象,编译它不缺什么。另一份把 `try`、`catch` 都用上,放在平时用的电脑上跑起来什么都不缺。

可源码要送进一个没有标准库的地方时,咱们给编译命令加上 `-nostdlib`,让链接器别去碰标准库。它当场翻脸:

```text
guard.cpp:(.text+0x1f): undefined reference to `__cxa_guard_acquire'
```

就这一行,已经够说明问题了。链接器要一个咱们从没写过的名字,而这名字是源码里那句 `static C c;` 招来的。完整的那一串放到后面第五节。

招来它的那一层,平时一直在场,只是从来没跟咱们打过照面。

说这话的是链接器,不是编译器 —— 编译器那边一路绿灯,直到要把大家链接成一个完整的镜像,这个名字才头一回浮上来。一个运行时被拿走的世界上,语言还剩哪些部件还能用?咱们到五处具体的地方去问,答案等看完再说。

咱们五处现场都走完,事情的样子会很清楚:语言部件都还在,替它干活的那个东西没了。

咱们这一篇用的是同一套编译姿势,`-fno-exceptions`、`-fno-rtti`、`-nostdlib` 一起上。前两个开关和后一个打的不是同一处地方:工具链那一卷按条讲过 `-ffreestanding` 和 `-nostdlib` 各关掉什么,`-fno-exceptions` 与 `-fno-rtti` 关掉的是语言里的两套约定,这活儿放在这一篇做。

## 一、异常跳转表整节消失

咱们写一个会出事的小函数,顺手拿 `try` 把它包起来,存成 `throw_catch.cpp`:

```cpp
#include <cstdio>

extern "C" int Divide(int a, int b) {
    if (b == 0) throw 42;
    return a / b;
}

int main() {
    try {
        return Divide(10, 0);
    } catch (int code) {
        std::printf("caught %d\n", code);
        return 0;
    }
}
```

编译它,咱们把异常的开关拿掉:

```text
$ g++ -std=c++23 -fno-exceptions -c throw_catch.cpp -o throw_catch.o
throw_catch.cpp: In function 'int Divide(int, int)':
throw_catch.cpp:4:23: error: exception handling disabled, use '-fexceptions' to enable
    4 |     if (b == 0) throw 42;
      |                       ^~
throw_catch.cpp: In function 'int main()':
throw_catch.cpp:12:36: error: 'code' was not declared in this scope
   12 |         std::printf("caught %d\n", code);
      |                                    ^~~~
```

不是“抛出以后没人接”,是这一整块语法被关掉了。`throw 42` 直接编不过,报错让咱们把 `-fexceptions` 加回去。

跟着 `catch (int code)` 的这一行更有意思。编译器说 `code` 这个名字没有声明。咱们明明在形参表里写了它。也就是说,`catch` 那一段连人带形参,在这次编译里整个被丢掉了,`printf` 那一行才成了漏网的孤儿。

## 二、一句可能抛出的调用照样编过

有人会想:那我不写 `throw` 就完了。咱们换一份源码,自己的 `throw` 一个都不写,只调用一个有可能抛出的标准库函数,存成 `maybe_throw.cpp`:

```cpp
#include <string>

extern "C" void Grow() {
    std::string s(64, 'x');
    s += "y";
}
```

源码在关掉异常之后,编译通过,一个警告都没有。可咱们拿 `readelf` 看节的名单,两边的差别清清楚楚。按平时带异常的编法,目标文件里多出来的正是 `.gcc_except_table`:

```text
$ g++ -std=c++23 -O1 -c maybe_throw.cpp -o maybe_throw_exc.o
$ readelf -S maybe_throw_exc.o | grep -iE "except|eh_frame"
  [ 6] .gcc_except_table PROGBITS         0000000000000000  00000163
  [12] .eh_frame         PROGBITS         0000000000000000  000001d0
  [13] .rela.eh_frame    RELA             0000000000000000  00000460
```

`.gcc_except_table` 就是编译器替 `try`/`catch` 准备的那套跳转表,谁抛了、往哪跳,全写在里面。关掉异常等于没人再填这套表。咱们把同一个命令加上 `-fno-exceptions`,它整节就不见了:

```text
$ g++ -std=c++23 -O1 -fno-exceptions -c maybe_throw.cpp -o maybe_throw_noexc.o
$ readelf -S maybe_throw_noexc.o | grep -iE "except|eh_frame"
  [ 8] .eh_frame         PROGBITS         0000000000000000  00000178
  [ 9] .rela.eh_frame    RELA             0000000000000000  00000318
```

节的名字是稳定的,条的编号和偏移不是。咱们盯的是同一件事:上面名单里有 `.gcc_except_table`,下面名单里没有了。

咱们也不要看错邻居。`.eh_frame` 在两份名单里都在,它管的是栈怎么一层层退回去,和异常表是两回事。这个节留着,不代表异常还能用。

第二份现场安静得让笔者心里发毛。`std::string` 会抛 `bad_alloc`,这一点咱们都知道。可关掉异常以后,编译器不再为这段代码准备任何“万一抛出”的路,它也不吭声。

上面这段 `maybe_throw.cpp` 拿 `-fno-exceptions` 编,照样能过,编译期一声不吭。咱们凭什么说它“活得下来”?那要到有内存发放的时候才谈得上。

## 三、谁把 `main` 递进去

再看一遍开头的 `guard.cpp`,咱们这次不链接标准库,把命令写成这样:

```text
$ g++ -std=c++23 -fno-exceptions -fno-rtti -O0 -nostdlib -o guard.bin guard.cpp
/usr/bin/ld: warning: cannot find entry symbol _start; defaulting to 0000000000001050
…
```

上面那行 `warning` 是刚敲下的命令印出来的头一句,末尾那个 `…` 略掉的是后面那一长串同类报错,第五节会连着源码一起看。咱们的程序里写的是 `main`,可链接器开口要的是另一个名字,`_start`:两个名字各管一边,`_start` 才是镜像真正的入口,`main` 只是在它手里被调用。入口这一套语义,工具链那一卷讲链接脚本的地方已经按条交代过,咱们不再重开。这里只补一句增量:C++ 的全局构造、还有函数局部静态对象的初始化与析构,也都挂在这一摞上。

还有一件事值得看:`-nostdlib` 真正拿走的是标准库和 C 运行库这一整摞。上面那个 `cannot find entry symbol _start`,说的是入口没人提供。第五节那六行 `undefined reference`,才是咱们这一摞里露头最早的一批名字。

## 四、RTTI:掐死在编译期,或者无处安放

多态类型想在下游问一句“你到底是谁”,语言给的两件工具是 `dynamic_cast` 和 `typeid`。这两件工具合起来叫 **RTTI**,运行期类型信息(Run-Time Type Information)。咱们把它们和 `-fno-rtti` 放一起,要编的源码写进 `rtti.cpp`:

```cpp
#include <typeinfo>

struct Base { virtual ~Base(); };
struct Derived : Base {};

extern "C" Base* Downcast(Base* b) { return dynamic_cast<Derived*>(b); }
extern "C" const char* NameOfObject(Base& b) { return typeid(b).name(); }
extern "C" const char* NameOfType() { return typeid(Derived).name(); }
```

```text
$ g++ -std=c++23 -fno-rtti -c rtti.cpp -o rtti_off.o
rtti.cpp: In function 'Base* Downcast(Base*)':
rtti.cpp:6:45: error: 'dynamic_cast' not permitted with '-fno-rtti'
…
rtti.cpp: In function 'const char* NameOfObject(Base&)':
rtti.cpp:7:62: error: cannot use 'typeid' with '-fno-rtti'
…
rtti.cpp: In function 'const char* NameOfType()':
rtti.cpp:8:60: error: cannot use 'typeid' with '-fno-rtti'
…
```

(每条报错下面,编译器原本还回显一行源码、再配一串指向出错位置的插入符,这几处就用 `…` 省掉。源码上面咱们已经印全,报错文字本身一个字没改。)

两种报错的措辞不一样,值得咱们分开念。`dynamic_cast` 是“不允许”,`typeid` 是“不能用”。而且 `typeid` 对多态对象和对一个类型名报的是同一句,连给类型取个名字这件事都一起被掐掉了。

咱们松开开关,把上一段命令里的 `-fno-rtti` 去掉,让 RTTI 全开,再看目标文件里多出什么(命令里那个 `-C` 是让 `nm` 把修饰过的名字翻成人能读的样子,第五节还会细讲):

```text
$ g++ -std=c++23 -c rtti.cpp -o rtti_on.o
$ nm -C rtti_on.o | grep -iE "typeinfo|vtable"
                 U typeinfo for Base
0000000000000000 V typeinfo for Derived
0000000000000000 V typeinfo name for Derived
                 U vtable for __cxxabiv1::__si_class_type_info
```

咱们看第二、第三行:`typeinfo for Derived` 是一个真正造出来的对象,`typeinfo name for Derived` 是一段真的类型名字符串,编译进 `.rodata` —— 只读数据段,和代码一起躺在镜像里。

咱们再看最后那一行,它是整件事的底牌。`vtable` 是虚函数表,多态调用按它找函数。名字里的 `__si_class_type_info`,则是 C++ 运行时库提供的一个类,`__cxxabiv1` 就是这个运行时的命名空间。RTTI 不是编译器凭空变出来的,它是运行时提供的一门服务,编译器只是来这里点单。

咱们把这个实验和上面讲 `main` 的那一段摆在一起看,说的是同一句话。`-fno-rtti` 把需求掐死在编译期,`-nostdlib` 是需求还在、供给没了,两条路都通到“这一手在裸地上用不了”。

## 五、函数局部静态的守卫变量

咱们回到开头,把惹毛链接器的源码整个写出来,存成 `guard.cpp`:

```cpp
struct C { C(); ~C(); };
C& Get() { static C c; return c; }
int main() { return 0; }
```

咱们看看,就这么几行。构造函数和析构函数只声明了,没定义,`Get()` 里只有一行 `static C c;`。拿源码编到汇编那一层,`Get()` 里会凭空多出一个符号,叫 `_ZGVZ3GetvE1c`。名字是修饰过的,按修饰规则还原,就是 `Get()` 里那个 `c` 的守卫变量。源码里咱们一个字都没写过它。

想看出名字对应哪个函数,加一个 `-C` 就行,`nm -C` 会把它翻成 `guard variable for Get()::c`。下面就叫它旗子吧。它插在函数外面。拿 `nm -S` 量它,是 8 个字节宽,不是随手给的一个字节:ABI 把守卫对象定成 64 位,标准库头再把它 typedef 成 `__guard`,编译器就按这个宽度给它留位置。它管的事只有一件:那个对象到底初始化过没有。咱们写 `static C c;` 的时候心里想的“就初始化一次”,落到机器上就是旗子的读写。

咱们把 `-nostdlib` 加上,让链接器替咱们数一遍,源码到底欠了谁:

```text
$ g++ -std=c++23 -fno-exceptions -fno-rtti -O0 -nostdlib -o guard.bin guard.cpp
…
guard.cpp:(.text+0x1f): undefined reference to `__cxa_guard_acquire'
/usr/bin/ld: guard.cpp:(.text+0x37): undefined reference to `C::C()'
/usr/bin/ld: guard.cpp:(.text+0x3e): undefined reference to `__dso_handle'
/usr/bin/ld: guard.cpp:(.text+0x4c): undefined reference to `C::~C()'
/usr/bin/ld: guard.cpp:(.text+0x57): undefined reference to `__cxa_atexit'
/usr/bin/ld: guard.cpp:(.text+0x66): undefined reference to `__cxa_guard_release'
/usr/bin/ld: guard.bin: hidden symbol `__dso_handle' isn't defined
…
```

(这些行是链接器一口气连着报下来的。哪几行前头带 `/usr/bin/ld:`,随工具链版本而变,咱们不拿它当规律。开头那个 `…` 略掉的是 `defaulting to` 那一行,以及编译器那串临时对象名和 `in function` 那一行。末尾那个 `…` 略掉的是 `final link failed` 与 `collect2` 两行,它们上面那行 `hidden symbol … isn't defined` 是印出来的。另外,括号里那些 `(.text+0x…)` 偏移、`defaulting to` 后面的入口地址、还有那串临时对象名,都是每次编译现生成的,换一版工具链就换一套数——要看的是名字,不是这几个数。报错文字本身一个字都没动。)

六条 `undefined reference`,全是从那一行 `static` 里长出来的。咱们把 `-O0` 明写进命令里,它也正是编译器默认的优化等级,不写就等于取它:源码、命令、输出三样得是同一份现场。

咱们一行行认。`C::C()` 和 `C::~C()` 是咱们自己欠的,前面说过。剩下的四个,一个都没在源码里出现过。`__cxa_guard_acquire` 和 `__cxa_guard_release` 管的是旗子的两个动作:进函数问一句“该我初始化了吗”,初始化做完再喊一声“我收工了”。一问一答之间,别人才知道要不要等。

两个名字不是某个编译器随手起的,Itanium C++ ABI 把它们定死了,跨 GCC、跨 Clang、跨版本都不改名。语言标准对函数局部静态的要求写得很严。控制流并发闯进声明的时候,没轮上的那一边必须等初始化做完,咱们不能只按单线程去想。编译器没打算自己扛这件事,于是外包出去。

咱们再看 `__cxa_atexit` 和 `__dso_handle`。这一对名字管的是收场,静态存储期的对象是“构造过才析构”,所以每构造成功一次,就得向运行时登记一次收场的活儿。这两样也是同一门服务的另一头。

到这里,咱们总算看清那个 `static` 的成本了。它压根不是“一个变量”那么便宜,背后还拖着一整套“只初始化一次,而且收场时记得析构”的约定。约定写在语言里,实现平常由 C++ 运行时提供。`-nostdlib` 的意思就是:供应商不来了。

不过名单是有弹性的,咱们别把它当常数记。构造和析构都补上定义、析构又是一个空体,再把优化换到 `-O1`,编译器才会把析构判成“没什么可做”,登记析构的那两行从目标文件里消失,名单缩回两行守卫。咱们这一屏全用 `-O0`,把定义补上也不见少:那一档下仍是四个符号、四行报错。换一档优化、换一个析构的写法,名单的长短又变。所以每回贴现场,咱们都得连源码和优化等级一起报,不然您在自己机器上复现出来的就是另一份。

## 六、还剩什么,谁没了

五处现场看完,咱们把这一篇的三列并排放一张对照:

| 语言部件 | 平时替它干活的那一层 | 谁没了 |
|---|---|---|
| `try` / `catch` / `throw` | 异常跳转表 + 展开运行时 | `-fno-exceptions` 一开,相关的表整节不再生成,语法本身也编不过 |
| `main` 当入口 | 启动代码 `_start`,外加标准库和 C 运行库 | `-nostdlib` 一来,链接器张口找 `_start`,咱们只有 `main` |
| `dynamic_cast` / `typeid` | typeinfo 对象和 `__cxxabiv1` 里的那些类 | `-fno-rtti` 掐死需求,`-nostdlib` 断掉供给 |
| 函数局部静态对象 | `__cxa_guard_acquire` 这类初始化守卫,加析构登记 | 编译器照旧生成调用,实现没人提供 |

咱们横着读一遍。左列全是语言里认得出来的东西,中间和右列全是替它干活的角色。运行时不来了,替它干活的那一层就空出来,语言部件自己不会伸手去补。想让您带走的也就是这个:不是“裸地上 C++ 变小了”,是语言把重活转包了出去,而转包的那几家现在都联系不上。

往后读正文的时候,您看到哪一样现代 C++,都可以拿前面排出的对照表问一句:这件事,平时是谁在替它干活?想得起来,就跟着想一下咱们手上有没有替代品。想不起来,那正好是后面要补的位置。
