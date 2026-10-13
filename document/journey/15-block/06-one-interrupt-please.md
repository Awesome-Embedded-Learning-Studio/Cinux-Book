---
title: "06 · 认盘等一次中断"
description: "中断脚从 PCI 0x3C 拿,走 16 条传统线。挂接四层放行:注册、开线、口子许可、总闸放开。开机阶梯把 PCI 排在 sti 前、AHCI 排在 sti 后——认盘等一次中断,这是本卷的轴。裁决三段:室友的中断放行,四位致命报败,三个条件凑齐报成,末了清状态还回读一道。"
chapter: 15
order: 6
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - storage
  - interrupt
---

# 认盘等一次中断

引擎转着,信也填得出来了,可咱们还压着没发——因为发出去之后,咱们得等盘回话,而等回话这件事,得有中断才等得起。本节把中断的路铺通,卷首卖的关子也随之解开:您想想,开机阶梯为什么把 PCI 和 AHCI 卡在一道 sti 的两边?

中断脚是现成的。走线报身份的时候,咱们顺手就把 0x3C 那个 dword 里的中断线寄存器收进了表——固件装配机器的时候,就替设备把路线留好了,低字节一读,线号到手。线走的是老一套的 16 条传统线,中断服务那一卷立起的分发表,这儿直接挂名字:

```cpp
line_ = interrupt::IrqLine{.value = device->interrupt_line};
port.interrupt_status = kWatched.raw;
regs_->interrupt_status =
    (cinux::base::bit::MaskBit<uint32_t>(static_cast<unsigned char>(port_index_))).raw;
interrupt::Irq::self().register_handler(line_, irq_thunk);
interrupt::Irq::self().enable_line(line_);
port.interrupt_enable      = kWatched.raw;
regs_->global_host_control = (kGhcAhciEnable | kGhcInterruptEnable).raw;
```

挂接是四层放行,一层都不能省,咱们一层层开。头一层,把处理函数注册到线上。第二层,把线在芯片那头放开。第三层,口的 `PxIE` 置上要盯的那些位——设备这边许诺:这些事发生了,我拉线。第四层,GHC 里的总闸 `IE` 放开。注册前咱们还顺手清了两处状态,口的 PxIS 和全局的 IS,旧的中断残留不留到新差事里。四层全开,中断的路才算通。

现在,咱们可以说说那个次序了。开机阶梯里,PCI 的走线排在第 12 步,sti 开中断排第 13,AHCI 的认盘排第 14:

```cpp
{.name = "pci",
 .run  = +[](const BootInfo* info) -> const BootInfo* {
     driver::PciBus::self().init();
     return info;
 }},
{.name = "interrupts",
 .run  = +[](const BootInfo* info) -> const BootInfo* {
     asm volatile("sti" : : : "memory");
     return info;
 }},
{.name = "ahci",
 .run  = +[](const BootInfo* info) -> const BootInfo* {
     driver::Ahci::self().init();
     return info;
 }},
```

为什么这么排?走线用不着中断——配置空间的问答,问一声答一声,全是同步的。认盘可不一样:IDENTIFY 发出去,盘什么时候把 512 字节搬完,没人知道,咱们就把 CPU 睡在 `hlt` 上,等盘搬完了拉线,处理函数把判决发下来,睡觉的人才醒。认盘要等的就是这一次中断——发问的人,必须站在开中断的后面。这层意思,阶梯表自己的头注写得明明白白(`kernel/init/init_sequence.hpp`):表里的次序,就是机器上的次序。

线通到这儿,还剩半件事:线上一旦响了,处理函数怎么判断这声是不是咱们的?传统线是大家合用的,一个线上可能住着好几家设备。咱们的裁决分三段:

```cpp
const AhciReg kEvents{port.interrupt_status};
if (!kEvents.has(kWatched)) {
    return;
}
```

头一段,认领。口的 PxIS 里,要是咱们盯的那几位一个都没立,起立走人,一声不吭——线是合用的,室友的中断,咱们既不抢着应答,也不顺手吞掉,后头接手的自然处理。

```cpp
if (kEvents.has(kFatal)) {
    completion_.finish(false);
} else if (kEvents.has(kIsDeviceToHost | kIsPioSetup) && !kIssue.has(kSlotZero) &&
           (kTask & kTfdNotClean).raw == 0) {
    completion_.finish(true);
}
```

第二段,咱们判它败。四位里立了任何一位——主机总线数据错、主机总线致命错、任务文件那一侧报错、接口致命错——这一单判负,判决发出去。第三段,咱们判它成,数着三个条件,缺一不可:设备到主机的回执到了,或者 PIO 就绪的回执到了。`PxCI` 里 0 号槽的位已经落下(命令确实跑完了,没有下一道压着)。TFD 的四个状态位(BSY、DRQ、DF、ERR)个个干净,盘手里没有没收尾的活。三样凑齐,判胜。

判成判败,咱们收尾的手续一样:

```cpp
port.interrupt_status = (kEvents & kWatched).raw;
regs_->interrupt_status =
    (cinux::base::bit::MaskBit<uint32_t>(static_cast<unsigned char>(port_index_))).raw;
[[maybe_unused]] const AhciReg kEcho{regs_->interrupt_status};
```

咱们把自己盯过的位清掉(写 1 清零的老脾气),全局状态里自己口对应的那个位也清掉,末了回读一道——设备窗上的写,有时还在路上,回读这一下把它催到地方,也把这一趟收得干干净净。下一道命令再来,状态从零起算。

判决有了,线也通了,可咱们还没讲等判决这个动作本身。睡觉,这一卷给您备了新姿势。
