---
title: 03 · 主线二 · 拿 inode_cache 当靶子:验证报警器真能抓
---

# 主线二 · 拿 inode_cache 当靶子:验证报警器真能抓

## 主线二 · 拿 inode_cache 当靶子:验证报警器真能抓

### inode_cache 是完美的靶子

工具建好不能空跑——要拿一个**确定有 race 的目标**喂给它,看它报不报。`ext2` 的 `inode_cache_` 就是完美的靶子。它把读过的 inode 缓在内存里(分离链 hash 表,`ino % 512` 个桶),入口 `get_cached_inode(ino)` 做三件事:walk(沿桶链找命中)/ evict(miss 且缓存满时找 `refcount==0` 的对象腾位置)/ insert(new 一个 `Ext2CachedInode`、`read_disk_inode` 读盘、populate 成 VFS Inode、挂桶头)。

这张表是 `Ext2` 实例成员、跨 CPU 共享。但在加锁前三步全裸奔——SMP 下两核可以同时 miss 同一个 ino:

- 核 A 走到 `evict` 把某个 slot 删了;
- 核 B 此刻正拿着指向那个 slot 的 `Inode*` 在用(fd 或 VMA 在引用);
- slot 被释放、或被重填成另一个 ino;
- 核 B 手里的指针就成了别名 UAF,读到「另一个文件的字节」。

这类错 ASAN 都看不见——对象还活着,只是内容被换了,是**静默别名**。这正是前面讲过的那类「比 panic 难抓十倍」的 bug。

### 设计意图 vs 当前工作树

讲「race-detect 作为工具能抓」,用 `kernel/test/main_test.cpp` 里的机制自测当例子最稳妥——它证明了检测逻辑本身端到端通:

```cpp
#ifdef CINUX_RACE_DETECT
static cinux::proc::RaceWatchpoint g_race_test_wp =
    RACE_WATCHPOINT_INIT("test.race_detect");
#endif
```

（[main_test.cpp](../../../kernel/test/main_test.cpp#L910-L917)。机制测试的 watchpoint。）测试的意图是:AP 先碰一次这个 watchpoint,BSP 再 probe,probe 拿到的 `prev` 是 AP 的 cpu id、不等于 BSP 自己,返 `true` = 检测到跨核交错,打出那一行专属的 PASS,FAIL 则把整个 suite 拖红:

```cpp
#ifdef CINUX_RACE_DETECT
    if (ok && ap_count > 0) {
        const bool race = cinux::proc::race_check_access_probe(g_race_test_wp);
        cinux::lib::kprintf("[F-DYN-COV] race-detect test: %s\n",
                            race ? "PASS (detected cross-CPU)" : "FAIL (no cross-CPU seen)");
        if (!race) {
            ok = false;   // FAIL 不是只打印——它会拖垮整 suite,这才是「不是哑火」的证据
        }
    }
#endif
```

（[main_test.cpp](../../../kernel/test/main_test.cpp#L1048-L1064)。用 probe 不用 `RACE_TOUCH`——机制测试不能挂内核;末尾 `if (!race) { ok = false; }` 让 FAIL 真的拖红整个 suite,这正是这一行测试「没被哑火」的硬证据。)

⚠️ **这里有个细节值得说清楚**:`main_test.cpp` 注释写着「AP touches it in `ap_test_selfcheck` before writing magic」,翻 `ap_test_selfcheck` 函数体([main_test.cpp](../../../kernel/test/main_test.cpp#L927-L960))确实如此——L940 一行 `#ifdef CINUX_RACE_DETECT race_check_access_probe(g_race_test_wp); #endif` 把 AP 侧的 touch 补上了。BSP 的 probe 拿到的 `prev` 是 AP 的 cpu id、不等于自己,返 `true` = 检测到跨核交错,打出 PASS。也就是说:只要三件套(option + 编译宏 + §14 文件门)齐了,这条机制自测端到端通、能真报 PASS。lab 会带你亲手补全三件套并验证这行 PASS 真的会亮。

至于 `ext2` 路径本身——CinuxOS 上游(提交 `bf2d2d7`)确实在 `get_cached_inode` 入口放过 `RACE_TOUCH(g_inode_cache_wp)`,GUI `-smp 2` 跑 gcc 立刻抓到凶手栈 `get_cached_inode ← execve /bin/sh`,证明检测器对真实代码路径有效。但 Book 回迁时**只落地了「锁 + `lockdep_assert_held` + old-style cast 修复」三样**,那个 `RACE_TOUCH` 靶子没回迁(Book 树 `grep kernel/fs/` 零命中)。所以教程里讲「race-detect 作为工具能抓」,用机制自测当例子;`ext2` 路径讲 `lockdep_assert_held` 当回归护栏——两个工具互补,A 抓「有锁忘持」、B 抓「根本没锁」。

### 抓 → 修 → 换护栏的标准闭环

这是验证期最该记住的范式:

1. **加锁前**用 `RACE_TOUCH` 抓「根本没锁」——检测器报警 = 证实有竞态;
2. **加锁后**把 `RACE_TOUCH` 删掉换成 `lockdep_assert_held`——因为加锁后两核串行访问**也是**跨 CPU 交错,race-detect 会误报(它不区分「有锁串行」和「无锁交错」);这时换成 assert,防的是「未来有人重构把 guard 调用删了」这种回归。

不换会怎样?加锁了还留着 `RACE_TOUCH`,LOCKDEP 构建下两个核正常串行访问也会被 race-detect 当成 race 打 panic——报警器反而成了误报源。所以「抓完就换」是必须的,不是可选的。
