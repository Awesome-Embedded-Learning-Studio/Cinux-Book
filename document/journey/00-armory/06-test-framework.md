---
title: 06 · 测试框架:三十五个用例,一条命令
description: "TEST 宏这回拆开给大伙看:一行登记怎么汇成一条命令,快照计数怎么判一个用例的生死,框架怎么考自己,还有现成的测试库为什么请不进来。"
chapter: 0
order: 6
platform: host
difficulty: beginner
cpp_standard: 23
tags:
  - host
  - beginner
  - unit-test
  - ctest
---

# 测试框架:三十五个用例,一条命令

上一节咱们把断言安顿好了:两个世界各交一份实现,管的都是"失败以后怎么办"。本节呢,轮到咱们把最后一件武器请出场了。说"请"其实不太准确,它早在[格式引擎](04-format.md)一节就朝咱们露过脸了:`TEST("名字")` 后面跟一对花括号,里头 ASSERT 几句、一个用例就立起来了。当时只让咱们看了它们用起来的样子,末了撂下一句:零件是测试框架的,框架得过两节才出厂呢。咱们这就到出厂现场。

可 TEST 到底是谁呢?花括号里的代码,谁在什么时候调用它呢?咱们眼看着它一口气跑完了二十一个用例,却始终没见过它本人的脸。本节咱们把它拆开,顺便把"为什么非要有它"从头说透。

## 深夜,第十次构建

咱们还没有测试框架的时候,验证一轮的完整动作是这样的:改一行代码、开一次虚拟机、等几十秒引导、看着串口一行行吐字,最后咱们拿肉眼判个对错。一轮几十秒嘛,喝口水的工夫就翻过去了。咱们一天几十轮,白天呢,顶多嫌它慢。

咱们真正的关口在深夜。第十次构建跑起来,心里的算盘跟着就打起来了:刚才就改了一行,跳过去,大概也不会出事吧?省下的可是实打实的三十秒呢。您看这笔账,每一笔都划算呢,可它偏偏骗人。bug 最喜欢的呢,就是"大概"这个词——它只要从跳过的那几次里溜进去一回,第二天就能请咱们花上半天,把这三十秒连本带利地还回去。那半天呢,搁咱们这套测试身上,够它跑上几千轮了。

所以呢,最后一件武器的目标,其实笔者定得特别平实:一条命令、几秒钟,要么全部通过,要么把挂了的那一行红着端到您眼前。全部通过的时候呢,屏幕上干干净净的。

光跑得快还不够呢,构建也得跟上节奏:咱们新增一个测试,等于加一个源文件,再写一行登记,它自己就汇进总目标了。咱们必须结束靠手维护清单的日子:新测试总有被漏掉的一天,这事笔者真撞见过,待会儿给您细讲。

## 一行登记

咱们从命令的源头看起。往后您新增一个测试,理想的体验就两步:写一个 test_foo.cpp 丢进 unit/,再添一行 add_cinux_test(foo),完事了,剩下的一概不用管。省心是 CMake 替咱们挣的——test/CMakeLists.txt 全文就这么长,咱们整个看一遍:

```cmake
enable_testing()

set(TEST_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/test/framework
    ${CMAKE_SOURCE_DIR}/base/include
)

function(add_cinux_test name)
    add_executable(test_${name}
        unit/test_${name}.cpp
        framework/framework.cpp
        framework/assert_host.cpp)
    target_include_directories(test_${name} PRIVATE ${TEST_INCLUDE_DIRS})
    target_link_libraries(test_${name} PRIVATE cinux_base cinux_warnings)
    add_test(NAME ${name} COMMAND test_${name})
    list(APPEND ALL_HOST_TESTS test_${name})
    set(ALL_HOST_TESTS ${ALL_HOST_TESTS} PARENT_SCOPE)
endfunction()

add_cinux_test(format)
add_cinux_test(framework)
add_cinux_test(result)

add_custom_target(test_host
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_CURRENT_BINARY_DIR} --output-on-failure
    DEPENDS ${ALL_HOST_TESTS}
    USES_TERMINAL
    COMMENT "Running host unit tests..."
)
```

咱们从上往下过:enable_testing() 打开 CTest 的登记开关,而 TEST_INCLUDE_DIRS 把两个头文件目录递给测试程序。中间的 add_cinux_test 呢,是注册函数,底下三行调用,出来的就是三个测试程序。压轴的 test_host 目标,依赖是清单里的全部程序,咱们把它们全都构建出来,再拿 CTest 跑一遍。您刚才想要的两步,靠的就是它:编译、链接、登记、汇进,全都自动了。

注册函数末尾的两行呢,笔者头一回看的时候琢磨了半天:list(APPEND ...) 明明把东西加进清单了,为什么还要紧跟一句 set(... PARENT_SCOPE) 再递一遍?后来才想明白呢。CMake 的函数自成一方小天地:函数体里的 ALL_HOST_TESTS 是带进来的副本,APPEND 全落在副本上,函数一返回,它就没了下文。PARENT_SCOPE 干的是显式交还的活:把改完的清单,点名递还给调用它的上层。您要是把它划掉,三个 add_cinux_test 个个把副本填得满满的,上层接到的清单呢,回回是空的,test_host 一个依赖都指望不上了。

末尾两行是学费换来的,笔者一点不夸张。当年第一遍呢,测试清单靠手维护:笔者新写一个测试,得记得去清单里添一笔。后来就出了事:有一个测试悄悄地漏在清单外,它连跑都没跑过,里面坏了也没人知道。出过一次漏网之后呢,手维护的清单当天就废了,换成注册函数:一个测试上不上 test_host 的依赖清单,全由 add_cinux_test 替咱们说了算,谁的记性都不用经过。

跑给您看,咱们就在仓库根目录:

```bash
cmake -B build && cmake --build build --target test_host
```

您头一回跑,它连配置带构建一起干,输出长一些。跑到最后呢,CTest 递给咱们的成绩单就是这么两行:

```text
100% tests passed out of 3

Total Test time (real) =   0.01 sec
```

三个测试程序全过了,咱们一共才用 0.01 秒。程序肚子里一共三十五个用例呢:格式引擎二十一个、Result 十个、框架自己四个。考框架自己的那四个,咱们放到后面单说。

您顺手绕开一条弯路:笔者构建完一时手痒,进 build/ 里裸跑了一句 ctest,结果被它糊了一脸:

```text
*********************************
No test configuration file found!
*********************************
Usage

  ctest [options]

```

ctest 找的是登记文件:enable_testing() 写在 test/ 子目录的 CMake 里,咱们的登记文件就生成在 build/test/ 下,而 build/ 根目录没有。解法二选一:咱们可以 ctest --test-dir build/test,指名道姓,或者干脆就构建 test_host——它内部就是这么调的,您刚才敲的命令,走的正是同一条路。

## 把 TEST 拆开

咱们来点一点框架的家底,一共四样:登记、判定、自测,外加一个所有测试程序共用的 main。CMake 嘛,只是登记的壳,真身在 C++ 这边。framework.hpp 里的 TEST 长这样:

```cpp
#define CINUX_TEST_CAT2(a, b) a##b
#define CINUX_TEST_CAT(a, b)  CINUX_TEST_CAT2(a, b)

#define TEST(case_name)                                                                            \
    static void CINUX_TEST_CAT(cinux_test_fn_,                                                     \
                               __LINE__)(); /* NOLINT(misc-use-anonymous-namespace) */             \
    static const ::cinux::test::Registrar CINUX_TEST_CAT(cinux_test_reg_, __LINE__){               \
        case_name, CINUX_TEST_CAT(cinux_test_fn_, __LINE__)};                                      \
    static void CINUX_TEST_CAT(cinux_test_fn_,                                                     \
                               __LINE__)() /* NOLINT(misc-use-anonymous-namespace) */
```

(夹在里头的两处 NOLINT 注释呢,是咱们递给仓库里静态检查器的暗号,跟逻辑无关,咱们跳过去不管)

咱们把 `TEST("名字") { ... }` 一次展开,变出三样东西:

- 一个前置声明,提前告诉编译器下面有个函数。
- 一个静态登记器。静态对象的构造函数在 main 之前就运行,把"名字 + 函数指针"塞进全局注册表。
- 最后是函数定义,您写的花括号就是它的函数体。

所以您写到 main 里,只需要一句 RunAll():注册呢,其实在程序真正开跑之前就已经办完了。

注册表的本体呢就住在 framework.cpp 里:一张定长数组,咱们把上限定在 256。登记动作呢,就是咱们刚才看到的登记器构造函数:

```cpp
Registrar::Registrar(const char* case_name, void (*case_body)()) noexcept {
    if (g_table_size < kMaxCases) {
        g_table[g_table_size] = TestCase{.name = case_name, .body = case_body};
        ++g_table_size;
    }
}
```

为什么是定长数组呢,而不是会自己长个儿的 vector?咱们在[七个头,五面旗](02-rules.md)一节给 base 划边界的时候,笔者有一条:不做堆分配。边界落到框架身上呢就是这个形状:登记只往表里放一个条目、不向谁要内存。咱们说句实话,宿主机上这个时点堆其实是可用的——静态构造虽在 main 之前,运行时呢,早把堆给咱们备好了。真正卡住的是将来:框架要跟着测试进内核。内核的 main 之前呢,才真的没有堆。咱们从第一天守起,到那天就不用重写框架了。

上限 256 呢,边界在咱们这儿:第 257 个测试不会报警,它只是无声地不进表。咱们要真撞上那天,该做的就是让越界当场炸出来,别继续吞。另有一条更漂亮的路呢——链接器自定义段,能把登记整个交给链接器去汇总,宿主机这一程用不上,咱们就不展开了。

咱们再看名字生成的两行 CINUX_TEST_CAT2 / CINUX_TEST_CAT,它们是整个宏里最绕的地方,咱们慢慢排。规则就一条:## 拼接符把两边的记号粘成一个,但其实粘贴时不做展开——参数原样上砧板。

咱们拿它走一遍:假如只有一层,咱们直接把 cinux_test_fn_ 和 __LINE__ 交给 CINUX_TEST_CAT2,那 ## 眼里的 __LINE__ 压根没被当成数字,它就是一个记号,原样粘过去,粘出来一个名字里嵌着 LINE 四个字母的标识符,可不是行号哦。同一个文件里呢,第二个 TEST 粘出来的还是同一个名字,当场撞名了。所以呢,咱们就在中间垫了一层,让 CINUX_TEST_CAT 接手:普通宏调用,参数其实在替换前会展开一遍,__LINE__ 就势变成 42。然后才轮到 CINUX_TEST_CAT2 上场。它拿着展开好的两边一粘,得到的就是 cinux_test_fn_42。两层各干一步:上层管的是展开,下层管的是粘贴。

咱们为什么要挑行号当锚点?有个候选看着更现代:编译器内置的 __COUNTER__,每出现一次自动加一,似乎天生就该干这个呢。笔者真试过它,而且走岔了:计数器只认自己出场的次数,每出现一次就涨一个数。TEST 一次展开里呢,同一个名字咱们要用好几回——前置声明一回、登记器一回、定义一回,三次出场就成三个数了,谁跟谁都对不上,编译当场翻脸。行号呢,在同一次展开里是恒定的:第 42 行的 TEST,写下的名字每一处都是 42。咱们要的就是这个。

## 快照计数

咱们接下来看判定,想看懂它,咱们得弄清楚断言失败那一刻发生了什么。04 里您见过 ASSERT_STREQ,这一族打底的是 ASSERT_TRUE,咱们看它的展开:

```cpp
#define ASSERT_TRUE(expression)                                                                    \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::println(stderr, "[FAIL] {}\n  ASSERT_TRUE({}) failed\n  at {}:{}",                \
                         ::cinux::test::CurrentCaseName(), #expression, __FILE__, __LINE__);       \
            ++::cinux::test::g_failures;                                                           \
            return;                                                                                \
        }                                                                                          \
    } while (0)
```

宏展开以后呢,咱们顺着执行往下走。头一个动作是往标准错误流打报告,而且不缓冲。为什么呢?[Result 一节](03-result.md)实验 B 验证过:程序终止时,攒在缓冲里的输出会跟着蒸发,报告必须在死之前落袋。紧接着呢,#expression 把咱们写的表达式原样变成字符串,报告里印的是您写下的原文,而不是干巴巴一句"失败了"。[格式引擎](04-format.md)里咱们用过的 ASSERT_STREQ(format_buffer, "abcd") 呢,真挂了的时候,两个参数名连同 got / want 一起端到您眼前。再往后呢就是失败计数 g_failures 加一,收尾靠的是 return——退出测试体。

收尾这个 return 呢,咱们得单独停一停:它退出的,正是您写下的那个测试体。别的谁都出不来,函数里的 return 只能出函数自己。咱们让 TEST 和 ASSERT_* 这两族宏住进"极端必要"那一档,而且只住测试代码,凭的就是这个:登记块、失败后的抽身都只有文本展开办得到。产品代码零宏的边界呢,[断言一节](05-assert.md)里立过的,Check 是函数,不因它们破例。

它跟 Check 的分工呢,到这儿正好划清:Check 守咱们的产品代码,失败了打一行报告、终止整个程序呢。ASSERT_* 呢,只管咱们的测试代码,失败了嘛,就是打报告、计数、退出当前用例,后面的用例接着跑。咱们在 [Result 一节](03-result.md)见过的 value() 第一行 Check,就是它守在消费端的样子——接住了,忘了检查就取值,当场终止。

接下来呢,是判定里最要紧的一步:框架看不见测试体里的 return——从外面,它根本不知道哪次 return 是失败的。它呢,唯一看得见的是那个计数。于是咱们给"通过"下的定义只能是快照:跑之前记下计数,跑完咱们看它动没动。

```cpp
bool RunSingle(const TestCase& test_case) {
    const int         kFailuresBefore = g_failures;
    const char* const kNameBefore     = g_current_case_name;

    g_current_case_name = test_case.name;
    test_case.body();

    g_current_case_name = kNameBefore;
    const bool kPassed  = g_failures == kFailuresBefore;
    g_failures          = kFailuresBefore;
    return kPassed;
}
```

咱们挨行走一遍。咱们一进来就记两个快照:失败计数一份、当前用例名一份。后者呢,管的是 ASSERT 报告里喊的名字,待会儿自测的输出里您会亲眼看到它干活。然后咱们让它跑:计数没动过,就算通过。最后一行呢,把计数恢复回去——内部的失败,内部消化嘛。

您猜当年第一遍是怎么判的?跑完就标通过,后果您也想得到:失败的测试也进通过数,报告里的数字全是虚的。快照规则呢,就是这么改出来的。

恢复计数的实现,也是咱们挨过打才落下的。咱们中间有一版实现,注释拍着胸脯说"恢复计数",代码里呢,却寻不见它的影子。赶上自测的玩法恰恰是一个用例肚子里再跑一个用例:里头那个失败了,涨上去的计数没人摁回来,就这么带到了外头。外层那个用例呢,本来干干净净的,结果被带出来的计数拖累:也判成了失败。当场逮住它的,还是自测。

## 框架考自己

判定机制本身呢,咱们凭什么信?它要是把失败也判成通过,三十五个用例的成绩单就成了废纸。所以呢,test/unit/test_framework.cpp 开头的用例不冲产品代码去,考的正是框架自己:

```cpp
namespace {

void must_fail_body() {
    ASSERT_TRUE(false);
}

void must_pass_body() {
    ASSERT_TRUE(true);
}

}  // namespace

TEST("framework: failing case is never counted as passed") {
    const TestCase kFailing{.name = "__injected_fail__", .body = must_fail_body};

    ASSERT_FALSE(RunSingle(kFailing));
}
```

路数是注入:咱们造一个必然失败的用例,咱们再断言 RunSingle 把它判成了失败。咱们这是用框架的断言,考框架的判定。这考法真抓过错:快照计数里,注释拍着胸脯说恢复,代码却没落实的那一回,当场把它揭出来的,就是自测。同文件里呢还摆着三个用例:

- 能过的,要被判过。
- 嵌套跑完,计数要复原。
- 运行期登记,要真的进表。

框架呢也就是几个普通源文件,咱们不靠 CMake,也能把它拼成一个能跑的测试程序,咱们从仓库根目录直接编(GCC 16.2.1 上实录):

```bash
g++ -std=c++23 -Ibase/include \
    test/unit/test_framework.cpp test/framework/framework.cpp test/framework/assert_host.cpp \
    -o /tmp/test_framework && /tmp/test_framework
```

输出第一眼能把您吓一跳:

```text
[FAIL] __injected_fail__
  ASSERT_TRUE(false) failed
  at test/unit/test_framework.cpp:24
[FAIL] __injected_fail__
  ASSERT_TRUE(false) failed
  at test/unit/test_framework.cpp:24

=== Cinux Test Runner ===
Running 4 case(s)...

[PASS] framework: failing case is never counted as passed
[PASS] framework: passing case is counted as passed
[PASS] framework: nested RunSingle restores outer counters
[PASS] framework: registrar appends to the registry
[PASS] __late_registered__

=== Results: 5 passed, 0 failed ===
```

还没开跑呢,就冒出来两个 [FAIL]?大伙别慌,咱们看到的正是自测在干活。第一、第三个自测用例呢,各自单独运行了一个注入的失败用例。它内部的 ASSERT 报告呢,走的是标准错误流,不缓冲,说走就走,横幅和 [PASS] 呢,走的是标准输出,攒在缓冲里排队出门。咱们隔着管道看,出来的次序就乱了——这是缓冲的脾气,[Result 一节](03-result.md)实验 B 里咱们已经见过它一回。您要是在终端里直接跑,它们会插回各自的位置。

咱们再留意两声 [FAIL] 喊的名字:__injected_fail__,不是外层用例的名字。快照计数里那句"管的是 ASSERT 报告里喊的名字",干的就是这个活。

咱们再看一个怪处:横幅报的是 Running 4 case(s),汇总报的却是 5 passed。多出来的 __late_registered__ 呢,是第四个自测用例在运行期当场登记进表的。RunAll 的循环呢,每一圈都重新读表长,新来的它跟着就被跑了。运行期登记呢,是登记器自己留的口子,它不挑时点,自测连这个口子也一并验了。

最后咱们看 main。所有测试程序共用的都是同一句 `int main() { return RunAll(); }`,失败数直接当进程退出码。CTest 呢拿退出码判生死,零是过、非零是挂。本站开头状态面板承诺过的命令呢,敢把三十多个用例一次验完,把关的就是它。

还有个谜呢,咱们现在能揭了:全部通过的时候,屏幕上为什么一个 [PASS] 都没有?框架明明每个用例都喊了一嗓子呢。答案呢,在 test/CMakeLists.txt 的 --output-on-failure 旗子上:CTest 只把挂掉的测试的输出倒出来,通过的测试,连嗓门一起咽掉。于是输出分两层:CTest 一层管的是详略,退出码一层管的是生死。全过时的安静就是这么来的——屏幕越素,咱们心里越踏实。

## 为什么不请现成的

拆到这儿,您心里怕是早憋了一句质问:这些讲究,GoogleTest、Catch2 早替咱们办得妥妥帖帖,咱们为什么还要自己遭这个罪?

这些库都当自己住在正常的操作系统上:底下有完整的 libc、异常开着、内存随要随有,可内核里呢,咱们一样都没有。Catch2 还多一条:它的 FetchContent 接法,从第一天起就离不开网络,configure 阶段要联网拉源码呢,咱们在离线环境就卡死在第一步。而咱们的测试,将来是要跟着代码进内核的:在虚拟机里跑、通过串口把结果报出来。运行器呢,横竖得咱们自己写,逃不掉。

咱们既然在内核那边注定要有一套自己的,宿主机上再借一套别人的,同一件事呢,咱们就得学两遍手势。TEST 加 ASSERT 呢,咱们从今天一路用到旅程终点。等测试真的跟着代码进了内核,断言钩子那边由内核另配一份实现,咱们这两族宏一件不用换。咱们的用例呢本来就长得朴素:一句条件、一个期望,验完收工。搁咱们这儿,现成库引以为傲的分节、匹配器就使不上劲了。

framework.hpp 加 framework.cpp 呢,咱们拢共两个文件,这会儿您再打开,应该没有一行是生面孔的了。四件武器到这儿也就全齐了。

咱们的下一站,就是 512 字节的引导扇区。那台机器上呢,什么都没有。怕什么,该带的咱们都带上了。
