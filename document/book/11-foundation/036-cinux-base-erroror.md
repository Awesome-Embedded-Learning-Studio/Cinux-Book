---
title: 036 · Cinux-Base 与 ErrorOr
---

# 036 · 给错误一个名字,给公共类型一个家:Cinux-Base 与 ErrorOr

> 先给你看个场景。你写了个用户程序,调 `read()`,内核返回 `-1`。然后呢?是文件读到尾了(EOF)?盘 IO 出错了?文件根本不存在?你没权限?——`-1` 把这四件完全不同的事揉成了一个数,你对着日志里一句 `[SYS_MKDIR] failed` 干瞪眼,不知道下一步该查哪。
>
> 这一章治两件越往后越扎手的事。第一,给内核的错误**一个名字、一个类型**——`ErrorOr`,让"出错了"不再是含糊的 `-1`,而是"什么错"。第二,把散在内核各处、东一份西一份的公共类型(`StringView`、`Span`、`RingBuffer`、`CRC32`…)收进一个共享库 `Cinux-Base`,别再每个子系统自己造一遍。这两件是后面文件系统、多进程、网络那些大特性的地基——地基不夯实,越往上晃得越厉害。

## 先看清病:-1 是个什么都能装的筐

在只有只读 ext2 + 单进程 shell 的小体量下,"出错返 `-1`、调用方看着办"勉强够用——出错路径就那么几条,`-1` 大致等于"没找到 / 读崩了 / 你给错了"三合一。可一旦系统要长大,这套就崩了。三个具体的疼:

**三态歧义。** `int read(...)` 这个返回值,`-1` 是错、`0` 是 EOF、正数是字节数。调用方得记着这套不成文的约定,稍不留神就把 EOF 当成错误处理了。

**错误信息丢了。** `-1` 不会告诉你"没找到"还是"盘崩了"还是"权限不够"。排查时日志只剩一句含糊的失败,对着猜。

**类型不帮你。** `-1` 是个 `int`,编译器没法在你**忘了检查错误**时提醒你。它就是个普通的返回值,你爱忽略就忽略,忽略了就把一个错误值一路传进文件系统深处,某天炸在一个莫名其妙的地方。

这三疼,根源是同一个:**错误没有身份**。它只是个约定俗成的数,没有名字、没有类型、没有"非处理不可"的强制性。

## ErrorOr:让错误变成类型

解法是给错误一个类型。`ErrorOr<T>`(`third_party/Cinux-Base/include/cinux/expected.hpp`)——一个**值或错**的判别联合体:要么持有一个 `T`(成功),要么持有一个 `Error`(失败),用一个内部标志区分。核心长这样:

```cpp
// expected.hpp
template <typename T>
class ErrorOr {
    ErrorOr(T value);      // 成功:存值
    ErrorOr(Error err);    // 失败:存错

    bool ok() const;       // 成了吗
    T&   value();          // 取值——失败时调它,直接 assert 给你看
    Error error();         // 取错
};
```

那个 `Error` 不是个数字,是个有名字的枚举:

```
Ok / OutOfMemory / InvalidArgument / NotFound / IOError / AlreadyExists /
PermissionDenied / WouldBlock / BufferOverflow / NotImplemented /
BrokenPipe / ConnectionRefused / TimedOut / Busy
```

配一个 `error_string(Error)`,把 `Error::NotFound` 翻成 `"NotFound"`。从此日志里再也不是光秃秃的 `-1`,而是"这是个 NotFound"——一眼就知道往哪查。

两个设计决定值得讲:

**为什么不抛异常?** 内核没有运行时,异常要靠 unwind 表和栈展开,那是用户态 C++ 的奢侈品,内核背不起。`ErrorOr` 是个判别联合体,**零额外开销**,还能把"这是个可能失败的调用"明明白白写进函数签名里——光看 `ErrorOr<Inode*> lookup(...)`,你就知道它可能失败、失败时拿到的是个 `Error`。

**`value()` 失败时为什么 assert?** 这是刻意的。它把"你忘了先 `ok()` 检查、就直接取值"这种最常见的误用,从"静默用一个错误值"变成"当场炸给你看"。在内核里,炸在 assert 上,比把一个垃圾指针一路传到文件系统深处要好排查一万倍——编译器和 assert 替你盯着,而不是等用户态收到一个莫名其妙的 `-1`。

## 三种"会失败的调用",三种 ErrorOr

会失败的调用,失败的样子各不相同,`ErrorOr` 用三种形态对应。看 `kernel/fs/ext2_common.hpp` 的真实签名:

```cpp
ErrorOr<int64_t> read (const Inode*, uint64_t off, void* buf, uint64_t cnt);  // 读:返回字节数
ErrorOr<void>    stat(const Inode*, struct stat* st);                          // 查属性:只关心成不成
ErrorOr<Inode*>  lookup(...);                                                  // 找一个对象:成功返指针
```

- **`ErrorOr<int64_t>`**:像 `read`/`write` 这种"返回一个数量"的。成功时 `value()` 是字节数,`0` 干干净净表示 EOF——不再是"0 到底是 EOF 还是错"的歧义;
- **`ErrorOr<Inode*>`**:像 `lookup`/`create` 这种"返回一个对象"的。失败时不用再靠"返 `nullptr`"这种和"合法的空值"混在一起的约定;
- **`ErrorOr<void>`**:像 `stat`/`unlink` 这种"只关心成不成"的。`ok()` 就行,不占返回位。

这套用下来,错误有了名字、有了类型、有了"非处理不可"的强制性——三态歧义、信息丢失、编译器帮不上忙,三个老病一起治。

## 用户态看不懂 ErrorOr:syscall 关口的翻译

到这,内核内部全是 `ErrorOr`,干净。可用户程序看不懂 `ErrorOr`——它是个 C++ 类型,而系统调用是 ABI,用户态可能是 musl、busybox 写的 **C 程序**。Linux 的 ABI 约定是:syscall 失败返回**负的 errno**(比如 `-ENOENT`、`-EACCES`),C 库的 `strerror(errno)` 能把它说成人话。

所以内核在 syscall 这道关上做一次**翻译**。`kernel/errno.hpp` 有个 `to_errno`:

```cpp
// kernel/errno.hpp —— 把内核的 Error 翻成 POSIX errno
constexpr int to_errno(cinux::lib::Error e);
// NotFound→ENOENT、PermissionDenied→EACCES、IOError→EIO ……
```

syscall handler 失败时这么写(`kernel/syscall/sys_mkdir.cpp`,真实代码):

```cpp
auto parent_result = fs->lookup(parent_buf);
if (!parent_result.ok()) {
    return -to_errno(parent_result.error());   // Error 翻成 -errno,交给用户态
}
cinux::fs::Inode* parent = parent_result.value();  // ok 了才敢取值
```

这套翻译是**双向**的好:内核内部全程 `ErrorOr`(错误有名字有类型,编译器帮你盯);用户态看到的还是标准 `-errno`(musl 的 `strerror`、busybox 的 `perror` 全照常工作,不用适配)。`ErrorOr` 这个 C++ 类型被这道翻译关**死死挡在内核里**,绝不泄到 ABI。

这背后是一条值得记住的原则:**内核用什么语言、什么范式,是它自己的事;但它对外的 ABI,得跟 Linux 对齐。** 内核内部用 C++ 的 `ErrorOr` 提升工程质量,对外老老实实讲 Linux 的 `-errno`——两不耽误。

## Cinux-Base:公共类型的家

第二个病:公共类型各处重复。在 `ErrorOr` 之前,`StringView`、`Span`、`RingBuffer` 这些东西在内核里东一处西一处地各写一份,口径还不一样。这一章把它们收进一个独立的共享库 `Cinux-Base`(`third_party/Cinux-Base`),21 个头文件,大致四家:

| 家 | 例子 | 干什么 |
|---|---|---|
| 控制 / 错误 | `expected`(`ErrorOr`)、`optional`、`function` | 表达"可能失败/可能空/回调" |
| 字符串 / 视图 | `string_view`、`span` | 只读地看一段内存,不分配 |
| 容器 / 结构 | `ring_buffer`、`intrusive_list`、`bitmap` | 不分配的容器(键盘、pipe 要用环形缓冲) |
| 算法 / 工具 | `crc32`、`checksum`、`endian`、`bit_ops` | 校验、字节序、位运算 |

你不用记每一项。记一句话:**它是内核的公共地基,后面每加一个子系统,都从里面拿东西,而不是自己再写一遍。**

### 引入一个"严纪律"的库:别让它替你的代码定纪律

引入 `Cinux-Base` 时有个**特别值得讲**的工程细节。这个库自带一份很严的编译开关(`-Wpedantic -Werror -Wold-style-style-cast -Wshadow` 之类)。最直觉的接法是 `add_subdirectory(Cinux-Base)`,把它当一个普通子项目链进来——**但偏偏不这么干**。看 `third_party/CMakeLists.txt`:

```cmake
# 只取 include 路径和 .cpp 源文件,不 add_subdirectory(Cinux-Base)——
# 免得它的 INTERFACE 编译开关(-Wpedantic -Werror ...)泄漏进 big_kernel_common,
# 把内核里早就写好的代码瞬间拧出几百个 warning-as-error。

target_include_directories(big_kernel_common PUBLIC ${CINUX_BASE_DIR}/include)
file(GLOB_RECURSE CINUX_BASE_SOURCES ${CINUX_BASE_DIR}/src/*.cpp)
```

原因:`add_subdirectory` 会把库的 `INTERFACE` 编译选项**传染**给内核的对象库,而内核里那些老代码,经不起 `-Werror` 这么一拧。所以这里只取两样:**头文件路径**(让内核能 `#include <cinux/expected.hpp>`)+ **它的几个 `.cpp` 实现**(直接编进内核对象库),不让它成为一个独立 target。

换句话说,`Cinux-Base` 在这里是**"只取所需、不连纪律"**地被吸收:它的头随便用,它的少数实现直接长进内核,但它那套严编译纪律**不替内核定**。等内核代码慢慢打磨到能扛 `-Werror` 了,再考虑合成一个 target。这种"先隔离依赖的纪律、后慢慢收敛"的手法,引入任何带严纪律的第三方库时都值得照搬。

## 两个真坑

**坑一:重构一个接口,grep 调用方别只信一种写法。** `InodeOps` 的方法有两种调法——通过指针的 `inode->ops->read(...)`(箭头),和通过局部对象的 `ops_obj.read(...)`(点号)。重构接口时只 grep 箭头形态、漏了点号形态,是常见的错。靠谱的做法是两种都查,或者干脆靠编译器把它们一个个揪出来——漏改的会成为编译错(算走运),而不是藏到运行时的逻辑错。

**坑二:不是所有"能上 ErrorOr 的"都"值得上"。** `fork` 这一处就故意**没**迁成 `ErrorOr`,留在老的 errno 层。原因是个有意思的耦合:`fork` 的子进程返回值是在**汇编里锻造**的(`fork_child_trampoline` 用 `xorq %rax,%rax` 让子进程的 `fork()` 看到 0)。一旦改成 `ErrorOr<int>`,这个 `rax=0` 会让"成功/失败"标志位变成"失败",子进程**误以为自己 fork 失败了**。要修就得拿汇编去拼 `ErrorOr` 的二进制布局——汇编死耦合 C++ 对象布局,纯成本零收益。所以 `fork`、`execve`、`waitpid` 这几个本来就用结构化 errno 的,留在 errno 层。**教训:asm 紧耦合的地方,动它之前先算清成本,别为了"统一"硬上。**

## 验证

这是一章重构,验证靠"构建 + 看签名 + 亲手触发一个有名字的错误",不是跑个用户功能。

```bash
# 内核内部:签名是不是都 ErrorOr 了
grep -rnE 'ErrorOr<(void|Inode\*|int64_t)>' kernel/fs/
# 期望:read/write/stat/lookup/create/mkdir 都返 ErrorOr<...>

# syscall 关口:是不是都走 to_errno 翻译
grep -rn 'to_errno' kernel/syscall/
# 期望:各 syscall 失败路径 return -to_errno(...)
```

构建:

```bash
cmake -B build -S . && cmake --build build -j$(nproc) > /tmp/b.log 2>&1; echo "build=$?"
```

想亲眼看"错误有了名字":对一个**不存在的路径**做 `mkdir`(让 `lookup` 返 `NotFound`)。在 `ErrorOr` 之前,你只能看到一句含糊的失败和 `-1`;现在日志里是命名的 `NotFound`,用户态收到的是标准 `-ENOENT`,`strerror(errno)` 直接说出 "No such file or directory"。

如果你想体会 `value()` 的 assert 有多实在:临时在某个 syscall 里把 `if (!result.ok())` 检查去掉、直接调 `result.value()`,然后构造一个失败场景跑一遍——内核会 assert 在 `expected.hpp` 的 `value()` 里,栈回溯直指"你在失败路径上取了值"。**这就是"错误变成类型"最实在的回报:你忘了检查,它当场炸给你看,而不是把垃圾值一路传进文件系统深处。看完记得改回来。**