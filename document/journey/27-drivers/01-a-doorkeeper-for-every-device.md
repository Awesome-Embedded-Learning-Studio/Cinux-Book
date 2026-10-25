---
title: "01 · 给每台设备立门房"
description: "散件盘点:注册各自为政,内核说不出自己有几台设备。门房照生产内核的对象系统思路搭:名字是身份,册类归生产,总线与驱动各记一笔名。门房头一批三样今天就有消费者,父子与挂树登记缓办,理由是现在无人消费,不是永不做。"
chapter: 27
order: 1
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - device-model
---

# 给每台设备立门房

咱们把自己数落一遍。机器长到了今天,进门当差的设备其实不少了:null 与 zero 是两个纯计算的,console 占了一台,ptmx 一开就铸出了一对,盘那边的一台控制器带一块盘,加上门口的键盘与串口,咱们两只手数得过来,可它们偏偏谁也不认识谁。注册的路数各开各的门:有的直接找设备文件系统要座位,有的压根不露面,盘那一摊倒是自成一国,块层认下了它,名册上却没有它的名字。你要是问内核一句,你管着几台设备,各自走的是哪条总线、归哪个驱动管,你问的这几样它一样也答不上来。散件的日子在机器小的时候过得下去,可 USB 马上就要进门了,门后头是一串还没影的设备:键盘、平板、往后还有存储与 hub。没有一座统一的门房,每来一位陌生的客人,咱们就得多开一扇直通的门,开的门越多,机器上住着的是谁、走的是哪条线、归哪个驱动管,咱们自己就越没有一处能查的地方。

所以这一卷的头一件活,是给设备立起的一座门房。立成什么样,咱们把话说在前头:不立一个玩具。这门差事生产内核早就干过了,它们给每台设备都立了一个对象,对象带名字、带父子、挂在了一片树上,发现设备的是总线,认领设备的是驱动,这套东西撑起了热插拔、电源管理与那片对用户可见的设备树。咱们照这个思路搭,但是不照单全收,咱们按今天有没有消费者来裁。头一批立起来的是名字、册类、记名这三样。名字是设备的身份,而且全模型内不许重。册类说的是设备归哪本册子,下一章咱们细说。记名记的是两个格子,一条记它是从哪条总线来的,一条记是哪个驱动认领的它。名字、册类、记名凑齐了三样,一座能登记、能查重、能读出来的名册就算立住了:登记一次,后面的人按名查、按册数、按总线归堆。

登记的脸咱们一口气递齐,免得谁来回补件的麻烦。设备上门时递的是一份六字段的申请,册类、名字、身份号、脸面、总线名、驱动名各占申请的一格。身份号说的是主号与次号打进同一个数里的那个词,设备文件世界认的就是它,而网络设备压根没有号。申请在代码里的真身,咱们摆出来看:

```cpp
struct DeviceSeat {
    DeviceClass          klass = DeviceClass::kCharacter;  ///< The class it files under.
    std::string_view     name;                             ///< Its name, its devfs leaf too.
    unsigned long        number = 0;                       ///< Packed identity, zero for network.
    const fs::DeviceOps* ops    = nullptr;                 ///< Faces; required for character.
    std::string_view     bus;                              ///< The bus claiming the device.
    std::string_view     driver_name;                      ///< The driver claiming the device.
};
```

咱们挨个对上去:klass 管的是册类,name 管的是名字,number 管的是身份号,ops 管的是脸面,bus 与 driver_name 管的是记名的两格。承重的注释落在 ops 身上:脸面是借来的指针,申请人的操作表得活过整个开机的装配期,寿命上的这个假设,与设备文件系统落座一直以来的章法是同一条。服务端收件的样子长这样:

```cpp
base::Result<void> DeviceModel::seat(const DeviceSeat& ask) {
    if (ask.klass == DeviceClass::kCharacter && ask.ops == nullptr) {
        return base::KernelError::kInvalidArgument;
    }
    const base::Result<void> kChecked = check_seat(ask);
    if (!kChecked.ok()) {
        return kChecked;
    }
    if (ask.klass == DeviceClass::kCharacter) {
        base::safety::Check(devfs_ != nullptr,
                            "device model seated a character device before devfs was bound");
        const base::Result<void> kSeated = devfs_->register_device(ask.name, ask.number, *ask.ops);
        if (!kSeated.ok()) {
            return kSeated;
        }
    }
    return seat_record(ask);
}
```

check_seat 立着三道检查:名字的形状对不对(非空、不超长、不带斜杠,总线与驱动的名字也在限内),字符设备的脸带没带(函数开头那一道),有没有重名的。真正承重的次序在函数的尾巴上:字符设备打头走 register_device 去设备文件系统要座位,座位要是被拒了,函数当场就返回了,seat_record 一行都轮不到跑了,名册一笔也不会记的,名册里绝不住着文件世界里找不到的设备。两本册子说的必须是同一个世界,这是名册的诚实底线。容量也照咱们的老法子进配置头:全模型至多十六台,总线与驱动的名字至多十二字节,满了就诚实答满,咱们不悄悄挤掉谁。

申请递上来的时候,检查其实还立过一次功,咱们如实记下。键盘的那件设备,枚举在开机序列里早就跑完了,可入座的这件事必须等设备文件系统绑定之后,登记的活排到文件世界的装配段里去。头一版咱们把入座塞在了开机序列那一步,名册一查就出了岔子,字符设备要落座而文件世界还没绑上,当场把件退了回去,开机也跟着停在了那里。改了回来,枚举的事归开机,入座的事归装配,谁的前提变了谁让路,全数过了。

生产内核的那套对象系统里,还有两样咱们今天没立,咱们把理由摆在明处。一样是父子:设备挂在谁的名下,谁又是谁的父桥。另一样是挂树与那片对用户可见的大树,连着事件广播与引用计数的激活。没立的理由只有一条:今天没有一个消费者。机器上没有热插拔的用场,没有电源管理的活儿,也没有用户态的工具要遍历那棵树,所以现在立它就是给空气写代码。咱们在验收里把它们登记成缓办,缓办给了一条期限:第一个消费者进门那天,这两样就动了工。当年第一遍的工程线上,这一段就是这么走岔的:框架立了个空架子,设备进门走的还是自己的老门,架子成了摆设。重走的这一遍,咱们反过来,头一个立的是名册,客人跟着进门就是了,树怎么个长法,等枝子真的要伸出去再说。

门房立好了,名册还是空的。下一章咱们把三本册子一本一本开张,把今天已经在机器上当差的散件,一个个请进各自的名下。
