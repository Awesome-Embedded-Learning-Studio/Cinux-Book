---
title: 05 · 一张脸,两张嘴
description: "console 的脸一个笔画不动:InitConsole、PutChar、PutString 照旧,变的只是户口和身子——从 boot 目录搬进 kernel/console,实现沉进 .cpp,内部 fan-out 两后端。串口立在前保调试,画面拒了 init 就静默空转,串口一个人扛。视频一锅炖分家,设备件、文字屏、字模各立门牌。面板亮起 screen console 一行,黑屏上头一回有了字。"
chapter: 10
order: 5
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - console
  - design
---

# 一张脸,两张嘴

console 的脸——从串口那一卷立起来就没动过一个笔画:InitConsole、PutChar、PutString,进的是字,别的什么都不认。这一卷咱们给它添上的是一张嘴,而咱们的头一个决定恰恰是:脸一个笔画都不动。上层的 print 一行不改,底下的家随便换——中立脸的第三次实践了:console 对着串口是一次、tick 对着定时器芯片是一次,今天轮到的是对上画面。

变的是户口和身子。户口从 boot 目录搬进 kernel/console——控制台是内核的器官,不是引导的家当。身子从一份全 inline 的薄脸,沉成了 .cpp 里的实现。实现全文就这么多,咱们整个摆出来:

```cpp
void InitConsole(const cinux::boot::BootInfo& info) {
    cinux::driver::SerialInit();
    cinux::console::TextConsole::self().init(info.framebuffer);
}

void PutChar(char character) {
    cinux::driver::SerialPutChar(character);
    cinux::console::TextConsole::self().put_char(character);
}
```

装配的次序是本节的正主:串口立在前、画面立在后。为什么非得这个次序:调试不等画面。串口的那一路,几个端口的写就立起来了,什么环境都不挑。画面的一路要过门、解析字模、清三兆字节,一路上出岔子的机会多的是,真出了岔子,咱们还得指着串口说话。画面要是拒了 init——单子上没有可用的几何,或者字模坏了——TextConsole 从此是安静的空转,PutChar 的第二笔空写、第一笔照旧走线。机器是照常说话的,缺的只是亮:画面是锦,串口是保命的。

两张嘴的分岔,就写在 PutChar 的两行里,咱们没有表、没有注册、没有回调。当年第一遍可不是这么办的:输出侧挂着一张函数指针的表,一挂挂了八张嘴、谁想说话谁注册。这一遍咱们不搬——道理还是那句老话:消费者没到齐,咱们不提前备货。今天的嘴,咱们就备两张,而且永远成对:一张给人看,一张给咱们自己查。哪天真添了第三张,再立表也不过是两行改表的事。

这一卷还办了一户分家。视频的这几件原本炖在一口锅里,咱们把锅解散了:framebuffer 平铺进 kernel/driver/,跟串口、定时器做的是邻居——它是一件设备。console 立了个新主题,专收文字的家当:中立的脸、格子的数学、文字屏、字模,四件合住的是一个门牌。目录就是课表:将来图形的活要借显存,咱们去 driver 找。文字的活要借字模,串门认 console 的门。三户各姓各的姓,namespace 也跟着分了家。

于是咱们开机。InitConsole 站在组合根的头一行,串口立好了,画面也立好了,然后面板上打字——打的是同一个 Println、两路同享,而面板上多出来的那一行,正是画面自己报的家门:

```text
[kern] screen console 128 cols 48 rows
```

面板里的新面孔就这一行,而且只在画面真活了的时候才打:活不过来的话,连这行也省了,面板照旧全走的是串口——诚实比热闹要紧。黑了几卷的屏幕,头一回亮起了白字:串口上那几行、画面上一行不差。字的模子是同一份,数的行列是同一套,一路过的是 16550 的窄门,一路过的是咱们自己开的门、自己的语法。两条线各走各的,说出来的话一个标点都不差——这就是中立脸的红利:脸只有一张,嘴想怎么长就怎么长。

输出的事,到这儿齐了。可面板上的每一行,说到底还是内核的独白——它还没听过一句外来的话。下一半咱们把耳朵接上,去会一会键盘背后那块比 PC 还老的芯片。
