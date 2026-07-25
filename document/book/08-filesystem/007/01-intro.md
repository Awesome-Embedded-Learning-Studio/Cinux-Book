---
title: 01 · 导引:点亮什么与为什么
---

# 导引:点亮什么与为什么

> 这章没有 006 那种跌宕的崩溃调试(它没留下 notes),但有两个同样值得讲的设计取舍:路径规范化怎么保证 `cd /..` 不越过根、以及为什么第一个用户进程得临时「造」一个 Task 才能让新 syscall 跑起来。

## 这一章我们要点亮什么

在 006 的写能力之上,补三件让文件系统「好用」的事。

第一,**路径解析模块化**。新建 `kernel/fs/path.{hpp,cpp}`,提供 `path_canonicalize`(把 `.`、`..`、重复斜杠折叠成规范绝对路径)和 `path_resolve`(把相对路径按工作目录拼成绝对路径)。再建 `kernel/syscall/path_util.{hpp,cpp}`,提供 `resolve_user_path`:把 006 各 syscall 里内联的「用户指针 canonical 校验」收进来,并叠上这一章新加的「按 cwd 解析 + 规范化」——一个函数换掉五段重复校验,还让所有路径 syscall 自动支持了相对路径。(拆父目录/叶子名的 `split_pathname` 007 仍让它内联在各 syscall,没动它;它消费的正是 `resolve_user_path` 的输出。)

第二,**per-process 工作目录**。`Task` 结构加一个 `cwd[256]` 字段,记录当前进程的工作目录;新增 `sys_chdir`(改 cwd)、`sys_getcwd`(读 cwd);为了让这套机制在第一个用户进程上就能用,还得给 `Scheduler` 加一个 `set_current`,并在 `launch_first_user` 里临时造一个 Task。

第三,**stat**。定义 `struct stat`(沿用 Linux x86_64 布局),给 `InodeOps` 加第 7 个虚方法 `stat`,让 ext2 把磁盘 inode 的字段翻译成 `stat`;新增 `sys_stat`(按路径查)、`sys_fstat`(按 fd 查)。

验收点:shell 里能 `cd /etc`、`pwd` 显示 `/etc`、`stat /hello.txt` 打出文件大小和类型,相对路径的命令能正确解析。

## 为什么现在需要它

为什么紧跟 006。006 把「写」补齐后,文件系统在功能上已经完整——能建、能写、能读、能删。但「完整」不等于「好用」。一个每次都得敲绝对路径、查不到文件信息的文件系统,只是个能跑的内核接口,不是给人用的系统。007 把它从「能用」推向「好用」。

还有一笔技术的账,和前几章一脉相承:**去重**。006 那会儿,`sys_creat`、`sys_mkdir`、`sys_rmdir`、`sys_unlink`、`sys_open` 五个系统调用,每个开头都内联了一份一模一样的用户指针 canonical 校验(那段 `bit47`/`upper` 判断)。五份重复,改一处要改五遍。007 把这段校验收进 `validate_user_ptr`,再包上「按 cwd 解析 + 规范化」做成 `resolve_user_path`:一个函数换掉五段重复校验,还让所有路径 syscall 自动获得了相对路径支持。这是个典型的「加新功能的同时还清技术债」——我们要加 cwd,正好把指针校验公共化。需要说明的是,`resolve_user_path` 收的只是「校验 + 解析 + 规范化」;找最后一个 `/` 拆父目录和叶子名的 `split_pathname`,仍内联在每个 syscall 里(它要的输入恰好是 `resolve_user_path` 产出的规范绝对路径),这章没把它公共化。

而且 stat 不只是给用户看的信息。它把原本藏在 ext2 磁盘 inode 里、只有 ext2 驱动自己知道的元数据(模式、链接数、大小、块数),通过一个统一的结构暴露给 VFS 层和用户态。从此上层不用直接碰 ext2 的内部结构,问一句 `stat` 就行。这是抽象边界往前推的一步。
