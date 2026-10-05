---
title: 05 · 心跳的两种身份
description: "计数与速率旋钮是服务 Tick,三写端口是设备 Pit,消费者只认前者。concept 的约束就一条“会 start”,类型擦除成函数指针加上下文指针对,Hertz 过接口所以带类型,除数 1193182/100=11931 截断。host 侧一个假后端作证:会 start 的就能当后端,PIT 只是第一个。"
chapter: 9
order: 5
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - timer
  - pit
---

# 心跳的两种身份

门房安顿好了,咱们来请敲拍子的人。会听的世界里得有时间观念——将来的睡眠、调度、超时,根子上都离不开数拍子的人。咱们要配的心跳,得从它的两种身份说起:一种叫 Tick,是管数拍子的服务——中断来一拍记一拍,拍子敲多快是它的旋钮。另一种是叫 Pit 的设备,拧的是真芯片:8253/8254,可编程的间隔定时器,老 PC 的年代就在给机器打拍子。往后的消费者只认 Tick——永远不认 Pit。为什么劈成两半,咱们拿一个反问就能讲清:哪天换一颗芯片、或者换一台机器,满树的调用方要是一人一句 Pit,改得过来吗?

服务对设备的全部要求,咱们写在 `kernel/time/tick.hpp` 里——就一个 concept:

```cpp
template <typename Backend>
concept TickSource = requires(Backend& backend, cinux::base::Hertz rate) {
    { backend.start(rate) } -> std::same_as<void>;
};
```

约束就只立了一条:会 start、收一个频率。声明面是刻意无知的——一颗定时器芯片还会一百样本事、读计数、换模式、配门铃,可服务的脸一样都没要。要求等真消费者出现的那天再加——不提前备货。这是咱们立过的家法:可扩展、不等于完备。

后端怎么接进来,咱们看 init 的身子:

```cpp
template <TickSource Backend>
void init(Backend& backend) {
    start_ = [](void* backend_ptr, cinux::base::Hertz rate) {
        static_cast<Backend*>(backend_ptr)->start(rate);
    };
    source_ = &backend;
    ticks_  = 0;
    start_(source_, kTickHz);
}
```

一个无捕获的 lambda 转成函数指针,后端的地址当上下文跟着走,存成了一对。消费面看到的 Tick 是一个稳定类型、不随后端漂移——类型擦除,行话是这么叫的:把“具体是哪颗芯片”藏进了一对指针里,脸面上剩下的只有“会 start”。注册的当场就清零计数、按 kTickHz 启动。中断的门还没开,设备就跑起来了,危险吗?咱们拿芯片的实情来答:线闸是全捂着的,它敲它的,声进不了门,拍子是一颗也记不上的。设备早跑是无害的。

为什么用 concept 而不用虚函数,咱们就在这儿说清。虚表要的是 vptr,vptr 的初始化要走动态,零构造的纪律容不下。concept 失配的话,编译期当场指认到出错的行号,连猜的功夫都省了。派生类不是唯一的多态——这正是 C++23 教给人的一课,咱们眼前用得着的就这些。也不上 CRTP 那路把派生类型烙回基类的老办法:消费面要的是一个单一稳定的类型,不是一个随设备变的模板。

频率要过接口——咱们就给它自己的类型:

```cpp
struct Hertz {
    unsigned long long value;  ///< Oscillations per second.
};
```

您对照着看字节那头就更明白了:4_KiB、16_GiB 也好,字面量只是把乘法收进了头文件,数本身还是普通的整数。频率不行——一个光秃秃的 100 递过来,您不知道是赫兹还是千赫兹——类型就是文档。调用处写出来的是 100_Hz。数的住处,两个家咱们分得清清楚楚。kTickHz=100_Hz 住的是 tick_config.hpp,配置类常量的家,pmm_config 走过的老路。kPitInputHz=1193182 住 pit.cpp 的匿名空间,道理是输入时钟属于板子的接线,不属于服务的旋钮,它不进 config 的门。分家的判据落在性质上、不看谁常被读:改一个是调系统、改另一个是换板子。

设备的三写,咱们摆开看:

```cpp
void Pit::start(cinux::base::Hertz rate) {
    const auto kDivisor = static_cast<unsigned short>(kPitInputHz / rate.value);

    const cinux::driver::PortWrite kWrites[] = {
        {.port = kCommandPort, .value = kSquareWaveCommand},
        {.port = kChannel0DataPort, .value = static_cast<uint8_t>(kDivisor & 0xFF)},
        {.port = kChannel0DataPort, .value = static_cast<uint8_t>(kDivisor >> 8)},
    };
    OutB(kWrites);
}
```

除数的算式是 1193182 除以 100,走的是整数除法,截断之后落的是 11931。0x43 写的是 0x36——选 0 通道、低字节接高字节、方波、二进制,位域的细节数据手册整章都有,咱们只认结果。方波翻成人的话:让输出每秒高低来回跳一百次、每次跳变就是一拍。0x40 写的是低字节、再写高字节,三写发完了,拍子就敲起来了。又是 PortWrite 的表——您数一数,本卷到现在、凡是一次性的端口序列,咱们全写成了表。

服务的数怎么算对,host 那头的假后端可以作证。咱们在 test_tick.cpp 里立了一个普通类:

```cpp
class FakeBackend {
public:
    void start(cinux::base::Hertz rate) {
        started = rate;
        ++starts;
    }

    cinux::base::Hertz started{};
    unsigned int       starts = 0;
};
```

它什么芯片都不是,会的只有 start。围着它转的用例有三件:注册即把 kTickHz 递给后端,拿到的频率对、次数是一次。重复注册的时候,计数就清了零。每来的一拍——计数就加一。假后端的存在本身就是证词:concept 收编的不是 PIT,是任何会 start 的东西。还有一层实惠咱们白捡:Tick 的头零内核依赖——host 直接编得动,长凳上的第十五件测试,就是它挣来的。设计当天的功夫,当场回了本。

中断的路径上,on_interrupt 的整个身子是一行 `++ticks_`,躺在头文件里的内联——在哪个编译单元用、就在哪个编译单元实例化。为什么咱们省到这个地步:它跑在中断的进门处、路径越短越好,而且它得住进不开向量化的岛。岛上的住户名单,咱们讲到桌的那一章一并数。

心跳的设备备好了,服务的脸也画好了。拍子敲了出来,总免不了要有人应门、转递、回礼——这些活,咱们留给一张桌子管。
