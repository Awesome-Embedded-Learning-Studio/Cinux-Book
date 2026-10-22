---
title: "03 · 一门纪律,多台终端"
description: "行规程剥掉单例,一台变成可以造的类:接线四座带上下文,一份纪律代码喂多台终端。构造零动作化配懒挂线,constinit 家法两连,首版函数内带构造的 static 翻车如实记。纯逻辑化之后,宿主第一次直测终端。"
chapter: 24
order: 3
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - tty
  - refactor
---

# 一门纪律,多台终端

单例的终端为什么非拆不可,设备的目录那一篇末尾咱们撂过话:那个单例还等着咱们去拆。道理就是伪终端成对地出生,每一对里要有一台自己的行规程,而单例连第一对都给不出。拆的思路咱们也定了:行规程这门纪律本身不含任何硬件,输入从哪来、回显往哪去、等待怎么让出 CPU,全是装配的事。那咱们把纪律留下,把管线拆了出去,交给了接线。

咱们给接线定的形状叫 TtyWiring:四座,再外加了一个上下文词:

```cpp
struct TtyWiring {
    /// The word every seat receives back, unchecked.
    void* context = nullptr;

    /// Pulls one input byte when one waits; false when none does.
    bool (*take_key)(void* context, char* out) = nullptr;

    /// Whether the input seat holds a byte right now, non-destructive.
    bool (*has_key)(void* context) = nullptr;

    /// Where the discipline's own echo characters go.
    void (*echo)(void* context, char key) = nullptr;

    /// How a reader with no input yet lets the rest of the world run.
    void (*wait)(void* context) = nullptr;
};
```

这个形状您眼熟:函数指针加上下文词,与后端接口的 pair 形状、设备脸的上下文词,是同一个思想的三次落地。一份纪律代码从此可以喂任意多台终端:控制台的实例把四座接到串口加键盘、屏幕回显与调度让出上,伪终端的从机把四座接到自家主机的字节环上,测试世界接的是四个假桩。termios 的记录与窗口的形状,也从单例的 globals 挪进了实例,一台带了一份,谁改谁的。

constinit 的家法在这一篇连考两回,咱们如实记。头一考是构造:内核自己的终端要住进函数内的 constinit 存储,构造就不能有任何运行期的步骤,所以咱们把 Tty 的默认构造做成零动作,出生的终端是安静的,wire 才把四座与出生的开关一次性交给它。第二考是懒挂:控制台的终端第一次被用到的时候,开机横幅早已把屏幕的行列定了型,窗口的形状要那时再问。咱们给具名的取用函数留了两个判据:没接线的现接上,窗口是零的现整形,运行期的 static 一个不剩。

首版翻车的经过也照实记档。头一版咱们在函数内写了带构造参数的 static 终端,syscall 的上下文里一声一般性保护异常。咱们头一个判读是 freestanding 的世界里没有 __cxa_guard,带构造的函数内 static 本就该是非法的,改零动作加 wire 无论如何都是对的。可咱们深挖下去,它其实只是第一嫌疑人,不是真凶:那一声异常另有更深的病根,埋在离终端很远的地方,下一篇咱们整整一篇讲它。判读后来翻了案,顺序咱们不颠倒,学费照样交了。

然后咱们做纯逻辑化。咱们把 console、键盘、串口、调度器这些名字从行规程的本体文件里全部搬了出去,搬进单独的控制台接线文件。搬完了之后,行规程的本体在宿主的世界里一眼可测,咱们不必开机就能喂键、读行、翻开关。宿主长凳从此添了终端一件,咱们从四案起步:喂一行收一行,翻下了 ICANON 裸字节直通,关了 ECHO 静默取键,行池的短读留余。机内的终端案子也跟着改了形:旧案与单例共居一室,新一律各造各的实例,上一卷清池的家务从结构上免了。单例的旧病根,连着它的病案一起清了。

纪律成了类,咱们却还没立开关。termios 的记录里,三位开关怎么生效、线格式到底几字节,这里头还有一桩咱们自己量错的层,下一篇咱们讲它。
