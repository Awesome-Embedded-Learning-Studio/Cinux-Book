---
title: "07 · 开机栽树"
description: "行李上车的三步都在构建期:tar 按 ustar 打包两份资产,objcopy 把包搬进目标文件、节改名 .rodata.initrd、符号改名 g_initrd_start/g_initrd_end,链接器把它接进每个内核。运行期 FileWorld 单例把 ramfs 挂上根、拿 UstarReader 逐条目栽进树里,每一步 Check,坏档案按构建 bug 处理,kernel.cpp 真正插进启动流程的只有一行。"
chapter: 14
order: 7
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - filesystem
  - initrd
---

# 开机栽树

行李怎么上车?三步,全在构建期走完。头一步,打包——assets/initrd 里的两份文件,咱们拿 tar 按 ustar 打成一个档案:

```cmake
tar --format=ustar -C ${CMAKE_SOURCE_DIR}/assets/initrd -cf
        ${CMAKE_CURRENT_BINARY_DIR}/initrd.tar readme.txt docs
```

第二步,把档案搬进目标文件。tar 打出来是个普通文件,内核链接器不认识它,咱们请 objcopy 把它当一节数据搬进去,顺手改两个名字:

```cmake
objcopy --rename-section .data=.rodata.initrd
        --redefine-sym _binary_initrd_tar_start=g_initrd_start
        --redefine-sym _binary_initrd_tar_end=g_initrd_end
        --set-section-flags .rodata.initrd=alloc,load,readonly,data
        initrd.tar initrd_blob.o
```

objcopy 对二进制输入的默认手法,是生成一对 `_binary_` 打头的符号,记着数据的头尾。名字又长又绕,咱们改成体面的 `g_initrd_start` 与 `g_initrd_end`,节也改成 `.rodata.initrd` 这个说明来历的名字,属性挂上只读。第三步不用咱们动手:目标文件进了内核的源文件清单,链接器把它接进每一个内核,两个符号一头一尾,就是档案在镜像里的边界。从此内核低头看自己,肚子里带着一座小小的档案馆,您说妙不妙。

运行期接手的,是咱们立起来的 `FileWorld`,又一位住进 kernel 编制的单例:物理内存的 Pmm 在册,报时的 Tick 在册,文件世界也来落户。它手里的 `init` 一共三拍:

```cpp
void FileWorld::init() {
    ramfs_                            = new RamFs{};
    const base::Result<void> kMounted = vfs_.mount("/", *ramfs_);
    base::safety::Check(kMounted.ok(), "root ramfs mount failed");
    plant_initrd();
}
```

new 一座 ramfs,挂上根前缀 `/`,栽行李。挂载要是拒绝,Check 当场把机器停下——根挂不上,后头的一切都没有意义,这样的错得算在装机头上,停机就是正解。栽行李的 `plant_initrd` 是个循环,咱们跟着它走一遍:

```cpp
UstarReader         reader(g_initrd_start, kArchiveBytes);
UstarEntry          entry;
while (reader.next(entry).value()) {
    if (entry.type == InodeType::kDirectory) {
        std::string_view path = entry.name;
        if (path.ends_with('/')) {
            path.remove_suffix(1);
        }
        const base::Result<FsNode> kDir = ramfs_->make_dir(path);
        base::safety::Check(kDir.ok(), "initrd directory refused");
        continue;
    }
    const base::Result<FsNode> kFile = ramfs_->make_file(entry.name);
    base::safety::Check(kFile.ok(), "initrd file refused");
    const base::Result<unsigned long> kStored =
        ramfs_->write(kFile.value(), 0, entry.data.data(), entry.size);
    base::safety::Check(kStored.ok(), "initrd content refused");
}
```

逐条目念档案:目录就去掉尾斜杠建目录——上一章说过,tar 给目录的名字带尾斜杠,建目录前得把尾巴剃了。文件就建文件,再从档案的视图里把字节抄进 ramfs 的堆缓冲。您留意这一抄:档案在镜像里当原件,永远只读,堆里长出来的是可写的活字,客人以后写的都是堆里长的字,原件不受搅扰。零拷贝省下的是读取器那一程,该搬的家当一程都不省。每一步都 Check:档案里的名字超长、路径缺爹、类型古怪,统统是构建环节的 bug——开机就死,比带着半个文件世界活着诚实得多。

最后看一眼这笔改动摊在老代码上是多大:kernel.cpp 里真正插进启动流程的只有一行,`FileWorld::self().init()`,摆在客人们收场之后、shell 下楼之前。其余的呢,一行头文件的包含,加一批顺着 shell 改的名,如此而已。文件世界赶在常客进门那一刻就绪,行李已经栽好了。两份行李的原文,咱们也交底在这儿,验收那一章的屏幕上还会见到它们:readme.txt 全文一行,`welcome to cinux - this file was baked into the kernel image`。docs/note.txt 也是一行,`station 14: the kernel knows files before any disk exists`。

树栽上了,行李进了库。可柜台还是上一卷的四件业务,念路名、开文件的活一件没接,咱们下一章就去翻新柜台。
