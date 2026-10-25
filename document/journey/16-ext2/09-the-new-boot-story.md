---
title: "09 · 开机的新故事"
description: "主流程改名 boot task,PID 0 退进怠速,kernel_init PID 1 接班:挂卷、报面板、放 shell。文件世界搬进任务,因为块队列会睡,boot 栈上没人可调度。三段演示任务光荣退役,面板瘦三段。关机走 ACPI:EBDA 扫到表根,表里请出 FADT,PM1a 一笔 16 位写出去,断电前串口等空。"
chapter: 16
order: 9
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - storage
  - acpi
---

# 开机的新故事

到上一卷为止,开机是主流程的独角戏:走线、认盘、挂文件世界、放 shell 下楼,一条道走完。这一卷,咱们看着主流程把机器交了出去——交给自己孵的头一个任务。

```cpp
Println("[kern] boot task (pid 0) parks here, kernel_init (pid 1) takes over");
cinux::proc::InstallKernelSwitchSink();
cinux::proc::Scheduler::self().set_preemption(true);
cinux::init::StartInitThread();
for (;;) {
    cinux::proc::Scheduler::self().run_until_done();
    cinux::arch::EnableIrqAndHalt();
}
```

主流程从此叫 boot task,借来的称呼是 PID 0。它的余生就两行:调度器没活,睡。EnableIrqAndHalt,上一卷的睡姿,sti 和 hlt 挨着写,睡是原子的一步。开机那段一级一级爬的主流程,到这一拍成了整台机器上最闲的人——它的余生,就剩等中断拍醒。接机器的是 kernel_init,PID 1,同样是借来的称呼:挂文件世界,面板上报一行 root mounted,然后把 shell 扶下楼。PID 0 和 PID 1 都借得坦白:fork 还没有,信号还没有,wait 还没有,借的只是称呼和交接的姿势,这里咱们把话说直。

文件世界为什么非搬不可?咱们把毛病摆在块队列的章程上:队列任务上下文专用。没抢到值班位的任务,睡在自己请求的判决上——睡,得是调度器名册上有名的人,叫得醒。boot 主流程不是任务,名册上没有它:它要是攥着 mount 睡在队列上,机器头一步就僵在那儿,没人叫得醒。而 mount 的头一块 superblock,就得过队列。所以文件世界搬进 kernel_init——一个正经任务,睡得起,叫得醒。

旧故事里跟着主流程跑的三段演示任务,这一卷光荣退役:echo_a、spin_a,还有生产者消费者那一对,调度器的考卷它们跑了好几卷,答卷早收了。面板上三段收据退场,机器的开场白瘦了三段,验收那一章咱们对数。

随行的新家当还有一件:关机。poweroff 背后的路叫 ACPI。固件在内存里摆了一棵表树,咱们要找的,是从树根到电闸的一条线。找树根两趟:EBDA 的头 1 KiB 一趟,EBDA 是固件在内存头上留的一小块地,住址因机而异,正路是去 0x40E 读两字节——那儿存着 EBDA 的段址,BIOS 留的指路牌,咱们按它算出住处再扫。0xE0000 到 0xFFFFF 的固件窗一趟。两趟都是 16 字节一步,认的是 RSD PTR。这名字规范里占八个字节,咱们只比前七个。命中的,还得把前 20 个字节加起来,和是零才算数。树根指着一张表目录,顺着目录挨个验表,认出 FACP(江湖叫它 FADT),里头有 PM1a 控制寄存器的端口号。断电的动作,就是往那个端口写一个 16 位:

```cpp
void PowerOff(const Fadt* fadt) {
    if (fadt == nullptr) {
        return;
    }
    Out16(PortWrite16{.port = static_cast<uint16_t>(fadt->pm1a_control), .value = kS5Word});
}
```

> 如实交底:写出去的那个词叫 S5,ACPI 睡眠态里管断电的那一档。类型位摆在 10 号位,使能位摆在 13 号,这是 QEMU 固件家族的编码。真正的开发板上,S5 的词写在 DSDT 表的 \_S5 包里,得解析 AML 字节码才拿得到,那是以后的站要补的课。代码头顶的注释自己交代过这层,咱们照实说:这一行写出去,模拟器断电。真正的机器上是不是同一根电闸,没人担保。

断电之前还有一件小事,咱们顺手办了:console::Flush,等串口的发送空位到位,话音落尽,电闸才拉。屏幕上最后几个字,一个都不丢。

kernel_init 挂好卷,把 shell 放下楼。开机的新故事,咱们到这儿讲完。
