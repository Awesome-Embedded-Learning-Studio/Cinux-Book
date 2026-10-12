---
title: "05 · 一座大门面"
description: "VFS 是路径的门面:客人报绝对路径,它找罩得住的挂载,把剩下的路递给后端。后端是什么,九个方法一份 concept 编译期把关;挂载那一下,EraseFs 一把生成九个无捕获转发函数,后端零手写样板。前缀只认组件边界,/tmp 罩不住 /tmpx;同路多罩,深的赢。"
chapter: 14
order: 5
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - filesystem
---

# 一座大门面

ramfs 站在那儿,客人却够不着它:shell 报的是 `/docs/note.txt` 这样的绝对路径,ramfs 收的是挂载点之后的相对路。中间得有个人翻译,还得替将来打算——真盘来了,客人念的路还是一个样子,接活的却换了人。这一章咱们装一座门面,名字就叫 VFS:客人报路径,它找出罩得住的挂载,把剩下的路递给该接活的后端。后端姓甚名谁,客人从头到尾不知道。

头一件事,立后端的门槛。什么样才算一个文件系统?咱们拿 concept 说话:

```cpp
template <typename Fs>
concept FileSystem = requires(Fs& backend, std::string_view path, /* 其余参数同类 */ FsDirent& entry) {
    { backend.lookup(path) } -> std::same_as<base::Result<FsNode>>;
    { backend.make_file(path) } -> std::same_as<base::Result<FsNode>>;
    // —— 中间五法同款:make_dir、unlink、read、write、read_dir,个个捧回 Result ——
    { backend.retain(node) } -> std::same_as<void>;
    { backend.release(node) } -> std::same_as<void>;
};
```

咱们把九个方法数一遍:lookup 找,make_file 与 make_dir 建,unlink 删,read 与 write 读写,read_dir 念目录,末了是占住与放手两个所有权的手续。前七件业务,条条捧回 `Result`。占住与放手这两件呢,回的是 void——它们没有失败的路可走,管的是上一章那套引用的命,签名对齐形状就够。concept 是编译期把把关,后端就是个普通类,九个名字对得上就够,虚表那套家什用不上。转回本体这一步也没有谁手写,它只发生在生成出来的转发里。ramfs 写的时候压根不知道头顶上有这门亲戚,它只是普通地实现了自己。

门面这边收下后端,得把方法调用转过去。转的手续是机械的,咱们就让它彻底机械——一把生成:

```cpp
template <FileSystem Fs>
[[nodiscard]] constexpr FsOps EraseFs() {
    return {
        [](void* self, std::string_view path) { return static_cast<Fs*>(self)->lookup(path); },
        // —— 余下八个转发同款,一个不落 ——
    };
}
```

咱们看它生成的货色:九个转发函数,个个无捕获,所以个个能落成普通的函数指针。后端自己的指针不藏进函数里,调用的时候当第一个参数递过去。九格的函数表 `FsOps`,加上一份后端指针,就是门面认识后端的全部。擦除发生在挂载那一下,一个后端一辈子只做一回,咱们到挂载的代码里看它:

```cpp
static constexpr FsOps kErased = EraseFs<Fs>();
Slot slot{.ops = &kErased, .self = &backend, .prefix_len = prefix.size()};
```

挂载表本身是个固定八槽的小容器,槽位、占用、插入发最低空号,这套家底咱们在锁座次那一章一起盘。挂载要过的检查有三道:前缀得以斜杠打头,中间不带尾斜杠。同一个前缀挂两回,回 `kAlreadyExists`。八格坐满还来,回 `kOutOfMemory`——丢挂载这样的事,照上一章立的脾气办:宁可回错,也不悄悄咽下。

真正见功夫的是认路。咱们看 `resolve` 怎么挑:它把表上每个活着的槽扫一遍,谁罩得住整条路、前缀又最长,留下的就是谁:

```cpp
if (!path.starts_with(kPrefix)) {
    return;
}
if (kPrefix.size() > 1 && path.size() > kPrefix.size() && path[kPrefix.size()] != '/') {
    return;
}
```

头一关好懂,前缀对不上直接出局。第二关是组件边界,咱们拿例子过一遍:您挂了 `/tmp`,客人递来 `/tmpx`——字符串层面 `starts_with` 是点头的,可 4 号位上的字符不是斜杠,是 `x`,这下当场退回。`/tmp` 罩的是 `/tmp` 和 `/tmp/build`,罩不了 `/tmpx`,邻居家的门牌再像,也不是一户。同一条路要是被两个前缀都罩住呢?深的赢:`/` 在底下垫底,哪天 `/docs` 挂上来,docs 里的路全归它,`/` 形同让位。

找到挂载,咱们还剩一段路要剥:前缀之后的余路,不带前导斜杠,交给后端。余路是空的,您就当客人要的是挂载点自己。送出去的每一路,后端收到的都是自己地界里的相对路,ramfs 的爬树函数从头到尾没见过斜杠打头的路,两边的合同清清爽爽。

门面装好了,后端站着,行李在门口等着进库。行李长什么样、咱们怎么念它?下一章,开箱。
