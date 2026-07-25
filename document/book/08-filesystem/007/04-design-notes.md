---
title: 04 · 设计现场:cd /.. 与 set_current 的顺序
---

# 设计现场:cd /.. 与 set_current 的顺序

007 没有崩溃调试的 note,但有两个写在代码里的真实隐患,值得拿出来讲——它们都是「要是漏了这一步,功能就错或者炸」的点。

### cd /.. 不能越过根:canonicalize 的根目录保护

`path_canonicalize` 处理 `..` 的那几行,有个不起眼但关键的保护:

```cpp
if (comp_len == 2 && ... '..' ...) {
    if (out_pos > 1) {            // ← 只有不在根时才回退
        --out_pos;
        while (out_pos > 0 && out[out_pos - 1] != '/') --out_pos;
        if (out_pos > 1) --out_pos;
    }
    continue;
}
```

那个 `if (out_pos > 1)` 是给根目录兜底的。`out[]` 一开始就写了 `out[0] = '/'`,`out_pos = 1` 表示「现在只有根」。如果输入是 `/..`(或者 `/a/b/../../..` 这种一路 `..` 想越过根的),处理 `..` 时 `out_pos` 已经是 1(在根),这个 `if` 不成立,直接跳过——结果还是 `/`。

POSIX 对此有明确规定:路径解析时,根目录的 `..` 仍是根,你不能「.. 出根」。如果漏了这个保护,`out_pos` 会被减到 0 甚至下溢,`out[]` 缓冲就会被越界写——轻则路径乱掉,重则写穿栈。这是个典型的「边界条件漏一个就崩」的隐患,代码里用一行 `if` 守住了。

### resolve_user_path 依赖 current,所以必须先 set_current

第二个隐患是上面已经埋下的伏笔。`resolve_user_path` 里有这么一句:

```cpp
cinux::proc::Task* current = cinux::proc::Scheduler::current();
const char* cwd = (current != nullptr) ? current->cwd : "/";
```

它对 `current == nullptr` 有兜底(退回 "/"),所以不会直接崩。但想想:如果第一个用户进程跑起来时没有 `set_current`,`current()` 一直返回 `nullptr`,那**所有**相对路径都会被当成相对于 "/" 解析——`cd etc` 会被当成 `cd /etc`(碰巧可能对),但更微妙的是 `sys_chdir` 后面还要 `Scheduler::current()` 来写 cwd,那个调用 `sys_chdir` 里是 `if (current == nullptr) return -1;`,直接失败——也就是说,没有 `set_current`,`chdir` 永远成功不了,`pwd` 永远显示 "/"。

这就是为什么 `launch_first_user` 必须在跳进用户态前 `set_current(&shell_task)`:不是为了让 shell「能跑」(没有它 shell 也能跑,只是 syscall 行为错),而是为了让 cwd 这套机制有「当前进程」可依附。新功能依赖一个前置条件,补丁就是来满足这个前置条件的。这种「功能正确性依赖一个看似无关的初始化」的依赖关系,比一个直接崩溃更难发现——因为症状是「功能静默地不工作」而不是「炸了」。
