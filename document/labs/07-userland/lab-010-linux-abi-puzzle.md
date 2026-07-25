---
title: Lab 010 · Linux ABI 拼图验证:让 stub 的艺术可观测
---

# Lab 010 · Linux ABI 拼图验证:让 stub 的艺术可观测

> 对应 `document/book/07-userland/010/`。验证档 **A 档**:教程即验证——把章节里讲的「判断一个号该怎么应答看 libc 拿到值后的行为」「`-ENOSYS` 是正面信号」「`pread64` 不动 offset」「`cinux_exit` ≠ `sys_exit`」这些认知点从源码注释变成**可观测事实**。**不写答案**——给方法论和观察路径,让读者自己跑出证据。

## 目标

把章节里的五条认知点逐个变成「跑了一下,看到 X,这说明 Y」的观察记录:

1. **让 `-ENOSYS` 降级肉眼可见**(改 stub 返回值,看 glibc/musl 启动行为差异);
2. **真实现/stub/策略型三类分类**(给方法论,自己填表);
3. **`getrandom` 真给字节 + 随机源验证**(改 seed 看可预测性);
4. **`pread64` 不改 offset 验证**(对照 `read` 推进 offset);
5. **诚实边界确认**(挑两条边界写程序触发,看「内核没真做」的表现)。

工具链:用 `tools/musl/cinux-exit.c` 同款的 musl sysroot 静态链小测试程序(参考 `tools/musl/build-musl.sh` 编出 sysroot + `musl-gcc` 静态链),`run-buildroot-usability` gate 跑(`cmake/qemu.cmake:604`)。**产出是观察记录,不是代码**——「看到 X,这说明 Y」。

## 任务一:让 `-ENOSYS` 降级肉眼可见

### 步骤

1. 读 `kernel/syscall/sys_linux_stubs.cpp:41-43`,把 `sys_set_robust_list` 的 `return 0;` 改成 `return -cinux::kEnosys;`(就是把它从「探测型返 0」改成「功能型返 ENOSYS」)。
2. rebuild 内核,跑一个带 SSP 的 musl 程序(或直接跑 `run-buildroot-usability` gate,看 busybox 起得来不)。
3. 对照原版(返 0)的行为,记录差异。

### 观察什么

- **返 0(原版)**:glibc/musl 的 robust-futex probe 通过,启动链继续。程序正常起。
- **返 `-ENOSYS`(你改的)**:glibc 把整条 robust-futex 路径判废。可能仍能起(没 pthread 没人真用 robust 锁),但某些 glibc 版本会在启动日志或行为上不同——比如打一条「robust futexes unavailable」之类的诊断,或者 `pthread_mutexattr_setrobust` 路径行为变化。

### 引导

这把章节里「`set_robust_list` 为什么偏偏返 0 不返 ENOSYS」从源码注释变成可观测事实。**自己 grep glibc 源码**(或 musl 源码)确认:

- 它真在启动时探这个号(grep `__NR_set_robust_list` 或 `set_robust_list`);
- 它真把返回值 0 当成功、把 `-ENOSYS` 当「不支持」。

如果 glibc 收到 0 走「支持 robust futex」路径、收到 ENOSYS 走「判废 robust」路径——那章节讲的「返 0 vs 返 ENOSYS 取决于探测语义」就坐实了。

> **不写的事**:不给你改好的 `sys_set_robust_list` patch(你刚改的 `return -kEnosys` 就是);不给你完整的 glibc 行为预测表——你自己跑、自己 grep、自己记观察。

## 任务二:真实现/stub/策略型三类分类

### 步骤

给一份方法论(不给答案表——那是答案 dump):

**分类维度**:判断一个 syscall 该怎么应答,看「libc 拿到这个返回值后行为对不对」,具体三类:

- **真实现**:用户态真要消费返回值里的字节/数据(如 `getrandom` 给字节、`access` 给权限裁决);
- **探测 stub**:返 `-ENOSYS` 让 libc 降级(如 `rseq`/`clone3`),或返 0 满足探测(`set_robust_list`);
- **策略型**:返「无限」或固定值哄过 probe,不真做 enforcement(如 `prlimit64` 返 RLIM_INFINITY、`getrandom` flags 忽略)。

### 观察什么

把章节里讲的十五个号(十四个 Linux ABI + 一个 Cinux 专有:`access`/`pread64`/`getrandom`/`prlimit64`/`time`/`gettimeofday`/`tkill`/`setitimer`/`sched_getaffinity`/`getcpu`/`rseq`/`clone3`/`sendfile`/`set_robust_list`/`cinux_exit`)填进你自己的表,每个号写:

- 一句话:**libc/用户态拿到这个返回值后会怎样?**(走降级?走真路径?踩空?)
- 分类:真实现 / 探测 stub / 策略型 / Cinux 专有。

引导问题(自检):

- `prlimit64` 为什么不返 `-ENOSYS`?如果返 `-ENOSYS` 会怎样?(提示:glibc 会以为 syscall 不存在,走 fallback。)
- `sched_getaffinity` 返字节数而不是 0——如果改成 `return 0` 会怎样?(提示:glibc 拿到「写了 0 字节」以为没 CPU 在线,`nproc` 报 0。)
- `set_robust_list` 返 0 和返 `-ENOSYS` 哪个对?(提示:看 libc 拿到 ENOSYS 后会不会判废整条 robust 路径——任务一的观察正好答这个。)

> **不写的事**:不给你完整的十五行答案表。你读章节 + grep 源码自己填——「学到」和「记下别人给的表」是两回事。

## 任务三:`getrandom` 真给字节 + 随机源验证

### 步骤

1. 写一个最小 musl 程序调 `getrandom` 取 32 字节、hex 打印(参考 `tools/musl/hello.c` 的编译方式,`musl-gcc -static`)。跑两次,看两次的 32 字节是否不同。

2. 打开 `kernel/lib/random.cpp:61-87`(`KRandom::init`),把 `uint64_t seed = rdtsc();` 那行改成 `uint64_t seed = 0x1234567890ABCDEFULL;`(固定 seed)。rebuild,再跑两次 `getrandom` 程序,看输出是否变得可预测(两次相同 / 不同机器相同)。

3. 进一步:把 `seed ^= cinux::drivers::PIT::get_ticks() << 16;` 也注释掉,再观察。

### 观察什么

- **原版**(`rdtsc` + `PIT` + addr + `rdrand`):两次运行的 32 字节大概率不同(因为 rdtsc 和 PIT 时序每 boot 不一样)——说明 seed 含时序熵。
- **固定 seed**(去掉 rdtsc):两次运行字节**相同**——说明 rdtsc 是随机性的主源。
- **再去掉 PIT**:仍然相同(已经固定了),但对照能看出 PIT 在原版里贡献了多少熵。

### 引导

这把章节里「随机源 = `rdtsc` ^ `PIT<<16` ^ addr>>4 ^ `rdrand`,boot 一次 seed」从读源码变成可观测。如果改 seed 后字节变可预测,说明 `rdtsc` 那行**真的**在贡献熵——它不是装饰,是随机性的关键来源。

> **不写的事**:不给你预期的 hex 串(那是答案 dump);不给你改好的 `random.cpp` patch(上面三行就是)。

## 任务四:`pread64` 不改 offset 验证

### 步骤

1. 写一个 musl 程序,`open` 一个文件,先 `pread64(fd, buf1, 5, 10)`(从 offset 10 读 5 字节),再 `read(fd, buf2, 5)`(顺序读 5 字节)。打印 `buf1` 和 `buf2` 的内容。

2. 对照实验:把 `pread64` 换成第二次 `read`(两次顺序 `read`),看第二次读到的内容。

### 观察什么

- **`pread64` + `read`**:`buf1` 是文件 offset 10-14 的内容,`buf2` 是文件 offset 0-4 的内容(从文件头读)——说明 `pread64` **没推进** `file->offset`,后面的 `read` 仍从文件头开始。
- **`read` + `read`**(对照):第一次读到 offset 0-4,第二次读到 offset 5-9——说明 `read` **推进了** `file->offset`。

### 引导

这把章节头号明星点「`pread64` 不推进 offset 不是靠锁或回滚,是靠接口签名」从源码注释变成可观测行为。如果你看到 `pread64`+`read` 的 `buf2` 是文件头内容,说明 `sys_pread64` 真没动 `file->offset`——`InodeOps::read(inode, offset, ...)` 的 offset 是入参,不写回。

读 `kernel/syscall/sys_pread64.cpp:25-44` 和 `kernel/syscall/sys_read.cpp:48-58` 对照:`sys_read` 有 `file->offset += n`,`sys_pread64` 没有。

> **不写的事**:不给你完整的 musl 程序(你照 `tools/musl/hello.c` 改);不给你预期的 `buf1`/`buf2` 内容(你自己跑出来记)。

## 任务五:诚实边界确认

### 步骤

`document/book/07-userland/010/` 的「诚实的边界」段列了 9 条刻意简化。挑**其中 2 条**写小程序触发并观察「内核没真做」的表现。

推荐挑这两条(都可观测):

1. **`prlimit64` 不强制任何资源限制**:写 musl 程序调 `setrlimit(RLIMIT_NOFILE, {rlim_cur=10, rlim_max=10})`,然后开 100 个 fd(`open("/dev/null", O_RDONLY)` × 100,或者就用 `dup`)。观察:是否被限在第 10 个 fd 上?

   - **预期**(`prlimit64` 不强制):100 个 fd 全开成功——内核忽略了 `setrlimit`,没真限 fd 表。
   - **对照**(Linux 真机):开到第 10 个之后 `EMFILE`——内核真限了。

2. **`access` 无 ACL 只有 root bypass**:写 musl 程序,创建一个 0644 文件,调 `access(path, X_OK)`。观察返回值。

   - **预期**:`access(X_OK)` 对 0644 文件返 `-1`(errno=EACCES)——因为 root 想执行也要文件有 x 位(`sys_access.cpp:46-53` 的 root bypass 只对 R/W 放行,X 看执行位)。
   - **进一步**:把文件 `chmod 0755` 再 `access(X_OK)`,应该返 0——因为现在有 x 位了。

其他可挑的边界(任选):

- `getrandom` 不是 CSPRNG(任务三已做);
- `pread64` 对 pipe 返 EBADF 而非 ESPIPE(写程序 `pread64` 一个 pipe fd 看返啥);
- `setitimer` 只支持 ITIMER_REAL(写程序 `setitimer(ITIMER_VIRTUAL, ...)`,看是否 `-EINVAL`);
- `cinux_exit` 占 Linux `fadvise64` 号 221(写 musl 程序发裸 syscall 221 当 `posix_fadvise`,看是不是触发 QEMU 退出——这是个**危险实验**,会直接关 QEMU,留给你自己判断要不要做)。

### 观察什么

把「内核没真做」从文字变成可验证事实。你写的不是「相信源码注释」,而是「跑了一下,真的没限/真的返 EACCES/真的不支持」。

## 工具链

- **musl sysroot**:`tools/musl/build-musl.sh` 编出 sysroot(`libc.a` + crt 文件 + 头文件),`musl-gcc -static` 编你的小测试程序。参考 `tools/musl/hello.c` 和 `tools/musl/cinux-exit.c` 的编译方式。
- **跑测试**:把编好的静态程序放进 ext2 盘(`scripts/create_ext2_disk.sh` 或 buildroot rootfs),`cmake --build build --target run`(进 shell 跑程序看输出)或 `cmake --build build --target run-buildroot-usability`(`cmake/qemu.cmake:604`,gate 跑 busybox + 测试脚本)。
- **改内核**:改完 `kernel/syscall/*.cpp` 后 rebuild kernel(`cmake --build build --target image`),重启 QEMU 看行为差异。

## 验收清单

- [ ] **任务一**:改了 `sys_set_robust_list` 返回值,记录返 0 vs 返 `-ENOSYS` 的行为差异;grep 了 glibc/musl 源码确认它真探这个号。
- [ ] **任务二**:十五个号填了自己的分类表,每个号写了「libc 拿到返回值后会怎样」一句话。
- [ ] **任务三**:跑了 `getrandom` 程序两次,字节不同;改 `rdtsc` 行后字节变得可预测。
- [ ] **任务四**:`pread64`+`read` 验证了 `pread64` 不推进 offset(`read` 仍从文件头读),对照 `read`+`read` 第二次接着第一次读。
- [ ] **任务五**:挑了 2 条边界写程序触发,记录「内核没真做」的可观测表现。

## 别做这些

- **别**把章节当 changelog 找「批号」「F6」「wholesale」——教程正文叙述里这些都被剥掉了(源码逐字引用块里保留的是源码真貌,不是教程在用批号)。你看到的只有「为什么需要这个 syscall + 真实现/stub + 反直觉点」。
- **别**把任务二做成答案表 dump——你要的是「自己跑、自己 grep、自己填」,不是抄章节里的表。章节里没给完整答案表是有原因的。
- **别**在任务一里只改返回值不跑真程序——光改不跑,你看到的只是代码差异,不是「glibc 收到 ENOSYS 后真降级」的可观测事实。
- **别**指望 `prlimit64` 真限 fd 表——它返 RLIM_INFINITY 是策略,不是没做完。如果你写程序发现真限了,那是别的原因(比如 fd 表本身满了),不是 `prlimit64` 起作用。
- **别**把任务五的「`cinux_exit` 占 221」实验随便做——发裸 syscall 221 会让 QEMU 直接退出,如果你在交互 shell 里试,你的 shell 会瞬间消失。在测试脚本里做,做好心理准备。
