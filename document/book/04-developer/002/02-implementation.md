---
title: 02 · SMP 唤醒与首故障捕获
---

# SMP 唤醒与首故障捕获

## SMP 空转:那个名存实亡的测试

这一章最该记的,是审计的头号盲区。`run-kernel-test-smp` 这个目标,名字里带 smp、QEMU 启动参数里也确实挂了 `-smp 2`——所以大家都以为「SMP 测试在跑」。但实测:**测试内核从来不唤醒第二个核**。它带着 `-smp 2` 起 QEMU,但内核的 `boot_aps()` 在测试环境里压根没被有效调用、AP 从来没醒、调度器从来不起——整个测试套件从头到尾**只跑了 BSP 一个核**。

这意味着什么?所有「只在第二个核上才暴露」的 bug,LSTAR 在 AP 上是 0 导致的 #DF、任务迁移的竞态、CoW 跨核的 UAF——**CI 永远抓不到**。因为第二个核根本没跑代码,它上面的 bug 自然不会发作。「SMP 绿」是个假象,这个门名存实亡。审计量化下来是 47/47 子系统的 SMP 测试全空转。

修法分两步。**先让测试内核探真实拓扑**:测试内核原来只在一个手造表上测 MADT parser,从不扫固件真表,所以 `cpu_count` 恒 0、`boot_aps()` 直接「single CPU」返回。改成测试内核真跑一遍 `acpi::init()` 扫固件表——单核腿报 `cpu_count=1`,`-smp 2` 腿报 `cpu_count=2`。**再让 AP 真醒 + 跑回读**:这步碰生产代码,用了一个精巧的「gated 钩子」范式——

```cpp
// ap_main.cpp,AP 信号 online 之后、进调度器自旋之前:
__atomic_add_fetch(&g_aps_online, 1, __ATOMIC_SEQ_CST);
// (g_ap_test_selfcheck_fn == nullptr) 生产路径直接跳过——零行为变化
if (g_ap_test_selfcheck_fn != nullptr) {
    g_ap_test_selfcheck_fn(cpu_id);   // 测试内核设的回读函数
    for (;;) { __asm__ volatile("cli; hlt"); }  // 跑完 halt,绕开调度器
}
// ... 生产 SMP 路径:进调度器自旋
```

（`ap_main.cpp:189`,钩子全局在 `:122` 默认 `nullptr`。）关键设计:生产里 `g_ap_test_selfcheck_fn` 是 `nullptr`,这段 `if` 整块跳过——**生产二进制一字节都不变**;只有测试内核往这个全局写一个函数指针,AP 才会去跑它(CR4/EFER/LSTAR/STAR 的回读,存进 `g_ap_selfcheck_results[cpu_id]`,最后写一个 magic `0xA5C0FFEE` 表示「这个槽填完了」)。BSP 不去读 `g_aps_online`(那个在匿名 namespace,外部看不见),而是轮询每个 AP 结果槽的 magic——AP 把回读全写完才置 magic,x86 的 TSO 保证 BSP 读到的是完整槽,天然同步,不用显式 fence。

这么一改,`-smp 2` 那条腿真的把 AP 唤醒了、在 AP 上读了 CPU 配置、回报给 BSP。SMP 门从「名存实亡」变成「真测了 AP 路径」。`lstar==0` 这类 AP-only bug,CI 立马能抓。

> **「gated 钩子」是个可复用的范式。** 给生产代码插测试钩子,最怕污染生产路径。这个范式解决:钩子是个「函数指针全局,默认 nullptr」,生产里 `if (fn)` 整块跳过、零行为变化;测试设了指针才驱动。这样最敏感的生产文件(ap_main,踩过最多坑的)也能安全接入测试,不用 `#ifdef` 把生产代码切两半。`ApSelfcheckFn` 返 `bool`:true 表示「AP 跑完进调度器」(给跨核 smoke 用),false 表示「halt」(纯套件回读)——同一个钩子管两种测试形态。

## 统一入口:别让人忘了跑 -smp 那条腿

戳穿空转之后,还得防「人忘了跑」。原来单核 `run-kernel-test` 和 `run-kernel-test-smp` 是两个目标,很容易只跑前者、把后者忘了(空转的 smp 腿跑了也白跑,但至少得跑)。这一步加了个**统一入口** `run-kernel-test-all`(`qemu.cmake:583`):一条命令先跑单核腿、再跑 `-smp 2` 腿(后者真 boot AP + 回读)。CI 和默认验证都切到这个统一入口,「忘了跑 SMP 变体」这个人为漏洞就堵上了。

## 首故障捕获:崩了也得留个全尸

另一个动态盲区是「崩了看不清」。一个 #GP 或 #PF 进了 handler,handler 想 `kprintf` 打现场——但 `kprintf` 自己可能又 #PF(它访问的内存也坏了),或者那个 #GP 根本就是 `%gs` 损坏引起的(`current()` 用 `%gs` 取当前任务,`%gs` 一坏就连环 #GP)。一旦递归崩,第一个 fault 的现场就被第二个、第三个覆盖,日志里只剩一团乱,看不出最初错在哪。

修法是把**第一个** fault 的现场,在任何可能递归的代码**之前**,dump 到一个**永远活着的通道**——QEMU 的 debug console(端口 `0xE9`,输出落到 `build/debug.log`):

```cpp
// exception_handlers.cpp:handle_gp 进来第一件事:
void handle_gp(InterruptFrame* frame) {
    capture_first_gp(frame);   // 先把首 #GP 的 rip/rsp 送 debugcon,再干别的
    ...
}
// page_fault.cpp:handle_pf 同理(:108 调 capture_first_pf,解码 err 的 P/W/U/RSV/I 位)
```

（`exception_handlers.cpp:232`/`:235`,`handle_gp` 进来第一件事就是 `capture_first_gp(frame)`;实现在 `fault_diag.cpp`,`>>> FIRST #GP rip=...` 在 `:59`。）debugcon 是一条「不经内存、不经调度器、不经任何可能崩的东西」的纯 IO 通道——`out %al, $0xE9` 就把一个字节送出去,它永远活着。所以哪怕后续 `kprintf` 自己 #PF、哪怕 `%gs` 损坏连环 #GP,**第一个 fault 的 rip/rsp/错误码已经稳稳落在 debug.log 里了**,不会被覆盖。`capture_first_gp/pf` 还带「只捕获一次」的逻辑(后续递归的 frame 直接跳过),保证你看到的就是首故障。

> **这条和上一章的 #DF 串得上。** 上一章(059)那个 `jump_to_usermode` #DF,之所以能定位,靠的就是这类「在递归崩之前留住首故障」的诊断——#DF 是「#PF 推栈失败」的产物,首 #PF 的现场最容易丢。把首故障 dump 到一个崩不掉的通道,是调试这类连环崩的通用招。
