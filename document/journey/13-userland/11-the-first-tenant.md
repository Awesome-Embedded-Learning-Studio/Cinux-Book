---
title: "11 · 第一位常客"
description: "零件齐了,写常驻的客人:read_line 逐字回显加退格三字节序列,tokenize 就地切分,echo/help/yield/exit 四命令,纯 Write 直写零 libc 依赖——考古翻过当年 user/libc 的成分,printf 是调试副产品,shell 一次没调过,所以不立 libc 目录。Main 退役为看门的,键盘主权交给 ring3,help/echo/yield 三轮实弹全链闭合。"
chapter: 13
order: 11
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - userland
  - shell
---

# 第一位常客

上一节末了咱们说,睡与醒通了,零件齐了。接下来咱们把零件组装起来,写一个真的常驻程序。常驻的意思,咱们得说清楚。过客是办完一件事就退场的,常客是坐堂的——读一行、办一行、再读一行,循环是不设出口的,除非人家自己说了 exit。开机时上场的三段演示,协作的、抢占的、生产者消费者的,里头的客人全是办完即走的,常客倒是头一个。它对内核的意义,咱们看也在这儿:从此调度器的世界里住进了“别人”,一位不归内核差遣、只在柜台上开口要东西的居民。

咱们来看骨架。shell 的入口头一件事是报个到,然后您看它在 for 循环里打印提示符、读行、分词、派活。咱们把读行这一段单独请出来,因为它几乎是 shell 的一半:

```cpp
unsigned long read_line(char* line) {
    unsigned long pos = 0;
    while (pos + 1 < kMaxLine) {
        char glyph = 0;
        if (user::Read(0, &glyph, 1) != 1) {
            continue;
        }
        if (glyph == '\n') {
            put_text("\n");
            break;
        }
        if (glyph == '\b' || glyph == 0x7F) {
            if (pos > 0) {
                --pos;
                put_text("\b \b");
            }
            continue;
        }
        user::Write(1, &glyph, 1);
        line[pos] = glyph;
        ++pos;
    }
    line[pos] = '\0';
    return pos;
}
```

一个字一个字地讨,讨到的当场回显:上一节说过,整行读上来就没法做退格了。退格的三个字节 “\b \b” 值得您多看一眼:光标退一格,印一个空格把敲错的字盖掉再退回来。就这么三个字节,屏幕上那个错字才真的消失。行的上限是 128——读满或换行收尾,末尾补了零交出去。分词是就地切分:空格和制表符原地改成 '\0',切出来的“词”是一串指向原行内部的指针——一个字节不多拷,而词数封顶 16。这法子糙吗?糙。可它对:freestanding 的世界里没有 strtok,shell 的分词本来就只需要这么些。

写出来的命令一共有四件,咱们挨个过一遍。echo 把参数连成一串打了出来,词与词之间垫的是一个空格。help 报的是菜名,一行就报完了。yield 把 CPU 让了出去,再被请了回来——它其实不睡:就绪队列里只有它一个,让出去马上又把自己领了回来,连上下文切换都省了,可这一来一回走的是调度器的正门。真正睡着的活儿在 read 里,每敲一个字的工夫,任务就在等待队列里又躺了一趟。exit 走的是一句 syscall,任务排干了,Main 也收了队。敲了不认识的词,shell 是不装聋的——unknown command 加上那个词,原样打了回来。跟当年的命令集比,咱们把 clear 换成了 yield。清屏的演示放在哪一卷都验不着东西,让位与回来的往返,只有常驻的客人当场演得出。

接下来有一道边界得跟您说清楚,这个 shell 是零 libc 依赖的。咱们开考古箱翻了当年 user/libc 的成分,翻出来的东西一共是三样:syscall 包装、string 三函数,再加一个 155 行的 printf。咱们查证下来,当年的 echo、help、clear 三个命令全是 write_str 一句,而 printf 从头到尾没被 shell 调过一次。它其实是那年“回显正常、命令全失效”那次排查的副产品,调试的时候立过功,平时是闲的。所以这一遍咱们不立 libc 目录。留下的只有 user/api/syscall.hpp 里那几个内联包装,它们不领 libc 的名头,干的是 ABI 常数单源的活——编号、寄存器、负 errno,两个世界读的是同一张表。头文件连自己的编译目标都没有,整个文件都是内联的,随每个用户程序一起进了镜像,改了一处,两边就一起知道了。shell 自己手写的,只有量长度和比相等的各十来行。将来真 libc 进来的那一卷,它自带的是全套包装,头文件光荣退休了,而它讲的约定,咱们今天就已经约好了。

面板上三段演示的字照旧滚完,那是给调度器那一卷的收据。咱们等它滚到 [kern] launching shell in ring 3——正戏开锣,Main 要钻进它这辈子最后一段代码了:

```cpp
for (;;) {
    cinux::proc::Scheduler::self().run_until_done();
    asm volatile("hlt" : : : "memory");
}
```

咱们眼看着 Main 交出最后一件家当,从主持人退成看门的:队列空了就睡,键来了醒,把客人请了回来再睡。键盘的主权至此交割完毕:从内核的回显主循环到机内测试台兜底,到今天收键的是 ring3 的常客。这以后内核自己反倒没有收键的门路了,秩序本来就该是这样:设备的下游只有一个,回显的职责跟着读的人走,所以谁读谁回显。首跳时当验收员、跑了三亿空循环的客人也交了差,构建链的名额换给了 shell。机器从此有了常态,咱们管这个常态叫闲:prompt 落下,睡着的任务等下一记键。

实弹打给您看。shell 报到的那一行 cinux shell - type 'help' 从串口上来,提示符 cinux> 跟着落下了,机器安静了下来——安静,但是活着。咱们从 QEMU 的门口把键一个一个递进去,三轮往返的串口原文一字不落地摆在下面:

```
cinux> help
commands: echo <text...>, help, yield, exit
cinux> echo hi
hi
cinux> yield
yielded and back
cinux>
```

咱们把您敲下的第一个 h 放慢了看。主机把 h 递给了 QEMU,IRQ1 进来了,字符进了队列,信号量的 post 也到了,睡着的 shell 就被请回了就绪队列。Main 从 hlt 里醒了过来,run_until_done 把客人请上了 CPU,它在 read 里醒来的时候,把 h 拿到了手,回显完又睡了回去。您跟着往后数,后面的 e、l、p 每个字都把全链原样走一遍,回车的那一拍换行返回,分了词,派了活,help 报出了菜名,新的提示符接着落下。echo hi 的那一轮里,词与词之间的那个空格也一样是个键,它走的是同样的路,回显出来的就是一个空格,存进了行里,只是分词的时候被切掉了。yield 的那一轮最省话:syscall 进去,在调度器里打了个转,客人由 sysret 送了回来,串口上只添了一句 yielded and back。咱们盯着串口看完这三轮,命令循环是活的,常客也算正式开张了。

咱们收个尾。常客住下了,全链绿了——可它一睡着,差点跟着睡死的就是整台机器。这是本卷的最后一案。
