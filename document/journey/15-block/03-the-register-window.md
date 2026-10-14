---
title: "03 · 推开寄存器的窗"
description: "BAR5 指进 AHCI 的寄存器窗:大页铺窗,禁缓存位把缓存里的旧数挡在门外。HbaMem 与 HbaPort 的布局拿 static_assert 钉死,0x100 起 32 个口、每口 0x80 字节。GHC 三拍:开 AHCI、落复位、等它自清,预算只诊断不放弃。挑端口看位图加 SSTS 低四位,只驱动一个。"
chapter: 15
order: 3
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - storage
  - ahci
---

# 推开寄存器的窗

设备醒了,可它的寄存器还隔着一条线。AHCI 控制器的家当——全局寄存器、三十来个口的私有寄存器——全铺在一片物理内存里,具体地址,就是上一节读来的六条 BAR 里第五条报的那个。咱们把窗开进页表:

```cpp
const auto kRegisters = static_cast<unsigned long>(BarBase(device->bars, kRegistersBar));
if (!mm::MapDeviceWindow(kRegisters, cinux::arch::page::kSize)) {
    print::Println("[kern] ahci register window refused to map");
    return false;
}
regs_ = cinux::base::PtrAt<HbaMem>(mm::IoremapVirt(kRegisters));
```

`MapDeviceWindow` 是给设备铺窗的专用手续:窗开在高半区的 ioremap 一带,按 2 MiB 大页落格,而且每格都带上禁缓存的位。为什么禁缓存这么要紧?您想,普通内存里的数,CPU 拿过一回就存一份抄本,下回读抄本,快。寄存器可不一样——它是对讲机的听筒,您要听的是设备此刻说的话。要是 CPU 把上一回的旧数当回音递过来,咱们对着空气就聊起来了。所以设备窗上的每个页格,都得明明白白告诉 CPU:此处不许缓存,读就真读。

窗推开了,里面的房间怎么认?AHCI 的版式,规范给得死死的:全局寄存器住 0x00 到 0xFF,从 0x100 起,排着 32 个口的私有寄存器,每口不多不少 0x80 字节。按字节对座的家当,咱们拿结构体直描,拿 static_assert 把座位表压进编译期:

```cpp
static_assert(sizeof(HbaMem) == 0x100 + (kPortCount * 0x80), "ports begin at 0x100");
static_assert(offsetof(HbaMem, ports) == 0x100, "the port array seats at 0x100");
static_assert(sizeof(HbaPort) == 0x80, "port register block is 0x80 bytes");
```

谁要是哪天手滑删了个字段、添了个字段,链接都过不去。规范里的偏移,一份都不许差——寄存器差一格,读到的就是邻居的门铃。这些口里跟本卷最相干的几间,咱们认认门牌。`PxIS`(0x10)是中断状态,发生了什么,咱们都在这儿读。`PxCMD`(0x18)是引擎的开关。`PxTFD`(0x20)报盘忙不忙、有没有出错。`PxCI`(0x38)记着哪些命令槽还压着没跑完。往后的章节,咱们一趟一趟回这几间屋。

进门第一件事,咱们把控制器调到 AHCI 的频道上。兼容的历史包袱让这类控制器开机时常常扮着别的角色,全局控制寄存器 GHC 里的 bit 31,亮出来就是 AHCI 的身份旗。起手三拍:

```cpp
regs_->global_host_control = kGhcAhciEnable.raw;
regs_->global_host_control = (kGhcAhciEnable | kGhcReset).raw;
wait_clear_observed(&regs_->global_host_control, kGhcReset, kResetBudgetTicks, "host reset");
regs_->global_host_control = kGhcAhciEnable.raw;
```

点亮 AHCI 位,落上复位位,然后等复位位自己清零——复位是控制器自己干的活,干完了它自己把旗放下来。等,是这一卷的新手艺,咱们给它配了个带预算的等法:

```cpp
while (!observe_once(reg, bits, false)) {
    report_stuck_once(reported, kDeadline, budget_ticks, what);
}
```

每一圈,咱们关着中断查一回位,没到就睡到下一回中断再来查。预算(复位给 100 拍,合 1 秒——时钟那一卷立的 100 Hz,在这儿正好用上)到点,面板报一声 stuck,句尾还带着 still waiting:预算的职责是诊断,不是放弃。规范担保会来的转变,咱们报了号照样等。真卡死的机器,这行字就是您在串口边能抓到的第一根线头。这一手是跟时钟那一卷学的:等,不空转。空转的机器,连报数的力气都没有。

控制器归位了,接下来挑口。32 个口,哪些真有,`ports_implemented` 寄存器的位图说了算。位图说有,咱们还得看口上真没真插着东西——SSTS 寄存器的低四位是设备探测的结果,读出 `0x3` 才算设备在、链路通。两个条件都满足的头一个口,就是咱们这一卷唯一的盘座:

```cpp
if (kPresent.raw != 0 && kStatus.extract(kPhyDetect) == kPhyDetectActive) {
    port_index_ = index;
    found       = true;
    break;
}
```

只驱动一个——这是这一卷给自己划的界:教学机器上就这一块盘,剩下三十一个口空着,咱们把界划在这儿。口挑定了,窗内的一切就绪。下一节,咱们把引擎发动起来。
