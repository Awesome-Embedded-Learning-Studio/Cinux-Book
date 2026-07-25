---
title: 02 · PS/2 控制器初始化:一板一眼的 init 序
---

# PS/2 控制器初始化:一板一眼的 init 序

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
