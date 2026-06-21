---
title: Lab 014 · 让内核听见键盘:PS/2 驱动与 IRQ1 回显
---

# Lab 014 · 让内核听见键盘:PS/2 驱动与 IRQ1 回显

> 配套章节:[014 · 听见键盘:第一个真正的输入设备](../../book/03-big-kernel/014-keyboard.md)。这一关给你目标和约束,不贴答案——尤其不贴那两张扫描码查找表,得你自己照着 set 1 编码填。

## 实验目标

让内核第一次能「听」:敲键盘,屏幕(和串口)同步回显。拆成几个能独立验证的子目标:

1. PS/2 控制器能初始化:按仪式 disable/flush/config/self-test/enable,自检回 `0x55`。
2. IRQ1 handler 能解码:从 `0x60` 读一个字节,翻译成「按下/松开 + 哪个键 + 修饰键状态 + ASCII」的 `KeyEvent`。
3. 有一个环形缓冲区:handler(中断上下文)往里生产,主循环(进程上下文)从里消费,满了优雅丢弃、空了返回 false。
4. 接线 + 回显:把 handler 挂到 vector `0x21`,放行 IRQ1,主循环把按键 ASCII 回显到 013 的 console。

做完这四条,内核就有了第一个输入设备,也是后面 shell 的最底层路。

## 前置条件

你得先过 Lab 013:屏幕能显示字符、console 能 `putc`、kprintf 双输出在工作。这一关的回显直接调 `console.putc`,屏幕不通就没法看效果。

另外,这一关建立在中断体系已就位(Lab 011 的成果):PIC 已 remap、IDT 里 vector `0x20`-`0x2F` 已注册、`irq1_stub` 当前挂的是 default handler。你要做的是把那个 default handler 换成真正的键盘 handler。

## 任务分解

**第一步:初始化 PS/2 控制器。** i8042 是个有状态的老硬件,得按它认的仪式来:先 disable 两个设备口(防止配置过程中来数据捣乱),排空输出缓冲(防止读 config 时读到残留字节),读出 config 字节、改几位、写回,做 self-test 期待 `0x55`,最后重新 enable 第一口。改 config 时注意三位:开第一口中断(IRQ1)、关第二口中断(IRQ12)、以及一个「把扫描码 set 2 翻译成 set 1」的位——这个位是关键,不开的话你拿到的码和查找表对不上,后面全乱。每条控制器命令前都要先等输入缓冲空(状态寄存器那位),并且加个超时兜底,别在硬件没响应时死转。

**第二步:写 IRQ1 handler,把一个字节翻译成 KeyEvent。** handler 从 `0x60` 读扫描码。先判断是不是扩展键前缀(`0xE0`)——这版可以见了就丢弃(发 EOI 返回),先只做普通键。然后一个位运算区分按下/松开(set 1 的性质:松开码就是按下码的最高位置 1),再去掉那个最高位得到「键编号」。修饰键(左右 Shift、Ctrl、Alt)要专门跟踪:见到它们的码不产生 ASCII,而是更新几个全局「按住」状态位。普通键才查 ASCII 表——按下且键编号在表内时,按修饰键状态查小写表或大写表。

**第三步:造环形缓冲区。** 一个定长数组(比如 64 项)加头尾两个游标。handler 解码完调 `enqueue` 把事件写进尾游标、主循环调 `poll` 从头游标读。判断「空」和「满」要想清楚:标准技巧是故意留一个空位不用,这样空(`head==tail`)和满(`(tail+1)%size==head`)判据不冲突。满了怎么办?对键盘来说,丢弃新事件比覆盖旧事件合理。

**第四步:接线 + 回显。** 把 `irq1_stub` 指向的 handler 从 default 换成你的键盘 handler(注意 C/汇编边界的符号匹配,用 `extern "C"` 避免 C++ 名字修饰)。main 里:先 `Keyboard::init()`,再 `PIC::unmask(1)` 放行 IRQ1(011 时只放了 IRQ0),再 `sti`。然后主循环改成 `hlt` 睡、醒来后用一个 `while(poll)` 把积压的事件全取出来,对「按下且有 ASCII」的事件调 `console.putc(ascii)`。顺手把 011 里 PIT handler 那句每秒一次的 `[TICK] uptime` 打印删掉,不然它会搅乱你的回显。

## 接口约束

你要实现出来的东西,对外长这样(职责和签名,不给实现、不给查找表):

- `Keyboard::init()`:静态。按仪式初始化 PS/2 控制器,清 buffer 和 modifier 状态。
- `Keyboard::irq1_handler(InterruptFrame*)`:静态。读 `0x60`、解码、`enqueue`、发 EOI。这是「要快」的中断上下文。
- `Keyboard::poll(KeyEvent& out)`:静态。出队一个事件,空返回 false。这是主循环用的消费接口。
- `KeyEvent { char ascii; uint8_t scancode; bool pressed; bool shift, ctrl, alt; }`:解码后的事件。
- 一个 `extern "C"` 的 C 桥函数,供汇编 stub 调用,转调 `irq1_handler`。

两张扫描码查找表(set1 → 小写 ASCII、set1 → 大写/Shift ASCII)得你自己照 scan code set 1 的编码填,这关不提供——填错一格,对应键就回显错字符。

## 验证步骤

纯逻辑(解码、状态机、buffer 算术)在 host 上镜像着测,不依赖键盘、不依赖 QEMU。把驱动里的查找表和状态机逻辑抄一份到测试里,用 `-O2` 编、`CINUX_HOST_TEST` 门控:

```bash
ctest --test-dir build -R keyboard --output-on-failure
```

建议覆盖:每个普通键的 set1 码 → 正确 ASCII(小写)、Shift + 码 → 大写/符号、make/break 判断(break 码 = make 码 | 0x80)、Shift/Ctrl/Alt 的按下置位/松开清位、ring buffer 的空/满/回卷。

真中断交互在 QEMU 里测,用 i8042 的 `0xD2` 命令「假装按键」:往 `0x64` 写 `0xD2`、再往 `0x60` 写扫描码,handler 下次 `io_inb(0x60)` 就读到它。这样能自动测「注入 `0x1E` 后 poll 出 `ascii=='a'`」「连续注入按 FIFO 出来」等:

```bash
cmake --build build --target run-big-kernel-test
```

想直接看回显效果,`make run` 起来,在 QEMU 窗口敲键,屏幕和串口应该同步出现你敲的字符(普通键 + Shift 大小写)。注意方向键等扩展键(`0xE0` 前缀)这版不回显,属正常。

## 常见故障

- **按一下键就没反应了**:handler 漏了 `PIC::send_eoi(1)`,或 EOI 的 IRQ 号写错。PIC 没收到 EOI 就不再送这个中断,键盘「只响一下」。第一件事查 EOI。
- **回显的字符全是乱的**:config 的 set2→set1 翻译位没开,你拿到 set 2 的码去查 set 1 的表。`init` 改完 config 后把它打出来,确认那位被置上。
- **make/break 分不清、松开也回显**:没正确用最高位区分按/放,或回显时没过滤 `pressed==false`。松开码是 make 码 | 0x80,回显只要 `pressed && ascii`。
- **修饰键状态不对,Shift 大小写乱**:modifier 跟踪写反了(松开该清位却置位),或查 ASCII 时没用当时的 shift 状态选表。
- **敲快了丢键**:ring buffer 满了被丢。检查满判据是不是 `(tail+1)%size==head`,以及 size 是否够大;这版「满则丢」是已知取舍。
- **C/汇编链接报错 undefined symbol**:handler 的 C 桥没用 `extern "C"`,被 C++ 名字修饰了,汇编里的符号对不上。
- **方向键等没反应**:这些是 `0xE0` 扩展键,这版 handler 见 `0xE0` 直接丢弃,是已知限制不是 bug。

## 通过标准

1. host 单测全绿:set1→ASCII(小写/Shift)、make/break 判断、modifier 按下/松开跟踪、ring buffer 空/满/回卷都对。
2. QEMU 机内测通过:用 `0xD2` 注入扫描码,decode 出的 `KeyEvent` 字段正确、多键按 FIFO 顺序、Shift 状态正确翻转。
3. 真回显成立:`make run` 后敲键,普通键和 Shift 大小写在屏幕和串口同步出现,松开不重复回显。
4. 中断规矩守住:handler 自己发 EOI;`Keyboard::init` 在 `unmask(1)` 之前、`unmask(1)` 在 `sti` 之前。

做到这四条,内核就有了第一个输入设备,「敲键→回显」的闭环成立。但你大概也发现了:这台机器还没有任何内存管理——想分配一页内存都没地方要。物理内存账本是下一关的开场。
