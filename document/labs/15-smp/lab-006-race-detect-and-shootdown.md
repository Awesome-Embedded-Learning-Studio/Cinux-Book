---
title: Lab 006 · race-detect 报警器点亮——把哑火链亲手走通
---

# Lab 006 · race-detect 报警器点亮——把哑火链亲手走通

> 对应 `document/book/15-smp/006/`。验证档 **C 档**(工具/可观测)。race-detect 是 opt-in 检测器,本章 lab 让你亲手把这条「跨 CPU 交错报警器」点亮、看到它真能抓,而不是写新机制代码。核心是把「哑火链」这条隐形链亲手走一遍——option 开了 ≠ 编译宏传了 ≠ 机制测试真跑了 ≠ 测试真 PASS,每一层断掉日志都还是绿的。

## 目标

亲眼确认五件事:

1. 当前工作树的 race-detect opt-in 链断在哪两环(`option` 段 + compile-def 传播段);
2. 补齐三件套后,编译宏真的传进了 `big_kernel_common` 的编译命令(不是 cmake 变量传了就算);
3. 开 `CINUX_RACE_DETECT=ON` + `CINUX_LOCKDEP=ON` 跑 `run-kernel-test-smp`,在串口日志里 grep 那一行**专属**的 `[F-DYN-COV] race-detect test: ...`;
4. 对照 OFF leg 重跑,亲眼看到机制测试段被 `#ifdef` 跳过、日志里没有那行——哑火不报错、只静默绿;
5. 撞上比「假 PASS」更阴的「假 FAIL」,理解为什么(AP 侧 touch 没落地)。

## 摸现状:哑火链的现场

### 第 0 步 · 看当前工作树断在哪

```bash
grep -n 'CINUX_RACE_DETECT' CMakeLists.txt kernel/CMakeLists.txt
grep -n 'CINUX_LOCKDEP'     CMakeLists.txt kernel/CMakeLists.txt
```

**预期**:第一条 `CINUX_RACE_DETECT` **零命中**(根 `CMakeLists.txt` 没有 `option(CINUX_RACE_DETECT ...)` 这行);第二条 `CINUX_LOCKDEP` 命中两处——根 `CMakeLists.txt:15` 的 `option(...)` + `kernel/CMakeLists.txt:141-142` 的 `target_compile_definitions` 段。`RACE_DETECT` 的对应两段都缺。

这就是章节里讲的「假 PASS 第二型」:即使你 `-DCINUX_RACE_DETECT=ON` 传给 cmake,`kernel/proc/CMakeLists.txt:23-35` 的 §14 文件门会老老实实链 `race_detect.cpp` 真实现(它消费的是 `CINUX_RACE_DETECT` 这个 cmake 变量,option 没定义时 cmake 仍接受 `-D` 传入的未定义变量为 ON),但编译宏**永远不传**——`kernel/CMakeLists.txt` 没有对应的 `target_compile_definitions` 段。结果:`race_detect.cpp` 被编译进内核,但 `RACE_TOUCH` 宏在所有访问点处展开成 `((void)0)`,机制测试段被 `#ifdef CINUX_RACE_DETECT` 跳过,日志里整 suite 照样 `ALL TESTS PASSED`。

> **思考**:为什么「文件门链了真实现、但宏没传」这个状态最阴?——因为 `grep` 能看到 `race_detect.cpp` 在编译列表里,你会以为「机制在跑」。但真正决定行为的是 `RACE_TOUCH` 宏展开成什么,而那由编译宏(`-DCINUX_RACE_DETECT`)控制,不是由「哪个 .cpp 被编译」控制。文件门管链接符号、compile-def 管宏展开——两件事,断任何一件都哑火。

## 补三件套:把链接上

### 第 1 步 · 补 option

在根 `CMakeLists.txt` 的 `LOCKDEP` option(L15)旁边补一行:

```cmake
option(CINUX_RACE_DETECT "Enable SMP data-race watchpoint detector (opt-in debug)" OFF)
```

放在 `option(CINUX_LOCKDEP ...)` 紧后面,保持和 `LOCKDEP` / `UBSAN` 一组。

### 第 2 步 · 补 compile-def 传播

在 `kernel/CMakeLists.txt:141-142` 的 `if(CINUX_LOCKDEP)` 段旁边,补对应段:

```cmake
if(CINUX_RACE_DETECT)
    target_compile_definitions(big_kernel_common PUBLIC CINUX_RACE_DETECT)
endif()
```

> **为什么必须这一步**:`option()` 只是声明了一个 cmake 变量,它不会自动变成编译宏。从「cmake 变量」到「C++ 编译宏」之间,必须有 `target_compile_definitions` 这座桥。章节里讲的「哑火链」,这座桥是最常被漏的一环。

### 第 3 步 · 构建两个 leg

```bash
# ON leg
cmake -B build-rd -S . -DCINUX_RACE_DETECT=ON -DCINUX_LOCKDEP=ON > /tmp/cmake-rd.log 2>&1; echo "cmake=$?"
cmake --build build-rd --target big_kernel_test -j$(nproc) > /tmp/build-rd.log 2>&1; echo "build=$?"
```

`big_kernel_test` 是带测试入口的内核 target(`run-kernel-test-smp` 的 DEPS 之一)。先确认它能编过。

### 第 4 步 · 验证编译宏真传了(grep 铁律)

```bash
grep -rn 'CINUX_RACE_DETECT' build-rd/CMakeFiles/big_kernel_common.dir/flags.make
```

**预期**:看到 `-DCINUX_RACE_DETECT` 出现在 `CXX_DEFINES` 那一行。这是 b2 笔记里的铁律——**grep `flags.make` 看到 `-D` 真传了才算数**,光看 cmake 不报错不够。

如果这一步零命中,说明第 2 步的 `target_compile_definitions` 没生效(常见原因:target 名写错、没重新 `cmake -B`、补在了错误的 `CMakeLists.txt`)。回去查。

## 跑:看到那行专属 PASS(或撞上假 FAIL)

### 第 5 步 · 跑 SMP 机制测试

```bash
cmake --build build-rd --target run-kernel-test-smp 2>&1 | tee /tmp/smp-rd.log
grep 'F-DYN-COV.*race-detect test' /tmp/smp-rd.log
```

**预期(理想)**:看到

```
[F-DYN-COV] race-detect test: PASS (detected cross-CPU)
```

**实际(当前工作树)**:大概率看到

```
[F-DYN-COV] race-detect test: FAIL (no cross-CPU seen)
```

### 第 6 步 · 撞上假 FAIL,理解为什么

撞到 FAIL 不是你三件套没补对。原因是章节主线二讲过的那个坑:`main_test.cpp:499` 注释声称「AP touches it in `ap_test_selfcheck` before writing magic」,但翻 `ap_test_selfcheck` 函数体([main_test.cpp](kernel/test/main_test.cpp#L515-L547))——AP 只做了 CR4/EFER 读回和 shootdown IPI 测试,**没有任何一行碰 `g_race_test_wp`**。

所以即使三件套补齐、compile-def 传对,BSP 的 probe 拿到的 `prev` 恒为 `kRaceCpuNone`(从没人碰过),返 `false`、报 FAIL。这是比假 PASS 更阴的「假 FAIL」——测试在跑、断言在执行、结果 FAIL 了(`main_test.cpp:640-642` 那行 `if (!race) { ok = false; }` 会把整 suite 拖红),你会以为是自己哪步做错,其实是被测代码本身缺了一步。

验证一下这个判断:

```bash
grep -n 'g_race_test_wp\|RACE_TOUCH\|race_check_access_probe' kernel/test/main_test.cpp
```

**预期**:只有 `main_test.cpp:504` 声明、`main_test.cpp:637` BSP probe 两处命中,**AP 侧零调用**。注释 L499/L630 的「AP touches it」是 replay 前的陈年残留。

### 第 7 步(思考题)· 补一个 AP touch 转 PASS

想一下:要让 probe 返 `true`,AP 得在写 magic 之前碰一次 `g_race_test_wp`。在哪补?

提示:看 `ap_test_selfcheck` 的 `#else`(suite-only)分支(L529-L546),那里 AP 在 `sti;hlt` 等 BSP 的 shootdown IPI。在 `sti` 之前补一行:

```cpp
#ifdef CINUX_RACE_DETECT
    cinux::proc::race_check_access_probe(g_race_test_wp);  // AP 先碰一下,留 last_cpu=AP
#endif
```

这样 BSP 后续 probe 时 `prev` 就是 AP 的 cpu id,返 `true` 转 PASS。**注意用 probe 不用 `RACE_TOUCH`**——机制测试不能挂内核(L637 行 BSP 那处也是 probe,同理)。

补完重跑第 5 步,应该看到 `PASS (detected cross-CPU)`。

> 这道题本身就是「replay 后必须带着『这个机制测试现在到底在测什么』去重读被测代码」的活教材——不能信任注释(它可能是陈年残留)、不能信任批次的绿(它可能被 `#ifdef` 跳过)。补上 AP touch 后的那行 PASS,才是这一行专属测试**真正**在测什么的证据。

## 对照:哑火的可视化

### 第 8 步 · OFF leg 重跑,看哑火长什么样

```bash
cmake -B build-nord -S . -DCINUX_RACE_DETECT=OFF -DCINUX_LOCKDEP=ON > /tmp/cmake-nord.log 2>&1
cmake --build build-nord --target run-kernel-test-smp 2>&1 | tee /tmp/smp-nord.log
grep 'F-DYN-COV.*race-detect test' /tmp/smp-nord.log
```

**预期**:**零命中**。机制测试段被 `#ifdef CINUX_RACE_DETECT` 整段跳过,日志里既没有 PASS 也没有 FAIL,只有整 suite 的 `ALL TESTS PASSED`。

这就是「哑火不报错、只静默绿」的现场——如果第 7 步你没补 AP touch、第 5 步看到 FAIL,你会去查;但 OFF leg 连 FAIL 都没有,你会以为「一切正常」。章节里讲的「断一环就哑火且哑火不报错只静默绿」,这一步是它的直接演示。

## 进阶观察:lockdep_assert_held 抓「有锁忘持」

### 第 9 步(可选)· 对照两种检测器的分工

开 `CINUX_LOCKDEP=ON`(第 3 步已经开了),故意在某处加一句 `lockdep_assert_held(&某未持有的锁)`,重跑:

```cpp
// 随便找个地方,比如 do_read_kernel 入口,断言一把没持有的锁
lockdep_assert_held(&file->inode->vfs_lock_);   // 假设这把锁此刻没持有
```

**预期**:LOCKDEP 构建下 kpanic `lockdep: assert_held failed 0x...`。

这一步对照理解章节主线一讲的分工:

- `race_check_access` / `RACE_TOUCH` 抓「**根本没锁**」的共享状态(`inode_cache_` 加锁前那种);
- `lockdep_assert_held` 抓「**有锁忘持**」的回归(`inode_cache_` 加锁后,万一未来重构删了 `guard()` 那行)。

加锁前用前者抓、加锁后换成后者防回归——这是「抓 → 修 → 换护栏」的标准闭环。测完记得把这句测试性 assert 删掉。

## 验收清单

- [ ] 第 0 步:确认当前工作树 `CINUX_RACE_DETECT` 的 `option` 段 + compile-def 段都缺(`LOCKDEP` 的两段都在);
- [ ] 第 1-2 步:补齐 `option(CINUX_RACE_DETECT ...)` + `target_compile_definitions(... CINUX_RACE_DETECT)`;
- [ ] 第 4 步:`grep flags.make` 看到 `-DCINUX_RACE_DETECT` 真传进了编译命令;
- [ ] 第 5 步:`run-kernel-test-smp` 跑起,日志里有 `[F-DYN-COV] race-detect test:` 那一行(无论 PASS 还是 FAIL);
- [ ] 第 6 步:理解 FAIL 的根因是 AP 侧 touch 缺失(不是三件套没补对),`grep` 确认 AP 侧零调用;
- [ ] 第 7 步(可选):补 AP touch 后转 PASS;
- [ ] 第 8 步:OFF leg 重跑,日志里**没有**那行 PASS——亲手验证哑火静默绿;
- [ ] 第 9 步(可选):`lockdep_assert_held` 对未持有的锁 kpanic,理解两种检测器分工。

## 别做这些

- **别**拿整 suite 的 `ALL TESTS PASSED` 当 race-detect 有效的证据——它可能被 `#ifdef` 跳过(OFF leg)、可能报 FAIL 你没注意(ON leg 没 AP touch)。必须 grep 那一行**专属**的 `[F-DYN-COV] race-detect test: PASS`。
- **别**只补 `option` 不补 `target_compile_definitions`——option 只声明 cmake 变量,不会自动变成编译宏。第 4 步的 `flags.make` grep 是硬门禁。
- **别**在机制测试里用 `RACE_TOUCH` 代替 `race_check_access_probe`——`RACE_TOUCH` 检测到交错会 `kpanic` 挂内核,测试套件自己就成了「凶手」,还没断言就炸了。机制测试只 probe、不 touch。
- **别**信任 `main_test.cpp:499` 那行「AP touches it in `ap_test_selfcheck`」注释——它是 replay 前的陈年残留,函数体里没有这个调用。replay 后必须带着「这个测试现在到底在测什么」去重读被测代码。
- **别**指望不开 `CINUX_LOCKDEP` 就能看 `lockdep_assert_held` 的效果——它是宏,`CINUX_LOCKDEP` 没定义时展开成 `((void)0)`,加了测试性 assert 也不会炸。
- **别**在 `-smp 2` 跑挂了就以为是 race-detect 抓到了什么——race-detect 用的是 `kpanic("[SMP-RACE] ...")`,日志里会明确打出 `[SMP-RACE] xxx: cpuN touched after cpuM without lock` + backtrace。没这行就不是 race-detect 报的,是别的 panic。
