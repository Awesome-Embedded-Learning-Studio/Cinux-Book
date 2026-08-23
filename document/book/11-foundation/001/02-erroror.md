---
title: 02 · ErrorOr:让错误变成类型,并在 syscall 关口翻译
---

# ErrorOr:让错误变成类型,并在 syscall 关口翻译

## ErrorOr:让错误变成类型

解法是给错误一个类型。`ErrorOr<T>`(`libs/base/include/cinux/expected.hpp`)——一个**值或错**的判别联合体:要么持有一个 `T`(成功),要么持有一个 `Error`(失败),用一个内部标志区分。核心长这样:

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

会失败的调用,失败的样子各不相同,`ErrorOr` 用三种形态对应。看 `libs/ext2/ext2_common.hpp` 的真实签名:

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
