---
title: 03 · Cinux-Base 收公共类型,加两个真坑与验证
---

# Cinux-Base 收公共类型,加两个真坑与验证

## Cinux-Base:公共类型的家

第二个病:公共类型各处重复。在 `ErrorOr` 之前,`StringView`、`Span`、`RingBuffer` 这些东西在内核里东一处西一处地各写一份,口径还不一样。这一章把它们收进一个独立的共享库 `Cinux-Base`(`libs/base`),21 个头文件,大致四家:

| 家 | 例子 | 干什么 |
|---|---|---|
| 控制 / 错误 | `expected`(`ErrorOr`)、`optional`、`function` | 表达"可能失败/可能空/回调" |
| 字符串 / 视图 | `string_view`、`span` | 只读地看一段内存,不分配 |
| 容器 / 结构 | `ring_buffer`、`intrusive_list`、`bitmap` | 不分配的容器(键盘、pipe 要用环形缓冲) |
| 算法 / 工具 | `crc32`、`checksum`、`endian`、`bit_ops` | 校验、字节序、位运算 |

你不用记每一项。记一句话:**它是内核的公共地基,后面每加一个子系统,都从里面拿东西,而不是自己再写一遍。**

### 引入一个"严纪律"的库:别让它替你的代码定纪律

引入 `Cinux-Base` 时有个**特别值得讲**的工程细节。这个库自带一份很严的编译开关(`-Wpedantic -Werror -Wold-style-cast -Wshadow` 之类)。最直觉的接法是 `add_subdirectory(Cinux-Base)`,把它当一个普通子项目链进来——**但偏偏不这么干**。看 `libs/CMakeLists.txt`:

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
grep -rnE 'ErrorOr<(void|Inode\*|int64_t)>' kernel/fs/ libs/ext2/
# 期望:read/write/stat/lookup/create/mkdir 都返 ErrorOr<...>(ext2 的签名在 libs/ext2/ext2_common.hpp)

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
