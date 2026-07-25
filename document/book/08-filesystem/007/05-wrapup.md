---
title: 05 · 收尾:验证与下一站
---

# 收尾:验证与下一站

## 验证

007 的测试叫 `test_cwd_stat`,分 host 单测和 QEMU kernel 测试。

**host 单测**(纯函数,不碰硬件):

```bash
cmake --build build
ctest --test-dir build -R cwd_stat --output-on-failure
```

它重点测两块纯逻辑:`path_canonicalize`(喂一批 `/a/b/../c`、`/a/./b//c`、`/..`、`/a/b/../../..` 之类的输入,断言规范化结果,尤其验根目录保护)和 `path_resolve`(相对/绝对路径拼接),以及 `stat` 的字段映射。这些是字符串和结构体操作,host 上就能跑全。

**QEMU kernel 测试**(端到端):

```bash
cmake --build build --target run-kernel-test
```

(和 006 一样,它会先 `regenerate-ext2-image` 重建干净的 ext2 盘。)

**预期现象**。在 shell 里:

```text
$ pwd                  # sys_getcwd → 当前 cwd
/
$ cd etc               # sys_chdir("/etc") → 验证是目录 → 写 cwd
$ pwd
/etc
$ stat motd            # 相对路径解析成 /etc/motd(它确实存在),打出 size、type、inode 号
$ cd /                 # 回根
$ stat /hello.txt      # 绝对路径也能 stat
```

几个能验证的点:`cd` 进非目录会被拒(`sys_chdir` 的类型检查);相对路径能正确解析(依赖 cwd);`stat` 打出的 size 和 debugfs 看到的一致(详见 lab);`pwd` 反映 `cd` 后的状态。

## 下一站

007 让文件系统有了工作目录和文件信息查询,还顺手把路径处理收拢成了公共模块。到此,单个进程视角下的文件系统已经很完整:能读写、能建删、能 cd、能 stat。但你可能已经察觉到一个隐患——这一路下来,我们所有的写操作(`echo >`、`touch`、`mkdir`)都是「直接写盘」,中途要是断了电,文件系统就可能处于半写完的不一致状态(位图改了、inode 没写回,或者反过来)。而且多个操作之间没有任何同步保护。下一章(008)会引入同步原语(spinlock)和一系列内存管理上的安全加固,给这些操作加上「并发安全」和一定的「一致性」保障。怎么加锁、加在哪些临界区,那是 008 的事——我们这一章的文件系统,已经能认得相对路径、能告诉你一个文件长什么样了。

---

**参考**

- POSIX 路径解析(4.11 Pathname Resolution):`.` 指当前目录、`..` 指父目录、根目录的 `..` 仍是根——`path_canonicalize` 的根目录保护由此而来。参见 Open Group Base Specifications。
- Linux man-pages:`chdir(2)`(目标须为目录)、`getcwd(2)`(返回工作目录)、`stat(2)`/`fstat(2)`(`struct stat` 字段含义);syscall 号 4/5/12/79 复用 Linux x86_64:<https://man7.org/linux/man-pages/>。
- Linux `struct stat`(x86_64)字段布局,`stat.hpp` 注释声明"follows the Linux x86_64 convention":<https://man7.org/linux/man-pages/man2/stat.2.html>。
