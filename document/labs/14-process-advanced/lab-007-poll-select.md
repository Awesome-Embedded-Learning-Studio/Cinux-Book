---
title: Lab 007 · poll / select 双源多路复用:pipe + AF_UNIX 同时等
---

# Lab 007 · poll / select 双源多路复用:pipe + AF_UNIX 同时等

> 007 章把 poll/select 的源码过了一遍:`do_poll_core` 的三段循环、`poll_events` 二合一虚方法、统一 park 防 lost-wakeup、poll vs select 两套 ABI 共享核心。这个 lab 不发答案,只给路径——咱们用 poll(2) 同时等「一个 pipe 读端 + 一个 AF_UNIX socket 读端」,亲手验证多路复用端到端跑通,再用 select(2) 重写一遍验证两个 ABI 行为一致。所有断言挂在 kernel 测试侧的 `test_poll.cpp` 已有 9 例上(就绪语义 / 有限 timeout 真 park + timer-wake / 事件唤醒 role-play),lab 只补「两个不同类型 fd 同时等」这一层用户态视角的折腾。

## 你要确认的事

开始之前,先在 Book 工作树(`/home/charliechen/Cinux`)上核这几样源码都在、行号对得上 007 章:

1. `kernel/syscall/poll_core.cpp` 存在,`do_poll_core` 入口在 `:144`,park 块入口在 `:180`。
2. `kernel/syscall/sys_poll.cpp` 存在,`sys_poll` 在 `:31`,`kPollMaxFds=64` 在 `:29`。
3. `kernel/syscall/sys_select.cpp` 存在,`sys_select` 在 `:69`,`kSetBytes=32B` 在 `:41`。
4. `kernel/test/test_poll.cpp` 存在,9 个用例在 `:303-317` 注册成 `Poll / Select Tests (028e)`。

```bash
# 在 Book 工作树根目录跑
ls kernel/syscall/poll_core.cpp kernel/syscall/poll_core.hpp
ls kernel/syscall/sys_poll.cpp kernel/syscall/sys_select.cpp
grep -n "do_poll_core" kernel/syscall/poll_core.cpp | head -3
grep -n "run_poll_tests\|RUN_TEST" kernel/test/test_poll.cpp | head -12
```

如果上面四样有对不上的,先回头核路径——007 章的链接全是 `kernel/syscall/poll_core.cpp` 这种,别被旧 dev note 误导。

## 第一步:读懂 do_poll_core 的三段循环

`do_poll_core`(`poll_core.cpp:144-218`)是 007 章的脊柱。先读一遍主循环,搞清三段时序:

```bash
sed -n '150,165p' kernel/syscall/poll_core.cpp
```

重点看三处:

- Pass 1 在 `:152-158`:扫所有 fd 调 `poll_one`(只查就绪、不注册),累加 `revents != 0` 的 fd 进 `ready`。
- 就绪返回在 `:159-161`:`ready > 0` 立刻返回就绪 fd 的个数。
- timeout 判定在 `:162-164`:`!infinite && monotonic_ns() >= deadline` 返 0。

**思考题**(自己答,别往下翻答案):如果 Pass 1 用 `poll_one` 时**注册了** waiter(传非 nullptr),会发生什么?(提示:`poll_one` 是「纯查询、不睡」语义;注册了 waiter 却不睡,谁来 detach?POLLHUP/POLLNVAL 这类 always_bits 还能正确透传吗?)

## 第二步:多 fd 同时等的最小证据

`test_poll_two_fds_one_ready`(`test_poll.cpp:168-189`)是 007 章「多 fd 同时等、只就绪的计返回值」的最小证据。先读一遍:

```bash
sed -n '168,189p' kernel/test/test_poll.cpp
```

核三件事:

- 两个 pipe(`rfd1`/`rfd2`),只有第二个 push 了数据(`push_write(wfd2, "x", 1)` 在 `:173`)。
- poll 两个 fd(events=POLLIN,timeout=0 立即返回),`do_poll_core(p, 2, 0)` 返回 1(`:183`)。
- 第一个 revents 保持 0(空 pipe 没就绪,`:184`),第二个 revents=POLLIN(`:185`)。

这条用例守的是「**返回值是就绪 fd 的个数,不是第一个就绪 fd 的下标**」——多 fd 同时就绪会全部填好 revents 并计入返回值。这里只有 1 个就绪所以返回 1,但若两个 pipe 都 push 数据,返回值会是 2、两个 revents 都非零。

**自己改一次**:把 `:173` 那行复制一份给第一个 pipe(`push_write(wfd1, "y", 1)`),改完跑 poll 测试。预期返回值变成 2、两个 revents 都=POLLIN。这反过来证明「多 fd 同时就绪全计入返回值」这条语义。**验完务必 `git checkout` 还原**——这是临时改动,不进 main。

```bash
# 跑 poll 测试的具体命令取决于 Cinux 的测试入口
# 一般是 make test_poll 或进入 kernel/test 目录跑
# 看一眼 kernel/test/Makefile 或 README 找入口
```

## 第三步:有限 timeout 真 park + timer-wake

`test_poll_finite_timeout_parks_then_returns_zero`(`test_poll.cpp:194-204`)是有限 timeout **真 park + timer-wake** 的端到端证据。读一遍:

```bash
sed -n '194,204p' kernel/test/test_poll.cpp
```

这条用例的妙处:它不靠 role-play(不像第五步那条手动驱动 wake),它**真跑** park 路径——空 pipe + timeout=30ms,`do_poll_core` 进 park 块:关 IRQ → `prepare_to_wait` 翻 Blocked → `register_all` 挂 read 队列 → `timer_queue_arm(30ms)` → `schedule_blocked` 真切走。30ms 后 timer tick 调 `Scheduler::unblock` 把 poller 翻 Ready,返回 0、revents=0。

这里有个 007 章点出的 stale:头注释 `poll_core.hpp:17-20` 还写「有限 timeout 只能 yield 自旋、真 timer-wake 是 DEBT」——但这条测试**就是**有限 timeout 真 park + timer-wake 的端到端证据。亲手核一下实现:

```bash
sed -n '188p'     kernel/syscall/poll_core.cpp   # timer_queue_arm 真调了
sed -n '207p'     kernel/syscall/poll_core.cpp   # timer_queue_disarm 配对
sed -n '17,20p'   kernel/syscall/poll_core.hpp   # 头注释却写 DEBT
```

两边对不上。以 `.cpp` 为准——`timer_queue_arm`/`disarm` 实打实调了,有限 timeout 是真 park 不是 yield 自旋。这条测试就是反证。如果你愿意,顺手把 header 那段 stale 注释修对(改成「finite timeout parks via timer_queue_arm,真 timer-wake 已落地」),提交一笔文档修复——但那是单独的 commit,不混进 lab。

## 第四步:事件唤醒 role-play

`test_poll_write_wakes_registered_poller`(`test_poll.cpp:260-299`)是「producer wake 把挂在 read 队列上的 poller 叫醒」整条链路的手动重放。读一遍,重点看 commit 序列:

```bash
sed -n '260,299p' kernel/test/test_poll.cpp
```

四步时序(在 `NoRescheduleGuard` 下手动驱动,因为测试 harness 单线程跑不了真阻塞循环):

1. `:276` `prepare_to_wait(poller)` 翻 Blocked。
2. `:278` `poll_events(ri, poller, &registered)` 注册到 pipe 的 read 队列(registered=true,mask=0 因为空)。
3. `:284-285` peer write 一字节 → `Pipe::write` push 数据 + `wake_one(read_waiters_)`。
4. `:287` 断言 `poller->state == Ready`——wake 命中。

**思考题**:为什么这条测试要套 `NoRescheduleGuard`?如果去掉会怎样?(提示:`schedule_blocked` 在 `no_reschedule_depth_ == 0` 时真切换任务;单线程 harness 切走了没人叫醒,测试挂死。`NoRescheduleGuard` 让 `schedule_blocked` 变 no-op,这样测试能手动驱动 commit 序列观察状态翻转。)

这条 role-play 在生产路径(busybox nc/wget/sh smoke)里跑的是**真**阻塞循环——`schedule_blocked` 真切走,producer 的 wake_one 真把 poller 叫醒上 CPU。测试侧的 role-play 只是 harness 妥协。

## 第五步:poll vs select 两个 ABI 行为一致

007 章声明「poll 和 select 两套 ABI、一个核心」——`sys_poll` 直接搬 pollfd 数组,`sys_select` 把 fd_set 位图翻成 pollfd 再喂同一个 `do_poll_core`。这一步用源码核行为一致。

**核 sys_poll 的路径**(`sys_poll.cpp:31-56`):

```bash
sed -n '31,56p' kernel/syscall/sys_poll.cpp
```

三步:`copy_from_user` 整块搬 pollfd 数组 → `do_poll_core` → `copy_to_user` 整块回填 revents。pollfd 是 8B 紧凑结构,直接搬。

**核 sys_select 的翻译路径**(`sys_select.cpp:114-165`):

```bash
sed -n '114,137p' kernel/syscall/sys_select.cpp   # fd_set -> pollfd 翻译
sed -n '144,165p' kernel/syscall/sys_select.cpp   # revents -> fd_set 回填
```

翻译规则:

- read 位置位 → POLLIN
- write 位置位 → POLLOUT
- except 位置位 → POLLPRI
- 回填:POLLIN/HUP/**ERR** 都进 read set(反直觉但符合 Linux)

**核那条反直觉语义**(`sys_select.cpp:155-157`):

```cpp
if (readfds != 0 &&
    (rv & (cinux::fs::kPollIn | cinux::fs::kPollHup | cinux::fs::kPollErr))) {
    fd_set_bit(rd, fd);  // POLLHUP/ERR 也进 read set
}
```

理由:一个写端关闭的 pipe,poll 在读 fd 报 POLLHUP,select 会把这个 fd 在 readfds 置位,让 app 醒来 read 拿 EOF 或错误,而不是永远阻塞。这条语义的依据是 Linux man select(2):「a file descriptor that has reached end-of-file will be reported as ready for reading」。

**自己验**:在 Cinux 的 QEMU 里跑真二进制验证两个 ABI 行为一致。最小可复现入口是:进 QEMU shell,跑 busybox 自带的 poll/select(`busybox nc -U /tmp/sock` 会同时等 stdin + socket),或自己写一个调用 poll(2) 的最小 freestanding testbin 用交叉工具链编进 initramfs。用 poll(2) 同时等「一个 pipe 读端 + 一个 AF_UNIX socket 读端」:

- (a) 先 pipe + socket 各开一对 fd,填 pollfd 数组(events=POLLIN)。
- (b) `poll(NULL, ..., -1)` 无限等,在另一个 shell 里 `echo x | nc -U /tmp/sock`(或写一个 echo 进 pipe 的命令)看 poll 醒来报 pipe fd 的 POLLIN。
- (c) fork 子进程往 socket 写(或后台进程 `nc` 往 socket 推),看 poll 醒来报 socket fd 的 POLLIN。
- (d) timeout=500ms 空等,验证返 0。
- (e) 换 select,FD_SET/FD_CLR/FD_ISSET 走一遍,验证两个 ABI 行为一致(就绪的 fd 在 readfds 置位)。

验点:poll 返回值 = 就绪 fd 个数、非就绪 fd 的 revents 保持 0、select 的 readfds 只有就绪位置位、POLLHUP 场景(关写端)在 select 的 read set 也置位。lab 走真二进制端到端跑,不靠 kernel test harness。

## 第六步:核对 always_bits 透传

007 章声明「POLLERR/POLLHUP/POLLNVAL 三位无条件透传,即使用户没在 events 里请求」。这一步用源码核:

```bash
sed -n '47p' kernel/syscall/poll_core.cpp        # always_bits 定义
sed -n '77,92p' kernel/syscall/poll_core.cpp     # poll_one: wanted = events | always_bits
```

三位来源:

- **POLLHUP**:writer 关了 → 读端 EOF。pipe 是 `!writer_open_`(`pipe.cpp:383`)。
- **POLLERR**:reader 关了 → 写端再写会 SIGPIPE。pipe 是 `!reader_open_`(`pipe.cpp:408`)。
- **POLLNVAL**:fd>2 且 fd 表无条目。`resolve_fd`(`poll_core.cpp:72`)派发到 `FdKind::kNone`,`poll_one` 返 `kPollNval`。

`test_poll_closed_fd_pollnval`(`test_poll.cpp:217-232`)守 POLLNVAL:close 一个 fd 后 poll 它,revents 必须=POLLNVAL、返回 1。`test_poll_writer_closed_pollhup`(`:238-250`)守 POLLHUP。这两条加起来守住 always_bits 透传的两位(POLLERR 在 pipe 写端的 `poll_write_events` 里,没单独测试,但同款机制)。

**自己验 + 思考**:把 `test_poll_data_ready_pollin`(`:131-141`)的 events 参数从 `kPollIn` 改成 `kPollOut`(只请求 POLLOUT,不请求 POLLIN),再 push 数据。预期 revents 会是什么?读 `poll_one` 那行 `poll_events(...) & wanted`,`wanted = events | always_bits`。POLLIN 既不在 `events`(你只请求了 POLLOUT)、也不在 `always_bits`(always_bits 只有 ERR/HUP/NVAL),所以 mask 里的 POLLIN 位会被 `& wanted` 清零——revents 不报 POLLIN,即使 fd 实际有数据就绪。这条反直觉但符合 poll 语义:**fd 就绪的位必须同时被用户请求才报**,except always_bits 三位无条件透传。你自己改一下、跑一下确认这个清零行为。

## 收尾:把 007 章的声明逐条对上

跑完上面六步,回头逐条核 007 章的「咱们要点亮什么」七条,每条都能在源码或测试里找到证据:

| 007 章声明 | 证据位置 |
|---|---|
| 多路复用这一层:`do_poll_core` 三段循环 | `poll_core.cpp:144-218`、`test_poll.cpp:168-189` 多 fd 同时等 |
| level-trigger,不是 edge | `poll_core.cpp:150` 无界 for 顶无条件重扫 |
| poll_events 二合一虚方法 | `inode.hpp:190-211` 签名 + 头注释、「二合一」契约 |
| always_bits 透传 | `poll_core.cpp:47` 定义、`:82` `wanted = events | always_bits` |
| 统一 park 防丢唤醒 | `poll_core.cpp:180` park 块、`InterruptGuard` + `register_all` + `timer_queue_arm`(`:188`) |
| 一个 poller 睡在 N 个队列上 | `poll_core.cpp:97-123` register_all、`:126-140` detach_all、`scheduler_block.cpp:48-70` unblock 幂等 |
| poll vs select 共享核心 | `sys_poll.cpp:31-56` 直接搬、`sys_select.cpp:114-165` 翻译、`do_poll_core` 不知 poll/select |

七条全对上,007 章的「教程即验证」才算闭环。如果某条对不上(比如 grep 不到 `timer_queue_arm`、或者 `register_all` 的行号偏了),回头读 007 章对应小节,看是源码演进了还是章节写错了——教程是 tag-bound 的,以当前工作树的源码真值为准。

### 进一步的折腾(可选)

- 把 `kPollMaxFds`(`sys_poll.cpp:29`)从 64 改成 2,跑 poll 测试——`test_poll_two_fds_one_ready` 这种 2-fd 用例还能过(恰好等于 cap),但任何 3-fd 用例(若有的话)会撞 `-EINVAL`。这能让你亲手触发栈 cap 的边界。
- 读 `sys_select.cpp:180-188` 的 `*timeout` 回填逻辑,对比 Linux man select(2) 的「never increases」语义——为什么 Cinux 照搬这条?(`has_deadline` 分支算 `deadline - now`,从不让 timeout 变大。)
- 读 `Scheduler::unblock`(`scheduler_block.cpp:48-70`)的幂等注释,思考「如果 unblock 不是幂等,双唤醒源会怎样」——fd 那侧叫了一次,timer 这侧又叫一次,后者若 enqueue 就 double-add 进运行队列,栈就乱了。这条注释的 `Idempotent (F4-M4 prepare-to-wait)` 就是防这个。
- grep `SYS_ppoll` / `SYS_pselect6` 在 `syscall_nums.hpp`——确认 Cinux 全无。Linux 的 ppoll/pselect6 在 poll/select 基础上原子换信号掩码,避免「poll 之前信号到了、handler 跑完、poll 又阻塞」的竞态。Cinux 这会儿没做,如实说「未实现」。

这些折腾不要求做完,挑一个顺眼的深挖。007 章主线是 `do_poll_core` 多路复用层 + poll/select 共享核心,这几个延伸是「顺手吃下的扩展 ABI」的入口。