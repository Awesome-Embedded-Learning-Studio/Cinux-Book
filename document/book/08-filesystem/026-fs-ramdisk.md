---
title: 026 · 内核第一次认识「文件」:嵌入式 initrd ramdisk
---

# 026 · 内核第一次认识「文件」:嵌入式 initrd ramdisk

> 025 让内核能自己读写磁盘扇区了,但读出来的是一坨 512 字节的裸数据——没有名字、没有「这是哪个文件」的概念。要从「扇区」走到「文件」,内核得先学会认一种文件格式。这一章,我们让内核解析一个 ustar(就是 `tar`)格式的归档,把里面每个文件的名字和大小列出来。先把丑话说在前头:这一章的归档**不是从 025 的 AHCI 盘上读来的**,而是构建期就被嵌进了内核镜像里、随内核一起加载进内存的。这是个有意的简化——先用最省事的办法把「文件」这个抽象立起来,至于「从磁盘上读真正的根文件系统」,留到后面。

## 这一章我们要点亮什么

一件能力,一个里程碑。

能力是:内核**第一次理解「文件」是什么**。我们准备一个 `tar` 归档(里头放几个测试文件:`hello.txt`、`readme.txt`、`etc/passwd`),在编译时把它整个塞进内核二进制;内核启动后,解析这个归档,逐个认出里面的文件,把名字和大小打到串口上。从这一章起,内核不再只认识「扇区」,它认识「文件」了。

里程碑是 `mount` 之后的那几行串口输出:

```text
[RAMDISK] Archive at 0x..., size 10240 bytes
[RAMDISK]   FILE: hello.txt  (18 bytes)
[RAMDISK]   FILE: readme.txt  (23 bytes)
[RAMDISK]   DIR:  etc/
[RAMDISK]   FILE: etc/passwd  (11 bytes)
[RAMDISK] 3 file(s) found in initrd.
```

看到这些文件名被正确解析出来,说明内核吃透了 ustar 归档的格式:头怎么排、大小怎么读、数据块怎么跳过。

不过得诚实划清边界:这一章只是「**列出**归档里的文件名和大小」。它没有「打开-读-关闭」的 API、不能按名字取文件内容、不能写、不维护目录树——`RamdiskEntry` 这个描述单文件的结构体虽然定义了,但本章的 `mount` 还不填充它(它是给后面按名查找预留的形状)。这一章是「认得文件长什么样」,不是「能操作文件」。

## 为什么现在需要它

为什么紧跟在 025 之后。025 给了扇区读写,但扇区是「设备语言」,不是「用户语言」。你没法跟 shell 说「帮我跑 `hello.txt`」——shell 听不懂扇区号。要往「能从盘上加载并运行程序」走,中间必须有一层「文件」抽象:把一串扇区解释成「有名字、有大小的文件」。这一章就是来补这层抽象的。

那为什么不直接用 025 的 AHCI 驱动从盘上读一个文件系统?因为那是两件事叠一起:既要「从盘上读对扇区」(块设备层),又要「把扇区解释成文件」(文件系统层)。两层一起上,调试时分不清是驱动读错了还是格式解析错了。这一章做了个聪明的解耦:它**不碰磁盘**,把一个现成的 `tar` 归档直接嵌进内核镜像——数据是构建期固定好的、确定无误的,内核只要专心学「怎么解析 tar 格式」这一件事。025 的 AHCI 驱动在这一章没参与加载,这是有意的;等到格式解析这关过了,再谈「从盘上读真正的根文件系统」,那时候驱动和格式才组合起来。

还有一笔技术上的账,关于「数据怎么进内核」。内核二进制本身是一堆 `.text`/`.data` 段,被 bootloader 整个加载进内存。要把一个外部文件(`initrd.tar`)也弄进来,得在编译期把它转成一段可链接的 ELF 数据、放进一个专门的段(`.initrd`)、再由链接器排进内核镜像里。内核代码靠两个链接器符号(`_binary_initrd_start` / `_binary_initrd_end`)在运行时找到这块数据的边界。这套「把任意文件嵌进内核」的流水线,是这一章的另一半工程,后面嵌入用户程序二进制(023 那条路)也是同款手法。

## 设计图

整件事分「构建期」和「运行期」两段。构建期把文件变成内核镜像里的一段数据:

```text
   构建期:
   initrd_contents/                ← 几个普通文件(hello.txt / readme.txt / etc/passwd)
        │  tar 打包
        ▼
   initrd.tar (ustar 归档)         ← 一串 512 字节的头 + 数据块
        │  embed_binary.sh:objcopy -I binary
        │  --rename-section .data=.initrd,CONTENTS,ALLOC,LOAD,READONLY,DATA
        │  --redefine-sym ...→ _binary_initrd_{start,end,size}
        ▼
   initrd.o (.initrd 段)            ← 一个可链接的 ELF object
        │  链接进 big_kernel
        ▼
   内核镜像里多了 .initrd 段         ← 符号 _binary_initrd_start / _end 标出边界

   运行期:
   Ramdisk::mount()
        base_ = _binary_initrd_start; size_ = end - start
        while 还有完整头(512B):
            hdr = base_ + offset
            if hdr->name[0] == '\0':  归档结束,跳出     ← 两个全零块 = EOF
            if magic != "ustar":      非法头,停下
            size = octal_to_uint(hdr->size)             ← 八进制 ASCII → 整数
            按 typeflag 分类('0'/'7' 文件,'5' 目录)打印
            offset += 512(头)+ ceil(size/512)*512(数据)  ← 数据按 512 补齐
```

两段的衔接是那对链接器符号:构建期把归档塞进 `.initrd` 段并定好符号,运行期靠符号找到归档再解析。

## 代码路线

### 先把 ustar 头摸清楚:512 字节的固定表

ustar 是 POSIX.1-1988 定的归档交换格式,也是最经典的 `tar` 格式之一。它的设计极简:归档就是一连串**512 字节的块**,每个文件由「一个 512 字节的头 + 若干个 512 字节的数据块」组成。头是定长的表,字段按固定偏移排布,见 [ramdisk_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk_config.hpp):

```cpp
struct [[gnu::packed]] UstarHeader {
    char name[100];     // 0:   文件名
    char mode[8];       // 100: 权限(八进制 ASCII)
    char uid[8];        // 108: 属主 ID
    char gid[8];        // 116: 属组 ID
    char size[12];      // 124: 文件大小(八进制 ASCII)
    char mtime[12];     // 136: 修改时间
    char checksum[8];   // 148: 头校验和
    char typeflag;      // 156: 类型标志(一个字符)
    char linkname[100]; // 157: 链接目标
    char magic[6];      // 257: "ustar\0"
    char version[2];    // 263: "00"
    // ... uname/gname/devmajor/devminor/prefix/padding ...
};
static_assert(sizeof(UstarHeader) == USTAR_BLOCK_SIZE, "UstarHeader 必须 512 字节");
```

几个关键点。第一,整个头**恰好 512 字节**,`[[gnu::packed]]` 加 `static_assert` 焊死——和 025 的 MMIO 结构体同理,格式布局差一个字节就全错,编译期必须卡住。第二,头里几乎所有字段都是**字符串**,包括数字(mode/uid/gid/size/mtime/checksum)——而且这些数字字段是**八进制 ASCII**(后面专门讲)。第三,`typeflag` 是单个字符,决定这一条目是什么:`'0'` 普通文件、`'5'` 目录、`'7'` 连续文件(等价于普通文件)、`'1'` 硬链接、`'2'` 符号链接等等。第四,`magic` 是 `"ustar\0"`,这是判断「这是一个合法 ustar 头」的凭据。记住这几个字段(name、size、typeflag、magic),mount 的逻辑就全围绕它们转。

### 八进制:ustar 的数字编码

ustar 最容易绊倒人的地方,是它的数字字段用**八进制** ASCII 存,不是十进制也不是十六进制。比如 `hello.txt` 有 18 字节,它的 `size` 字段里写的是 `"000000000022"`(八进制 22 = 十进制 18)。`octal_to_uint` 负责把这个八进制串转回整数:

```cpp
uint64_t octal_to_uint(const char* s, size_t len) {
    uint64_t result = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if (c == '\0' || c == ' ') break;          // null 或空格 = 字段结束
        result = (result << 3) + static_cast<uint64_t>(c - '0');  // result*8 + digit
    }
    return result;
}
```

两件事要注意。一是 `result << 3` 就是乘 8(八进制每位进 3 个 bit),这是八进制解析的标准写法。二是**遇到 null 或空格就停**——ustar 的字段是定长的(比如 size 占 12 字节),实际数值短的部分用前导 `0` 或空格填充,结尾也可能有 null。所以不能傻乎乎读满 `len` 个字符,得在终止符处截断。host 单测里专门测了各种边界:`"144\0xxxx"` 解出 100、`"1234  "` 在空格处停解出 668、全空返回 0。这个终止符处理错了,解析出来的大小就乱套,后面整个归档遍历都会错位。

### mount:在内存里走一遍归档

格式清楚了,`mount` 就是在内存里从头走到尾。看 [ramdisk.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk.cpp) 的主循环:

```cpp
uint32_t Ramdisk::mount() {
    base_ = _binary_initrd_start;                              // 边界来自链接器符号
    size_ = static_cast<uint64_t>(_binary_initrd_end - _binary_initrd_start);
    if (base_ == nullptr || size_ == 0) { return 0; }

    uint32_t entry_count = 0;
    uint64_t offset = 0;
    while (offset + sizeof(UstarHeader) <= size_) {
        auto* hdr = reinterpret_cast<const UstarHeader*>(base_ + offset);

        if (hdr->name[0] == '\0') break;                       // 全零头 = 归档结束

        if (!is_valid_ustar(hdr)) {                            // magic 必须是 "ustar"
            kprintf("[RAMDISK] Invalid ustar magic at offset %u, stopping.\n", offset);
            break;
        }

        uint64_t file_size = octal_to_uint(hdr->size, sizeof(hdr->size));   // 八进制读大小
        char type = hdr->typeflag;
        if (type == UstarType::REGULAR || type == UstarType::CONTIGUOUS) {
            kprintf("[RAMDISK]   FILE: "); print_bounded(hdr->name, RAMDISK_NAME_MAX);
            kprintf("  (%u bytes)\n", file_size);
            ++entry_count;
        } else if (type == UstarType::DIRECTORY) {
            kprintf("[RAMDISK]   DIR:  "); print_bounded(hdr->name, RAMDISK_NAME_MAX);
            kprintf("\n");
        }

        uint32_t blocks = data_blocks(file_size);              // 数据补齐到 512 的块数
        offset += sizeof(UstarHeader) + static_cast<uint64_t>(blocks) * USTAR_BLOCK_SIZE;
    }
    return entry_count;
}
```

逻辑是直的:从一个头开始,检查「是不是结束」「合不合法」,读出大小和类型,打印,然后**跳过这个头 + 它的数据块**,落到下一个头。三个细节决定它对不对。

第一,**怎么判断归档结束**。ustar 规定归档末尾用两个全零的 512 字节块收尾。代码的判断更保守:只要遇到 `name[0] == '\0'`(一个全零头)就停。对内核读归档来说,这够了——读到全零头意味着后面没有合法条目了。

第二,**数据块怎么跳**。文件数据紧跟在头后面,而且**按 512 字节补齐**:一个 19 字节的文件,实际占 1 个 512 字节的数据块;513 字节的文件占 2 个块。`data_blocks` 算的是「向上取整的块数」:

```cpp
uint32_t data_blocks(uint64_t size) {
    if (size == 0) return 0;
    return static_cast<uint32_t>((size + USTAR_BLOCK_SIZE - 1) / USTAR_BLOCK_SIZE);  // 向上取整
}
```

`(size + 511) / 512` 是向上取整的标准写法。这一步算错一个块,下一个头的位置就偏了 512 字节,后面所有条目全乱——调试现场里这是高发坑。

第三,**合法头校验**。`is_valid_ustar` 逐字节比对 `magic` 字段的前 5 个字符是不是 `"ustar"`。没有这道校验,读到损坏或越界的数据会被当成合法条目继续解析,一路读到归档外头去。

`print_bounded` 是个小而重要的辅助:它最多打印 `RAMDISK_NAME_MAX`(100)个字符、遇 null 停。ustar 的 `name` 是定长 100 字节、不一定有 null 结尾,直接当 C 字符串 `kprintf("%s", ...)` 打印会越界读到下一个字段。所有「定长字段当字符串用」的地方,都得这么带长度地处理。

### 数据怎么进内核镜像:embed 流水线 + .initrd 段

mount 能跑的前提是 `_binary_initrd_start/_end` 这两个符号真的指向了归档数据。这要靠一条构建期流水线。第一步,把普通文件打成 tar(`kernel/data/initrd_contents/` → `initrd.tar`),这一步在仓库里手工准备好(`data/initrd.tar` 是二进制,跟着提交)。

第二步,把 `initrd.tar` 转成可链接的 ELF object。这用 GNU `objcopy` 的「二进制输入」模式,见 [embed_binary.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/embed_binary.sh):

```bash
objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
    --rename-section .data="${SECTION}",CONTENTS,ALLOC,LOAD,READONLY,DATA \
    "${INPUT}" "${OUTPUT}"
# objcopy 会按输入文件的绝对路径派生符号名(非字母数字变下划线),不 portable
# 所以再把它重命名成稳定的 _binary_initrd_{start,end,size}
objcopy --redefine-sym "${SYM_START}=${SYM_PREFIX}_start" \
        --redefine-sym "${SYM_END}=${SYM_PREFIX}_end" \
        --redefine-sym "${SYM_SIZE}=${SYM_PREFIX}_size" "${OUTPUT}"
```

这里有两个「为什么」。第一,`--rename-section` 把这段数据放进名为 `.initrd` 的段(而不是默认的 `.data`),并标上 `ALLOC,LOAD`——这意味着它会被分配地址、会被加载进内存(随内核镜像一起)。漏了 `LOAD`,这段数据就只存在于文件里、运行时不映射进内存,`_binary_initrd_start` 指向的是空。第二,符号重命名:`objcopy -I binary` 生成的符号名是从**输入文件的绝对路径**派生的(`/home/.../build/kernel/initrd.tar` → `_binary_home_..._initrd_tar_start`),换台机器、换个构建目录名字就变,没法在代码里写死。所以脚本先让 objcopy 生成、再用 `nm` 抓出那几个符号名、用 `--redefine-sym` 统一改成稳定的 `_binary_initrd_start/end/size`。代码里 `extern const uint8_t _binary_initrd_start[];` 才能稳定地引用到。

第三步,链接器把 `.initrd` 段排进内核镜像。[linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/linker.ld) 加了一节:

```text
.initrd : AT(ADDR(.initrd) - KERNEL_VMA) ALIGN(4096) {
    *(.initrd)
}
```

两个细节。`ALIGN(4096)` 让归档按页对齐——方便后续如果要把这段映射/搬移时按页处理。`AT(ADDR(.initrd) - KERNEL_VMA)` 是高半区内核的老把戏:段在**虚拟地址空间**里排在 `KERNEL_VMA` 之上(内核代码看到的地址),但它的**加载地址**(LMA,bootloader 实际把它放到物理内存的位置)是虚拟地址减去 `KERNEL_VMA`,落在低端物理内存。这样内核用一个高半区虚拟地址访问它,靠的就是 016 那套「物理地址 ↔ 虚拟地址」的高半区约定。整个 `initrd.o` 由 [CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/CMakeLists.txt) 用 `add_custom_command` 生成,再作为源文件塞进 `big_kernel` 和 `big_kernel_test`,链接时就和 `main.cpp` 编出来的代码并到了一起。

## 调试现场

026 没有 notes 文件,但 ustar 解析 + 链接流水线有几个经典坑,值得当调试现场。

一是 **八进制读成十进制**。`size` 字段是八进制 ASCII,如果你下意识按十进制解析(`result*10 + digit` 而不是 `<<3`),或者以为它是二进制整数直接 `*(uint64_t*)hdr->size`,解析出来的大小全是错的——而且往往偏大,导致 `data_blocks` 算出天文数字,`offset` 一下子飞出归档,循环提前结束、只列出零个或一个文件。看到「明明归档里有 3 个文件,内核只认出 1 个甚至 0 个」,先怀疑八进制解析。

二是 **数据块向上取整算错**。`data_blocks` 必须向上取整(`(size+511)/512`),且 `size==0` 返回 0 块(空文件不占数据块)。如果写成 `size/512`(向下取整),或忘了处理 size==0、或差一个块,`offset` 跳偏 512 字节,下一个头落到文件数据的中间,`magic` 校验失败、遍历中断。症状和上一条类似(认不全文件),但根因在块数。host 单测把 1B/512B/513B/1024B/1025B 这些边界全测了,就是防这个。

三是 **没用 magic 校验 / 没判断结束**。不判 `magic=="ustar"`,读到任何字节都当合法头,一旦归档结构有偏差(比如上面两条导致的错位),内核会拿一堆垃圾数据当文件名打出来,甚至读越界。不判 `name[0]=='\0'` 的结束标志,循环可能越过归档边界、读到相邻的内核数据段里去(那里多半不是全零),停不下来。这两道闸缺一不可。

四是 **`.initrd` 段没进加载镜像**。`objcopy --rename-section` 的 flags 必须含 `ALLOC,LOAD`。如果只写了段名、漏了这俩属性,链接器可能把这段当「只在文件里有、运行时不加载」的段(类似某些 debug 段),内核运行时 `_binary_initrd_start` 指向的地址没有数据,`mount` 读到的 base 非空但内容全零,认不出任何文件。`base()` 非空、`total_size()` 也对,但一个文件都列不出来——先查段的加载属性。

五是 **objcopy 的符号名没重命名**。如果你直接用 `objcopy -I binary` 生成的原始符号(路径派生名),代码里写 `extern ... _binary_initrd_start[]` 链接时就会「未定义符号」。这是因为符号名跟着构建目录路径走,和你代码里写死的对不上。`embed_binary.sh` 那步 `--redefine-sym` 不能省。

## 验证

验证还是两层:纯逻辑在 host 上镜像测,真归档在 QEMU 里跑。

host 单测 [test_ramdisk.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_ramdisk.cpp) 是这一章最厚的一块(800 多行),把能脱离真归档测的逻辑全覆盖了:`UstarHeader` 各字段偏移和 512 字节大小、`UstarType` 类型标志、`USTAR_MAGIC`、`octal_to_uint`(各种边界:0、单 digit、null/space 截断、全空、12 字段)、`data_blocks`(1B/512B/513B 边界)、以及最关键的——**在合成的假归档上跑一遍 mount**(单文件、带数据、目录、无效 magic 停止、数据块跳过、全零归档返回 0)。因为内核的 `mount` 读的是链接器符号指向的真数据,host 上没有那块数据,所以测试自己造合成归档、跑同样的遍历逻辑:

```bash
ctest --test-dir build -R ramdisk --output-on-failure
```

真归档、真链接器符号,在 QEMU 里验。机内测 [test_ramdisk.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_ramdisk.cpp) 检查:`UstarHeader` 是 512 字节、`octal_to_uint` 解析对(0/10/100/668/512)、`mount` 后 `base()` 非空且 `total_size()>0`、以及 `mount()` 返回 3(测试归档里正好 3 个文件):

```bash
cmake --build build --target run-kernel-test
```

或者直接跑完整内核,看启动时那段 `[RAMDISK]` 输出——能正确列出 `hello.txt`、`readme.txt`、`etc/passwd`(和 `etc/` 目录)的名字与大小,这一章就成了。验证的难点在于「格式正确性靠现象间接验证」:你只能从「列出的文件名/大小对不对」反推解析对不对,所以那批焊死格式和算法的 host 单测 + 机内测(真跑一遍)缺一不可。

## 下一站

到这里,内核第一次认出了「文件」——它能把一个 ustar 归档解析成「名字 + 大小」的列表。但你很快会发现这个抽象还太薄:它只是「列出」文件,没有「打开某个文件读它的内容」、没有路径、没有目录层级、没有统一的接口。现在内核知道 `hello.txt` 存在、有多大,但拿不到它的内容,更没法对各种来源(ramdisk、将来的磁盘文件系统)用同一套 API 操作。

下一站,我们在这层「认得文件」的基础上,搭一个虚拟文件系统(VFS):一套 `open/read/close` 的统一接口,让内核(和用户态)能用「打开一个名字、读它的内容」的方式访问文件,而不用关心文件背后是 ramdisk 还是别的什么。不过那是下一章的事,我们先把「内核能认出归档里的文件」这个里程碑坐实。

---

### 参考

- POSIX.1-1988 "ustar" 交换格式(`[ramdisk_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk_config.hpp)` 头注释已引):512 字节定长头布局、字段偏移(name@0、size@124、typeflag@156、magic@257)、数字字段的八进制 ASCII 编码、typeflag 字符含义(`'0'` 文件 / `'5'` 目录 / `'7'` 连续)、`"ustar\0"` magic、末尾两个全零块收尾。权威格式依据。
- Wikipedia — [tar (computing)](https://en.wikipedia.org/wiki/Tar_(computing)):ustar/POSIX 头布局与历史演变的社区参考,字段速查方便。
- GNU binutils — `objcopy`(`-I binary`、`--rename-section`、`--redefine-sym`):embed 流水线的工具依据;`-I binary` 按输入路径派生符号名是其已知行为,这正是 `embed_binary.sh` 要做重命名的原因。
- 025 章 · [让内核自己找到磁盘:PCI 枚举与 AHCI 驱动](025-driver-ahci.md):存储前一章。本章明确说明 ramdisk 数据是构建期嵌入、**不走** AHCI 盘,两章在「数据来源」上解耦。
- 本 tag 源码:[ramdisk.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk.cpp) / [ramdisk.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk.hpp) / [ramdisk_config.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/fs/ramdisk_config.hpp)、[embed_binary.sh](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/embed_binary.sh)、[linker.ld](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/linker.ld)(`.initrd` 段)、[CMakeLists.txt](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/CMakeLists.txt)(embed 流水线)、[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp)(Step 22);测试 [test_ramdisk.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_ramdisk.cpp)(host 镜像)、[test_ramdisk.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_ramdisk.cpp)(QEMU 真归档)。
