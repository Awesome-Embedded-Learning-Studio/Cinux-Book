---
title: 01 · DevFS:把设备挂进 /dev
---

# DevFS:把设备挂进 /dev

> 到目前为止内核认识的文件,要么来自内存里的 ramdisk 归档,要么来自磁盘上的 ext2——都是有「后端」的:数据真的存在某处。可有一类「文件」不该有后端,那就是**设备**。`/dev/null`(写进去全丢、读出来是 EOF)、`/dev/zero`(读出来全是零)、`/dev/console`(写进去打到串口)——它们不在任何盘上,「读写」它们其实是触发一个设备行为。这一章立一个 **DevFS**:一个纯内存的虚拟文件系统(没有磁盘后端),把设备行为包成一种特殊的 inode(device inode),挂在 `/dev` 下。做完之后,`ls /dev` 能看到 null/zero/console,对它们的读写真的触发对应设备行为。
>
> 这一章真正的主题是「**怎么往已有的 VFS 里加一种新文件,而不动 VFS 的接口**」。前面立 VFS 时定义了一个 `InodeOps` 虚表(每个 inode 的行为:读、写、stat、readdir…);这一章加设备,做法是给 `InodeOps` 写**子类**(NullDevOps/ZeroDevOps/ConsoleDevOps),设备的「读写触发什么行为」全在子类的 override 里——基类接口一行不动。这是 Linux 加新文件类型的套路(ramdisk 那章也是这么干的),也是这一章能跟别的改动并行的原因:既然不动基类,各加各的子类,互不干扰。
>
> punchline 是 `/dev` 真挂上了,`ls /dev` 见到 null/zero/console,读写它们触发对的设备行为(null 丢、zero 给零、console 打串口)。一条诚实的边界先说在前头:这一章的 `/dev/console` 只接了**写**(写到串口),没接**读**——接 console TTY 的真 stdin(让 `read /dev/console` 读键盘)、`/dev/tty`、PTY,是后面 TTY 那条线 Phase 2 的事。`/dev/null`/`/dev/zero` 是完整的;console 这半,先把写做对。

## 这章咱们要点亮什么

1. **设备也是一种「文件」**:它没有磁盘后端,「读写」它是触发设备行为。DevFS 是个纯内存虚拟 FS,把设备行为包成 device inode。
2. **加新文件类型不动基类接口**:`InodeOps` 虚表一行不改,只给它写子类——设备的读/写/stat 全在子类的 override 里。这跟「加新协议挂 L4 表」(上一卷)、「加新设备挂 NetStack」是同一个套路:基类定接缝,子类填行为。
3. **怎么让设备逻辑能在 host 上单测**:设备写串口这件事离不开 `Serial`(host 链不了),所以抽一个 `CharSink` 接缝——核心逻辑只认 `CharSink`,内核注入真串口 sink、host 注入 mock,派发逻辑就能在 host 上链真码测。
4. **boot 怎么挂上 /dev**:ext2 挂了根 `/` 之后,再 mount 一个 DevFS 到 `/dev`,VFS 的 mount 表多一条。

## 设备文件:没有后端的「文件」

先说清「设备文件」跟普通文件差在哪。普通文件(ramdisk 里的、ext2 里的)有后端——数据存在内存或磁盘的某处,`read` 是把那处数据拷给你,`write` 是把你的数据存过去。设备文件没有后端:`/dev/null` 的「写」是把数据扔掉,「读」是给你 EOF;`/dev/zero` 的「读」是无尽地给你零,「写」是扔掉;`/dev/console` 的「写」是把数据打到串口。它们的「读写」不是搬运数据,是触发一个**动作**。

所以 DevFS 不能像 ext2 那样「背后有个盘」,它得是**纯内存的虚拟 FS**:不读写任何块设备,inode 的行为就是一段代码(读触发什么、写触发什么)。VFS 早就留好了这个口子——`InodeOps` 是个虚表,inode 的读/写/stat/readdir 全是虚函数;前面 ext2 给了它一套实现(读写转成块设备操作),这一章给它**另一套**实现(读写转成设备动作),靠的就是子类多态。

## device inode:给 InodeOps 写子类

DevFS 核心是 `DevFs : FileSystem`(`devfs.hpp:110`)——一个内存设备表(`DevNode{name, Inode}` × 16),`mount()` 建标准节点,`lookup()` 按名查。真正有意思的是设备行为怎么写:在 `devfs.cpp` 的匿名 namespace 里,给 `InodeOps` 写四个子类,每种设备一个:

```cpp
class NullDevOps : public InodeOps {    // /dev/null:写丢弃,读给 EOF
    ssize_t read(...) override  { return 0; }        // 0 = EOF
    ssize_t write(...) override { return count; }    // 假装全写了(实际丢)
    void stat(stat* st) override { st->st_rdev = devfs_makedev(1,3); ... }
};
class ZeroDevOps : public InodeOps {     // /dev/zero:读给零,写丢弃
    ssize_t read(...) override  { memset(buf, 0, count); return count; }
    ...
};
class ConsoleDevOps : public InodeOps {  // /dev/console:写打到串口
    explicit ConsoleDevOps(CharSink* sink, ConsoleInput* input)
        : sink_(sink), input_(input) {}
    ssize_t write(...) override { return sink_->write(buf, count); }
    ...
};
class DevDirOps : public InodeOps {      // /dev 目录本身:readdir 遍历节点表
    ...
};
```

(`devfs.cpp:48` 起;匿名 namespace 整体从 L23 起。)几个细节。其一,设备的 `stat` 要填 `st_rdev`(设备号,`devfs_makedev(major, minor) = (major<<8)|minor`,比如 null 是 1:3、zero 是 1:5)和 `st_mode`(`kSIfChr|0666`,字符设备 + 读写权限)——这两个字段是 `stat()` 的 override 里填的,**不往 `Inode` 结构体里加字段**。其二,`DevDirOps` 是 `/dev` 这个目录自己的行为——`readdir` 遍历 DevFs 的节点表,列出 null/zero/console。

> 这一步的关键纪律:**`InodeOps` 基类的虚函数签名一行不动,只加子类**。这不是洁癖,是能并行的前提——device inode 子类、ext2 的子类、还有别的改动,全都给 `InodeOps` 加子类,只要不动基类接口,各加各的,merge 时不撞。要是改了基类签名(比如给 `Inode` 加个 `st_rdev` 字段),所有子类都得跟着改,并行就炸了。把「设备号」收进 ops 子类的 `stat()` override、而不是 `Inode` 字段,就是为这个。「加新东西靠子类、不动基类」是这一卷反复出现的套路(上一卷加 UDP 靠 L4 表,一个道理)。

### 对照 Linux:cdev、设备号,与「字符设备的对象模型」

Cinux 的 device inode(`Inode` + `InodeOps` 子类)在 Linux 里有个更专门的载体——`struct cdev`。Linux 把字符设备单独抽成一个内核对象,围绕它有一套分工清楚的三件套:设备号(major:minor)是**接线口**、cdev 是**设备对象**、`file_operations`(fops)是**行为**。`cdev_init` 把一个 cdev 跟一套 fops(驱动的 read/write/ioctl 回调)绑上,`cdev_add` 再按设备号把它登记进全局的「设备号 → cdev」表;用户 `open("/dev/null")` 时,VFS 从 inode 里的设备号出发查这张表找到 cdev,再调它的 fops。

Cinux 没抽这层:设备行为直接绑在 inode 的 `InodeOps` 上,device inode **本身就是**设备对象,设备号只在 `stat()` 时填进 `st_rdev` 给用户态看,不参与内核派发——派发靠 inode 的 ops 指针,不查设备号表。这不是图省事漏了一层,是个有意的取舍:Linux 那套「设备号注册表 + cdev + fops」是为了支持运行时加载驱动、动态分配设备号、同一主设备号下挂多个从设备;Cinux 的设备在 boot 时固定建好,这些需求都没有,这层间接就是纯负担,砍掉。

Linux 那边还有两样咱们也没有,顺带点一下:sysfs(把设备/驱动/总线的拓扑暴露成 `/sys` 下的设备树)和 devtmpfs(`/dev` 默认靠它,驱动注册设备时内核自动 mknod)。Cinux 的虚拟 FS 借了「DevFS」这个名,但实现是 boot 时固定建几个节点,既不是 sysfs 的设备模型,也不做 devtmpfs 的动态 mknod——`/dev` 里有什么,完全由 `mount()` 里写死的节点表决定。

## CharSink:让设备逻辑能在 host 上单测

`ConsoleDevOps` 的 write 要打到串口,可 `Serial` 是内核硬件的东西,host 单测链不了。要是 devfs.cpp 直接 `#include "Serial.h"` 调串口,host 一链就 undefined。

解法是抽一个接缝——`CharSink`(`devfs.hpp:61`):

```cpp
class CharSink {
public:
    virtual ~CharSink() = default;
    virtual ErrorOr<int64_t> write(const void* buf, uint64_t count) = 0;
};
```

`ConsoleDevOps` 只持一个 `CharSink*`,write 时调 `sink_->write`——它根本不知道 sink 背后是串口还是别的。于是:内核里注入一个**真** sink(`SerialConsoleSink`,write 转成 COM1 逐字节 `putc`,见 `devfs_init.cpp:42`);host 单测注入一个 **mock** sink(把写下来的字节收进一个 buffer 好断言)。同一份 `ConsoleDevOps` 派发逻辑,内核和 host 都能跑。

> 这跟上一卷 TTY 的「回显走注入 callback」、net 的「NetDevice 接缝」是一个模子:**核心逻辑只认抽象接缝,具体后端靠注入**。好处是核心逻辑(host 能链的那份)可单测,后端(硬件相关的)单独放、不污染可测的部分。代价是多一层胶水,但比起「host 里 mock 掉一整个 Serial」干净太多。
>
> 这里有个 §14 的文件级 gate 顺带讲一下。`devfs.cpp`(核心 + 设备 ops,host 可链,零内核 I/O)和 `devfs_init.cpp`(boot 接线,用 `Serial`/`kprintf`,kernel-only)是**两个文件**——CMake 决定编不编,源码里一行 `#ifdef` 都没有。host 测只 link `devfs.cpp`,`Serial` 依赖进不来;内核两个都编。这是「同一个特性、按能不能 host 链拆两个文件」的套路,比源码里满篇 `#ifdef CINUX_HOST_TEST` 干净。

## boot:挂上 /dev

核心和设备 ops 就位,最后把 DevFS 接进 boot。`devfs_init.cpp` 里一个 boot 钩子 `devfs::init()`:构造 DevFs(注入 `SerialConsoleSink`)、`mount()` 建标准节点、`vfs_mount_add("/dev", ...)` 注册到 VFS 的 mount 表。`init.cpp` 在 ext2 挂了根 `/` 之后调它(`init.cpp:137`)——boot 装配就多一行。

`make run` 起 QEMU,boot 序列会打:`[VFS] ext2 mounted at /` → `[DEVFS] mounted at /dev (3 nodes)`。从这一刻起,`/dev` 是个真实的挂载点,`ls /dev` 见到 null/zero/console。

> 一个验证上的细节得提醒:`run-kernel-test` 的 test kernel 走的是 `main_test.cpp`,**不调** boot 那个 `devfs::init()`——所以 run-kernel-test 验不了「boot 真挂了 /dev」,它只证 devfs_init.cpp link 进 test kernel 没把构建弄坏。「boot 真挂 /dev」要靠 `make run` 起真内核冒烟,看那行 `[DEVFS] mounted`。DevFS 核心逻辑(设备 ops、lookup、stat)则由 host 单测 + kernel 内的 `test_devfs` 罩着,这俩跟 boot 无关。

## 验证

三层验证。

**第一层:host 单测,核心逻辑。** `test/unit/test_devfs.cpp` 十九个 case:mount 建出三个标准节点、`lookup("null")`/`("zero")`/`("console")` 各命中对的 inode、null 读给 EOF 写丢弃、zero 读给零写丢弃、console write 走 mock sink(断言写下来的字节对)、stat 填对 `st_rdev`/`st_mode`、`/dev` 目录 readdir 列出节点、mount 幂等(重复 mount 不累加节点)。这层靠的就是 `CharSink` 解耦——mock sink 让 console 的派发在 host 上可断言。`./build/test/test_devfs` 跑下来全绿。

**第二层:kernel 内 test_devfs。** `kernel/test/test_devfs.cpp` 七例,在内核态跑同一套核心逻辑(设备行为、lookup、stat),证它在内核环境也对。

**第三层:boot 冒烟。** `make run` 起真内核,看 `[DEVFS] mounted at /dev (3 nodes)`,然后 `ls /dev` 见 null/zero/console,`echo hi > /dev/null` 不报错(null 吃掉)、`cat /dev/zero`(限量)读出零。这一层 headless 自动测试罩不到(test kernel 不走 boot),得 `make run`。

`run-kernel-test-all` 两腿(单核 + `-smp 2`)全绿(含 kernel test_devfs 七例)。

## 这章没做的

- **`/dev/console` 只接了写**:写到串口做对了,但**读**没接(读 console 该读键盘,要接 062 那个 console TTY 的真 stdin)。完整的 `/dev/console` 双向、`/dev/tty`、PTY master/slave,是 TTY 那条线 Phase 2 的事——这一章先把 console 的写和三个基础节点立住。
- **mknod**:还不能用户态 `mknod` 造设备节点。设备是 boot 时 `mount()` 预建的固定三个。
- **块设备 inode**:这一章的设备都是字符设备(`kSIfChr`)。块设备(磁盘那种,要支持 buffer cache)留后面。
- **ProcFS / tmpfs / ext4**:同为虚拟 FS 或 FS 扩展,各自单独的里程碑,不在这一章。

## 小结

- 设备文件没有磁盘后端,「读写」它是触发设备动作。DevFS 是纯内存虚拟 FS,把设备行为包成 device inode。
- 加设备靠给 `InodeOps` 写子类(Null/Zero/Console/Dir DevOps),基类接口一行不动——设备号 `st_rdev` 收进 ops 子类的 `stat()` override,不加 `Inode` 字段。这是「加新东西靠子类、不动基类」的套路,也是能跟别的改动并行的前提。
- 设备写串口离不开 `Serial`(host 链不了),所以抽 `CharSink` 接缝:核心逻辑只认 `CharSink`,内核注入真串口 sink、host 注入 mock,派发逻辑 host 可测。配套有 §14 文件 gate(devfs.cpp host 可链 / devfs_init.cpp kernel-only,源码零 `#ifdef`)。
- boot 接线:ext2 挂 `/` 后 `devfs::init()` mount DevFS 到 `/dev`,`ls /dev` 见 null/zero/console。
- `/dev/console` 只接了写;读(接 console TTY stdin)、PTY、mknod、块设备留后面。
