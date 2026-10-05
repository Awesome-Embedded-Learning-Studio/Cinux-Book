---
title: 04 · 让编译器写桩
description: "当年的 interrupts.S:两个宏加十五桩手搓,五百多行汇编——不带码的异常得伪补零对齐栈面。这一遍一行宏都不写:interrupt 属性生成整套桩,模板实例化把向量号烤进去,三十二桩用折叠表达式安装。"
chapter: 8
order: 4
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - exceptions
  - cpp
---

# 让编译器写桩

表铺好了,该往里装桩了。咱们得弄清桩是一份什么差事:异常来的时候,CPU 把现场往栈上一压就跳进了门里。桩要干的,是把寄存器们保护好、把现场递给正经的报告函数,报告完了再 iretq 回去。这活听着是不复杂的,麻烦在一个不齐整的地方:有的异常,CPU 在进门之前是会自己多压一个错误码的,有的又不压。可咱们公共的报告函数,想要的栈面是齐的。差了一个字,后头全错了位。

当年第一遍是怎么干的?咱们去考古箱里把 interrupts.S 翻出来:两个宏。ISR_NOERRCODE 是给不压码的异常用的,进了门就 pushq $0 伪补一个零,为的是把栈面找齐。ISR_ERRCODE 给压码的用,是不补的。然后十五个桩一个一个地手写、每个宏展开一回,凑出了五百多行的汇编。活是干完了的,可这段代码您读过就知道,密得像极了另一门语言,而它确实是另一门语言。

而这一遍,咱们一行宏都不写。靠的是 GCC 在 x86 上给的一个属性:interrupt。函数一标上了它,整套桩编译器包了——寄存器保存、栈面对齐、iretq 的返回,全不用咱们经手。咱们要写的只有两个模板:

```cpp
template <unsigned Vector>
__attribute__((interrupt)) void exception_no_code(InterruptFrame* frame) {
    cinux::arch::isr::ReportFault(Vector, *frame, 0);
}

template <unsigned Vector>
__attribute__((interrupt)) void exception_with_code(InterruptFrame*    frame,
                                                    unsigned long long error_code) {
    cinux::arch::isr::ReportFault(Vector, *frame, error_code);
}
```

双签名就是双形状:不带码的收一个 InterruptFrame*,带码的多一个 error_code。当年手搓的伪补零,如今是编译器的活,它对齐栈面的手艺,比咱们手稳。向量号怎么进桩?走模板参数。Vector 是编译期的常量,实例化 exception_with_code<13> 的时候,13 就烤进了函数体,ReportFault 收到的就是自家那个数。当年宏展开干的活,如今交给了模板实例化,还白送了一套类型检查。

哪些向量是带码的、哪些又不带?事实就记在一张表里:kVectorPushesErrorCode,三十二格的表,8、10 到 14、17、21、29、30 记的是真,其余记的是假。当年只认了六个——8,加上的是 10 到 14。咱们新认的这几个里,17 记的是对齐检查,21 记的是控制保护,29 和 30 是后来新添的两粒号,一粒归虚拟化的路数,一粒归安全异常的路数,连名表里它们的栏位还写着 Reserved。咱们把它们和 kExceptionCount 一起,收在 isr.cpp 的匿名空间里,判据还是当年的老判据:没有外部消费者的东西,是不进公告面的。

安装的活,咱们也交给了编译期。install_one 拿 if constexpr 查的是表,选的是形状,install_all 拿的是 make_integer_sequence,在编译期里把 0 到 31 的号码展开成一条折叠表达式,一口气就装完了:

```cpp
template <unsigned Vector>
void install_one() {
    if constexpr (kVectorPushesErrorCode[Vector]) {
        install(Vector, &exception_with_code<Vector>);
    } else {
        install(Vector, &exception_no_code<Vector>);
    }
}

template <unsigned... Vectors>
void install_all([[maybe_unused]] std::integer_sequence<unsigned, Vectors...> sequence) {
    (install_one<Vectors>(), ...);
}
```

make_integer_sequence 干的事,是把 0 到 31 展成一串模板的实参,后头那句折叠表达式替咱们干的,就是对每个号各调一次 install_one 的活。您对照着想一下:三十二行安装的排比,当年靠人手写的话,就是三十二行肉眼对错的苦活,如今它是类型系统里的一行。到了链接器眼里,这是三十二个不同的函数,各自身上烤着各自的向量号,跟宏展开的产物同形,只是到了这一回,形状选对了没、号码传对了没,全是编译期就有人把关的事了。每个桩进门的时候,EncodeGate 记下的选择子是 `0x08`,自己新表里的代码段。这里咱们暂且记下一句:`0x08` 在 boot 的旧表里是另一格的意思,次序上的讲究,留到起居重排的时候再细说。

isr.hpp 的公告面,拢共三样的家当。咱们头一样看 `InterruptFrame`,它装的是 CPU 压栈的五件:rip、cs、rflags、rsp、ss,通用寄存器是编译器在桩里保护的,不劳咱们经手。`ReportFault` 是报告的正主。`InstallExceptionStubs` 是装桩的开关。当年五百行汇编的活计,如今头文件里三个名字就装下了。

桩进了门,可桩只是传话的。传出去的话得有人接,接了还得说人话,下一节的活,就是咱们给疼配上名字。
