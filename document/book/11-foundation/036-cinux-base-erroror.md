---
title: 036 · Cinux-Base 与 ErrorOr
---

# 036 · Cinux-Base 与 ErrorOr:在系统长大之前,先把「错误」和「公共类型」扶正

> 得先跟你交个底:这是 **v1.0.0 回迁弧的第一章**,但它**一行让用户能看到的新功能都没有**。我们在 035 把系统推到了多终端——能 fork、能开几个 shell 并行跑;从这一章起,我们要把这条教学线逐步对齐到真正发过 v1.0.0 的那条 CinuxOS 主线。而那条主线回迁过来的第一件事,不是加功能,是**返工两个地基**:把散落各处、各自为政的公共类型收拢进一个共享库(Cinux-Base),把满内核的「出错就 `return -1`」换成类型安全的 `ErrorOr`。
>
> 所以这是一章**重构**(对应验证档里的 B 档:没有新的用户可见现象,但有清晰的 before/after 信号)。它的价值不在「现在能干什么」,而在「后面 SMP、网络、musl 那些大弧,都得站在这两个地基上才扛得住」。先把它扶正。

## 这一章我们要点亮什么

两件看得见的事,外加一条边界。

第一件,**内核从此有了一个共享的类型/工具库**。`ErrorOr`、`StringView`、`Span`、`RingBuffer`、`optional`、`CRC32`…… 这些东西以前在内核里东一处西一处地各写一份;现在它们被收进一个独立的子模块 `third_party/Cinux-Base`,21 个头文件,谁要用谁 `#include <cinux/expected.hpp>`。

第二件,**错误从「裸 int」升级成「类型」**。内核内部一律用 `ErrorOr<T>` 传错误:成功了 `value()` 拿值,失败了 `error()` 拿一个有名字的错误码。`-1` 那套「正数/0/-1 三态歧义」退场。

那条边界:**`ErrorOr` 是内核的私事,绝不泄给用户态**。系统调用是 `ErrorOr` 和用户程序之间的翻译关——trap 入口把内核的 `Error` 枚举经 `to_errno()` 翻成 Linux 约定的 `-errno` 返回给用户。这一层翻译是这一章真正精巧的地方。

## 为什么现在需要它

先说为什么是现在,而不是更早或更晚。

035 之前,内核的错误处理基本是「能返就返个 -1,调用方看着办」。在只读 ext2 + 单进程 shell 的体量下,这套**勉强够用**——出错路径就那么几条,`-1` 大致等于「没找到 / 读崩了 / 你给错了」三合一。可一旦要往 v1.0.0 走(多进程并发、AHCI DMA、后面的网络栈、musl 动态链接),「-1」就崩盘了:

- **三态歧义**:`int read(...)` 返回 `-1` 是错、返回 `0` 是 EOF、返回正数是字节数——调用方得记住这套不成文约定,稍不留神就把 EOF 当错误;
- **错误信息丢失**:`-1` 不告诉你「没找到」还是「盘 IO 崩了」还是「权限不够」,排查时只剩一句 `[SYS_MKDIR] failed`,对着日志猜;
- **类型不帮你**:`-1` 是个 `int`,编译器没法在你忘了检查错误时提醒你。`ErrorOr<T>` 不一样——你不 `ok()` 就敢 `value()`,它直接 assert 给你看。

CinuxOS 那条主线的迁移笔记里把这条铁律写得很直白:**禁异常**(`throw`/`try`/`catch` 一律不许),错误只能走 `ErrorOr`。理由很实际——内核没有运行时,异常的 unwind 表、栈展开都是负担;而 `ErrorOr` 是个判别联合体,零额外开销,还能把「这是个可能失败的调用」写进函数签名里。

至于「为什么不更早做」——因为更早的时候,内核里连 `StringView` 都只有一处在用,提前抽库是过度设计;而现在,FS 层、syscall 层、proc 层都在重复造同样的轮子,抽库的成本被摊薄了,收益开始大于成本。这是个**时机问题**,不是对错问题。

## Cinux-Base:把公共类型扶正

Cinux-Base 是一个**独立的仓库**,以 git 子模块的形式挂在 `third_party/Cinux-Base` 下。打开它的 `include/cinux/`,21 个头文件,大致分四家:

| 一类 | 头文件 | 干什么 |
|---|---|---|
| 控制 / 错误 | `expected.hpp` `optional.hpp` `function.hpp` | `ErrorOr<T>`、`optional<T>`、函数对象(回调用) |
| 字符串 / 视图 | `string_view.hpp` `static_string.hpp` `span.hpp` | 只读视图、定长串、内存视图——不分配 |
| 容器 / 结构 | `ring_buffer.hpp` `intrusive_list.hpp` `bitmap.hpp` `static_hash_map.hpp` | 无锁环形缓冲(键盘/pipe 用)、侵入链表、位图 |
| 算法 / 工具 | `crc32.hpp` `checksum.hpp` `endian.hpp` `algorithm.hpp` `bit_ops.hpp` `random.hpp` `numeric.hpp` `logger.hpp` `scope_guard.hpp` `buffer.hpp` | 校验、字节序、位运算、日志、RAII 守卫 |

> 这一张表不是 API 手册——你现在不用记住每一项。记住一句话就够:**它是内核的「公共地基」,后面每一弧都会从里面拿东西。** 比如 037 弧会让 `pipe` 和键盘驱动复用 `ring_buffer.hpp`;文件系统弧会用 `crc32.hpp` 校验 ext2 超级块。

### 怎么接进构建:一个必须讲的坑

Cinux-Base 自带一份很严的编译开关(`-Wpedantic -Werror -Wold-style-cast -Wshadow` 之类)。最直觉的接法是 `add_subdirectory(Cinux-Base)`,让它作为一个普通子项目链进来——**但这一章偏偏不这么干**。看 `third_party/CMakeLists.txt`:

```cmake
# Exposes only the include path and .cpp sources — does NOT call
# add_subdirectory(Cinux-Base) to avoid its INTERFACE compile flags
# (-Wpedantic -Werror -Wold-style-cast -Wshadow) leaking into
# big_kernel_common and breaking existing kernel code.

target_include_directories(big_kernel_common PUBLIC
    ${CINUX_BASE_DIR}/include
)

file(GLOB_RECURSE CINUX_BASE_SOURCES ${CINUX_BASE_DIR}/src/*.cpp)
```

原因写在注释里了:`add_subdirectory` 会把 Cinux-Base 的 `INTERFACE` 编译选项**传染**给 `big_kernel_common`(内核的对象库),而内核里那些早就写好的代码,根本经不起 `-Werror` 这么一拧——会瞬间爆出几百个 warning-as-error,构建直接挂。所以这里只取两样东西:

1. **头文件路径**(`target_include_directories`):让内核能写 `#include <cinux/expected.hpp>`;
2. **源文件**(`file(GLOB_RECURSE ... src/*.cpp)`):把 Cinux-Base 那几个有 `.cpp` 的实现(crc32、checksum、logger、vformat)**直接编进内核对象库**,而不是让它作为一个独立 target。

换句话说,Cinux-Base 在这里是**「无头无尾地被吸收」**的——它的头随便用,它的少数实现直接长进内核。这是一个**刻意为之的隔离**:把「库的严纪律」和「内核的现状」分开,等内核代码慢慢打磨到能扛 `-Werror` 了,再考虑合 target。这种「先隔离、后收敛」的手法,在引入任何带严纪律的第三方库时都值得照搬。

根 `CMakeLists.txt` 只负责把 `third_party/` 整个挂上来(`kernel/CMakeLists.txt:52` 的 `add_subdirectory(${CMAKE_SOURCE_DIR}/third_party ...)`),其余的隔离逻辑全在 `third_party/CMakeLists.txt` 里。子模块本身用 `git submodule update --init third_party/Cinux-Base` 拉到 pin 死的 commit。

## ErrorOr:把错误变成类型

`ErrorOr<T>` 住在 `third_party/Cinux-Base/include/cinux/expected.hpp`。它的本质是一个**值/错判别联合体**(discriminated union):要么持有一个 `T`,要么持有一个 `Error`,用一个 `is_ok_` 标志区分。核心 API(全是 `constexpr`,零开销):

```cpp
// expected.hpp:97
template <typename T>
class ErrorOr {
    ErrorOr(T value)   : is_ok_(true)  { /* 存值 */ }   // :108 成功构造
    ErrorOr(Error err) : is_ok_(false) { /* 存错 */ }   // :113 失败构造

    constexpr bool ok() const { return is_ok_; }                 // :168
    constexpr explicit operator bool() const { return is_ok_; }  // :171
    T& value();   // :174 —— 失败时调它,直接 assert
    Error error();
};
```

那个 `Error` 是个 `enum class : uint32_t`,一共 14 个变体,每个都有人话名字(`expected.hpp` 上半段):

```
Ok / OutOfMemory / InvalidArgument / NotFound / IOError / AlreadyExists /
PermissionDenied / WouldBlock / BufferOverflow / NotImplemented /
BrokenPipe / ConnectionRefused / TimedOut / Busy
```

配一个 `error_string(Error)`,把 `Error::NotFound` 翻成 `"NotFound"`,日志里再也不是光秃秃的 `-1`。

### 三种用法,看真实签名

这一章把内核里 14 个文件改成了 `ErrorOr`,三种形态各司其职。直接看 `kernel/fs/ext2_common.hpp:35-55` 的真实签名:

```cpp
ErrorOr<int64_t> read (const Inode*, uint64_t off, void* buf, uint64_t cnt);  // 读:返回字节数(0=EOF)
ErrorOr<int64_t> write(Inode*, uint64_t off, const void* buf, uint64_t cnt);  // 写:返回字节数
ErrorOr<void>    stat(const Inode*, struct stat* st);                          // 查属性:只关心成/败
ErrorOr<Inode*>  lookup / create / mkdir(...);                                 // 找/建:返回 inode 指针
```

- `ErrorOr<int64_t>`(全内核 25 处):read/write/readdir 这类「返回一个数量」的——`value()==0` 干干净净地表示 EOF,不再是「0 到底是 EOF 还是错」的歧义;
- `ErrorOr<Inode*>`(19 处):lookup/create/mkdir 这类「返回一个对象」的——失败时不用再用「返 `nullptr`」这种和「合法的空值」混在一起的约定;
- `ErrorOr<void>`(18 处):stat/mkdir/unlink 这类「只关心成不成」的——`ok()` 就行,不占返回位。

> 注意 `value()` 在失败路径上会 `assert`。这是**刻意的**——它把「你忘了检查 `ok()`」从「静默用错值」变成「当场炸给你看」。在内核里,炸在 assert 上比把一个垃圾指针一路传到文件系统深处要好排查一万倍。

## syscall 边界:Error 怎么变回 `-errno`

到这一层为止,内核里全是 `ErrorOr`,干净。可用户程序看不懂 `ErrorOr`——它是个 C++ 类型,而系统调用是 ABI,用户态可能是 musl、busybox 写的 C 程序。**Linux 的约定是:syscall 失败返回 `-errno`**(负数,绝对值是 errno,如 `-ENOENT`、 `-EACCES`)。

所以内核在 syscall trap 这道关口上做一次翻译。新建的 `kernel/errno.hpp:52`:

```cpp
constexpr int to_errno(cinux::lib::Error e);
```

它把 14 个 `Error` 变体一一映射到 POSIX errno(`NotFound→ENOENT`、`PermissionDenied→EACCES`、`IOError→EIO`……)。syscall handler 失败路径上长这样(`kernel/syscall/sys_mkdir.cpp:57`,真实代码):

```cpp
auto parent_result = fs->lookup(parent_buf);
if (!parent_result.ok()) {
    kprintf("[SYS_MKDIR] Parent directory not found for '%s'\n", resolved);
    return -to_errno(parent_result.error());          // Error → -errno,翻给用户态
}
cinux::fs::Inode* parent = parent_result.value();     // ok 了才敢取值
...
auto mkdir_result = parent->ops->mkdir(parent, leaf_name, name_len);
if (!mkdir_result.ok()) {
    return -to_errno(mkdir_result.error());
}
return 0;                                              // 成功才是非负
```

这套翻译的好处是双向的:

- **对内核**:内部全程 `ErrorOr`,错误有名字、有类型,编译器帮你盯着;
- **对用户态**:看到的还是标准 `-errno`,musl 的 `strerror(errno)`、busybox 的 `perror` 全照常工作,无需任何适配。

`ErrorOr` 这种 C++ 类型,被这道翻译关**死死挡在内核里**,绝不泄到 ABI。这是「内核用什么语言/范式是自己的事,ABI 跟 Linux 对齐」这条原则的一次干净落地。

## 踩坑(从 CinuxOS 的迁移笔记里搬来)

这一弧是返工,返工的坑最值得记。三个,全是真的。

**坑一:CMake 别 `add_subdirectory(Cinux-Base)`**。这条前面讲构建时已经说过——它的 `-Werror` 等 INTERFACE 标志会泄漏进内核对象库,瞬间几百个 error。正解:只取 include + glob 它的 `.cpp`。**教训:引入带严纪律的第三方库时,先隔离它的编译选项,别让它替你的代码定纪律。**

**坑二:grep 调用方,箭头和点号两种形态都得查**。`InodeOps` 的方法有两种调法——通过指针的 `inode->ops->read(...)`(箭头)和通过局部对象的 `ops_obj.read(...)`(点号,比如 `test_pipe.cpp` 里的 `PipeReadOps`)。批 2b 只 grep 了箭头形态,点号形态漏改,最后是**编译器**把它们揪出来的(成了编译错而不是逻辑错,算走运)。**教训:重构接口时,grep 别只信一种语法形态;`PipeReadOps`/`PipeWriteOps` 这种「局部实现的 InodeOps 子类」最容易被漏。**

**坑三:`fork` 故意不迁 `ErrorOr`**。这一弧把 FS 全迁了,但 `proc` 层的 `fork` 故意留在 errno 层不动。原因是 `fork` 的子进程返回路径要**在汇编里锻造返回值**(`fork_child_trampoline` 里 `xorq %rax,%rax` 让子进程的 `fork()` 看到 0)。一旦改成 `ErrorOr<int>`,这个 `rax=0` 会让判别标志 `is_ok_=0`,子进程**误以为自己 fork 失败了**。要修就得把锻造指令改成 `movq $0x100000000,%rax` 去拼 `{value=0, ok}`——asm 死耦合 C++ 对象布局,纯成本零收益。所以 `fork`/`execve`/`waitpid` 这几个本来就用结构化 errno 的,留在 errno 层。**教训:不是所有「能上 ErrorOr 的」都「值得上」;asm 耦合的地方,动它之前先算清成本。**

> 还有一条小尾巴:`to_errno` 表和 `proc` 里原有的 `errno_values` 是两套 errno 来源。这一弧故意**不归一**(批 4 让 `to_errno` 自包含,不动 `errno_values`、不碰 proc 测试),归一留作可选清理。重构要懂得「见好就收,别在一个里程碑里顺带改全世界」。

## 验证

这是一章重构,验证靠「构建 + 测试 + 看签名」,不是跑个用户程序。三步:

```bash
# 1. 拉 Cinux-Base 子模块(到 pin 死的 commit)
git submodule update --init third_party/Cinux-Base

# 2. 重新 configure(让 CMake 认 third_party)+ 构建
cmake -B build -S . && cmake --build build -j$(nproc)
# 期望:[100%] Built target big_kernel, 0 error

# 3. 内核测试跑一遍(ErrorOr 迁移后 run-kernel-test 仍应全绿)
cmake --build build --target run-kernel-test
```

再人肉确认两件事,证明「错误真的变成了类型」:

```bash
# 内核内部:签名是不是都 ErrorOr 了
grep -rnE 'ErrorOr<(void|Inode\*|int64_t)>' kernel/fs/
# 期望:能看到 read/write/stat/lookup/create/mkdir 都返 ErrorOr<...>

# syscall 边界:是不是都走 to_errno
grep -rn 'to_errno' kernel/syscall/
# 期望:sys_mkdir/sys_creat/sys_read/sys_stat/sys_getdents/sys_chdir 等失败路径 return -to_errno(...)
```

如果哪天你把一个 `ErrorOr` 失败了却忘了 `ok()` 就去 `value()`——内核会直接 assert 在 `expected.hpp` 的 `value()` 里,栈回溯一眼可见。这就是「错误变成类型」最实在的回报:**编译器和 assert 替你盯着,而不是等用户态收到一个莫名其妙的 -1。**

## 小结与下一站

这一章没给系统加任何新本事,但它在底下换了两根承重柱:

- **Cinux-Base 子模块**——公共类型有了正经的家,后面每一弧都从这里取;
- **ErrorOr + to_errno**——内核内部错误有了名字和类型,在 syscall 关口翻回 Linux 的 `-errno`,谁也不耽误。

它们是 v1.0.0 那些大家伙(SMP、网络、musl、文件系统升级)的地基。地基不夯实,后面每一层都会跟着晃。

下一站 **037** 我们继续在这个地基上干活:把 `RingBuffer`(pipe 和键盘驱动都要用)、内核日志、DMA 池也搬上 Cinux-Base,顺带把块设备的抽象(`IBlockDevice`)立起来——那是文件系统升级的前置。
