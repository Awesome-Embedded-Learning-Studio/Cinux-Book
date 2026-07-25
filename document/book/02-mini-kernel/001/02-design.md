---
title: 02 · 设计图:串口、kprintf、双轨测试
---

# 设计图:串口、kprintf、双轨测试

先看串口这一层。一个 UART(NS16550A)挂在一段连续的 I/O 端口上,基址 `0x3F8`,各寄存器按偏移区分:

```text
偏移   寄存器   读/写    用途
 0     RBR/THR  读/写    收/发缓冲(同一个偏移,靠读/写区分)
 1     IER      写       中断使能(我们关掉,轮询)
 2     FCR      写       FIFO 控制
 3     LCR      写       线路控制(8N1 = 0x03)
 4     MCR      写       Modem 控制(RTS+DTR = 0x03)
 5     LSR      读       线路状态(bit5=可发, bit0=可收)
```

发一个字符的流程就是死循环查 LSR 的 bit5(发送保持寄存器空了没),空了就往 THR(offset 0)写字节。收字符类似,查 bit0。

再看 kprintf 怎么把"格式化"和"输出"解耦。关键是模板加一个输出函数对象:

```text
vkprintf_impl<OutputFn>(putc, format, args)
   ├─ 遍历 format 串,遇 % 走格式化分支
   ├─ 数字/指针 → 调 format.cpp 的纯函数算出字符串
   └─ 每个字符 → 调 putc(c)            ← 输出门户在这里抽象掉
        ├─ kprintf:  putc = serial.putc   (打到 COM1)
        └─ kdebugf:  putc = debugcon_putc (打到 0xE9)
```

`OutputFn` 是个抽象:你给它一个"怎么吐一个字符"的函数,`vkprintf_impl` 只管把格式化好的字符逐个喂给它。于是同一套格式化逻辑,串口、debugcon、甚至以后接帧缓冲,都只是换个 `putc`。

而双轨测试的纽带,就是中间那个 `format.cpp`:

```text
              format.cpp(纯算法:format_decimal/hex/binary)
              ┌────────────────────┴────────────────────┐
        编进内核                                  编进 host 测试
   kprintf.cpp 调它                      test_kprintf_format.cpp 调它
   (走 serial 输出)                      (走 ASSERT_EQ 比对字符串)
        │                                          │
   mini_kernel(QEMU 跑)                     test_host(CTest 跑)
```

同一份 `format.cpp`,两个编译上下文:内核里它被 kprintf 调用输出到串口;host 上它被单元测试调用、结果拿去和期望字符串比对。算法只有一份,内核和测试不会各写各的。
