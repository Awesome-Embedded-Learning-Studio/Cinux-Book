---
title: "07 · 名字的相对论"
description: "任务长出 cwd,折路的语义从 shell 下放进内核:FoldOntoPath 折点、弹层,缓冲兼作弹层的栈,shell 自攒的拼路函数整段退休。getcwd 79、chdir 80、reboot 169 同号。stat 坐了 6 号,人家号表上那是 lstat 的座,咱们自己指出来。"
chapter: 16
order: 7
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - storage
  - filesystem
  - syscall
---

# 名字的相对论

上一卷的 shell,报的路都归它自己收拾:参数不带斜杠,自攒的小函数给补上一根,别的就不管了。这一卷的屏幕上,cd /etc 之后一个光杆的 cat motd 也认了——相对路径这一课,补上了。补在哪儿?咱们从旧的样子说起。

旧的样子:那个小函数只管补斜杠,. 和 .. 认不得,您站在哪个目录里它也不知道——家里定的约定,每个客人自己背一遍,背错了算客人自己的。一两个客人还背得动,客人多了呢?每门语言、每个程序,都得自带一份拼路的手艺,拼出来的还各不相同。

这一卷,咱们把约定下放进内核。任务长出的头一件随身家当是 cwd:

```cpp
char cwd[cinux::fs::kFsPathMax] = {'/'};
```

文件册按值住 Task,这一回又添一件:96 字节的工作目录,生来就是根。折路的活,咱们给柜台新开一条走廊:

```cpp
long long FoldUserPath(unsigned long long source, char* out, unsigned long cap) {
    char            raw[fs::kFsPathMax] = {};
    const long long kRaw                = CopyUserPath(source, raw, sizeof(raw));
    if (kRaw < 0) {
        return kRaw;
    }
    const char* const  kBase = raw[0] == '/' ? "/" : CurrentTask()->cwd;
    const base::Folded kFold = base::FoldOntoPath(kBase, raw, out, cap);
    if (!kFold.ok) {
        return -kEnametoolong;
    }
    return static_cast<long long>(kFold.length);
}
```

客人的路径落地,绝对路独自折,相对路折到 cwd 上。折的手艺住在 base,叫 FoldOntoPath,PathCursor 家族的新亲戚:. 折掉,.. 弹一层,弹过根就停在根,再弹也不出圈。有一处咱们想停下来夸一夸:折的缓冲,兼作弹层的栈——名字一段一段 push 进去,段与段之间隔着斜杠,弹一层,就是退回最后一个斜杠的地方。存储和算法住在同一块缓冲里,一个字节都没多花。

走廊一通,旧门全换新把手:open、mkdir、unlink、stat、chdir,进门头一件事全是 FoldUserPath。shell 那个自攒的拼路函数,整段退休——往后客人报什么,内核折什么,咱们把约定收敛成一份,谁也不用再背。

柜台添的号,咱们照例逐个报:getcwd 79,chdir 80,reboot 169,全跟 Linux x86_64 同号。stat 呢——stat 坐 6 号。这里咱们得停一下,当着您的面把一处错认下来:

> 编号那块砖,柜台翻新那一章立过约:号码照 Linux x86_64 的号表落座。这一格没照住:Linux 的 stat 坐 4 号,5 号是 fstat,6 号上住着的是 lstat,咱们把 stat 挂在了 6 号。眼下机器上没有别人家的程序来敲门,号错了暂时没人撞见。错处亮在这儿,不替它圆,改的手续留着往后办。

chdir 与 getcwd 的细节,咱们也过一遍:chdir 折完验一遍,得是目录才放行,验过把折好的路抄进 cwd。getcwd 反着来,把 cwd 抄给客人,塞不下回 `ERANGE`。错误名册这一卷添了两格,`ERANGE` 与 `ENAMETOOLONG`——折出来的路超长,回的是后一个。

cwd 生来就有,stat 报的 mtime 却得现盖——新建文件的那一刻,谁告诉内核现在几点?下一节,咱们问芯片。
