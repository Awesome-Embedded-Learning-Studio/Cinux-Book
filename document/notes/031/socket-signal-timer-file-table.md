# 站 31：socket 收官的等待、计时器与文件表边界

本文记录 2026-10-24 的实施设计与验收边界。代码已实现不代表整站实弹验收完成；结果记录见末节。

## A．本站保留 yield 等待，站 32 统一引入可打断睡眠

本站 `recvfrom`、pipe read、ppoll、nanosleep 保留现有等待形式。接收暂不可用时，在可投递信号到来后返回 `-EINTR`，否则 yield 重试；非阻塞 recvfrom 返回 `-EAGAIN`。检查 pending 使用 `HasPendingSignal(const Task&)`，取得关中断的 signal gate，避免和另一 CPU 的发送或 IRQ 到期置位发生数据竞争。

`SendSignal` 不对普通 `kBlocked` 任务调用 unblock。SIGALRM 的 IRQ 到期路径也只置 pending。默认处置是终止，但执行终止的是任务自身的信号投递路径：blocked 任务需先由原等待事件唤醒，才能处理这个 pending。这样不会污染等待链，但**不具备任意睡眠立即被 SIGALRM/SIGKILL 终止的活性保证**。本站的用户态 ping 等待采用 yield，不依赖这项能力。

控制台另设 `tty_poll` 常驻输入泵，在没有 console reader 时也以有限预算运行行规范；console read 改为非阻塞后端，由既有 syscall read 循环等待。console 的 read、poll、termios/window 操作和输入泵共用 gate，前台 pgid 使用 atomic；发送组信号前先释放 console gate。USB HID 补齐左右 Ctrl 与 Ctrl+Shift 的控制字符转换，保持和 PS/2 的 VINTR/VQUIT/VSUSP 字节一致。否则 QEMU 的 USB 键盘会把 Ctrl+C 当作普通 c，或输入一直滞留到下一次终端 read。

窄切 recvfrom 真睡在技术上可行：让 socket 使用专门的 interruptible 等待队列，并且只允许登记到该队列的任务被信号唤醒。不同等待点使用不同睡眠语义本身没有矛盾。但它需要登记、pending 握手、竞争裁决和摘链机制；下一站 TCP 的超时与多事件等待马上也需要这些机制。本站单独增加一种 socket 私有睡眠协议，教学和验证成本大于收益，因此推迟。

### 站 32 的等待协议

推荐使用**显式等待 token + 阻塞点自摘 + 调度器幂等通知**，而非 SendSignal 注入任意对象的回调：

```cpp
WaitToken token;
PrepareInterruptibleWait(task, queue, token);
SchedulePreparedWait(token);
FinishWait(queue, token);
```

这些是后续接口草图，本批没有添加同名产品 API。具体约束如下：

1. token 带等待代数及 waiting/event/signal/timeout 状态。每轮登记只允许一个通知者赢得转换；调度器只为胜者执行 ready 转换。不能用“目标当前是不是 blocked”替代等待代数。
2. 等待点持对象队列锁入链，在调度器锁和 signal gate 下完成 pending 检查与 prepared 状态发布，然后释放对象锁，再 schedule。检查与发布必须是一次握手，覆盖“检查后信号先到”“事件先到 schedule 后到”。
3. SendSignal 在调度器锁内只通知已登记为 interruptible 的 token，不获取对象队列锁。它不能持 registry lock 调用会取对象锁的任意回调，否则容易形成对象锁→调度器锁→对象锁的环。
4. 正常事件可以摘链再尝试通知 token。若信号先赢，事件方不再 unblock，也不重复计数。任务恢复后持对象锁自摘仍留在队列的节点；**摘链和注销 token 必须先于 syscall 返回或任务退出**。
5. 普通唤醒返回后重查谓词。事件授予资源、信号取消等待的优先级必须明定，不能丢掉已转交的 semaphore token 或 mutex 所有权。

统一锁序为对象队列锁→调度器锁→signal gate；发送者释放 signal gate 后才做其他调度操作。禁止反向持锁调用和持对象自旋锁切换。

改动面：Task 等待 token、Scheduler 的 prepare/try-wake、signals 的可打断登记分支、socket 接收队列、futex 自摘与精确唤醒计数、wait_child，随后才考虑用户态 semaphore 等等待者。Mutex 及内核内部资源等待默认维持不可打断。Completion 若表示 DMA/块请求完成，还需先解决请求取消和缓冲区生命期；仅给 wait 加 EINTR 会使设备继续写入已经离开的栈或复用的请求槽，不能这样改。

验证必须包括 pending-before-park、signal-between-prepare-and-schedule、event 与 signal 双核竞争、timeout 与 signal 竞争、重复 finish/wake、每轮队列为空、rearm 成功、取消后资源计数守恒及退出前摘链。

## B．itimer 的当前实现

号位为 getitimer=36、alarm=37、setitimer=38。仅支持 ITIMER_REAL；ITIMER_VIRTUAL/PROF 返回 EINVAL。ABI 使用两个 16 字节 timeval，it_interval 在前，it_value 在后；所有用户数据使用 accessor。

关键接口为：

```cpp
struct RealTimerValue {
    unsigned long long value_ns;
    unsigned long long interval_ns;
};

void Scheduler::real_timer(Task&, unsigned long long now_ns,
                           RealTimerValue&, bool replace);
void Scheduler::on_timer_tick(unsigned long long now_ns = 0);
```

任务字段保存绝对 deadline 和 interval，统一由 QueueGuard 保护。系统调用按 tgid 找组长的 timer，使同组线程查询/替换同一计时器；到期 pending 当前投给组长。当前信号系统尚无完整的进程级 pending 分派，未实现“组长屏蔽后自动改选其他未屏蔽线程”。ping 单线程路径不依赖该能力。

使用 `MonotonicNanoseconds()`：硬件在场用 HPET，否则用 PIT Tick。PIT 和 AP LAPIC 的 tick 入口都传入同源单调时间；每核时间片计数不充当墙钟。测试可直接传入时间，不等真实中断。

Scheduler 持有 atomic 最近 deadline 下界。tick 未到该下界只做常数时间判断，到期才持 QueueGuard 扫全任务链，任务指针始终不出锁。取消或进程退出可以留下过早的下界，最多导致一次无效扫描；下界不能比实际最早到期值更晚。没有 timer wheel，也没有未绑定生命期的任务裸指针快照。教学规模下，比每次 IRQ 扫全链更合适；有大量并发 TCP timer 时再引入统一有序队列。

到期路径只在 signal gate 下置 SIGALRM，显式忽略 SIG_IGN，屏蔽信号仍积累 pending；不调用可能 resume/withdraw 的通用 SendSignal。QueueGuard 本身关本核中断，其 queue_depth 支持本 CPU 嵌套，因此已有 registry visitor 内的 resume/withdraw 不会再次取得同一底层队列锁。不过这不是在 IRQ 执行所有通用信号行为的理由。

锁序是 registry→signal gate。普通发送、清 pending、exec reset 及安装 action 也遵守 signal gate 的关中断约束，避免 IRQ 打断正在持有同一 gate 的本核任务后自旋死锁。信号提取在同一次 gate 下取 action、摘 pending，后续同号发送不会被末尾的清位吞掉。

单发到期清 deadline。周期按原相位跳到严格晚于当前时间的下一次到期，错过多次只合并为一个普通信号，避免 IRQ 补发风暴。alarm 取消时也清 interval，返回旧剩余秒数向上取整；微秒查询也向上取整，已到期但尚未扫描的 armed timer 报非零最小余量。负数、微秒超界和纳秒转换溢出返回 EINVAL。setitimer 的 old value 与替换在同一锁域；写 old value 失败可返回 EFAULT，此时替换已生效。

fork 新进程不继承 timer，exec 保留 timer，exit 后不继续发信号。最早 deadline 即使已取消，也不会撤销已经置位的 SIGALRM。IRQ 时钟调用处及相关 clock/HPET 实现进入 no-SSE 编译清单，防止在 FPU 保存边界引入寄存器污染。

## C．FileTable 的锁与所有权

表级 gate 使用现有 `base::SpinMutex`，TableGuard 先保存 IF 并关中断，再取 gate，出锁后恢复 IF。这个实现避免让 file_table.hpp 与包含 Task 的 proc/sync.hpp 形成循环依赖。它具备 SMP 互斥和 IRQ 排斥，**不进入 proc::SpinLedger/lockdep 的锁图**；不能把检查版构建成功解释为此锁已获得 lockdep 全覆盖。

所有槽读写和 CLOEXEC 位图访问都取同一锁，包括 get/shared/cloexec。内部只使用 `shared_locked` 和 `detach_locked` 等无锁内芯，不在锁内重入 public 方法。

```cpp
Result<Shared<OpenFile>> FileTable::get(unsigned long);
Result<Shared<OpenFile>> FileTable::shared(unsigned long);
Result<unsigned long> FileTable::duplicate_at(unsigned long source,
                                               unsigned long destination);
Shared<FileTable> FileTable::clone();
```

get 现在也返回拥有引用的 Shared。引用递增发生在槽仍受锁保护时；调用方持有结果直到 I/O/等待结束。SocketOf 同样把 OpenFile 的 hold 留给整个 handler，不能只把 backend 裸指针传出局部查询。mmap 的 hold 保持到 VMA 接过 backend 引用。描述符并发 close/reuse 不会使正在进行的调用访问已经析构的 open，也不会把当前调用改绑到新对象。

free、close_all、close_on_exec 在表锁内把 Shared 移到局部、清槽和清标志，出锁后才允许最后一次 release 调后端。alloc 在表锁外创建、retain 和初始化 open，再锁内发布；状态和 CLOEXEC 在发布时已就绪。快速满表检查不 retain；若发布前别核填满了表，临时引用在锁外退回。adopt 只增加 OpenFile 的原子引用，不再调用 backend retain。

因此表锁下没有后端锁：查表并取得 hold→解表锁→后端操作。后端 release 可以重入同一表，host 有专门判例。dup2 的源验证与目标替换原子化，坏源不会先关目标；fork 的 clone 在一次源表锁内复制所有 Shared 与 CLOEXEC，目标表尚未发布。

这解决 P6-D3 的槽争用与出锁生命期部分。共享 OpenFile 的 offset read-modify-write、F_SETFL 并发更新、文件系统 lookup/unlink 到首个 retain 之间的节点固定协议，仍属于各自对象的并发语义；表锁不覆盖后端 I/O，也不允许持表锁 yield。它们不是“查表已上锁”就能一并宣称解决的事项。

## 批次和验证

| 批次 | 本次实施 | host 断言 | KVM 实弹 |
|---|---|---|---|
| 31 三 A | FileTable gate、拥有引用查询、锁外释放、原子 dup2、fork 快照 | 双分配线程+查询线程、close/reuse 后旧 hold 存活、后端重入、坏源 dup2、CLOEXEC 快照 | SMP fork/exec/管道压力、ash 作业控制、PTY/ppoll 原有场景 |
| 31 三 B | ITIMER_REAL 三门面、deadline 快路径、现有 EINTR | 单发边界、延迟周期相位、取消、向上取整、非法 timeval/指针、masked/ignored blocked 状态不变 | 无回包 ping 的 SIGALRM 退出、连续计时发送、普通版与检查版 |
| 31 三 C | 控制台输入泵与 USB Ctrl | 无 reader 时 poll_input 生成信号、左右 Ctrl/Ctrl+Shift 字节 | ping 等 socket 时真实 Ctrl+C、TTY/PTY/管道与后台 wait |
| 31 四 | ICMP raw、完整 IP 头 RX、独立 RX ring、4 字节 socklen | loopback echo request/reply、EINTR、EAGAIN、长度 canary | ping 10.0.2.2 有回复；Ctrl+C 打印统计并回 ash |
| 32 | 可打断真睡与软超时 | 上述 token 竞争矩阵、futex 计数、Completion 请求生命期 | 双核网络/信号/超时/退出压力 |
| 31 可选五/六 | ifconfig / DNS 尚未实施 | 单独立项 | 独立实弹，不作为当前核心实现通过证据 |

raw 注册表持锁调用接收回调，关闭先注销并等回调结束再销毁 ring；锁序 raw registry→ring。UDP 端口表补上同类注册/注销互斥，生产泵与 UDP adapter 的模块入口用 PreemptGuard 防止持 portable gate 时发生同核调度反转。回调内不得睡眠或重入注册表。IPv4/UDP 发送暂存移到调用栈，避免共享发送缓冲覆盖；网卡接入后的 UDP sink 接回 IPv4 封装。ARP 路由查询和泵学习由生产接线的 route gate 排斥。UDP 最大 payload 明确扣除 IPv4 与 UDP 两层头，固定为 1472B，并以整 MTU 回环判例验证；raw socket 创建要求 euid=0。

QEMU 复跑入口（必须存在 /dev/kvm，不回退 TCG）：

```sh
scripts/checks/acceptance.sh
python3 scripts/test_net_ping.py --image build/boot/cinux.img
python3 scripts/test_net_ping.py --image build-checks/boot/cinux.img \
    --log build/station31-ping-checks.log
```

脚本覆盖有回包、无回包 alarm、真实 Ctrl+C、TTY 关闭/恢复回显以及 SMP fork/exec/管道压力。PTY/ppoll 和作业控制仍须补做既有实弹；host 相关用例不能代替机内调度证据。

## 验收暴露的退出钩子 ABI

新的 FileTable close_all 局部 Shared 数组使编译器生成了对齐 SIMD 栈访存。机内 pipe_reader、调度器和 SMP 退出用例因此暴露 TaskTrampoline 的旧错误：入口调用返回后 rsp 已恢复为 16B 对齐，原先 jmp 到 C++ task_exit_hook 没有压入返回地址，导致 C++ 入口应有的 rsp%16=8 被破坏，最终在 movdqa 处 #GP。

修复为 call 进入不返回的退出钩子，并在意外返回处 ud2；没有把 FileTable 也改成 no-SSE 来掩盖 ABI 错误。机内退出用例必须按修复后的镜像复跑。

## 本次结果

2026-10-24 最终代码在沙箱外完成验收，`scripts/checks/acceptance.sh` 返回 0，日志 `build/station31-acceptance-final.log`：

| 矩阵 | host CTest 目标 | KVM 机内目标 | 结果 |
|---|---:|---:|---|
| 普通 | 76/76 | 21/21 | 通过 |
| ASan + UBSan + 泄漏检查 | 76/76 | 不适用 | 通过，未关闭 detect_leaks |
| TSan | 76/76 | 不适用 | 通过 |
| 内核 UBSan + lockdep | 76/76 | 21/21 | 通过 |

本树的 97 项由 76 个 host 目标和 21 个 KVM 目标组成；新增断言在原有目标内，不另加 CTest 号位。file_table 17 个 case、net_socket 12 个 case，TTY 和 HID 均新增信号入口判例。

最终普通与检查镜像的 `test_net_ping.py` 都返回 0，串口证据分别在 `build/station31-ping.log`、`build/station31-ping-checks.log`，运行结果分别在 `build/station31-ping-run.log`、`build/station31-ping-checks-run.log`。两边均验证：3 发 3 收、无回包 SIGALRM 退出、真实 USB Ctrl+C 返回 ash、TTY 关闭/恢复回显、PTY 执行 hello、管道、后台 wait、SMP 96 轮 fork 与 24 轮 busybox 压力。修复退出钩子后的调度/管道/SMP/块设备定点复验 4/4 通过，证据 `build/station31-abi-kvm.log`。

沙箱内最初看不到 /dev/kvm，LeakSanitizer 也无法扫描线程；沙箱外两者都可用，最终结果全部来自允许的沙箱外运行，没有回退 TCG 或关闭检查。此前失败属于排障记录，不能代替上述最终结果。

clang-format、git diff --check、Python 编译检查和磁盘脚本语法检查通过。**clang-tidy 尚未清零**，完整运行日志 `build/station31-tidy-final.log`：含 LLVM 23 对 GCC 16 标准库在 -mgeneral-regs-only 下的 long double 解析问题、匿名自由函数命名规则与本次 CamelCase 要求的冲突，以及复杂度/部分存量诊断。四树运行验收通过不等于静态检查已通过。

代码尚未提交、未打 tag，等待作者 CR；ifconfig/DNS 仍为可选批，通用可打断真睡按上文留站 32。
