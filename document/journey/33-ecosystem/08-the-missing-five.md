---
title: "08 · 十四位客人,缺的五件"
description: "点名的单子从当年的判例原样传来:uname、id、whoami、pwd、true、false、sleep、env、hostname、echo、cat、wc、ps、free,十四位 busybox applet 当试金石,考法也是老模子,逐位 fork 加 execve,退出码对表,false 专等退出码一,当负向对照。当年头一场考九位过五位缺,号位一位没欠,五位全是自己错退,欠的料四样:/proc 的挂载、/proc 布告栏上的 meminfo、passwd 与 group 两册户口,连 cat 要嚼的文件都没进盘,机制测绿替代不了真二进制的验收,环境件由此立名。这一遍考卷原样重考,咱们拿 strace 跟在客人身后看它开谁家的门:id 与 whoami 认户口本,ps 认 pid 目录,free 认 meminfo 加 sysinfo 那个号。家底逐件点:/proc 开机即挂免修,户口本上一章的种子已上盘,meminfo 与 pid 目录是假文件与真终端那一卷活着的镜子那一章写好的,真正空着的是 mounts 与 uptime 两张纸,这一章补上,字由挂载表与单调钟当场生成。重考十四位,退出码逐位对上。布告栏有了纸,户口本上了盘,当家的可以上座了。"
chapter: 33
order: 8
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - ecosystem
  - busybox
  - procfs
---

# 十四位客人,缺的五件

点名的单子,咱们不用现编,它是从当年的判例里原样传下来的一张。上面十四个名字,咱们挨个念:uname、id、whoami、pwd、true、false、sleep、env,一路念到 hostname、echo、cat、wc、ps、free。清一色 busybox 的 applet,一个名字一样本事,同一具身子,个个身板小,个个是真程序。当年拿它们当试金石,试的就是一件事:真客人进了门,机器伺候不伺候得起。

考法也是当年定下的模子。咱们一位一位跑,fork 出去,execve 换上客人的身子,waitpid 收退出码,串口看真输出。false 在单子上专门占一席,它的营生就是失败,退出码对上一才算过,负向的对照不能缺。

咱们把时间拨回第一遍,看当年的头一场考。那时候盘上的 busybox 还是静态的身子,每一批号位也都有机制测守着,案案绿,看上去万事俱备。真客人一进门,九位过,五位没过。五位栽跟头的姿势出奇地一致:内核没崩,号位没缺,客人是自己错退的。顺着串口翻案,根子一样一样浮上来,没有一样长在内核里。案卷上记的欠料是四样:/proc/meminfo、/etc/passwd、/etc/group,还有 /proc 的挂载。同一页还记了一处考卷自带的马虎:cat 要嚼的文件,也没进盘。陈设补齐,再考,十四位全过,free 念出了页管理器的真数:`Mem: 9437184 1092600 8344584`。

当年的案卷里,笔者给这一场落过一笔:机制测绿,只证明号位的逻辑对,证明不了真程序跑得起来。真二进制的验收,替代不了。这一课咱们连考卷一起收下。

这一类东西,也得有个名字。客人进门问的不止号位,还有家里的陈设:号位是内核答话,陈设是家里拿不拿得出户口本、布告栏这些册子。内核一个号都没欠,欠的全在盘与家里的布置上。这一类东西,咱们给它立名,叫环境件。

考卷传到这一遍,原样重考。动考之前,咱们把当年的欠料对到自家家底上,一件一件点。当年案卷记了料、没记到人,这一遍也顺便补上:哪位客人认哪样家什,不用猜,在咱们的开发机上拿 strace 跟着它跑一趟就有数。strace 是跟在程序身后记系统调用的工具,开了谁家的门、问了哪个号,一条不漏。咱们看 id:头一扇门开的是 /etc/passwd,跟着再开 /etc/group,两册翻完才报身份。whoami 只敲 passwd 一家的门。ps 开的是 /proc,顺着目录一位一位数,数出一位,翻它的 stat 与 cmdline 两页。free 进门问过 sysinfo 那个号,那个号是假文件与真终端那一卷补上的,问完还要开 /proc/meminfo,MemTotal 与 MemFree 两个词,它要拿去算那行报告。

家底对下来,情况比当年体面。/proc 的挂载免修:文件世界起手就把 /、/dev、/proc 三座挂上,布告栏开机就在墙上,说的就是 /proc,墙上的纸不存底稿,谁去读,字才当场生成。户口本两册,上一章烤盘时当种子进了盘,您看就这两行:

```text
root:x:0:0:root:/root:/bin/sh
root:x:0:
```

uid 0 在第一册里挂着 root 的名字,在第二册里挂着 root 的组,id 认人,认的就是这两行。meminfo 与 ps 要翻的 pid 目录也不用现写:假文件与真终端那一卷活着的镜子那一章,把它们写好了,纸上的字读取那一刻现场生成,事实变了,咱们读到的字跟着变。pid 目录的门槛,代码拢共这几行:

```cpp
        unsigned long tgid = 0;
        if (!parse_whole_decimal(path, tgid) || !tgid_lives(source_, tgid)) {
            return base::KernelError::kNotFound;
        }
```

咱们细看它在拦什么:名字解析成号,人还得活着。人不在了,目录答的是查无此人,不拿旧事实搪塞,ps 数到的每一位,都是当下真活着的。

欠料点完,咱们数下来,三样有着落,真正空着的,是布告栏上两张纸:mounts 与 uptime。补纸的活,落在名册这两行:

```cpp
    {.name = "mounts", .type = InodeType::kFile, .step = kMountsIno - kRootIno},
    {.name = "uptime", .type = InodeType::kFile, .step = kUptimeIno - kRootIno},
```

字照活着的镜子那一章定下的章法来,不存底稿,当场写。接线处,咱们一眼能看到两位供字的:

```cpp
        .mounts =
            [](auto*, char* out, unsigned long capacity) {
                return FileWorld::self().mounts_text(out, capacity);
            },
        .uptime_ns     = [](auto*) { return syscall::MonotonicNanoseconds(); },
```

mounts 的每一行由挂载表现场生成,一座一行,谁、挂在哪儿、什么型、读写什么权。uptime 由单调钟折算成秒,小数点后跟两位,后头还跟着一个数,记的是空闲时间。咱们没有空闲的记录,如实写零。布告栏上三张纸到齐:meminfo、mounts、uptime。

咱们把考卷重开。认人的两位,id 与 whoami,串口上交的答卷是这样:

```text
/ # id
uid=0(root) gid=0(root)
/ # whoami
root
```

十四位挨个过,退出码逐位对表,false 照旧交一。您自己在壳里一位一位敲也一样,退出码用 echo $? 看。当年卡住五位的那些陈设,这一遍都在家里了。

咱们把考卷收档。布告栏有了纸,户口本上了盘,当家的可以上座了。
