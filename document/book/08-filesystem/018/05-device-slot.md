---
title: 05 · InodeOps::block_device:一个设备身份槽
---

# InodeOps::block_device:一个设备身份槽

## InodeOps::block_device():一个「设备身份」槽

上一节用到的 `block_device()` 这个虚方法,值得单独拎出来讲透——它是整条链能把「路径」转成「设备对象」的**唯一钩子**。先看基类默认实现:

```cpp
/// backing block device for a block-device inode (e.g. /dev/sda in DevFs).
/// sys_mount resolves its source path to an Inode, then calls this to obtain
/// the IBlockDevice to mount.  Default nullptr -- regular files, char devices
/// and pseudo-fs nodes have no block-device backing.  (有删节——省略源码里的内部 milestone 标记。)
virtual cinux::drivers::IBlockDevice* block_device(const Inode* /*inode*/) {
    return nullptr;
}
```

(`inode.hpp:230-236`。)`InodeOps` 是所有 inode ops 的基类——`Ext2FileOps`、`TmpFileOps`、`TmpDirOps`、`ProcStatFileOps`、`DevDirOps`、`BlockDevOps`...全都是它的子类。这个虚方法**默认返 `nullptr`**,只有 `BlockDevOps` override 返 `dev_`(`devfs.cpp:163-164`)。要核「全树多少处 override」,得把 grep 收紧到只匹配代码行,不然文档注释里同含 `block_device` 和 `override` 两个词的那行(`devfs.hpp:151` 的注释)也会被命中——用 `grep -rn 'block_device.*override' kernel/ libs/ | grep -v '///'` 过滤掉文档注释,应见唯一一处真 override:`devfs.cpp:164`。

这个设计的精妙在于「**多态默认值**」:现有所有 `InodeOps` 子类(几十个)vtable 多一个槽,但**行为零变化**——它们继承默认 `nullptr`,不需要写任何代码。`sys_mount` 拿到 `nullptr` 就判 `ENXIO`。好处很明显:**`sys_mount` 不需要知道 DevFS 内部结构、不 down-cast、不硬编码「如果是 DevFs 就走某条路」**,全靠虚分派——加一个新的 FS 子类(比如将来加个 nfs),它的 inode ops 自然继承 `nullptr`,不需要动 `sys_mount` 一行。代价是 `InodeOps` vtable 每类多一个指针槽(几十个虚类 × 一个指针,几百字节,可忽略)。

这是「**给基类加虚方法却不破坏派生类**」的标准手法:默认实现承担绝大多数派生类的正确行为(无块设备),只有真正有块设备的子类 override。对比 Linux 的 `block_device` 是独立的 `struct file_operations`——Cinux 选择复用 `InodeOps` 多态,代价是 `block_device()` 进了所有 inode 的 vtable;好处是 `sys_mount` 一行虚调用就拿到设备,不用类型开关。

> **为什么必须 `NoFollow`?** 看起来多余——`/dev/sda` 谁会做成 symlink?但语义上必要:`Follow` 模式下若末段是 symlink,会被 follow 到目标,目标可能不再是块设备节点,`block_device()` 返 `nullptr` 误判 `ENXIO`。`NoFollow` 保证拿到的是路径末段那个 inode **本身**,不是它指向的东西。和 `readlink`/`lstat` 用 `NoFollow` 同一原理:要看节点本身,不看它指向哪。这条纪律在 `sys_mount.cpp:88` 的注释里明写:「`NoFollow: /dev/sda is the device node itself, not a symlink to follow`」。
