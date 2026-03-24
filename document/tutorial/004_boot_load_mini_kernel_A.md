# Bootloader 开发日记 004：在 Real Mode 读磁盘，比你想象中更"坑"

> 本篇对应 Cinux 开发路线图的第 4 步：Bootloader 在 real mode 完成磁盘读取

## 写在前面

这次要讲的两件事——E820 内存枚举和磁盘读取——听起来平平无奇，但实际上坑很多。最离谱的是：明明数据读进来了，AH 寄存器却报错。这种"看起来成功但没真正成功"的陷阱，让我浪费了不少调试时间。

先说结论：

1. **INT 13h 的 AH 寄存器不是最终判断依据，CF 标志才是**
2. **E820 调用成功不代表数据解析正确**
3. **Real mode 的段地址计算反直觉（segment << 4 + offset）**

---

## 一、为什么这两件事必须在 Real Mode 做？

我们现在的位置：Stage2 已经跑起来了，VESA 图形模式也设置好了，接下来要进入保护模式。

但在此之前，有两件事只能在 Real Mode 完成：

### 1.1 E820 内存枚举

BIOS 提供的 INT 0x15/E820 只能在 Real Mode 调用。进入保护模式后，你就没法再用 BIOS 中断了。

E820 返回的是系统内存布局：
- 哪些地址是可用的 RAM
- 哪些是保留的（MMIO、ACPI 等）
- 每个区域的基地址和长度

这些信息对内核的内存管理至关重要，必须在跳入保护模式之前采集好。

### 1.2 磁盘读取

虽然 INT 13h 的扩展读取（AH=0x42）在某些环境下可以用虚地址，但最稳妥的方式还是在 Real Mode 完成。

我们的 mini kernel 编译成一个 ELF 文件，写入磁盘的 LBA=16 位置。Bootloader 的任务就是把这块数据读进内存。

---

## 二、E820 内存枚举

### 2.1 调用约定

E820 的调用方式有点复杂：

```
输入：
    EAX = 0x0000E820（功能号）
    EBX = continuation value（首次调用为 0）
    ECX = buffer size（我们用 24）
    EDX = 'SMAP'（签名 0x534D4150）
    ES:DI = buffer 地址

输出：
    CF = 0 表示成功
    EAX = 'SMAP' 签名
    ECX = BIOS 实际写入的字节数
    ES:DI 指向的 buffer 被填充
    EBX = 下一次调用的 continuation value
```

EBX 是个"迭代器"：每次调用后 BIOS 会更新它，当 EBX=0 时表示枚举完成。

### 2.2 数据结构

每个 entry 24 字节：

```c
struct MemoryMapEntry {
    uint64_t base;    // 基地址（8 字节）
    uint64_t length;  // 长度（8 字节）
    uint32_t type;    // 类型（4 字节）：1=可用，2=保留，等
    uint32_t acpi;    // ACPI 扩展属性（4 字节，可选）
} __attribute__((packed));
```

我们把这些数据存放在物理地址 0x5000 的位置：

```
0x5000: uint32_t entry_count
0x5004: MemoryMapEntry entries[32]
```

### 2.3 完整代码实现

```assembly
# file: boot/common/boot.S

# ============================================================
# Constant definitions
# ============================================================
# E820 memory layout
.set E820_BUFFER_ADDR,          0x5000
.set E820_BUFFER_COUNT_ADDR,    0x5000
.set E820_BUFFER_ENTRIES_ADDR,  0x5004
.set E820_MAX_ENTRIES,          32
.set E820_ENTRY_SIZE,           24

# Pre-calculated segment/offset values
# Real mode: physical = segment << 4 + offset
# For physical 0x5000: segment = 0x0500 (0x0500 << 4 = 0x5000)
.set E820_COUNT_SEG,            0x0500      # 0x0500 << 4 = 0x5000
.set E820_COUNT_OFF,            0x0000
.set E820_ENTRIES_SEG,          0x0500      # 0x0500 << 4 = 0x5000
.set E820_ENTRIES_OFF,          0x0004      # 0x5000 + 4 = 0x5004

# MemoryMapEntry structure offsets (24 bytes/entry)
.set MEM_MAP_BASE,              0
.set MEM_MAP_LENGTH,            8
.set MEM_MAP_TYPE,              16
.set MEM_MAP_SIZE,              24

# E820 BIOS call constants
.set E820_SIGNATURE,            0x534D4150  # 'SMAP'
.set E820_CMD,                  0x0000E820  # E820 function ID
.set E820_MAX_BIOS_SIZE,        20          # Max entry size BIOS returns

# ============================================================
# Function: query_memory_map
# Responsibility: Execute E820 memory enumeration
# Input: None
# Output: %cx = number of memory map entries
# ============================================================
.global query_memory_map
.type query_memory_map, @function
query_memory_map:
    pushaw
    pushw %es
    pushw %ds

    # 初始化 entry_count = 0
    movw $0x0, %ax
    movw $E820_COUNT_SEG, %dx
    movw %dx, %ds
    movw %ax, E820_COUNT_OFF     # count = 0

    # 设置 ES:DI 指向 entries 数组
    movw $E820_ENTRIES_SEG, %ax
    movw %ax, %es
    movw $E820_ENTRIES_OFF, %di

    # EBX = 0（首次调用）
    xorl %ebx, %ebx

.e820_loop:
    # 检查是否超过最大 entry 数量
    movl E820_COUNT_OFF, %eax
    cmpl $E820_MAX_ENTRIES, %eax
    jae .e820_done

    # 准备 E820 调用参数
    movl $E820_SIGNATURE, %edx       # EDX = 'SMAP'
    movl $E820_CMD, %eax             # EAX = 0x0000E820
    movl $E820_ENTRY_SIZE, %ecx      # ECX = 24（buffer size）

    int $0x15

    # ===== 关键错误检查 =====
    jc .e820_failed              # CF=1 表示失败

    cmpl $E820_SIGNATURE, %eax   # 验证签名
    jne .e820_failed

    # BIOS 可能返回 <24 字节，但至少要有 20
    cmpl $20, %ecx
    jb .e820_failed

    # 移动到下一个 entry
    addl $E820_ENTRY_SIZE, %edi  # DI += 24

    # 增加 entry_count
    movl E820_COUNT_OFF, %eax
    incl %eax
    movl %eax, E820_COUNT_OFF

    # 检查是否继续迭代
    testl %ebx, %ebx             # EBX=0 表示完成
    jnz .e820_loop

.e820_done:
    # 返回 entry count
    movl E820_COUNT_OFF, %eax
    movl %eax, %ecx

    popw %ds
    popw %es
    popaw
    ret

.e820_failed:
    popw %ds
    popw %es
    popaw

    movw $msg_e820_failed, %si
    jmp panic
```

### 2.4 关键点讲解

**段地址计算的坑**

Real mode 的物理地址计算公式是：`physical = segment << 4 + offset`

比如我们要访问物理地址 0x5000：
- 段地址不能直接用 0x5000（那会变成 0x50000）
- 正确做法：0x5000 >> 4 = 0x0500 作为段地址
- 然后 0x0500 << 4 + 0x0000 = 0x5000

这就是为什么代码里用 `E820_COUNT_SEG = 0x0500` 而不是 0x5000。

**签名验证的重要性**

有些文档告诉你只检查 CF 标志就够了，但实际上：
- CF=0 只表示 INT 调用没崩溃
- 你还需要检查 EAX 是否返回 'SMAP' 签名
- 最后还要验证 ECX >= 20（BIOS 至少要写 20 字节）

三管齐下才能保证数据有效。

---

## 三、磁盘读取

### 3.1 INT 13h 扩展读取（AH=0x42）

传统的 CHS 寻址已经被淘汰了，我们用扩展读取（LBA 方式）：

```
输入：
    AH = 0x42（扩展读取功能号）
    DL = 驱动器号（0x80 是第一块硬盘）
    DS:SI = DAP（Disk Address Packet）地址

DAP 结构（16 字节）：
    +0:  Size（1 字节，固定 16）
    +1:  Reserved（1 字节，固定 0）
    +2:  Count（2 字节，扇区数）
    +4:  Buffer offset（2 字节）
    +6:  Buffer segment（2 字节）
    +8:  LBA（8 字节，起始扇区号）

输出：
    CF = 0 表示成功
    AH = 0 表示成功
```

### 3.2 DAP 结构设计

我们把 DAP 放在低内存的固定位置 0x7B00（MBR 的 DAP 区域，MBR 执行完之后可以复用）：

```assembly
# DAP (Disk Address Packet) structure offsets (16 bytes)
.set DAP_SIZE,                0           # Packet size (bytes)
.set DAP_RESERVED,            1           # Reserved (always 0)
.set DAP_COUNT,               2           # Number of sectors to transfer
.set DAP_BUFFER_OFFSET,       4           # Transfer buffer offset (16-bit)
.set DAP_BUFFER_SEGMENT,      6           # Transfer buffer segment (16-bit)
.set DAP_LBA,                 8           # Starting LBA (64-bit)

# DAP fixed location in low memory
.set DAP_PHYS_ADDR,           0x7B00      # Fixed DAP location
.set DAP_SEGMENT,             0x07B0      # DAP segment = 0x7B00 >> 4
.set DAP_OFFSET,              0x0000      # DAP offset
```

### 3.3 完整代码实现

```assembly
# file: boot/common/boot.S

# ============================================================
# Disk read constants
# ============================================================
.set MINI_KERNEL_LBA,         16          # Mini kernel start LBA (sector 16)
.set KERNEL_LOAD_SEGMENT,     0x1000      # 0x10000 = 0x1000:0x0000
.set KERNEL_LOAD_OFFSET,      0x0000

.set DISK_READ_CMD,           0x42        # INT 0x13 AH=0x42 extended read

# ============================================================
# Function: load_kernel_from_disk
# Responsibility: Read kernel ELF header (4KB) from disk LBA=16 to 0x10000
# Input: None
# Output: %ax = number of sectors read (8), 0 means failure
# ============================================================
.global load_kernel_from_disk
.type load_kernel_from_disk, @function
load_kernel_from_disk:
    # 保存寄存器
    pushaw                               # 保存所有通用寄存器
    pushw %es                            # 保存 ES
    pushw %ds                            # 保存 DS

    # ===== Step 1: 设置 ES 指向 DAP 所在的段 =====
    movw $DAP_SEGMENT, %ax               # ES = 0x07B0
    movw %ax, %es

    # ===== Step 2: 在 0x7B00 处构建 DAP =====
    movw $DAP_OFFSET, %di                # DI = 0x0000

    # 填充 DAP.size = 16（DAP 结构大小）
    movb $16, %es:(%di)                  # set DAP size

    # 填充 DAP.count = 8（4KB = 8 扇区）
    movw $8, %es:2(%di)                  # set sector count to 8

    # ===== Step 3: 填充目标缓冲区地址 0x10000 =====
    # buffer = segment:offset 格式
    movw $0x0000, %es:4(%di)             # offset = 0x0000
    movw $0x1000, %es:6(%di)             # segment = 0x1000

    # ===== Step 4: 填充起始 LBA = 16 =====
    movl $MINI_KERNEL_LBA, %eax          # EAX = 16
    movl %eax, %es:8(%di)                # LBA low 32 bits
    xorl %eax, %eax                      # EAX = 0
    movl %eax, %es:12(%di)               # LBA high 32 bits

    # ===== Step 5: 调用 INT 0x13 AH=0x42 =====
    # DS:SI 必须指向 DAP（0x07B0:0x0000 = 物理 0x7B00）
    movw $DAP_SEGMENT, %dx               # DX = 0x07B0
    movw %dx, %ds                        # DS = 0x07B0
    movw $DAP_OFFSET, %si                # SI = 0x0000
    movb $0x80, %dl                      # DL = 0x80（第一块硬盘）
    movb $DISK_READ_CMD, %ah             # AH = 0x42
    int $0x13                            # 执行磁盘读取

    # ===== Step 6: 错误检查 =====
    # 【关键】CF=1 或 AH!=0 表示失败
    jc disk_read_failed                  # CF=1 表示失败
    cmpb $0, %ah                         # AH 应该为 0
    jne disk_read_failed                 # AH!=0 表示错误

    # ===== Step 7: 成功返回 =====
    popw %ds                             # 恢复 DS
    popw %es                             # 恢复 ES
    popaw                                # 恢复通用寄存器
    movw $8, %ax                         # 返回 8 扇区
    ret

disk_read_failed:
    # 失败处理：恢复寄存器然后跳转到 panic
    popw %ds
    popw %es
    popaw
    movw $(msg_disk_read_failed), %si
    jmp panic
```

### 3.4 那个"看起来成功但没成功"的坑

我最开始只检查了 AH 寄存器：

```assembly
# 错误的检查方式
cmpb $0, %ah
jne disk_read_failed
```

结果发现：即使 AH != 0，数据也已经正确读进来了！

查了半天才知道：**AH 寄存器在某些 BIOS 实现中不代表错误码，真正可靠的是 CF 标志位**。

正确的做法是：

```assembly
# 正确的检查方式
jc disk_read_failed      # 先检查 CF
cmpb $0, %ah             # 再检查 AH（防御性编程）
jne disk_read_failed
```

### 3.5 地址转换再提醒

DAP 里的缓冲区地址用的是 segment:offset 格式：

- 我们想把数据读到 0x10000
- segment = 0x1000，offset = 0x0000
- 物理地址 = 0x1000 << 4 + 0x0000 = 0x10000 ✓

如果直接写 segment = 0x10000 就错了，因为 16 位段寄存器存不下。

---

## 四、在 Stage2 中调用

两个函数都实现好了，在 stage2.S 中调用就行：

```assembly
# file: boot/stage2.S

.extern query_memory_map
.extern load_kernel_from_disk

# ...

_start:
    # ... VESA 初始化 ...

    # ===== 004_boot_load_mini_kernel_A: Real mode 内完成 =====
    call query_memory_map           # [→0x5000] E820 memory map
    call load_kernel_from_disk      # [→0x10000] Load mini kernel ELF

    cli                             # 再次关闭中断

    # ... 进入保护模式 ...
```

调用顺序很重要：先 E820 后磁盘读取，因为磁盘读取的代码需要内存（虽然我们只是把数据放到 0x10000）。

---

## 五、总结与感慨

到这里，Real Mode 的工作就告一段落了。

回过头看，这两个任务"听起来简单"但"做起来坑多"：

1. **E820**：看起来就是循环调用 INT 0x15，但签名验证、buffer 大小检查、段地址计算，每一步都能踩坑
2. **磁盘读取**：INT 13h AH=0x42 文档上写得清清楚楚，但 AH 寄存器的语义在不同 BIOS 上不一致，必须以 CF 为准

Real Mode 的段地址计算（segment << 4 + offset）真的很反直觉。我一开始总想直接用物理地址，结果总是访问错位置。后来把所有转换关系预先计算好（用 `.set` 定义常量），才避免了运行时的各种 BUG。

下一章，我们就要进入保护模式，然后是 Long Mode，最终完成完整的内核加载。但那又是另一场折腾了。

---

## 参考代码位置

- E820 实现：`/home/charliechen/cinux/boot/common/boot.S` (query_memory_map 函数)
- 磁盘读取实现：`/home/charliechen/cinux/boot/common/boot.S` (load_kernel_from_disk 函数)
- 调用位置：`/home/charliechen/cinux/boot/stage2.S` (_start 函数)
