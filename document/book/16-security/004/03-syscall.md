---
title: 03 · 修法一的载体:syscall 切两层
---

# 修法一的载体:syscall 切两层

accessor 准备好了,但还有个现实问题:上一章的 syscall handler 满地都是「直接解引用用户指针」。一个个改成 accessor,工作量是一方面,更麻烦的是——很多 handler **既要碰用户内存,又要 block**。比如 `sys_read`:它要从用户 buf 写回数据(碰用户内存),可 fd=0 读键盘时要 block 等输入。你要是在同一个函数里边 block 边 accessor,就违反了刚立的窗口铁律。

Linux 的解法是**把 handler 切两层**,照搬过来:

- **`do_*_kernel(...)`**:纯 kernel-to-kernel 的逻辑。收的是 kernel 指针 / kernel buf,**可以 block、可以 unmap 旧用户页、可以持任何内核状态**——因为它根本不碰用户内存。测试和内核内部直接调它。
- **`sys_*(...syscall 参数)`**:薄薄一层边界。先用 accessor 把用户参数(path、buf、argv…)搬进内核暂存(小的放栈、大的 `kmalloc` 放堆),再调 `do_*_kernel`。block 发生在 `do_*_kernel` 里(AC=0 安全),回来后 `sys_*` 再用 accessor 把结果搬回用户。

举个具体的——path 家族。原来 `resolve_user_path` 拿到用户 path 地址后,直接 `path[0] == '\0'` 裸读用户字节,然后整条字符串裸遍历。七个 path syscall(open / openat / creat / mkdir / chdir / unlink / rmdir)外加 stat 尾巴全经它,是最大一块裸解引用源头。改造后多了个 `read_user_path`:

```cpp
bool read_user_path(uint64_t path_virt, char* out, size_t cap) {
    if (!cinux::user::access_ok(reinterpret_cast<const void*>(path_virt), 1)) {
        return false;
    }
    size_t len = 0;
    while (len + 1 < cap) {
        char c;
        if (!cinux::user::get_user(&c, reinterpret_cast<const char*>(path_virt + len)))
            return false;
        if (c == '\0') break;
        out[len++] = c;
    }
    char term = 0;
    if (!cinux::user::get_user(&term, reinterpret_cast<const char*>(path_virt + len)))
        return false;
    if (term != '\0') return false;   // cap 内没 NUL,路径太长
    out[len] = '\0';
    return len > 0;
}
```

(`path_util.cpp:16`。)`get_user` 一次读一个字节,每读一字节就是一扇 AC 小窗(开、读、关)。`access_ok` 先把坏地址挡掉,循环逐字节读到 NUL,再确认 NUL 真的在 cap 范围内(防越界)。`resolve_user_path` 改成先 `read_user_path` 把用户 path 暂存到堆上的 `PathBuf`——为什么是堆不是栈?因为 4 KB 的 `char[PATH_MAX]` 加上 canonicaliser 的 scratch,会撑爆 16 KB 的内核栈,这是早先踩过的坑。

read / write / stat / signal / execve / 杂项,每个家族都照这套切:`do_read_kernel(fd, kbuf)` 在内核 buf 上 block,`sys_read` 用 accessor 搬进搬出;`do_stat_kernel(resolved_path, kst)` 做 VFS lookup 写 kernel stat,`sys_stat` 用 `copy_to_user` 搬给用户;`do_execve_kernel(kpath, kargv, kenvp)` 收 kernel 字符串、**可以 unmap 旧用户页**(execve 本来就要摧毁调用方地址空间,这一步必须在没持用户指针的时候做)。block-then-write 的样板就是 `sys_read`:block 在 `do_read_kernel` 里读键盘到内核 buf,runnable 后 `sys_read` 再 `copy_to_user`。

> 切两层不是洁癖,是正确性的载体。没有这层切开,「block 时不持用户指针」这条铁律根本没地方落——你没法在一个既碰用户内存又会 block 的函数里保证窗口不跨 schedule。分层之后,block 永远在 `do_*_kernel`(AC=0),碰用户内存永远在 `sys_*` 的 accessor 小窗(不 block),两者物理隔离。
