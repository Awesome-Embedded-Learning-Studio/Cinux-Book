---
title: 02 · 主线一:从字符串规范化到 vfs_lookup 组件遍历
---

# 主线一:从字符串规范化到 vfs_lookup 组件遍历

### 以前 `path_resolve` 干了什么、没干什么

[path.cpp](../../../kernel/fs/path.cpp) 的 `path_resolve(cwd, path, out)` 做的事:把相对路径拼到 cwd 上,再 `path_canonicalize` 压掉 `.`/`..` 和多余斜杠,输出一个绝对规范路径字符串。比如 `path_resolve("/a", "b/../c")` → `/a/c`。

它没干的事:**没有真的去找任何 inode**。它只产出一个字符串,不验证 `/a/c` 这条路上的 `a`、`c` 是不是真的存在、是不是目录、有没有符号链接。真正「字符串 → inode」的步骤散在各 syscall 里(`fs->lookup(rel_path)`),而且 `lookup` 是把整条相对路径丢给底层文件系统一次性的——底层要么自己走完、要么走不全。

这套在「没有符号链接、单挂载点」时凑合能用。可一旦 `/sbin/init` 是个指向 `/bin/busybox` 的符号链接,`execve("/sbin/init")` 就废了——`fs->lookup` 看到 `init` 这个目录项,不知道它是链接,直接当普通文件加载,失败。

### `vfs_lookup`:挂载点感知的组件遍历

[vfs_lookup.cpp](../../../kernel/fs/vfs_lookup.cpp) 重写了路径解析,核心是一个**按组件(component)遍历**的循环。`/a/b/c` 被拆成 `a`、`b`、`c` 三个组件,逐个 `lookup_child`:

```cpp
// resolved 是规范化的绝对路径;vfs_resolve 找到它落在哪个挂载的哪个 FileSystem
FileSystem* fs = vfs_resolve(resolved, &rel);   // rel = 挂载点后的相对部分
Inode*      cur = fs->lookup("").value();        // 从挂载根开始

while (*p != '\0') {
    // 切下一个组件 [p, p+comp_len)
    Inode* child = fs->lookup_child(cur, p, comp_len);   // 一层一层问
    // ... 符号链接处理 ...
    cur = child;
    p += comp_len;
}
```

为什么要一层层、而不是整条丢给底层?因为**符号链接**和**挂载点**都发生在某一层:

- **符号链接**:某一层解析出来是个链接,就得读出它的目标,把目标拼回路径里(绝对目标从根重开,相对目标拼到当前父目录),重新 canonicalize、重新解析。这必须逐层做,因为不知道哪层会踩到链接。
- **挂载点**:`vfs_resolve` 把路径前缀对应到挂载的 FileSystem,组件遍历在那个 FileSystem 内走。跨挂载点的路径(挂了 A 再在 A/a 挂 B,走 `/a/b/...`)靠每次循环重新 `vfs_resolve` 当前规范路径来切对的 FileSystem。

### 符号链接:splice 目标 + 重启 + 环检测

follow 符号链接是这套遍历最绕的部分。踩到一个链接时:

```cpp
if (child->type == InodeType::Symlink) {
    // 读出链接目标(快链接在 i_block[] 里内联,慢链接在数据块)
    auto n = child->ops->readlink(child, target, ...);
    // 把目标拼进「剩余路径」:
    //   绝对目标(/xxx):从根重开,replace 整个 resolved
    //   相对目标(xxx):拼到当前父目录路径后面
    splice_target_into(resolved, target, /*剩余未解析的尾巴*/);
    path_canonicalize(resolved);
    if (++depth > 40) return Error::Loop;   // 环检测:最多 follow 40 层
    restarted = true;                       // 外层 for(;;) 重新从头解析
    break;
}
```

**重启**是关键:follow 一个链接后,目标可能又是一条多层路径(甚至跨挂载点),不能「接着当前层往下走」——得拿拼好的新规范路径**从头重新** `vfs_resolve` + 组件遍历。外层一个 `for(;;)` 跑到不再 restart 就是最终结果。

**环检测**:符号链接可以互相指(`/a → /b`,`/b → /a`),无限 follow 就是死循环。跟 Linux 一样用一个深度计数,封顶 40(超过就是 `ELOOP`)。40 这个数是 Linux 的 `MAXSYMLINKS`,够任何合法用例、又能在合理步数内判定「这八成是环」。

> 一个细节:组件遍历里每一层要先 `lookup_child` 拿到子 inode,**再用子 inode 的 type 判断是不是链接**。所以 `lookup_child` 得能返回「这个目录项是链接」这个事实——这要求 inode 有 `InodeType::Symlink` 这个类型(以前 ext2 的符号链接被诚实但无奈地标成 `Unknown`,因为枚举里没有 Symlink)。这章顺手把 `InodeType` 补上 `Symlink`,让链接在类型层面可见。

## 主线二:inode 引用计数——谁在用我、最后一个关了才 release
