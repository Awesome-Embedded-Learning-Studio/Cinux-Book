---
title: "08 · 柜台翻新"
description: "柜台一口气添五件业务:open 2、close 3、mkdir 83、unlink 87、getdents 217,号码全照 Linux,三块砖一格没改。分发表开到 218 格,越界空槽一律 -ENOSYS;错误名册从六格添满十格,ErrnoFrom 一处翻成负 errno。客人的路径指针不盲信,CopyUserPath 查边界、内核栈落地。"
chapter: 14
order: 8
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - syscall
  - filesystem
---

# 柜台翻新

上一卷柜台开张,业务一共四件:read 管读,write 管写,yield 管让位,exit 管退场。这一卷,文件世界进了门,柜台一口气添五件:open 管开,close 管关,mkdir 管建目录,unlink 管删名字,getdents 管念目录。号码照旧不由咱们定:open 是 2,close 是 3,mkdir 是 83,unlink 是 87,getdents 是 217——Linux x86_64 的号表上它们坐在哪,咱们就坐哪。卷首亮过的三块砖,这一章兑现头一块:编号那块砖,一格没动。

新业务进了门,家当也得重新摆。五件新来的,照老样子往原来的文件里挤,四件老业务加五件新的,九件一间屋,添一件就得进去搅一回,咱们这才动的工。这一回一号一间:每件业务一个 `.cpp`,配一个小头文件,门脸统一成 `HandleX` 加三个一样宽的机器字参数。机器字说的就是 `Word` 这个类型,`unsigned long long` 的别名,64 位的宽度正好一个寄存器躺一个。用不满三个的业务,空着的参数也照递,反正座位是统一的。表也重排了:

```cpp
constexpr auto kHandlers = [] {
    std::array<Handler, kSyscallCount> table{};
    for (const Row& row : kRows) {
        table[std::to_underlying(row.number)] = row.handler;
    }
    return table;
}();
```

号码和函数的对应写在一张小表里,编译期把它铺进一个 218 格的定长数组。getdents 的号远在 217,数组就得开到 218,号与号之间的空档,咱们一格都不省。客人报的号越了界,或者报了个编了号没人坐的空槽,一律 `-ENOSYS`:柜台不装懂,家法没改。

出错的报法也得翻新,咱们把两本册子分开数。头一本是内核里头的错误名册,开张那天一共六格,这一卷添满十格:`kAlreadyExists`、`kNotADirectory`、`kIsADirectory`、`kDirectoryNotEmpty`,四张新面孔全是文件世界带来的。另一本是柜台上的负 errno,上一卷只立了三格——没这项业务的 `ENOSYS`,号牌不对的 `EBADF`,指针越界的 `EFAULT`。这一卷添满十格,词表上的原词全请了进来。两本册子说的不是一种话,中间得有人翻译,这桩活本卷立成一件公共零件 `ErrnoFrom`:查无此人翻成 `ENOENT`,内存吃紧翻成 `ENOMEM`,名字已经有人占了翻成 `EEXIST`,半道踩着文件翻成 `ENOTDIR`,对着目录办文件的差事翻成 `EISDIR`,屋里还有人删不得翻成 `ENOTEMPTY`——六格逐一翻,只有其余的零星几种,落回 `EINVAL`。十种负 errno,每一种都是 Linux 词表上的原词,负 errno 那块砖,一块没动。

五件新业务里,open 最有看头,咱们把它从头走一遍。客人的第一件递来的东西不是路径,是路径的地址——一个指针。内核不盲信客人的指针,老底线柜台开张头一天就立了,这一卷把它落成一件公共零件:

```cpp
long long CopyUserPath(unsigned long long source, char* out, unsigned long cap) {
    if (source >= kUserAddressLimit) {
        return -kEfault;
    }
    auto const* text = reinterpret_cast<const char*>(source);
    for (unsigned long index = 0; index < cap; ++index) {
        out[index] = text[index];
        if (out[index] == '\0') {
            return index == 0 ? -kEinval : static_cast<long long>(index);
        }
    }
    return -kEinval;
}
```

地址越过了用户半区的边界,咱们当场回 `-EFAULT`。逐字节往内核的落点搬,见到 NUL 收工。落点是哪?调用者递进来的 `out`,open 给它备的是内核栈上一块 96 字节的格子——路径的长度上限,名字那一章立过法度的,这里正好用上。空串不收,超长不收,都回 `EINVAL`。

路径落地,后头就顺了:门面 resolve,后端 lookup。查无此人、而客人的 flags 里带着创造的旗子,就建个文件再走一遍——`kOpenCreat`,十六进制的 `0x40`,Linux 的 open 旗子里管创造的就是它,咱们认的也只有创造这一档,多出来的位这一卷不认识,也不拦。节点到手,往当前任务的文件册里领一个号,这一领还有一份引用跟着落定,细节下一章讲。

五件业务,件件有座。可座上坐的号——文件描述符,后头咱们按行话叫它 fd——住在哪、管多少、怎么还?下一章,咱们把册子摊开。
