---
title: 05 · 验证、附带增量与小结
---

# 验证、附带增量与小结

## 验证:三个角度,绕开「SMAP 在本机不生效」

本机不透传 SMAP,所以验证不能靠「触发一次 SMAP 拦截看效果」——那个本机永远看不到。换成三个不依赖 SMAP 真生效的角度。

**角度一:exception table 的纯函数,host 单测罩住。** `extable_search`(二分)和 `extable_sort`(插入排序)都是纯函数,host 直接测:空表、单元素、命中头中尾、miss、gap 在上下,七例全过(`test_extable`,7 passed / 0 failed)。这验证了查表逻辑本身是对的,跟内核环境无关。

**角度二:accessor fault 的负测试,实证 exception table 真拦得住。** 给 `copy_from_user` / `copy_to_user` 传一个**真未映射**的用户地址,看它是不是返回 false 而不是把内核炸了:

```cpp
// test_user_ptr.cpp:test_extable 命名空间
void test_copy_from_unmapped_returns_false() {
    char kbuf[16];
    // 0x7000000000:落在用户半区,但既不在 identity/direct-map 覆盖区,
    // 也不在 mmap/brk/栈范围 —— 真未映射,rep movsb 必 fault
    bool ok = cinux::user::copy_from_user(kbuf, (const void*)0x7000000000ULL, sizeof(kbuf));
    TEST_CHECK(!ok);   // 期望 false:extable 拦住 fault,没 panic
}
```

(`test_user_ptr.cpp:153`。)`copy_from_user` 一执行,`rep movsb` 第一字节就 #PF,handle_pf 查表命中,改 `frame->rip` 到 fixup,accessor 返回 false。测试看到 false,PASS。要是没有 exception table,这一下就 panic 了,测试根本跑不到断言。这个测试走的是**内核态 accessor 指令的 fault**,由 exception table 拦,跟 SMAP 开没开无关——所以本机能验。

> 这个负测试有个坑值得记一笔:地址不能乱挑。头一版用 `0x40000000`(1 GB 用户址),结果返回 true——因为测试内核的 identity/direct-map 把物理 RAM 映射到了 1 GB 往上,这个地址碰巧有映射,`rep movsb` 成功读完不 fault,exception table 没机会拦。教训:accessor fault 的负测试地址,得确认它**没被任何内核映射覆盖**(identity map / direct-map / mmap / brk / 栈),用高位用户地址(481 GB 那种)避开。

**角度三:全量测试两 leg 不回归。** accessor 化和分层动了几乎每一个 syscall,最大的回归风险就在这。`run-kernel-test-all` 跑两 leg——单核和 `-smp 2`,各 962 passed / 0 failed。单核 leg 跳过 AP 唤醒(没 AP),`-smp 2` leg 真启动 AP1、读回 cr4=0x300620(SMEP/SMAP 位都在)、efer=0xd01(NXE),AP 机制回读 PASS。两 leg 全绿,说明这一通「撤全局 stac + accessor + 分层 + extable」的大改造没把既有路径弄坏。

> 有个老朋友在这章里又露了一面,值得点一下。`run-kernel-test` 的测试都是直接调 `do_*_kernel`、传内核地址——**根本不碰 accessor**,所以 SMAP 在测试内核下压根不触发。这就是上一章说的「假绿」同一个根:测试用内核地址,accessor 的真闸碰不到。accessor 的真闸是 ring-3 的 musl smoke(真用户地址)。可本机 SMAP 不透传,smoke 也就验不了「SMAP 真拦」。所以这一章能本机闭环验证的,就是上面三个角度;accessor 在真用户路径下的正确性,代码照已验证模式写、靠真机/TCG 兜底。诚实交代,不假装在本机验全了。

## 这一个 tag 还顺带带进来了什么

最后交代一句,免得你 checkout 这个 tag 看到一堆「这章没讲」的东西犯迷糊。这一章对应的源码增量是个大块,除了上面两条主线,整弧 diff 还夹带了几样别的东西,它们的教学分别留到后面的章节:

- **TTY 行规范 + 阻塞读 + 键盘接通**(一批):用户态终端的行规范(termios UAPI、Ctrl+C 生成信号字符、stdin 阻塞读替忙等)。这是 F10 用户态运行时的下一块,单独成一章(062)细讲。
- **CoW 写时复制的 U 位门控松绑**:内核态写 CoW 用户页的 panic 门松了一点。
- **`-smp 2` 下 fork exit/reap 的跨核修复**:子进程在另一个核上退出、父进程 reap 的时序竞态,AP idle 重建。
- **几条 CI 红线**(测试调度器代码量、forktest 的 C99 写法)。

这几样跟「SMAP-SMP + exception table」是同一个源码增量里一起进来的(patch-replay 按 CinuxOS 的时间序整弧落地,保 1:1),但它们各自有各自的教学脊梁,挤进这一章只会喧宾夺主。这一章只管把 SMAP 和 exception table 这两条线讲透;剩下的,该哪章哪章。

## 小结

- 上一章开 SMAP 用的「入口级全局 stac」,在 SMP 下跟物理冲突:RFLAGS.AC 是 per-CPU 位,而 `CpuContext` 不存 RFLAGS——任务跨核迁移就丢 AC,裸解引用用户内存撞 SMAP #PF。
- 修法是换模型:撤全局 stac,改局部 `stac`/`clac` 的 accessor;accessor 窗口绝不阻塞(block-then-write);syscall 切 `do_*_kernel`(可 block、不碰用户)/ `sys_*`(accessor 边界)两层,让铁律有落点。
- accessor 的 fault 容错交给 exception table:`__ex_table` 收注解,启动排序,PF handler 二分查表,命中改 `frame->rip` 到 fixup,accessor 返回 false、syscall 返回 `-EFAULT`,不再 panic。这是上一章末尾那笔债的偿还。
- 验证绕开「本机 SMAP 不生效」:extable 纯函数 host 单测 + accessor fault 负测试(内核态 RIP,不依赖 SMAP)+ 全量两 leg 不回归。
