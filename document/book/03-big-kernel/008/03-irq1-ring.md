---
title: 03 · IRQ1 handler、ring buffer 与回显接线
---

# IRQ1 handler、ring buffer 与回显接线

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
