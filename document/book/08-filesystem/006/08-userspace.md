---
title: 08 · 接上用户态:sys_creat 与 sys_rmdir
---

# 接上用户态:sys_creat 与 sys_rmdir

### 接上用户态:sys_creat 的链路与 sys_rmdir 的空目录检查

底层都齐了,最后看 syscall 怎么把用户态的路径接到底层。`sys_creat` 是个范本,`sys_mkdir`/`sys_unlink` 结构几乎一样:

```cpp
int64_t sys_creat(uint64_t path_virt, ...) {
    // ① 校验用户态指针是合法的规范地址(canonical)
    if (path_virt == 0) return -1;
    /* bit47 / upper 检查,非规范地址直接拒 */

    // ② 经挂载表解析:得到具体的 FileSystem 和相对路径
    const char* rel_path = nullptr;
    cinux::fs::FileSystem* fs = cinux::fs::vfs_resolve(path, &rel_path);

    // ③ 把路径拆成「父目录路径」+「叶子名」
    char parent_buf[PATH_MAX];
    split_pathname(rel_path, parent_buf, &leaf_name, &name_len);
    //   "etc/foo/bar" -> parent="etc/foo", leaf="bar"
    //   "bar"         -> parent="" (根),    leaf="bar"

    // ④ 找到父目录 inode,调它的 create
    cinux::fs::Inode* parent = fs->lookup(parent_buf);
    cinux::fs::Inode* new_inode = parent->ops->create(parent, leaf_name, name_len);

    if (new_inode != nullptr) return 0;

    // ⑤ create 返回空:文件可能已存在 -> truncate 到 0(POSIX creat 语义)
    cinux::fs::Inode* existing = fs->lookup(rel_path);
    if (existing != nullptr) existing->size = 0;
    return 0;
}
```

两个点值得说。

第一,**指针校验**。用户传进来的是一个 `uint64_t` 地址,内核不能直接信。代码检查它是不是「规范地址」(x86_64 的 canonical address:第 47 位为 0 时高 16 位必须全 0,为 1 时必须全 1)。非规范地址访问会触发 #GP,所以这里提前挡掉。这是用户态和内核态打交道时的基本功——别拿用户给的指针当可信的。

第二,**`split_pathname` 这一步**。它找到路径里最后一个 `/`,把前面截成父目录、后面截成叶子名。注意 006 里这个函数是**每个 syscall 文件内联一份**的(`sys_creat`/`sys_mkdir`/`sys_unlink`/`sys_rmdir` 各有一份几乎相同的 `split_pathname`)。重复,但能用。把它抽成公共的 `path.cpp`,是后面的事(别急,下一章再说)。

`sys_creat` 的第 ⑤ 步对应 POSIX `creat(2)` 的语义:`creat` 一个已存在的文件,不报错,而是把它截断成 0。所以 `create` 返回 `nullptr`(可能因为已存在)时,代码再 `lookup` 一次,把已有文件的 `size` 清零。

`sys_rmdir` 多一道**空目录检查**,而且这道检查的位置值得专门讲——它**在 syscall 层,不在 ext2 里**:

```cpp
int64_t sys_rmdir(uint64_t path_virt, ...) {
    // ...resolve、split、lookup parent...
    cinux::fs::Inode* target = fs->lookup(rel_path);
    if (target->type != InodeType::Directory) { /* 不是目录 */ return -1; }

    // 关键:用 readdir 取第 3 项(index 0=".", 1=".."),有就说明非空
    char check_name[16];
    int64_t rc = target->ops->readdir(target, 2, check_name, sizeof(check_name));
    if (rc > 0) { /* 目录非空 */ return -1; }

    return parent->ops->unlink(parent, leaf_name, name_len);
}
```

它利用了 ext2 `readdir` 的索引约定:`readdir(dir, 0)` 返回 `.`,`readdir(dir, 1)` 返回 `..`,从 `index = 2` 起才是真正的子项。于是 `readdir(target, 2)` 能读到东西(`rc > 0`),就说明这个目录除了 `.`/`..` 还有别的条目,非空,拒绝删除。这是个聪明但有点脆的办法——它强依赖 `readdir` 的 index 语义恰好是 0/1/2+。

这里有个文档和实现不一致的小坑要提醒:`sys_rmdir.cpp` 的文件头注释写着「the backend filesystem is responsible for verifying that the target is an empty directory」(空目录检查由后端文件系统负责)。但代码里检查明明是在 `sys_rmdir` 自己做的,底层的 `Ext2::unlink` 根本不区分文件和目录、也不查空——你拿它删一个非空目录,它会照删不误(把目录项和数据块全释放)。以代码为准:006 的空目录检查在 syscall 层。直接绕过 `sys_rmdir` 调 `unlink` 的话,这道防线就不存在了。这是个值得记下来的「注释漂移」。

四个新系统调用的号码,我们直接复用了 Linux 的:`mkdir=83`、`rmdir=84`、`creat=85`、`unlink=87`。这样将来真要跑为 Linux 编译的简单用户程序,号码能对得上。用户态 `user/libc/syscall.h` 加了对应的 `sys_creat`/`sys_mkdir`/`sys_unlink`/`sys_rmdir` 封装,shell 则加了 `touch`(=creat)、`mkdir`、`rm`(=unlink)、`rmdir`,以及 `echo` 的 `>` 重定向——`echo hi > /hello.txt` 会先 `sys_creat` 建文件、`sys_open(O_WRONLY)` 打开、`sys_write` 写入、`sys_close` 关闭。
