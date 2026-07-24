---
title: 078 · VFS 地基重做:组件遍历、引用计数、inode 缓存
---

# 078 · VFS 地基重做:组件遍历、引用计数、inode 缓存

> 咱们之前的 VFS 能跑,但有三块「凑合」在底下撑着,到了要加缓存(下一章的 dentry cache)、要跑真用户态(busybox 频繁 open/execve)、要在 SMP 下并发访问文件的时候,这三块凑合就成了bug 的温床。这一章把 VFS 的地基重做一遍,把它们换成「正经该长那样」的形态:
>
> 一是**路径解析**——以前 `path.cpp` 只做字符串规范化(拼 cwd、压 `.`/`..`),根本没有「一层层问文件系统拿子 inode」的组件遍历,符号链接更是没人 follow。咱们补一个真正的 `vfs_lookup`:挂载点感知的组件遍历 + 符号链接跟随 + 环检测。二是**inode 引用计数**——以前 inode 没有「几个 fd 正在用我」的计数,谁都能拿到指针、谁都不负责释放,缓存稍一激进就别名释放(use-after-free)。咱们给 inode 加引用计数 + last-close 钩子。三是**ext2 的 inode 缓存**——以前是固定值数组、靠「驱逐覆盖」腾位置,可被覆盖的 slot 可能正被某个活 fd 指着,覆盖完那个 fd 就读到「别的文件的字节」。咱们改成堆分配 + 引用计数驱动的回收,被引用的对象绝不被动。
>
> 三块各自的 punchline:`vfs_lookup` 让 `execve("/sbin/init")` 能 follow 符号链接找到真正的 busybox;引用计数让 pipe 的 `dup` 不再误发 EOF、让缓存返回的指针不会突然失效;ext2 inode 缓存改成堆 + refcount 后,那个「ASAN 都看不见的静默别名 UAF」从结构上没了。

## 这章咱们要点亮什么

1. **路径解析 ≠ 字符串规范化**:`/a/b/c` 要真的从根 inode 出发,一层层 `lookup_child` 拿到 `a`、`b`、`c` 的 inode。`.`/`..` 是字符串层面能压,但符号链接必须「读出目标、拼回去、重新解析」——这是字符串干不了的。
2. **inode 得有引用计数,而且 release 要在「最后一个引用」时才发**:多个 fd(dup/fork)指向同一个 inode,关一个 fd 不能就发 EOF/拆连接——得等最后一个关。这要外层 inode 引用计数 + 锁外 release。
3. **缓存返回的对象必须 pin 住**(引用计数 > 0),不能返回一个「随时可能被驱逐覆盖」的指针。否则调用方拿着指针,缓存转身把那块内存给了别的 inode,调用方就读到错的数据——而且是静默错,工具看不见。
4. **环检测靠深度计数封顶**:符号链接可以互相指(`a→b→a`),follow 时得有一个「最多跟 40 层」的上限,否则死循环。

## 主线一:从字符串规范化到真正的组件遍历

### 以前 `path_resolve` 干了什么、没干什么

[path.cpp](kernel/fs/path.cpp) 的 `path_resolve(cwd, path, out)` 做的事:把相对路径拼到 cwd 上,再 `path_canonicalize` 压掉 `.`/`..` 和多余斜杠,输出一个绝对规范路径字符串。比如 `path_resolve("/a", "b/../c")` → `/a/c`。

它没干的事:**没有真的去找任何 inode**。它只产出一个字符串,不验证 `/a/c` 这条路上的 `a`、`c` 是不是真的存在、是不是目录、有没有符号链接。真正「字符串 → inode」的步骤散在各 syscall 里(`fs->lookup(rel_path)`),而且 `lookup` 是把整条相对路径丢给底层文件系统一次性的——底层要么自己走完、要么走不全。

这套在「没有符号链接、单挂载点」时凑合能用。可一旦 `/sbin/init` 是个指向 `/bin/busybox` 的符号链接,`execve("/sbin/init")` 就废了——`fs->lookup` 看到 `init` 这个目录项,不知道它是链接,直接当普通文件加载,失败。

### `vfs_lookup`:挂载点感知的组件遍历

[vfs_lookup.cpp](kernel/fs/vfs_lookup.cpp) 重写了路径解析,核心是一个**按组件(component)遍历**的循环。`/a/b/c` 被拆成 `a`、`b`、`c` 三个组件,逐个 `lookup_child`:

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

### 以前为什么凑合能跑、什么时候崩

以前 `Inode` 结构没有「当前几个 fd 在用我」这个计数。`FDTable::close(fd)` 直接 `delete` 掉 File 对象、把 fd 槽清空,inode 的生死完全由底层文件系统自己的缓存管(ext2 的 inode 缓存自己决定何时驱逐)。pipe 的 EOF、socket 的 FIN 这种「关 fd 时要通知对端」的逻辑,没有统一的钩子。

这套在「单进程、打开-关闭一一对应」时没事。可一旦 `dup`(同一个 inode 被两个 fd 指着)或 `fork`(子进程继承一份 fd 表):

- pipe 的读端被 `dup` 成两个 fd。关掉一个,旧代码可能就发 EOF 了——可另一个 fd 还在读呢,EOF 是错的。这就是 `ls | grep` 管道偶发崩的根因。
- 任何「缓存返回 inode 指针」的设计(下一章的 dentry cache、ext2 的 inode 缓存),都面临「我把指针给出去了,底层会不会在我用着的时候把它回收掉」——没有引用计数,这个问题没法回答。

### 外层 inode 引用计数 + last-close release

修法是给 `Inode` 加一个**外层引用计数** `refcount`,语义是「当前有几个 File(fd)指着这个 inode」:

```cpp
struct Inode {
    // ...
    uint32_t refcount{0};   // DEBT-023:几个 fd 指着我
};

// file.cpp:进出 FDTable 时 bump/dec
void inode_ref(Inode* i)   { if (i) __atomic_add_fetch(&i->refcount, 1, ...); }
void inode_unref(Inode* i) {
    if (i && i->ops && __atomic_sub_fetch(&i->refcount, 1, ...) == 0) {
        i->ops->release(i);   // 最后一个引用关了 → 通知底层(pipe EOF / socket FIN)
    }
}
```

`alloc`/`dup`/`dup2`/`set` 时 `inode_ref`(新 fd 拿一个引用),`close` 时 `inode_unref`(fd 释放它的引用)。**`release` 只在 refcount 减到 0(最后一个 fd 关了)时才调**——这就是「last-close 语义」,治了 `dup` 误发 EOF 的病。

`InodeOps::release` 是个虚函数,默认 no-op(普通文件、ext2 inode 不需要关 fd 时干啥);pipe 的读/写端 override 它——`release` 时 `release_read_ref`/`release_write_ref`,pipe 内层再判断「这个端的所有 fd 是不是都关了」来发 EOF/BrokenPipe。

> **两层计数**:注意 pipe 这里有**两层**——外层 `Inode::refcount`(这个 inode 几个 fd 在用)+ 内层 `Pipe::read_refs_`/`write_refs_`(这个端几个 inode 在指)。外层到 0 触发 `release`,`release` drop 一个内层引用;内层到 0 才真发 EOF。dup 一个 pipe 读端 fd:外层 refcount 2,内层 read_refs 2。关一个 fd:外层 2→1(没到 0,不 release),pipe 不发 EOF——对。两个 fd 都关:外层 1→0(两次,第一次到 1 第二次到 0)→ 第二次 release → 内层 read_refs 2→1→0 → 发 EOF。

### 为什么 refcount 要原子、release 要锁外

`refcount` 用 `__atomic_*_acq_rel`:fork 之后父子进程的 fd 表在不同 CPU 上可能同时 `inode_unref` 同一个 inode(共享的 pipe 端),非原子就丢更新。`acq_rel` 保证「减到 0 的那个 CPU 看到之前所有对 inode 的写」。

`release` 在 `FDTable::close` 里**锁外**调:close 先在 fd 表锁内把 File detach 出来(`fd→null`),放锁,再 `delete file` + `inode_unref`。为什么?因为 `release` 可能 block(pipe close_writer 要唤醒阻塞的对端、socket 要发 FIN),如果持着 fd 表锁调,整个进程的 fd 表操作都被卡住。锁内只做 detach 这个 O(1) 动作,重活放锁外。

## 主线三:ext2 inode 缓存——从「驱逐覆盖」到「堆 + 引用计数」

### 别名 UAF:最阴的 bug

ext2 的 inode 缓存以前是个**固定值数组** `Ext2CachedInode inode_cache_[N]`,每个 slot 装一个缓存条目。要缓存一个新 inode 而所有 slot 都满了,就**驱逐**一个(slot 0 是根,不动;其他按哈希挑一个),把那个 slot 覆盖成新 inode。

问题:**被驱逐的那个 slot,可能正被某个活 fd 或 VMA 指着**。比如:

1. `open("/bin/ld")` → ext2 把 ld 的 inode 装进 slot 5,返回 slot 5 的 `Inode*` 给 fd。
2. 程序跑别的,触发了对另一个 inode 的缓存,slot 5 被驱逐、覆盖成新 inode。
3. 程序从 fd 读 ld 的内容 → 走的是 slot 5 的 `Inode*` → 可 slot 5 现在是**另一个文件**了 → 读到错的字节。

这个 bug 最阴在:**没有内存错误**(slot 还在、指针还合法、读出来的是合法的别的文件数据),ASAN 看不见、KASAN 看不见,只有「数据偶尔错」这种说不清的症状。它是 GCC 自举时 `ld`/`cc1` 偶发崩的根因之一。

### 结构性修法:堆分配 + 引用计数驱动的回收

[c6abfff / ext2_inode.cpp](kernel/fs/ext2/ext2_inode.cpp) 的 `get_cached_inode` 重写成:**条目是堆分配的对象,对象的地址是它的身份;只要有人引用(refcount > 0),它就绝不被移动、绝不被重填。**

```cpp
struct Ext2CachedInode {
    Ext2Inode        disk_inode;
    Inode            vfs_inode;
    uint32_t         ino{0};
    bool             stale{false};            // 被带外改过(chmod/unlink),命中时原地刷新
    Ext2CachedInode* hash_next{nullptr};      // 分离链 hash(ino % SIZE 桶)
};

Inode* Ext2::get_cached_inode(uint32_t ino) {
    // 1. 查 hash 桶:命中 → 若 stale 原地重读 → inode_ref(给调用方一个引用)→ 返回
    // 2. 未命中 + 软 cap 满:驱逐一个 refcount==0 的(全活就失败,绝不腐蚀)
    // 3. new 一个堆对象,读盘,inode_ref,挂到桶头,返回
}
```

三个关键点:

1. **堆分配 + hash 桶**:对象 `new` 出来,地址固定。用 `ino % SIZE` 的分离链 hash 组织,不再是用固定数组下标。
2. **引用计数驱动回收**:结合上一节的 `Inode::refcount`——`get_cached_inode` 返回前 `inode_ref`(缓存自己持一个引用 + 给调用方一个),调用方 `inode_unref`。软 cap 满时只驱逐 `refcount==0`(没人用的)对象;有活引用的对象绝不驱逐。
3. **`stale` 而不是驱逐**:`chmod`/`chown`/`utimensat`/`unlink` 改了一个**正被引用**的 inode,不能驱逐(有活引用),改成标记 `stale`,下次命中时**原地重读盘**(同一个对象、同一个地址,刷新内容)。只有 refcount==0 的过期条目才直接释放。

这套下来,「被覆盖」从结构上不可能了——活引用的对象地址永不变,调用方手里的指针永远指着自己当初拿的那个 inode。

> **这个修法依赖前两块地基**:引用计数(主线二)让「refcount==0 才能驱逐」可判断;`vfs_lookup` 的组件遍历(主线一)让 `lookup_child` 返回的 inode 能安全穿过缓存层。三块地基是一体的——这也是为什么它们要一起进、不能拆。

## 三块地基拧成一根绳

把这一章的三块放一起看,它们其实是在回答同一个问题的三个层面:**「拿到一个 inode 指针之后,我能用它多久、用它的时候它会不会变」**。

- `vfs_lookup`(主线一)回答**怎么拿到**对的那个 inode(逐层遍历、follow 链接、不会拿错)。
- 引用计数(主线二)回答**拿到之后它多久有效**(只要我持引用,它就不被释放;我放了,最后一个放的人负责通知)。
- ext2 inode 缓存(主线三)回答**底层缓存在这期间会不会捣乱**(堆 + refcount,被引用的对象绝不被动)。

下一章的 dentry cache(缓存解析过的路径)和 flock(文件锁)就直接建在这三块上:dentry 命中返回新引用(主线二)、`invalidate` 配对 unref、缓存项 pin 住 child(主线三的同款思路)。地基打牢了,上面那两个功能就是顺水推舟。

## 范围与边界(诚实说)

- **`vfs_lookup` 不跨进程 chroot**:咱们没有 chroot,路径解析都以全局根为起点;有 chroot 的话每进程根要进 `vfs_lookup` 的入参。
- **引用计数是「外层 fd 计数」,不是「inode 本身的生死」**:inode 的最终释放仍由底层文件系统(ext2 的 inode 缓存)决定;引用计数只管「几个 fd 指着 + last-close release」。一个 refcount==0 的 ext2 inode 仍可能在缓存里(没被驱逐),只是没有 fd 指着它。
- **ext2 inode 缓存的软 cap 是 4096**:`EXT2_INODE_CACHE_MAX`。超过且全活(refcount>0)就缓存失败(返 nullptr),不腐蚀。正常工作集远小于 4096;真超了是容量问题,不是正确性问题。
- **SMP 下的 inode 缓存遍历加锁**:`get_cached_inode` 整个 walk/evict/insert 套 `inode_cache_lock_`(持锁跨读盘 I/O),这是另一弧(并发竞态修复)加的,这章的 cache 重设计本身是单核正确的,加锁是后续。

> 这一章三块地基都有随它们一起进来的机制测(`test_syscall_ext2` 的符号链接 follow/环、`test_sys_pipe` 的 dup last-close EOF、ext2 inode 缓存的别名场景),`run-kernel-test` 跑过、绿。它们是 VFS 这层「能正经用」的底线——后面所有文件系统相关的功能(dentry cache、flock、busybox 频繁 open/execve、GCC 自举跑真编译)都站在它们上面。
