---
title: 03 · 实现:ustar 头、八进制、mount、embed 流水线
---

# 实现:ustar 头、八进制、mount、embed 流水线

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

几个关键点。第一,整个头**恰好 512 字节**,`[[gnu::packed]]` 加 `static_assert` 焊死——和 001 的 MMIO 结构体同理,格式布局差一个字节就全错,编译期必须卡住。第二,头里几乎所有字段都是**字符串**,包括数字(mode/uid/gid/size/mtime/checksum)——而且这些数字字段是**八进制 ASCII**(后面专门讲)。第三,`typeflag` 是单个字符,决定这一条目是什么:`'0'` 普通文件、`'5'` 目录、`'7'` 连续文件(等价于普通文件)、`'1'` 硬链接、`'2'` 符号链接等等。第四,`magic` 是 `"ustar\0"`,这是判断「这是一个合法 ustar 头」的凭据。记住这几个字段(name、size、typeflag、magic),mount 的逻辑就全围绕它们转。

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
