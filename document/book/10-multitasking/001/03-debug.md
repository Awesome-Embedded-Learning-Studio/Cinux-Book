---
title: 03 · 调试现场:fork 还没让子进程「返回 0」
---

# 调试现场:fork 还没让子进程「返回 0」

按惯例这一节该讲真实踩坑。但 034 这个 tag **没有留下调试笔记**(`document/notes/034/` 是空的,context pack 的 notes 也是空)。按写作契约,没素材就不硬造——我不会编一段「某天 QEMU 里炸了 #GP,然后我发现……」的虚构故事来凑数。

但这不代表这一节没东西可讲。恰恰相反,把源码读细之后,034 有一个**真实存在、源码可证、却和教科书直觉相悖**的设计缺口,值得摆到台面上。这一节就讲它。

## 没有 notes,但有真相:034 的 fork 还没让子进程「返回 0」

`fork(2)` 的 man page 写得明明白白:**fork 在父进程里返回子进程的 PID,在子进程里返回 0**。这是 fork 最标志性的语义——同一个调用点,两条返回路径,靠返回值区分「我是谁」。Cinux 的 `process.hpp` 里 `fork` 的文档注释也这么写着:

> *Return value semantics (set in the child's TCB via ctx.rax): Parent: returns child PID; Child: returns 0*

听起来天经地义。但去代码里找「在子进程的 ctx.rax 里写 0」这件事——**找不到**。原因很硬:`CpuContext` 这个上下文结构体**根本没有 rax 字段**:

```cpp
struct alignas(16) CpuContext {
    uint64_t r15, r14, r13, r12, rbp, rbx;   // 只存 callee-saved
    uint64_t rsp, rip;
    // 注:034 时就是上面 8 个字段(64 字节);后来又加了
    // gs_base / kgs_base / fs_base 三个 TLS 基址字段(sizeof 扩到 96)。
    // 但无论哪版,都没有 rax 字段——下面的论证不变。
};
```

它只保存 **callee-saved 寄存器**(r15-r12、rbp、rbx)加 rsp、rip——这是 019 那会儿为协作式调度设计的:切换发生在已知的调用边界上,caller-saved 寄存器(包括 rax)本来就按约定算「已废」,不用存。所以上下文里没有 rax,`fork()` 里也没有任何一处去写子进程的 rax。

那子进程到底怎么「跑起来」的?回头看前面 fork 的第 ⑤⑥ 步:子进程的 `ctx` 是从父进程**整块 memcpy** 来的,内核栈也拷贝了「已用区」,`ctx.rsp` 对齐到新栈同样高度。这意味着子进程被调度器选中、`context_switch` 切进去时,它**从父进程上次被切进来时保存的 rip 处继续**,沿着调度器的返回路径往上走——就像它「刚被调度器唤醒」一样,而**不是**像 fork 系统调用刚刚 return 回来。

换句话说:034 的 fork **确实造出了子进程**(TCB、内核栈、CoW 页表、挂进 children、排进调度器,一应俱全),父进程也**确实**拿到了 `child_pid`(那是 `fork()` 同步返回给父进程的)。但「子进程从 fork 返回 0」这条**控制流**并没有被接上——子进程不会执行 `SYSRET` 带着 rax=0 回到用户态的 fork 调用点,它只是作为一个上下文副本被调度起来。

这不是「偶发性 bug」,而是**这一章的 fork 在语义上还没闭环**。证据不止源码一处,测试也在「让」着它:

- host 单测的 `fork_semantics` 全是**字面量模拟**——`int child_pid = 42; ASSERT_GT(child_pid, 0);`、`int child_return = 0; ASSERT_EQ(child_return, 0);`,根本没真跑 fork。
- 内核测试 `test_dispatch_sys_fork` 的注释直接挑明:测试环境不跑调度循环,`sys_fork`「会优雅失败、返回 -1」,断言放宽到 `ret == -1 || ret >= 0`。

没有一条测试在真刀真枪地验「fork 之后子进程看见 0、父进程看见 PID」——因为这个端到端语义在 034 还立不住。

为什么会留这个缺口?因为要把它补上,得动比较深的东西:要么给 `CpuContext` 加 rax(那就得改 019 以来的整套上下文/切换约定),要么给子进程造一个**手工的内核栈帧**,让它的 `ctx.rip` 指向一个「把 rax 设成 0 再走系统调用返回路径」的小 trampoline。两条路都不是「顺路」能干的活,而 034 这一章的重心是「把五大原语和 CoW 铺起来」,子进程返回值的闭环被自然地推到了后面。

这正是「调试现场」该传递的东西:不是「我踩了个坑然后修了」那种已经愈合的伤,而是**还没填、却在源码和测试里肉眼可见**的坑。而且它不是孤例——034 的进程三件套普遍处于「搭好骨架、尚未通电」的状态:fork 的子进程返回值没闭环、CoW 的 `handle_cow_fault` 没接进 `#PF`(真写 CoW 页会 fatal_halt)、`waitpid` 还是非阻塞。这些都不是偶发 bug,而是这一章有意识地把「把原语和机制铺好」做完了、把「让它们端到端跑通」留给了后面。顺带也提个醒:别盲信头文件里的文档注释,`process.hpp` 那句「set in the child's TCB via ctx.rax」描述的是一个**还没实现**的意图,结构体里压根没有 rax。注释是愿望,代码才是事实。
