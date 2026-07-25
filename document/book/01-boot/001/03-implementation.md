---
title: 03 · 代码路线:从第一条指令到 framebuffer 存档
---

# 代码路线:从第一条指令到 framebuffer 存档

源码主要在四个文件:[mbr.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S)、[stage2.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/stage2.S)、[serial.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/serial.S),以及把它们组装起来的 [CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt)。我们从"CPU 上电后执行的第一条指令"一路讲到"Stage2 把 framebuffer 信息存好"。

### 1. CPU 一上电,世界从 0x7C00 开始

[mbr.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S) 的入口是这样的:

```asm
_start:
    ljmp $0, $real_start    # 远跳:CS=0,IP=real_start

real_start:
    cli
    xorw %ax, %ax
    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    cld
    movw $0x7000, %sp        # 栈
    sti
    movb %dl, boot_drive     # BIOS 把启动盘号放在 dl
    call load_stage2
    movw $msg_booting, %si
    call print_string_mbr
    ljmp $0x8000 >> 4, $0    # 远跳到 0x8000
```

第一条指令 `ljmp $0, $real_start` 看着多余,其实是在做**段归一化**。BIOS 跳到 MBR 时,`CS:IP` 理论上是 `0x0000:0x7C00`,但有些 BIOS 会用 `0x07C0:0x0000`——这两种表示指向同一个物理地址,但 `CS` 的值不一样。后面我们所有"按 `CS` 算地址"的操作都会受影响,所以干脆先一个远跳把 `CS` 强制钉成 `0`,从此进入一个确定的状态。

紧接着的 `cli`/设段/`cld`/设栈/`sti` 是实模式初始化的标准动作。这里有个**容易翻车的点**:必须先把段寄存器全部理顺、栈搭好,再 `sti` 开中断。栈没设好就允许中断,一个异步中断进来压栈,压到不可预期的地址,直接黑屏重启。

`movb %dl, boot_drive` 是个保命操作:BIOS 调用 MBR 前会把**启动盘的编号**放进 `dl`(硬盘通常是 `0x80`)。我们要读盘,就得告诉 BIOS 读哪块盘,所以必须趁早把这个 `dl` 存起来——后面 BIOS 中断随时可能把 `dl` 改掉。

### 2. 实模式地址模型:为什么 DS 必须等于 CS

实模式的地址翻译是 `物理地址 = 段寄存器 << 4 + 偏移`。也就是说,`DS:SI` 指向哪,完全取决于 `DS` 和 `SI` 两个值合起来的结果。

我们后面要用 `print_string` 打印一个字符串。字符串是这样定义的:

```asm
msg_booting:
    .asciz "Cinux Booting...\r\n"
```

这个标号 `msg_booting` 在链接后得到的是一个**偏移**。问题是:偏移要配上哪个段才能算对地址?

- 字符串的标号是跟着 `mbr.S` 一起链接的。MBR 的链接脚本是 `. = 0x7C00`(见下面 CMakeLists),所以 `msg_booting` 的链接地址是个 `0x7C00` 附近的值。
- 我们访问它用的是 `DS:SI`(BIOS 的 `lodsb` 默认用 `DS:SI`)。
- 因此**只有当 `DS` 指向和 `msg_booting` 同一个段基址时,`DS:SI` 才能正确读到字符串**。

如果 `CS` 被归一化成 `0`,而 `DS` 还是 BIOS 留下的某个乱七八糟的值,`DS:SI` 算出来的物理地址就完全不对——`lodsb` 读出来的是垃圾,打印出一串乱码,或者干脆什么也不显示。这就是把 `DS=ES=SS=CS` 全设成同一个值的根本原因:**让"标号算出来的偏移"和"访问用的段"对得上**。这一步省不得,省了就是一屏幕乱码。

### 3. 用 BIOS 读盘:INT 0x13 AH=0x42 与 DAP

`load_stage2` 是 MBR 最核心的活:让 BIOS 把 Stage2 从磁盘读到内存。它用的是 BIOS 的**扩展读**接口 `INT 0x13 AH=0x42`,参数通过一个叫 **DAP(Disk Address Packet)** 的 16 字节结构传递:

```asm
load_stage2:
    movw $0x7B00, %si            # si 指向 DAP

    movb $0x10, (%si)            # DAP.size      = 16
    movw $15, 2(%si)             # DAP.sectors   = 15
    movw $0x8000, 4(%si)         # DAP.offset    = 0x8000
    movw $0, 6(%si)              # DAP.segment   = 0
    movl $1, 8(%si)              # DAP.lba.low32 = 1   ← 从第 1 扇区开始
    movl $0, 12(%si)             # DAP.lba.high32= 0
    movb boot_drive, %dl         # 恢复启动盘号

    movw $0x4200, %ax            # AH=0x42 扩展读
    int $0x13
    jc disk_error
    ret
```

DAP 的布局是 BIOS 定死的,几个关键字段:

```text
偏移   字段        值          含义
0x00   size        0x10        结构大小(16 字节)
0x01   reserved    0
0x02   sectors     15          要读几个扇区
0x04   offset      0x8000      读到哪个内存偏移
0x06   segment     0x0000      读到哪个段(段:偏移 = 0:0x8000 = 物理 0x8000)
0x08   lba (64位)  1           从第几个扇区开始(LBA 编号,0 = MBR 自己)
```

`int $0x13` 之后看进位标志 `CF`:`CF=0` 成功,`CF=1` 失败跳 `disk_error`。读完,Stage2 就躺在 `0x8000` 了。

这里有个细节值得留意——**为什么不用更简单的 `AH=0x02`(老式 CHS 读)?** 因为老接口要你给"柱面/磁头/扇区"三个数,在软盘和某些老硬盘上才靠谱;`AH=0x42` 用的是 LBA(线性扇区号),不用关心磁盘几何,跨设备更稳。现代 bootloader 基本都走扩展读。

> 外部依据:Ralf Brown's Interrupt List 详细记录了 `INT 0x13 AH=0x42` 的 DAP 各字段含义与进位标志约定;OSDev 的 ATA in x86 RealMode (BIOS) 页对这套读盘流程有社区总结。

### 4. 为什么 MBR 自带 print_string_mbr,不复用 common

你可能注意到,[mbr.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/mbr.S) 里有一个自带的、极其精简的打印函数:

```asm
print_string_mbr:
    cld
._loop:
    lodsb               # 从 DS:SI 取一字节到 al,si++
    test %al, %al
    jz ._done
    mov $0x0E, %ah      # INT 0x10 AH=0x0E:teletype 输出
    int $0x10
    jmp ._loop
._done:
    ret
```

而 [serial.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/serial.S) 里明明有一个功能更全的 `print_string`(带寄存器保护)。为什么不直接复用?

因为 **MBR 只有 512 字节,而且必须链接成一个自包含的整体**。

这里有个真实的坑(见"调试现场"):如果把 `common/serial.S` 也链进 MBR,加上它那些 VESA、A20 函数,MBR 的 `.text` 很容易就超过 512 字节。而 **BIOS 只加载第 0 扇区的 512 字节**——超出的部分压根没被读进内存。你的代码里 `call print_string` 跳过去,跳到的是一段"还没加载"的内存,结果就是一次毫无头绪的死机或重启。

所以 Cinux 的取舍是:

- **MBR**:只链 `mbr.S`,连一个多余的函数都不带。需要打印时,用一个不 push、极省字节的 `print_string_mbr`。
- **Stage2**:把 `common/serial.S` 以对象库的形式链进来,享受功能完整的 `print_string`(带保护)。Stage2 没有 512 字节的死线。

这条"红线"在 [CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/CMakeLists.txt) 里看得一清二楚:

```cmake
add_executable(mbr mbr.S)                       # 只有 mbr.S
# ... 链接脚本 . = 0x7C00 ...

add_executable(stage2
    stage2.S
    $<TARGET_OBJECTS:boot_common>               # 含 common/serial.S
)
# ... 链接脚本 . = 0x0 ...
```

MBR 链接在 `0x7C00`(因为 BIOS 就把它放那),Stage2 链接在 `0x0`(因为它会被放在 `0x8000`,靠"段=0x800"来寻址,下面解释)。链接完用 `objcopy -O binary` 把 ELF 抽成裸二进制,`scripts/build_image.sh` 再把 MBR 写进扇区 0、Stage2 写进扇区 1,拼成 `cinux.img`。

### 5. Stage2:趁还在实模式,把 A20 和图形模式配好

`ljmp $0x8000 >> 4, $0` 这个远跳把 `CS` 设成 `0x800`、`IP` 设成 `0`,合起来物理地址正好是 `0x8000`,跳进 Stage2 的 `_start`。Stage2 第一件事还是理顺段——因为它链接在 `0x0`,得靠 `DS=CS=0x800` 才能让标号和访问对得上(这就是把 Stage2 链接地址设成 `0x0`、运行时把段设成 `0x800` 的配合):

```asm
_start:
    cli
    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw $0x900, %ax          # SS=0x900 → 栈基址物理 0x9000
    movw %ax, %ss
    movw $0xFFFE, %sp
    sti
    movw $(msg_stage2_ok), %si
    call print_string         # 用 common 里那个带保护的版本
    call enable_a20
    call vesa_get_controller_info
    call vesa_get_mode_info
    movw $(msg_mode_info_ok), %si
    call print_string
    call vesa_set_mode        # ← 屏幕在这里切到图形模式,文本没了
    call vesa_save_framebuffer_info
    cli
.halt_loop:
    hlt
    jmp .halt_loop
```

**开 A20** 是个历史包袱。早期 8086 的地址回绕 bug 在后来的 CPU 上被一条叫 A20 的地址线"修"着,很多机器开机时这条线是关的,导致访问高于 1MB 的地址会绕回 0。我们要进保护模式后迟早要碰高地址,所以趁还在实模式、BIOS 还能用,先用 `INT 0x15 AX=0x2401` 把它打开:

```asm
enable_a20:
    movw $0x2401, %ax
    int $0x15
    jc .a20_failed            # CF=1 失败
    ret
```

**VESA 配屏**是三步走,全靠 `INT 0x10` 的 VBE 子功能:

1. `AX=0x4F00`:拿控制器的整体信息,写到 `0x6000`(请求前要先在缓冲区开头写 `"VBE2"` 签名,BIOS 才会按 VBE 2.0+ 填)。
2. `AX=0x4F01` + `CX=0x0118`:拿某个具体模式(这里选 `0x118`)的详细信息,写到 `0x6200`。这里能读到物理地址、每行字节数(pitch)、分辨率。
3. `AX=0x4F02` + `BX=0x4118`:切到这个模式。`0x4118 = 0x118 | (1<<14)`,第 14 位表示"用线性 framebuffer"——我们要的就是一块平坦的显存,不要那种 bank-switching 的老古董。

三步里最重要的是**第 3 步之后的那次保存**:

```asm
vesa_save_framebuffer_info:
    # ES → 0x6200(BIOS 写的 ModeInfo),GS → 0x6400(我们的存档)
    movl %es:0x28(%di), %eax        # PhysBasePtr  → 0x6400+0  (物理地址)
    movl %eax, %gs:0(%di)
    movw %es:0x10(%di), %ax         # BytesPerScanLine → 0x6400+8 (pitch)
    movw %ax, %gs:8(%di)
    movw %es:0x12(%di), %ax         # XResolution → 0x6400+12
    movw %ax, %gs:12(%di)
    movw %es:0x14(%di), %ax         # YResolution → 0x6400+14
    movw %ax, %gs:14(%di)
    ret
```

这块 `0x6400` 的存档是留给**将来的内核**的:等内核进了保护/长模式,BIOS 没了,它想知道"显存在哪、多宽、每行多少字节",就只能靠我们现在替它存好的这份参数。所以我们老老实实把物理地址、pitch、宽、高抄下来,放进一个约定好的固定地址。

这里有个**别想当然**的点:源码注释把 `0x118` 标成 `1024x768x32`,但**真正的每像素位数(bpp)以 BIOS 返回的 ModeInfo 为准,不能假设**。pitch(`BytesPerScanLine`)就是用来兜这个底的——`1024 × 每像素字节数` 可能是 3072(24bpp)也可能是 4096(32bpp),算显存偏移时必须用读出来的 pitch,而不是自己拍脑袋乘个 4。这也是为什么我们不嫌麻烦、非要把 pitch 单独存下来的原因。

至于 `print_string` 为什么前面要 `push %ax / %bx / %si`——因为 **BIOS 中断不是普通函数,它会弄脏你的寄存器**。`INT 0x10` 调完,`ax/bx/si` 乃至 `DS/ES` 都可能被改掉。不保护的话,`print_string` 返回后,调用者手里的 `si` 已经不是原来的字符串指针了,下一个函数接着用,就炸。这是 [serial.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/serial.S) 里 `print_string` 比 MBR 版"啰嗦"的原因——MBR 版那是为了省字节,在"打印完就跳走"的简单场景下可以赌一把;通用场景必须保护。
