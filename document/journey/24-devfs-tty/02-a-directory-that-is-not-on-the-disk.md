---
title: "02 · 不在盘上的目录"
description: "设备文件是没有介质的文件。设备表八座,注册制而非出生制,null 与 zero 纯计算头一批住,console 接线另住一层,devfs 本体不碰内核 I/O。四条缝从 inode 类型一路补到 stat 的 rdev,挂载点在盘上预建空目录,ls / 从此见得到 dev 与 proc。"
chapter: 24
order: 2
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - devfs
  - filesystem
---

# 不在盘上的目录

设备文件是什么,咱们把话说穿:它是没有介质的文件。咱们 open 它、read 它、write 它,走的全是文件的门面,可名字底下没有任何字节是躺在盘上的,每一次的读写,干的活都是设备在现场干的。在 Linux 的世界里这靠 inode 类型里的一位与一对设备号撑着,咱们的树里,inode 的类型只有文件与目录两值,设备号这个词咱们更是连格子都没给过。所以这一篇的活是两层的:立一个后端,再把缝一条一条地补齐。

后端咱们起名叫 devfs,形状照的是 ramfs 的老样子,普通方法直接就满足了后端接口,不同之处全落在了节点上。设备表咱们立了八座,一座坐一行:名字、设备号、还有一张设备的脸。设备的脸咱们立成五张,用的是函数指针外加一个上下文词:

```cpp
struct DeviceOps {
    DeviceRead     read;       ///< Pulls bytes out of the device.
    DeviceWrite    write;      ///< Pushes bytes into the device.
    DeviceHasInput has_input;  ///< Optional: can a reader progress now.
    DeviceIoctl    ioctl;      ///< Optional: answer one device question.
    /// Optional: the last descriptor on this device has closed.
    void (*release)(void* context);
    void* context;  ///< The word every seat receives back.
};
```

上下文词这样的东西,咱们在后端接口的 pair 形状里见过同款:一份代码喂多个设备,靠的就是它。头两位是必填的,后三位可以空着:没有就绪脸的设备读作恒就绪,没有问话脸的答 ENOTTY,没有还座脸的什么都不必还。null 与 zero 两个纯计算设备头一批住了进来,咱们把 null 的两张脸抄来,您看看假文件的活有多实在:

```cpp
base::Result<unsigned long> null_read(void* /*context*/, void* /*sink*/,
                                       unsigned long /*count*/) {
    return 0;
}

base::Result<unsigned long> null_write(void* /*context*/, const void* /*source*/,
                                       unsigned long count) {
    return count;
}
```

读它给的是零,写它吞得干干净净的,zero 多的一手是把缓冲填满零,谁也不碰任何的硬件。console 是第一件碰真硬件的设备,咱们不让它住进 devfs 本体:read 接的是终端的行规程,write 逐字节地画屏,这些接线就单独住在了一层文件里,devfs 本体保持的是零内核 I/O,宿主的测试就能把它整个驱动起来。

注册这件事上咱们交过一笔返工的学费。头一版咱们把 null 与 zero 的注册写进了 devfs 的构造函数,机器当场就没起来:文件世界是咱们的单例,单例走的是 constinit 的链,所以构造必须是零动作,出生时干的活会把整条链撅断。同一部的家法,咱们已经是第三次领教了,前两回的学费也都交在出生动作上。改成注册制:新造的 devfs 是个空目录,入住的设备自己来注册,注册的活归的是接线层,在挂载上线之前就一口气办完了。

要补的缝有四条,咱们一条一条地补。inode 的类型添了第三值,指的就是字符设备。FsStat 的摘要添了第六字段 rdev,这一栏 ramfs 与 ext2 是不填的,默认给了零。mode 的词表添了一枚 020666,说的是字符设备的众可读写。目录念法的类型词添了第三枚,设备给的是 DT_CHR。开门迎客那一卷立的一百四十四字节 stat ABI 里,设备号的槽位本来就是留着的,如今 HandleStat 与 HandleFstat 把 rdev 搬了进去,客人的 stat 读到的就是真设备号。设备号咱们照 Linux 的原样编:null 是一比三,zero 给的是一比五,console 用的是五比一,拼的章法是设备类挪到高位,实例落在了低位。

挂载点的事,这一篇咱们当面来解。咱们的挂载不回写父盘的目录念法,ext2 的 /dev 目录原本连存在都不存在,咱们退一步说,就算它存在了,挂上去了之后 ls / 也看不见它。咱们把造盘的脚本改了,盘上预建了 /dev 与 /proc 两个空目录,再让文件世界在挂完了 ext2 根之后,顺手把 devfs 挂到了 /dev 上去。空目录是占位的牌子,真内容从挂上去的那一刻起由 devfs 应答。

实弹咱们跑标准盘,四场戏码逐字地念。头一场跑的是 ls dev,五条记录里 null、zero、console 各占了一条,多的一条没有,少的一条也没有。cat dev/null 干净地答了 EOF:零输出零报错。echo hi 重定向进了 dev/null,hi 就被吞得一点不剩了。末一场跑的也是 ls /,老住户之外多了 dev 与 proc 两个新门牌,占位目录立住了。宿主长凳添了一件,十六案点齐:枚举的次序、null 的 EOF、zero 的零填、stat 的两种形态、注册面的拒绝、挂载后的解析,全数过了。设备的目录立住了,可它眼下能接的真终端只有一个,那个单例还等着咱们去拆,下一篇就拆了它。
