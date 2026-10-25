---
title: "03 · 遗言一站式"
description: "崩溃的收尾收成一张脸:调用方报自己的原因行,断言知道文件行号,异常知道向量与寄存器,然后都把走回溯的起点递给同一张脸。三段式:命名的回溯,当前任务带着用户世界的条数或内核主流程的自认,内存的家底。走链零分配,机器死于内存枯竭也得把话说完。断言与异常两路接线,AssertionFailed 里一条 movq 把 rbp 抓出来递过去。"
chapter: 20
order: 3
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - debugging
  - panic
---

# 遗言一站式

机器挺不住的方式有好几种,咱们不让它们各说各的话。断言碎了是一种,异常没人接了是一种,往后还会有别的。这一件活立的是一张统一的脸:谁崩了,谁就自己把原因的那一行报完,然后把话说下去的活交给它,收尾走的只有一个入口。名字咱们起得直白,取的就是 PanicDump。

原因行为什么不由 PanicDump 自己报,咱们把理由摆开。断言手里有的是文件名、行号、函数名,那是源码位置送来的现成话。异常手里有的是向量号和一排寄存器,那是现场第一手的实况。这些事实住在了知情者的手里,报起来的代价最便宜,硬搬进收尾的通用脸里反而要造一堆的参数。所以分工定成了:崩的是您,您报原因,报完了把走回溯的起点递过来。为什么连起点也要递?因为只有调用方知道,故事该从谁的栈上开讲:断言死在了检查的那个栈帧里,异常死在了故障的那一层,起点不一样了,回溯开走的位置也就不一样。

断言那一路的接线,咱们把代码原样抄来,多出来的就三行:

```cpp
void AssertionFailed(const char* k_message, std::source_location loc) {
    cinux::print::Println("[panic] %s:%u in %s: %s", loc.file_name(), loc.line(),
                          loc.function_name(), k_message);
    unsigned long long frame = 0;  // NOLINT(misc-const-correctness) written by the asm output
    __asm__ volatile("movq %%rbp, %0" : "=r"(frame));
    cinux::debug::PanicDump(frame);
}
```

原因的那一行还是老样子,动过的字一个都没有。从前的日子,打完了这行它就去停机,现在多出来的那一拍,是一条 movq 把 rbp 抓出来递了过去——回溯件的粮食就是帧指针,这个您在轮流当家那一卷穿护具的时候就认识了。异常那一路是同款的接法:报告员把向量、寄存器、缺页的判词报完了,从前自己调回溯再停机的两个调用,现在一并交给了 PanicDump,递过去的起点是故障帧的底。

脸收到了起点之后说三段话。头一段说的是回溯,逐帧地往外走,有名字簿的日子每帧报的是名字加深度,没簿子的日子报的是裸地址,两头用的是同一份代码。第二段报的是当时谁在跑,咱们把原样抄在下面:

```cpp
cinux::print::Println("[kern]  task pid=%u name=%s state=%s", current->pid,
                      current->name != nullptr ? current->name : "unnamed",
                      state_name(current->state));
cinux::print::Println("[kern]  vma regions=%u", current->vmas->region_count());
cinux::print::Println("[kern]  vma none (kernel-only)");
cinux::print::Println("[kern]  task none (kernel Main)");
```

有任务的,报出来的是号、名字、状态,状态的名册有五态,咱们念一遍:ready 和 running 与 blocked,后面挂着的还有 zombie 和 dead。任务带着用户世界的,紧跟在后面报的是登记册的条数。登记册说的就是代码里的 `current->vmas`——用户世界的一张张内存划分记在这本册子上,一条记的是一段,为此它添了一个数条数的口子。这个数是给看崩溃的人的,册上的划分报了几条,程序的本意就摊开到了哪一步。任务没有用户世界的,报的就是一句 kernel-only。没有任务的,咱们让它自认:跑着的只有内核的主流程——崩溃现场上头一件事,就是把当时的当家人对上号。第三段是内存的家底,咱们的原则是有什么就报什么,原样地抄在下面:

```cpp
cinux::print::Println("[kern]  mem pmm free=%u managed=%u heap free=%u top=%u",
                      pmm.free_page_count(), pmm.managed_page_count(), heap.free_bytes(),
                      heap.top());
```

物理的池子报的是空闲和总管,堆报的是空闲和高位,咱们有什么就报什么。三段说完了,机器就停了机。

三段长什么样,咱们端一张真的样张给您看。手法照轮流当家那一卷放实弹的路数交代:在内核主流程的面板打齐、调度类装好之后,咱们临时塞了一条 ud2 探针,开机采到了真串口,验完了撤得干干净净,出货的镜像里不含它。样张采的是收卷后的完整镜像,名字簿那时候已经装满了,回溯里的拼写您提前过了一眼,它们的来路是下一节的事。采下来的遗言,从原因行起逐字地抄录:

```text
[kern] exception #6 Invalid Opcode
[kern]  rip=FFFFFFFF8020060C cs=8 rflags=10282 rsp=FFFFFFFF806A8930 err=0
[kern]  trace:
[kern]   #0 _ZN6kernel4MainERKN5cinux4boot8BootInfoE+0x58C (at 0xFFFFFFFF8020060C)
[kern]   #1 KernelStart+0x109 (at 0xFFFFFFFF802127D9)
[kern]   #2 KernelEntry+0x2E (at 0xFFFFFFFF8020006E)
[kern]  task none (kernel Main)
[kern]  mem pmm free=31264 managed=32479 heap free=65488 top=18446603885977079808
```

这一屏咱们念三遍。头一遍看的是原因,咱们塞的六号异常,无效的指令,rip 落在探针的位置上。第二遍看回溯:#0 那串下划线开头的长名,是 kernel Main 的修饰名,名字簿照 nm 给的原始拼写一字未改,+0x58C 报的是咱们进了它多深,后面的两帧 KernelStart 与 KernelEntry,从塞针的地方一路指到了进门的头一条指令,这正是名字簿的本事。第三遍看家底:task none 自认跑着的只有内核主流程,mem 一行里的 pmm free=31264,跟面板上的同一个数对得上——探针塞在面板打完之后,数的口径也对。

这套话带了一个硬约束,咱们单独地点出来:三段里的每一段,都得在一个可能就是死于内存的机器上说完。所以走链的是不分配的,概览只读现成的计数,依赖收得干干净净——打印、布局的事实、手里已经有的数,再多一样咱们都不敢要。当年工程线给这一件立的话,咱们原样地搬过来当家法:崩在哪、调用链、内存态,一站式地看清。名字的来路,咱们下一节去接。
