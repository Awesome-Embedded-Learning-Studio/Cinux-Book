---
title: 02 · 设计图:ustar 与 initrd
---

# 设计图:ustar 与 initrd

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
