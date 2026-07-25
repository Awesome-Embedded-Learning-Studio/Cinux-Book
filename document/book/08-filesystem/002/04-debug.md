---
title: 04 · 调试现场与验证
---

# 调试现场与验证

002 没有 notes 文件,但 ustar 解析 + 链接流水线有几个经典坑,值得当调试现场。

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
