---
title: "10 · 活着的控制台"
description: "假文件与真终端那一卷立的是谁在读谁驱动:读的人摇一遍终端,信号的翻译长在读的路上。ping 一阻塞在 recvfrom,没人读了,路就断,Ctrl+C 压根进不了翻译。三件活把控制台救活:一具常驻的 tty_poll 泵,权重二百,大数排队尾,点亮网卡那一卷立过的同款约定,身体是摇一圈让一圈。一把五张脸共用的输入门,泵与读者同门进同门出,前台组是一个原子量,默认一组。信号送组在放锁之后办,表锁是叶子的老话在这边同款。终端的收字带预算,收着收着见待送信号就停,别让一记 Ctrl+C 淹在大水里。USB 键盘的控制字节补齐了:左右 Ctrl 是修饰字节里的两位,按着 Ctrl 敲 c,终端收到的是字节三,VINTR,翻译成 SIGINT。控制台的本读保持非阻塞,等的圈照旧住在门上。"
chapter: 31
order: 10
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - tty
  - console
  - usb
  - signals
---

# 活着的控制台

收官大戏的排面里有一样容易漏的:ping 发出去之后,整个壳就阻塞在 recvfrom 的圈里。咱们停下来问一句:这时候谁在读控制台?没有人。而控制台的命脉,按假文件与真终端那一卷立下的老话,是有人才有驱动的:ping 一阻塞,读的门没人进,Ctrl+C 按下去连翻译的那一步都到不了,键躺在键盘的缓冲里,ping 打死也不知道你想喊停。这一章的活,就是把控制台从读的路上解下来,让它自己是活着的。

头一件咱们立泵。泵的身子就一个循环,咱们挂出来:

```cpp
void console_pump_entry() {
    for (;;) {
        poll_console_input();
        proc::Scheduler::self().yield();
    }
}
```

一具常驻的内核线程,名字取的是 tty_poll,权重给的是二百,大数是排在队尾的,点亮网卡那一卷立过的同款约定:泵是不跟人抢的,排队的末位,轮到了就摇一圈。摇什么,咱们接着抄:

```cpp
void poll_console_input() {
    int                signal = 0;
    unsigned long long group  = 0;
    {
        const auto kInput = ConsoleInput::self().guard();
        auto&      tty    = driver::ConsoleTty();
        tty.poll_input();
        signal = tty.take_pending_signal();
        group  = ConsoleForeground::self().get();
    }
    if (signal != 0 && group != 0) {
        proc::SendToGroup(group, signal);
    }
}
```

咱们把三步连起来看:锁内摇终端收字,取走待送的信号,问的是前台组是谁。锁一放信号就送了组。为什么送组必须出了锁再办,这里有一句老话:送信号要碰调度器的锁,输入门是自旋的叶子,拿着叶子去够别人的锁,次序就乱了。表锁是叶子的道理,前一章咱们刚念过,这边是同款的。台前的组是谁,答案记在一个原子量里:控制台的前台组号,默认的是一组,壳起了作业控制谁在台前就记谁,Ctrl+C 永远落在台前那一组的头上。输入门是五张脸共用的:本读、泵摇、问有无、termios 的读、termios 的写,全都是从同一扇门进出的,泵与读者不会再踩到对方的字。

第二件是终端收字的预算与停点。咱们跟 poll_input 讲好的:一圈最多收六十四个字,收着收着要是见到了待送的信号,咱们就地就停。一记 Ctrl+C 已经是在门里候着的,再收下去就是把命令行里后头的水也灌进来,信号是排在队尾等的,客人倒是指望着它马上到。停下的这一下,信号是头等的事。翻译在喂字的路上早备好了:终端的行纪律开着 ISIG,收到的字进门问一句是不是控制字,signal_for 查的就是它:VINTR 的字节三翻成二号的 SIGINT,记进了待送,头一个记上了就不再盖。

第三件要说的是那个字节三从哪来。USB 键盘的报告头一个字节是修饰键:左右 Shift 占两位,左右 Ctrl 又占的是两位。咱们按着 Ctrl 敲一个字,换算是这么写的:

```cpp
if ((now[0] & kControlModifiers) != 0) {
    if (kGlyph >= 'a' && kGlyph <= 'z') {
        return static_cast<char>(kGlyph - 'a' + 1);
    }
    if (kGlyph >= '@' && kGlyph <= '_') {
        return static_cast<char>(kGlyph - '@');
    }
}
```

字母的位置,按着 Ctrl 就变成字母在字母表里的位次:c 是第三个,翻出来的就是字节三。@ 到 _ 的那一段,变成的是零到三十一,补齐控制字符的后半张表。字节三一路到了终端的行纪律,行纪律认出的是 VINTR,翻成了 SIGINT,送到前台组的手里,整条链到这里就合龙了:手指按下的是 Ctrl 与 c,USB 的报告进来,控制字节算了出来,终端翻成了信号,泵送进了组,壳圈里的 recvfrom 看见名册,答的是 EINTR,出了门,投递进了 handler,ping 收了摊,统计落在了屏上,提示符也回来了。实弹里这一幕咱们跑过全程:不限发数的 ping 敲下去,跟着的一声 Ctrl+C,面板上落的是 ping 的统计与提示符。

本读的脸咱们留着没动,还是非阻塞的老样子:收得着答字数,收不着的话答 would-block,带着信号的话答 interrupted,等的那一圈照旧住在门上,与 recvfrom 的圈是同一位老师教的。控制台从此是不靠人读着活的,泵在后台一圈一圈地摇,台前坐的是谁,谁的 Ctrl+C 都有人接。
