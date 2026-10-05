---
title: 09 · 会听:验自己的伤,听世界的话
description: "上一卷立起了表,出了事会喊疼,可内核还是听不见的人:从进保护模式那会儿起,中断总闸就一直关着。本卷两件事:给内核一条自己验自己的路——测试内核在 QEMU 里开机,成绩用退出码亲口报给构建。再把电话线接进来——两片 8259 搬家,PIT 敲起 100Hz 心跳,一张中立的中断桌,末了 sti 一声,面板亮起第六行。"
chapter: 9
order: 0
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - interrupt
  - testing
---

# 09 · 会听:验自己的伤,听世界的话

上一卷收工的时候,内核把自己的表立了起来:描述表管住自家代码的模样,中断表立起了 256 个门,出了事会喊疼,六件遗言也备齐了。可您要是追问一句:它听得见吗?答案有点尴尬——听不见。从咱们进保护模式那一卷起,中断的总闸就一路关着,外设在门外把门都敲破了,内核偏偏一个字也收不着。会喊疼和听得见走的是两条道:一个是嗓子的方向,一个是耳朵的方向。这一卷咱们给内核治聋。

治之前还有一件更要紧的事:得让内核验得了自己的伤。本卷要动的是机器当场死给咱们看的手术——中断要是接错了,机器是说没就没的,最坏的时候连一句遗言都留不下来。咱们要在动刀之前铺好一条自己验自己的路:让一个专管测试的内核在 QEMU 里开一次机,亲口把成绩报给构建——报的是退出码,红绿由数字说了算,不再靠咱们的肉眼守着串口数。所以本卷的前一半,咱们根本不碰中断,办的全是测试的家务。家务有三样:零构造世界里的注册簿,打完就退场的出口,还有一步一步往上走的初始化阶梯。伤都验不了,手术刀是不敢动的。

本卷的后一半,咱们才接电话线。两片 8259 搬了家,十六条线让出 CPU 的异常区。PIT 敲起每秒一百拍的心跳,计数的事归服务、敲拍的活归设备,一张脸不认芯片的名字。十六个桩站进 32 号之后的空位,座位与门分了家,应答彻底内化进了服务里。末了 sti 一声,两道闸一齐开了,面板亮起了第六行——机器从关灯等死,翻成了睡着听世界。

<StationPanel tag="r09_intr_with_pic_pit" build="cmake -B build && cmake --build build --target run" />

<ChapterNav variant="sub">
  <ChapterLink num="1" href="01-out-of-reach" desc="host 的长凳测的是能搬上桌面的零件,IDT 装没装对、sti 之后活不活着,只有开机才知道。测内核的是另一个产品:自家的 Main、同一套核心源、两个二进制,两个世界共用的凭据是两张头文件">肚子里的事,桌面够不着</ChapterLink>
  <ChapterLink num="2" href="02-register-book-of-a-quiet-world" desc="静态构造在内核里是死路,用例记录摆进 .test_cases 链接段,链接脚本立围栏,KEEP 保命。每件用例打一行 [RUN] 再执行,死机时串口最后一行自报家门。围栏对齐差一档,8 字节的缝被当成用例跳进去——实弹教训整段复盘">零构造世界的注册簿</ChapterLink>
  <ChapterLink num="3" href="03-exit-with-a-verdict" desc="isa-debug-exit:往 0xF4 写一个字节,退出码由设备自己算成两倍加一。退出码的解码表、CTest 三层、注册表的名字与层、初始化阶梯——模块测试的世界等于该模块出生之前的世界。塞一件必败用例亲眼看它抓红,外加会疼的自证三件:sgdt、sidt、ud2 布防可恢复">打完就退场</ChapterLink>
  <ChapterLink num="4" href="04-sixteen-lines-move-house" desc="PC 的门房 PIC:BIOS 默认把 IRQ0 报成 0x08,CPU 当双重故障处理,设备一响就送命。每片五写,四道 ICW 加一道全捂的掩码,收成两张 PortWrite 表,主片 0x20 从片 0x28。EOI 纪律:忘了应答,线只响一次,机器活着,时间冻住">十六线搬家</ChapterLink>
  <ChapterLink num="5" href="05-two-identities-of-a-heartbeat" desc="计数与速率旋钮是服务 Tick,三写端口是设备 Pit,消费者只认前者。concept 的约束就一条“会 start”,类型擦除成函数指针加上下文指针对,Hertz 过接口所以带类型。host 侧一个假后端作证:会 start 的就能当后端,PIT 只是第一个">心跳的两种身份</ChapterLink>
  <ChapterLink num="6" href="06-a-neutral-interrupt-table" desc="十六个桩与异常桩同一个机制,IRQ 永不带错误码,单签名连分岔都省了。register_handler 只落座,enable_line 才开门。dispatch 跑完 handler 无条件应答,没人坐的线照答后丢弃。IrqLine 是词汇不是裸数字,芯片名锁在 arch,异常岛从两户扩到六户">一张中立的中断桌</ChapterLink>
  <ChapterLink num="7" href="07-open-the-gates" desc="remap、启动心跳、装桩、开 0 号线、sti——五步的次序一步不能乱,理由逐条给。芯片线闸加 CPU 总闸,与进保护模式那年的关中断纪律首尾对影。Halt 搬进 arch,hlt 从睡死翻成睡着等叫醒。内核侧两件测试:下界三拍,关门静默">开闸</ChapterLink>
  <ChapterLink num="8" href="08-audit-and-the-vow" desc="面板六行逐字对数,host 十五件,test_kernel 两个条目、每台零点三秒上下。考古箱:三月二十六的 3338 行里埋着双轨测试的志向,当天的提交说明只有一句修运行时崩溃。四月二十单日九站,本卷的祖站是当天头一站">验收,和立志的日子</ChapterLink>
</ChapterNav>

耳朵开了,心跳也响起来了。0 号线上跑的是定时器,下一卷轮到的键盘走 1 号线,一开口说的就是人话。其余的线还静着,各有各的日子,日子到了您自然认得出它们。机器合上了眼,胸口的拍子一下也没停。
