---
title: Lab 001 · Cinux-Base 与 ErrorOr 验证
---

# Lab 001 · Cinux-Base 与 ErrorOr 验证

> 对应章节:`document/book/11-foundation/001/`。
> 验证档:**B 档**(机制/重构)——这一弧没有新用户可见现象,验证靠「构建 + 测试 + 看签名 + 故意触发错误路径」。不要求你从零搭,跟着核对即可。

## 目标

确认三件事:

1. Cinux-Base 子模块正确落地,内核能用上 `#include <cinux/expected.hpp>`;
2. 内核内部的错误签名真的从裸 `int` 换成了 `ErrorOr<T>`,syscall 边界走 `to_errno`;
3. `ErrorOr` 失败时不是静默返垃圾,而是有名字、并在误用时 assert。

## 步骤

### 1. 子模块 + 构建绿

```bash
git checkout 001_foundation_cinux_base     # 切到本 tag 的源码状态
git submodule update --init third_party/Cinux-Base
cmake -B build -S . && cmake --build build -j$(nproc)
```

**期望**:`[100%] Built target big_kernel`,`0 error`。如果 CMake 报找不到 `cinux/expected.hpp`,说明子模块没拉到位,重跑 `git submodule update --init`。

### 2. 看签名:错误真的变成类型了吗

```bash
# 内核内部签名
grep -rnE 'ErrorOr<(void|Inode\*|int64_t)>' kernel/fs/ | head
# 期望:ext2_common.hpp 里 read/write/stat/lookup/create/mkdir 都返 ErrorOr<...>

# syscall 边界的翻译关
grep -rn 'to_errno' kernel/syscall/ | head
# 期望:sys_mkdir/sys_creat/sys_read/sys_stat/sys_getdents/sys_chdir 失败路径 return -to_errno(...)
```

**思考**:为什么 `read` 返回 `ErrorOr<int64_t>` 而不是 `ErrorOr<void>`?——因为 `value()` 要承载「实际读到的字节数」,`0` 表示干净的 EOF,而失败走 `error()`。这正是 `ErrorOr` 相对裸 `int` 的核心收益:**EOF 和错误不再挤在同一个返回值里互相歧义**。

### 3. 故意触发错误路径(看「错误有了名字」)

找一个会失败的路径,观察日志。最简单的是对一个**不存在的路径**做 `mkdir`,让 `lookup` 返回 `Error::NotFound`:

- 在内核测试里(`kernel/test/`)加一条 `mkdir("/no/such/dir/child", ...)`,或者直接在 shell 里 `mkdir` 一个父目录不存在的路径;
- 观察串口日志:你应该看到 `[SYS_MKDIR] Parent directory not found ...`,然后 syscall 返回 `-to_errno(Error::NotFound)` 即 `-ENOENT`。

**对照**:在 035(本弧之前)做同样的事,你只能看到一句含糊的失败和 `-1`;现在错误有了名字(`NotFound`),并且用户态收到的是标准 `-ENOENT`,`strerror(errno)` 能直接说出 "No such file or directory"。

### 4.(可选)体会 `value()` 的 assert

`ErrorOr::value()` 在失败路径上会 `assert`。想亲眼看一眼的话,在某个 syscall handler 里**临时**把 `if (!result.ok())` 检查去掉、直接调 `result.value()`,然后构造一个失败场景跑一遍:

- **期望**:内核 panic/assert 在 `third_party/Cinux-Base/include/cinux/expected.hpp` 的 `value()` 里,栈回溯直指「你在失败路径上取了值」。
- 这就是「错误变成类型」的兜底回报:你忘了检查错误,它当场炸给你看,而不是把垃圾值一路传进文件系统深处。**看完记得改回来。**

## 验收清单

- [ ] Cinux-Base 子模块 `update --init` 成功,`include/cinux/` 下能看到 21 个头。
- [ ] 构建绿,`0 error`。
- [ ] `kernel/fs/` 的关键签名是 `ErrorOr<...>`,`kernel/syscall/` 失败路径走 `-to_errno`。
- [ ] 能说出 `read` 返 `ErrorOr<int64_t>` 里 `value()==0` 和 `error()` 各代表什么。
- [ ] (可选)触发过一次 `value()` 的 assert,并理解为什么这是设计而非 bug。

## 别做这些

- **别**把 Cinux-Base 改成 `add_subdirectory` 链法——它的 `-Werror` 会瞬间淹没内核构建(见章节「踩坑一」)。
- **别**尝试把 `fork` 也迁成 `ErrorOr`——它撞 `fork_child_trampoline` 的汇编锻造(见章节「踩坑三」),这一弧故意不动它。
