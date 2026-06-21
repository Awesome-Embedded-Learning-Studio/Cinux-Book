---
title: 014 · 听见键盘:第一个真正的输入设备
---

# 014 · 听见键盘:第一个真正的输入设备与 IRQ1

> 到 013 为止,内核有了「脸」——能在屏幕上画字,kprintf 也能双路输出了。但它只会说,不会听:你敲键盘,它一个字都收不到。这一章,我们把第一个真正的输入设备接上来。键盘是个 PS/2 设备,每按一个键,它通过 IRQ1 给 CPU 发一个中断,顺带把一个「扫描码」丢到端口 `0x60`。我们写一个驱动,把这个字节解码成「按了什么键、按下还是松开、当时 Shift/Ctrl/Alt 有没有按着」,塞进一个环形缓冲区,再让主循环把它取出来回显到屏幕。做完这一步,内核才第一次有了「交互」——你敲什么,它就显示什么。

## 这一章我们要点亮什么

一件最直观的事:敲键盘,屏幕上就出现对应的字符。`a` 键出 `a`,Shift+`a` 出 `A`,回车换行,退格往回抹。整个链路是:你按下键 → 键盘控制器发 IRQ1 → 我们的 handler 读扫描码、解码、入队 → 主循环从队列里取出来、回显到 013 搭好的 console。

这件小事背后,其实点亮了好几个「第一次」:

这是内核**第一个真正的输入设备**。之前所有的交互都是单向的——内核往外吐串口、吐屏幕。从这一章起,外面能往里塞东西了。

这是**第一个非时钟中断的真 handler**。回想 011:我们装了一整套 PIC + IDT,IRQ0(时钟)挂了真 handler,但 IRQ1 到 IRQ15 全是那个「发个 EOI 就把数据丢掉」的 default handler。键盘挂上 IRQ1,是这套中断体系第一次为一个真正的外部设备干活。

它还顺带示范了一个在内核里反复出现的结构:**中断里生产、主循环里消费**的环形缓冲区。中断 handler 要快、不能阻塞,所以它只负责「把数据收下来塞进队列」;真正怎么处理这些数据(回显、将来交给 shell),是主循环的事。这个「ISR 入队 + 轮询出队」的分工,后面还会在很多地方见到。

## 为什么现在需要它

先说为什么是现在。013 让屏幕能显示了,可一个只能显示、不能接收输入的系统,离「能用」还差得远。你能让它说话,却没法回应它。键盘是最自然的第一个输入设备——每个 PC 都有,PS/2 接口简单(不需要 USB 那套复杂的枚举),而且它的中断 IRQ1 我们在 011 装中断体系时就已经 remap 好了、给它留了 vector `0x21`,只是当时挂的是个空 handler。现在把真 handler 挂上去,水到渠成。

再往前看一步:键盘回显是后面所有用户态交互的预演。再过几个 tag,我们要做一个 shell,shell 的本质就是「读键盘输入 → 解析命令 → 输出结果」。而「读键盘输入」这一步,正是这一章搭好的 `Keyboard::poll`。所以这一章不仅是「让键盘响」,它还在给 shell 铺最底下那层路:一个能取出解码后按键事件的接口。

还有一笔小账要还。011 的时候,PIT 的 handler 里有一句每秒打印一次 `[TICK] uptime: Ns` 的输出,当时用它验证时钟在走。可现在键盘要回显了,如果每秒还有一行 `[TICK]` 刷进来,你敲键的回显就会被它搅得乱七八糟。所以这一章顺手把那句 tick 打印**删掉**了——它的历史使命(验证时钟)已经完成,现在该让位给真正的交互输出。这是个很典型的「调试代码用完就撤」的小动作。

## 设计图

整个键盘子系统,核心是一个生产者/消费者的环形缓冲区,夹在「中断」和「主循环」这两个异步的世界之间:

```text
   按键物理动作
        │
        ▼
   PS/2 键盘控制器 (i8042)
        │  把扫描码放进输出缓冲, 拉 IRQ1
        ▼
   ┌─────────────────────────────────────────┐
   │  CPU 收到中断, 跳到 irq1_stub (vector 0x21) │
   │  → keyboard_irq1_handler (C 桥)           │
   │  → Keyboard::irq1_handler(frame):         │  ← 中断上下文, 要快
   │      sc = io_inb(0x60)                    │
   │      解码 make/break + modifier + ASCII    │
   │      enqueue(KeyEvent)  ──────┐           │
   │      PIC::send_eoi(1)         │           │
   └───────────────────────────────┼───────────┘
                                   ▼
                          ┌─────────────────┐
                          │  ring buffer     │  ← 64 项, 满则丢
                          │  queue_[64]      │
                          └────────┬─────────┘
                                   ▼  主循环 (进程上下文)
   ┌─────────────────────────────────────────┐
   │  while (1) {                              │
   │      hlt;          // 睡到下一个中断      │
   │      while (Keyboard::poll(ev))           │  ← 慢慢消费
   │          if (ev.pressed && ev.ascii)       │
   │              console.putc(ev.ascii);      │
   │  }                                        │
   └─────────────────────────────────────────┘
```

PS/2 控制器只有两个端口要记:`0x60` 是数据口(读它拿扫描码、写它发命令数据),`0x64` 是状态/命令口(读它看状态、写它发控制器命令)。handler 每次从 `0x60` 读一个字节,这个字节就是全部的输入。

## 代码路线

### 先把 PS/2 控制器请起来:一板一眼的 init 序

PS/2 控制器(i8042)是个有状态的老硬件,不能上来就用,得按它认的仪式走一遍初始化。[keyboard.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/keyboard/keyboard.cpp) 的 `Keyboard::init()` 把这个仪式拆成了清楚的几步:

```cpp
// Step 1: 先关掉两个设备口, 免得配置过程中有数据捣乱
send_command(Ps2Cmd::DISABLE_PORT1);   // 0xAD
send_command(Ps2Cmd::DISABLE_PORT2);   // 0xA7

// Step 2: 排空输出缓冲里可能残留的旧数据
while ((io_inb(Ps2Port::STATUS) & Ps2Status::OUTPUT_FULL) != 0)
    io_inb(Ps2Port::DATA);

// Step 3-4: 读出当前 config, 改几位的设置, 再写回去
send_command(Ps2Cmd::READ_CONFIG);     // 0x20
uint8_t config = io_inb(Ps2Port::DATA);
config |= 0x01;    // bit0: 开第一口(键盘)的中断 → IRQ1
config &= ~0x02;   // bit1: 关第二口(鼠标)的中断 → IRQ12
config |= 0x40;    // bit6: 开「set2 → set1 翻译」  ← 关键, 下面专门讲
send_command(Ps2Cmd::WRITE_CONFIG);    // 0x60
io_outb(Ps2Port::DATA, config);

// Step 5: 让控制器自检, 期待它回 0x55
send_command(Ps2Cmd::SELF_TEST);       // 0xAA
uint8_t result = io_inb(Ps2Port::DATA);
// result == 0x55 → self-test passed

// Step 6: 重新打开第一口(键盘)
send_command(Ps2Cmd::ENABLE_PORT1);    // 0xAE
```

每一步都不能省,顺序也不能乱:不先 disable 就改 config,中途来的按键数据会和你的配置写撞车;不 flush 缓冲,读 config 时可能读到一个残留的扫描码而不是真正的 config 字节;不做 self-test,你没法确认这个控制器还活着。这种「一板一眼」是和老硬件打交道的常态——它们不报告错误,你只能靠固定的仪式保证它进入已知状态。

这几步里最值得展开的是 config 的 **bit6:「set2 → set1 翻译」**。PS/2 键盘默认发的是 scan code set 2,但我们整个驱动是按 set 1 写的(后面那张查找表就是 set 1 的)。为什么不直接处理 set 2?因为 set 1 有个极其舒服的性质:**按下一个键是一个字节、松开是「那个字节或上 0x80」**,判断按下还是松开只要看最高位。set 2 就没这么省心(它的 break 码是 `0xF0` 前缀加 make 码)。幸运的是,i8042 控制器硬件就自带一个「把 set 2 翻译成 set 1」的功能,只要 config bit6 置 1,控制器就会在把扫描码交给你之前自动翻译好。于是我们写的代码按 set 1 处理,实际从键盘到 CPU 中间被控制器悄悄转了一道。这个 bit 不开,你拿到的就是 set 2 的码,你的 set 1 查找表全对不上,出来的字符全是乱的——这是这一章最容易栽的坑之一,调试现场会再提。

发给控制器的每条命令,前面都套了一个 `send_command`,它的实现藏着和 PS/2 时序相关的小心思:

```cpp
void send_command(uint8_t cmd) {
    wait_input_empty();             // 等控制器把上一条命令消化完
    io_outb(Ps2Port::COMMAND, cmd);
}
void wait_input_empty() {
    uint32_t timeout = 100000;
    while ((io_inb(Ps2Port::STATUS) & Ps2Status::INPUT_FULL) != 0) {
        if (--timeout == 0) return;          // 兜底, 不能死等
        __asm__ volatile("pause");
    }
}
```

控制器处理命令需要时间,你连续往 `0x64` 灌命令,它来不及消化就会丢。`wait_input_empty` 读状态寄存器的 `INPUT_FULL` 位(bit1),等它清掉再发下一条。这里特意加了个 `timeout` 兜底——如果硬件卡住了(或者根本没接键盘),不能让内核在一个死循环里永远转下去,超时就认了、继续往下走。这种「轮询 + 超时兜底」是和慢速硬件握手的基本套路。

### IRQ1 handler:从一个字节到一个 KeyEvent

控制器就绪后,每按一次键,中断就会把控制权交给 `Keyboard::irq1_handler`。它的活是:从 `0x60` 把那个字节读出来,翻译成一个结构完整的 `KeyEvent`,塞进队列。

先看它产出的 `KeyEvent` 长什么样([keyboard.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/keyboard/keyboard.hpp)):

```cpp
struct KeyEvent {
    char    ascii;     // 对应的 ASCII 字符(不可打印则为 0)
    uint8_t scancode;  // 原始 set1 扫描码
    bool    pressed;   // true=按下(make), false=松开(break)
    bool    shift, ctrl, alt;  // 此刻这三个修饰键的状态
};
```

一个原始字节,被翻译成了「是什么键、按还是放、修饰键啥状态、有没有对应 ASCII」这么一个完整的事件。翻译过程:

```cpp
void Keyboard::irq1_handler(InterruptFrame* /*frame*/) {
    uint8_t sc = io_inb(Ps2Port::DATA);          // 从 0x60 读这一字节

    if (sc == ScanCode::EXTENDED) {              // 0xE0: 扩展键前缀
        PIC::send_eoi(1);
        return;                                  // 这版直接丢弃, 不处理方向键等
    }

    bool    pressed   = (sc & 0x80) == 0;        // bit7=0 是按下, bit7=1 是松开
    uint8_t make_code = sc & 0x7F;               // 去掉 bit7, 得到「是哪个键」

    // 跟踪修饰键(无论按下还是松开都要更新状态)
    if (make_code == ScanCode::LSHIFT || make_code == ScanCode::RSHIFT) shift_held_ = pressed;
    if (make_code == ScanCode::LCTRL)  ctrl_held_  = pressed;
    if (make_code == ScanCode::LALT)   alt_held_   = pressed;

    KeyEvent ev{};
    ev.scancode = sc;  ev.pressed = pressed;
    ev.shift = shift_held_;  ev.ctrl = ctrl_held_;  ev.alt = alt_held_;
    ev.ascii = 0;

    // 只有「按下」且码在表内, 才查 ASCII
    if (pressed && make_code < SCAN_TABLE_SIZE)
        ev.ascii = shift_held_ ? kScToUpper[make_code] : kScToLower[make_code];

    enqueue(ev);
    PIC::send_eoi(1);                            // ← 别忘了, 调试现场专门讲
}
```

几个关键判断。`(sc & 0x80) == 0` 区分按下和松开——这正是 set 1 的好处:松开码就是按下码或上 `0x80`,一个位运算搞定。`make_code = sc & 0x7F` 把那个区分用的 bit7 抹掉,剩下的就是「这是哪个键」的编号(比如 `0x1E` 是 `a` 键),这个编号才是去查表的索引。

修饰键的处理值得留意:Shift/Ctrl/Alt 是**持续按住**才有意义的,不是「按一下产生一个字符」。所以 handler 每次见到它们的 make/break 码,不产生 ASCII,而是更新三个全局状态位 `shift_held_/ctrl_held_/alt_held_`。之后任何普通键的事件里,`ev.shift` 这些字段反映的就是「按这个键的瞬间,修饰键是不是被按着」——这就是为什么 Shift+a 能出 `A`。

ASCII 翻译用的是两张数据驱动的查找表,而不是一串 `if/switch`:

```cpp
static constexpr char kScToLower[128] = { /* 0x1E 位置是 'a', 0x30 是 'b', ... */ };
static constexpr char kScToUpper[128] = { /* 0x1E 位置是 'A', 0x02 是 '!', ... */ };

ev.ascii = shift_held_ ? kScToUpper[make_code] : kScToLower[make_code];
```

把扫描码当数组下标,直接查出 ASCII——`make_code` 是 `0x1E` 就取数组第 `0x1E` 项,正好是 `'a'`(小写表)或 `'A'`(大写表)。两张表分别覆盖「没按 Shift」和「按了 Shift」两种情况(数字键 `1` 在大写表里就是 `!`,`2` 就是 `@`,以此类推)。用查找表而不是 switch,是因为键盘映射本质就是「键码 → 字符」的一张映射表,数组下标就是最快的查法,而且表是 `constexpr`,编译期就生成、进了 `.rodata`,运行时零开销。不可打印的键(功能键、方向键)在表里就是 `0`,自然就不会产生 ASCII。

### ring buffer:ISR 生产、主循环消费

handler 解码出 `KeyEvent` 之后,自己不回显——回显是慢活(I/O、画字),不能待在中断上下文里干。它只做一件事:`enqueue(ev)`,把事件塞进一个环形缓冲区,然后赶紧返回。慢活留给主循环。

这个 ring buffer 是经典的「定长数组 + 头尾两个游标」([keyboard.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/keyboard/keyboard.hpp) 里 `KEY_QUEUE_SIZE = 64`):

```cpp
void Keyboard::enqueue(const KeyEvent& ev) {
    uint32_t next = (tail_ + 1) % KEY_QUEUE_SIZE;
    if (next == head_) return;        // 满了 → 直接丢弃, 不覆盖
    queue_[tail_] = ev;
    tail_ = next;
}
bool Keyboard::poll(KeyEvent& out) {
    if (head_ == tail_) return false; // 空 → 返回 false
    out = queue_[head_];
    head_ = (head_ + 1) % KEY_QUEUE_SIZE;
    return true;
}
```

`head_` 是消费位置(主循环读到哪),`tail_` 是生产位置(handler 写到哪)。`head_ == tail_` 表示空;`(tail_+1) % size == head_` 表示满——注意这里**故意留了一个空位**不用,这是环形缓冲区判断「满」的标准技巧:如果不留空位,满的时候 `head_ == tail_`,就和「空」撞车了,分不清。留一个空位,满和空就有了不同的判据。满了怎么办?`enqueue` 选择**丢弃新事件**(不覆盖旧数据)——对键盘输入来说,丢一个「你敲太快」的键,比覆盖掉一个还没处理的键更合理。

这里有个设计取舍值得点一句:这个 buffer **没有任何锁**。在中断上下文写 `tail_`、在主循环读 `head_`,看起来是经典的竞态。为什么此刻敢不加锁?因为这是单核系统、而且 IRQ1 不会重入(一个 IRQ1 没处理完 EOI,CPU 不会再接受同级中断),「handler 在写」和「主循环在读」在时间上被天然隔开了——它们不会真的同时执行。等以后上了多核、或者允许中断嵌套,这个无锁假设就不成立了,到时候得加锁或用无锁环形队列。把「为什么现在不用锁」想清楚,比无脑加锁更值得。

### 把 handler 挂上 IRQ1,再把回显接上 console

驱动写好了,还得把它接进中断体系,并接到主循环。接线点在 [interrupts.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/interrupts.S),一行之差:

```asm
 ISR_NOERRCODE irq0_stub,  pit_irq0_handler     /* IRQ0(0x20): PIT Timer */
-ISR_NOERRCODE irq1_stub,  irq_default_handler   /* IRQ1(0x21): Keyboard */
+ISR_NOERRCODE irq1_stub,  keyboard_irq1_handler /* IRQ1(0x21): Keyboard */
```

`irq1_stub` 是 IDT 里 vector `0x21` 那一项指向的中断入口桩,它负责保存现场,然后调一个 C 函数。011 的时候它调的是什么都不干的 `irq_default_handler`;这一章把它换成了我们的 `keyboard_irq1_handler`(那个 `extern "C"` 的 C 桥,转调 `Keyboard::irq1_handler`)。注意这里 C 函数名得和汇编里写的一致,且要 `extern "C"` 避免 C++ 的名字修饰——这种 C/汇编边界上的符号匹配,错一个字符就是链接错误或跳到野地址。

光挂上 handler 还不够,得让 PIC **允许** IRQ1 通过,这就是 `main.cpp` 里的接线:

```cpp
Keyboard::init();            // Step 10: 初始化 PS/2 控制器

PIC::unmask(0);              // Step 11: 放行 IRQ0(时钟)
PIC::unmask(1);              //         放行 IRQ1(键盘) ← 011 时只有 IRQ0
__asm__ volatile("sti");     //         开中断

KeyEvent ev;                 // Step 12: 回显循环
while (1) {
    __asm__ volatile("hlt");              // 睡到下一个中断来
    while (Keyboard::poll(ev))            // 醒来后把队列里的事件全取出来
        if (ev.pressed && ev.ascii != 0)
            console.putc(ev.ascii);       // 回显到 013 的 console(串口+屏幕)
}
```

`PIC::unmask(1)` 是关键一差——011 的时候只 unmask 了 IRQ0,IRQ1 一直被屏蔽着,键盘中断根本到不了 CPU。现在把它也放行,键盘才会真正产生中断。注意 `unmask` 必须在 `Keyboard::init` 之后(控制器得先就绪)、在 `sti` 之前(先配好再开中断,免得配置过程中就来中断)。

主循环那个 `hlt` + `poll` 的结构,正是设计图里画的「中断生产、主循环消费」。`hlt` 让 CPU 睡到下一个中断(省电,也避免空转),中断把事件塞进队列后返回,`hlt` 醒来,`while (Keyboard::poll(ev))` 把队列里所有积压的事件一次性排空、回显。为什么用一个 `while` 而不是 `if`?因为你睡着的时候可能敲了好几个键,队列里积了好几个事件,醒来得一次取完,否则下次 `hlt` 前 `poll` 就漏掉了。

回显那一行 `console.putc(ev.ascii)` 把这一章和 013 缝了起来:键盘解码出的 ASCII,直接喂给 013 的 `Console::putc`,它自会画到 framebuffer、经 kprintf 的 sink 同步到串口。你看到字符同时出现在屏幕和串口,就是因为这条链接通了。至于 `ev.pressed && ev.ascii != 0` 的过滤:只回显「按下」(不回显松开,否则每个字符画两遍)且「有 ASCII」(功能键、Ctrl 组合不产生可见字符,不回显)的键。

## 调试现场

这一章没有 notes 文件,但键盘驱动有四个高频坑,几乎每个第一次写 PS/2 驱动的人都会撞上至少一个。

**第一个,也是最经典的:「按一下就哑了」。** 你按下第一个键,屏幕回显出来了;再按第二个,没反应;之后怎么按都没反应。十有八九是 handler 里漏了 `PIC::send_eoi(1)`。EOI(End-Of-Interrupt)是告诉 PIC「这个中断我处理完了,你可以再发下一个了」。不发 EOI,PIC 就认为这个 IRQ 还没处理完,再也不给你送同优先级及以下的中断——键盘(IRQ1)就此哑掉。注意 011 时就立过规矩:EOI 由 C handler 自己发,不是汇编 stub 发。所以 `irq1_handler` 的最后一行 `PIC::send_eoi(1)` 是命脉,漏了或写错了 IRQ 号(写成 `send_eoi(0)`),键盘就只响一下。遇到「按一下就没反应」,第一件事就是查 EOI。

**第二个:「字符全是乱的」。** 你按 `a` 出来的是别的字母,或者一堆乱码。这通常是 config 的 **bit6(set2→set1 翻译)没开**。前面说过,键盘默认发 set 2,我们的查找表是 set 1 的。bit6 没置位,你从 `0x60` 读到的就是 set 2 的码,拿它去查 set 1 的表,当然全错。验法是:在 `init` 改完 config 后,把那个 `config` 字节用 kprintf 打出来,确认 bit6(0x40)被置上了。值得一提的是,QEMU 的 PS/2 模拟在某些配置下默认行为和真机略有差异,所以这个 bit 在 QEMU 上可能「不开也碰巧能工作」,但搬到真机就乱——养成「显式置位、不依赖默认」的习惯,能少踩这种「QEMU 上好好的、真机上崩」的坑。

**第三个:扩展键(方向键、右 Ctrl/Alt 等)「没反应」。** 这些键的扫描码带 `0xE0` 前缀(比如方向键是 `0xE0 0x48`)。这一版的 handler 见到 `0xE0` 直接 `send_eoi` 丢弃了,根本没处理前缀后的那一字节。所以方向键、Home/End、右侧的 Ctrl/Alt 这一章都不可用——这是个**已知的有意限制**,不是 bug。把它说清楚:要支持扩展键,handler 得维护一个「上一个是 0xE0」的状态,把后续字节路由到另一张扩展键表。这一章为了把主线(普通键 + 修饰键)讲透,先不碰它。

**第四个:PS/2 时序导致「init 偶尔失败」。** 表现是 `init` 里的 self-test 偶尔读不到 `0x55`,或者配置写进去没生效。根因是命令发太快——控制器还没消化完上一条,下一条就来了。对策就是前面 `send_command` 里那个 `wait_input_empty`:每条命令前等 `INPUT_FULL` 清掉。如果碰到间歇性初始化失败,先检查每条控制器命令前有没有老老实实等输入缓冲空。另外,真机上某些老 8259/PIC 组合对连续 I/O 写有 timing 要求,可能还需要 `io_wait`(往 port `0x80` 写一字节制造约 1µs 延时)——这一章在 QEMU 上用不到,但搬真机时要心里有数。

## 验证

键盘驱动的验证,难点在于「按键」是个外部物理动作,不好自动化。Cinux 的解法是分两层:纯逻辑在 host 上用单测镜像测,真硬件交互在 QEMU 里用「注入扫描码」测。

纯逻辑(扫描码解码、修饰键状态机、ring buffer 算术)完全不依赖真键盘,所以 [test_keyboard.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_keyboard.cpp) 把这些逻辑镜像了一份,在 host 上 `-O2` 编、用 `CINUX_HOST_TEST` 门控,跑了四十多个 `TEST` 用例、近百条断言:

```cpp
TEST("keyboard: scancode 0x1E -> 'a' (lowercase)") { ... }
TEST("keyboard: scancode 0x1E + shift -> 'A' (uppercase)") { ... }
TEST("keyboard: break code 0x9E has make_code 0x1E") { ... }
TEST("keyboard: LShift press sets shift modifier") { ... }
// ... 还有 ring buffer 满/空/回卷的用例
```

它测的是「给我一个扫描码序列,解码出的 `KeyEvent` 对不对」「buffer 满了是不是真的丢」「空的时候 poll 是不是返回 false」这些纯数据变换。注意这是一种**镜像**测法——把 `keyboard.cpp` 里的查找表和状态机抄一份到测试里测,因为内核代码本身(带 `io_inb` 内联汇编、PIC 调用)在 host 上跑不起来;机内测 `kernel/test/test_keyboard.cpp` 才会真正跑内核代码(下一节讲它怎么注入扫描码)。跑它们:

```bash
ctest --test-dir build -R keyboard --output-on-failure
```

真硬件交互(IRQ1 真能触发、handler 真能从 `0x60` 读到码)就得在 QEMU 里测了。可自动测键盘,靠的是 QEMU 的 i8042 模拟支持一条巧妙的命令:`0xD2`(write to first PS/2 port output buffer)。测试代码往 `0x64` 写 `0xD2`,再往 `0x60` 写一个扫描码,QEMU 的模拟控制器就会把这个字节放进它的输出缓冲,handler 下一次 `io_inb(0x60)` 就会读到它——等于「假装你按了这个键」:

```cpp
void inject_scancode(uint8_t sc) {
    // 往 0x64 写 0xD2: 把下一字节塞进第一口输出缓冲
    io_outb(0x64, 0xD2);
    // 往 0x60 写扫描码: 下次 io_inb(0x60) 就读到它
    io_outb(0x60, sc);
}
// 然后: inject(0x1E); Keyboard::irq1_handler(nullptr);
//       Keyboard::poll(ev);  断言 ev.ascii == 'a'
```

于是 [test_keyboard.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_keyboard.cpp)(机内测)就能自动验证:注入 `0x1E` 后 `poll` 出来 `ascii=='a'`、注入 `0x9E`(break)出来 `pressed==false`、连续注入多个码按 FIFO 顺序出来、Shift 状态正确翻转。跑它用带测试钩子的内核:

```bash
cmake --build build --target run-big-kernel-test
```

这套「注入扫描码」的测法,把「键盘交互」这个看似只能手测的东西,变成了可自动回归的测试,是这一章工程上最值钱的一笔。

## 下一站

到这里,内核第一次「有问有答」了:你敲键盘,它回显;屏幕和串口同步显示。第一个真正的输入设备接上了,中断体系第一次为一个外部设备好好干了活,「ISR 生产 + 主循环消费」的环形缓冲区也立起来了。这些是后面 shell、用户态交互的地基。

但你会察觉到一个明显的短板:这台机器至今**没有内存管理**。所有东西都跑在 bootloader 给的那套固定页表上,内核想分配一页内存、想给将来的进程分独立的地址空间,都没有机制——我们连「哪些物理页可用、哪些被占了」都还没记录。键盘、屏幕这些外设都点亮了,可内核最该管的「内存」,还是一片没开垦的荒地。

下一站,我们就动这块。要给物理内存建立账本,记录每一个页框的占用状态,提供一个「给我分配一页」「我还你一页」的接口。那是物理内存管理器(PMM)的活,也是整个内存子系统的第一块基石。不过那是下一章的事了,我们先享受一下「敲键能回显」这个小里程碑。

---

### 参考

- OSDev — [PS/2 Keyboard](https://wiki.osdev.org/PS/2_Keyboard):scan code set 1 的 make/break 编码(bit7 区分)、`0x60` 数据口与 `0x64` 状态/命令口、set 1 与 set 2 的差异。本章的扫描码解码以此为准。
- OSDev — [8042 PS/2 Controller](https://wiki.osdev.org/I8042_PS/2_Controller):控制器 config 字节各位含义(bit0 第一口中断、bit1 第二口中断、bit6 set2→set1 翻译)、self-test 命令 `0xAA` 期待 `0x55`、命令 `0xD2`(写第一口输出缓冲,本章测试用它注入扫描码)、初始化仪式。
- 011 章 · [big kernel PIC/PIT](011-big-kernel-pic-pit.md):PIC remap、IRQ0-15 → vector 0x20-0x2F、EOI 由 C handler 发送的规矩。本章 IRQ1 接线建立在那套中断体系之上。
- 本 tag 源码:[keyboard.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/keyboard/keyboard.hpp) / [keyboard.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/keyboard/keyboard.cpp)、[interrupts.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/interrupts.S)(`irq1_stub` 改接)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(Step 10-12 回显循环)、[pit.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/pit/pit.cpp)(删 `[TICK]` 噪声);测试 [test_keyboard.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_keyboard.cpp)(host 镜像)、[test_keyboard.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_keyboard.cpp)(QEMU `0xD2` 注入)。
