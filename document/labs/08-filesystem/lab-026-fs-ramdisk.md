---
title: Lab 026 · 内核第一次认识「文件」:嵌入式 initrd ramdisk
---

# Lab 026 · 内核第一次认识「文件」:嵌入式 initrd ramdisk

> 配套章节:[026 · 内核第一次认识「文件」:嵌入式 initrd ramdisk](../../book/08-filesystem/026-fs-ramdisk.md)。这一关给你目标和约束,不贴 ustar 头布局、不贴 mount 遍历、不贴 embed 流水线的具体命令组合。

## 实验目标

让内核第一次理解「文件」:解析一个嵌入式 ustar(就是 `tar`)归档,把里面的文件名和大小列出来。拆成三个子目标:

1. 能解析 ustar 头:读懂那 512 字节定长表的字段(name/size/typeflag/magic),把八进制 ASCII 的大小转成整数。
2. 能遍历归档:从头走到尾,认出文件和目录,正确跳过每个文件的数据块。
3. 能把归档嵌进内核:搭一条构建期流水线,让一份外部 `tar` 文件变成内核镜像里的一段数据,运行时靠链接器符号找到它。

做完这三条,内核就从「只认识扇区」走到了「认识文件」。注意边界:这一章只「列出」文件,**不**实现打开/读/写——那是下一关 VFS 的活。

## 前置条件

你得理解 ustar 归档是个什么格式:一串 512 字节的块,每个文件 = 一个 512 字节头 + 若干个 512 字节数据块(数据按 512 补齐)。数字字段(size/mode/uid…)是**八进制 ASCII**。这些是 POSIX.1-1988 定的,不懂的话先去查 ustar 头布局再动手。

构建这块要知道 GNU `objcopy` 能把任意二进制文件转成可链接的 ELF object(`-I binary`),以及链接器段(`section`)怎么控制一块数据的排布与加载。023 嵌入用户程序二进制用的也是同款手法,有印象会更顺。

## 任务分解

**第一步:摸清 ustar 头。** 定义一个 512 字节、`[[gnu::packed]]` 的头结构体,字段按规范偏移排(name@0、mode@100、size@124、typeflag@156、magic@257…),配一条 `static_assert(sizeof==512)` 卡死大小。想清楚为什么必须 packed(默认对齐会在字段间塞 padding,512 就对不上了)。把类型标志和 magic 串(`"ustar"`)定成常量。

**第二步:八进制转换。** 写一个把八进制 ASCII 串转成整数的函数。两个要点:用 `result<<3 + digit`(乘 8)逐位累加;**遇到 null 或空格就停**——ustar 字段是定长的,实际数值短的部分用前导 0 或空格填、结尾可能有 null,读满定长会把填充位也当数字。把 0、全空、带 null 截断、带空格截断这些边界想清楚。

**第三步:遍历归档。** `mount` 从归档基址开始,一个头一个头地走:先判断归档结束(遇到 `name[0]=='\0'` 的全零头就停),再校验 magic 是 `"ustar"`(不合法就停下,别硬解),然后按 typeflag 分类(普通文件/连续文件算文件、目录算目录)打印名字和大小,最后**跳过头 + 数据块**落到下一个头。数据块数是文件大小**向上取整**到 512(`(size+511)/512`,size==0 是 0 块)。这步的灵魂是「块数算对、终止条件对」,错一个块后面全错位。打印文件名时要带长度上限(定长 100 字节、未必有 null),别直接当 C 字符串 `%s` 打,会越界读到下一字段。

**第四步:embed 流水线。** 这一步让 `_binary_initrd_start/_end` 这两个符号真的指向归档。先用 `tar` 把几个测试文件打包成 `initrd.tar`。再用 `objcopy -I binary` 把它转成 ELF object,放进一个叫 `.initrd` 的自定义段——注意段的属性要带 `ALLOC,LOAD`,否则数据不进加载镜像、运行时读不到。然后链接器脚本里加一节 `.initrd`,4096 对齐,高半区内核记得用 `AT(ADDR - KERNEL_VMA)` 处理加载地址。最后有个坑:`objcopy -I binary` 生成的符号名是按输入文件**绝对路径**派生的(换机器/换目录就变),代码里没法写死,得用 `--redefine-sym` 统一改成稳定的 `_binary_initrd_start/end/size`。

## 接口约束

你要实现出来的东西,对外长这样(职责和签名,不给实现):

- `UstarHeader`:512 字节、packed 的头结构体,字段按 ustar 偏移。
- `octal_to_uint(const char* s, size_t len) -> uint64_t`:八进制 ASCII 转整数,null/space 截断。
- `Ramdisk::mount() -> uint32_t`:解析嵌入式归档,打印每个条目,返回找到的文件数。
- `Ramdisk::base() -> const void*` / `total_size() -> uint64_t`:归档基址和大小。

硬约束:

- **只读、不写**;`mount` 只列出文件名和大小,**不**填充按名查找用的条目结构(那是留给下一关的形状)。
- 数据按 512 字节块补齐,块数向上取整;归档结束以全零头(`name[0]=='\0'`)为准。
- 文件名打印带长度上限;数字字段一律按八进制解析。
- 归档是**构建期嵌入内核镜像**的,不是从磁盘读的——别在本章引入块设备/磁盘读取。

ustar 字段偏移、类型标志值、magic 串、objcopy 的命令组合、链接器段写法,都得你照规范/工具手册来定,这关不提供。

## 验证步骤

纯逻辑(头偏移、八进制、块数、在合成归档上跑遍历)在 host 上镜像测。自己造几个合成的 ustar 归档字节(单文件、带数据、目录、无效 magic、全零),把同样的遍历逻辑抄一份到测试里,`CINUX_HOST_TEST` 门控——因为内核 `mount` 读的是链接器符号指向的真数据,host 上没有那块数据,只能造假的:

```bash
ctest --test-dir build -R ramdisk --output-on-failure
```

建议覆盖:`UstarHeader` 512 字节且各字段偏移对;八进制解析的 0/全空/null 截断/space 截断;`data_blocks` 的 1B/512B/513B 边界;mount 在合成归档上(单文件+数据正确跳过、目录、无效 magic 停止、全零归档返回 0)。

真归档、真链接器符号,在 QEMU 里跑机内集成测试(归档基址非空、size>0、mount 返回 3):

```bash
cmake --build build --target run-kernel-test
```

跑完整内核看启动日志,验收点是那段 `[RAMDISK]` 输出:

```text
[RAMDISK]   FILE: hello.txt  (18 bytes)
[RAMDISK]   FILE: readme.txt  (23 bytes)
[RAMDISK]   DIR:  etc/
[RAMDISK]   FILE: etc/passwd  (11 bytes)
[RAMDISK] 3 file(s) found in initrd.
```

## 常见故障

- **归档里有 3 个文件,内核只认出 1 个甚至 0 个**:八进制读成了十进制(`*10` 而非 `<<3`),或把 size 字段当二进制整数直接读。size 偏大 → 块数爆表 → offset 飞出归档。先查八进制解析。
- **认不全文件,且 magic 校验报错**:数据块数没向上取整(`size/512` 下取整),或 size==0 没特判。offset 跳偏 512,下一个头落到数据中间,magic 不符、遍历断。
- **列出了一堆乱码文件名,甚至读到归档外头**:没做 magic 校验,或没判断 `name[0]=='\0'` 的结束。任何字节都被当合法头。两道闸都要有。
- **`base()` 非空、size 也对,但一个文件都列不出**:`.initrd` 段的属性漏了 `ALLOC,LOAD`,数据没进加载镜像,运行时读到全零。查 `objcopy --rename-section` 的 flags。
- **链接时报「未定义符号 `_binary_initrd_start`」**:objcopy 生成的符号名是路径派生的,没做 `--redefine-sym` 重命名,和代码里写死的对不上。
- **文件名打印后面带一串乱码**:`name` 是定长 100 字节、未必有 null,直接 `%s` 打越界读到下一字段。打印要带长度上限、遇 null 停。

## 通过标准

1. host 单测全绿:`UstarHeader` 512B 及字段偏移、类型标志、magic、八进制(含截断/全空)、`data_blocks` 边界、mount 在合成归档上的遍历。
2. QEMU 机内测通过:归档基址非空、size>0、`mount` 返回 3、八进制解析(0/10/100/668/512)正确。
3. 八进制用 `<<3` 累加且 null/space 截断;数据块向上取整;结束以全零头为准;magic 校验不漏;文件名带长度打印。
4. embed 流水线:`.initrd` 段带 `ALLOC,LOAD` 且页对齐,高半区 `AT(ADDR - KERNEL_VMA)`;objcopy 符号重命名为稳定的 `_binary_initrd_*`。

做到这四条,内核就第一次认出了归档里的文件。但还只是「列出」,没法打开读内容——下一关,我们在这层之上搭一个 VFS,给内核和用户态一套统一的 `open/read/close` 接口。
