---
title: 03 · 并发直觉
---

# 03 · 并发直觉:一条语句、两个线程,八次全不同

咱们写一个短得不能再短的程序:开两个线程,每个线程把同一个全局变量加一百万次。两个线程都干完,这个变量理应是一百万加一百万,两百万。可真跑起来,屏幕上那个数就是到不了两百万,而且一次一个样,八次下来八个不同的答案。笔者照着这个思路,在自己机器上跑了一遍,当场就是这个样子。

咱们手里的东西短得可以当场抄完,可它每次跑出来的数都不一样。会写多线程的人不少,能把这个数为什么不对讲明白的人没那么多,咱们把它摊开看一眼。

## 两百万的活,两个人干,结果少了一截

咱们把造件写出来,就这么长,一个第三方依赖都没有。

```cpp
#include <cstdio>
#include <cstdlib>
#include <thread>

static long counter = 0;
static const long kLoops = 1000000;

static void worker() {
    for (long i = 0; i < kLoops; ++i) {
        counter = counter + 1;
    }
}

int main(int argc, char **argv) {
    int n = (argc > 1) ? std::atoi(argv[1]) : 2;
    std::thread t[8];
    for (int i = 0; i < n; ++i) t[i] = std::thread(worker);
    for (int i = 0; i < n; ++i) t[i].join();
    std::printf("counter = %ld (expected %ld)\n", counter, kLoops * n);
    return 0;
}
```

咱们把上面这段存成 `race.cpp`,命令行参数就是线程数,不写就是两个。按最朴素的姿势编出来,老老实实连着跑八次。

```bash
$ g++ -O0 -pthread -o race-O0 race.cpp
$ for i in $(seq 8); do ./race-O0 2; done
```

```text
counter = 1506806 (expected 2000000)
counter = 1010695 (expected 2000000)
counter = 1065935 (expected 2000000)
counter = 1047781 (expected 2000000)
counter = 1049362 (expected 2000000)
counter = 1087182 (expected 2000000)
counter = 1054419 (expected 2000000)
counter = 1034982 (expected 2000000)
```

八行,八个不同的数,没有一个够得着两百万。上面这几行,是笔者从自己机器上这一次跑出来的读数,您那边跑出来的是另外八个,唯一不变的是它们都少。

教科书里有一拍是同一件事,只是它的量级大一些。那边每个线程跑一千万次,期望两千万,书里跑出来是一千九百多万,再跑一次又是一千九百多万。咱们看它原话怎么说的,**Not only is each run wrong, but also yields a different result!**,每一次都是错的,而且每次还不一样。(OSTEP 第 26 章,书页 8)

## 窗口只有三条指令

看着不对,咱们就去问编译器到底编出了什么。把 `worker` 那个函数体反汇编出来,`-O0` 下最要紧的一段是这样,那一百万次的循环被摊成了下面这几行:

```text
$ objdump -d --no-show-raw-insn -Mintel race-O0 | sed -n '/worker.*>:/,/ret/p'
00000000000011c9 <_ZL6workerv>:
    11d7:	mov    rax,QWORD PTR [rip+0x2e92]        # 4070 <_ZL7counter>
    11de:	add    rax,0x1
    11e2:	mov    QWORD PTR [rip+0x2e87],rax        # 4070 <_ZL7counter>
    11e9:	add    QWORD PTR [rbp-0x8],0x1
    11ee:	cmp    QWORD PTR [rbp-0x8],0xf423f
    11f6:	jle    11d7 <_ZL6workerv+0xe>
```

咱们要盯的是头三条:把 `counter` 从内存读进 `rax`,在寄存器里加一,再写回内存。完整那一屏是十五行,前面铺栈帧和跳转的四行、后面 `nop`、`nop`、`pop rbp`、`ret` 四行收尾都省掉了,上面只留了循环主体这六行。咱们在源码里写下的是一个 `counter = counter + 1`,落到机器上就是这三步。这三步之间,操作系统随时可以把当前线程换下去、把另一个线程换上来。两边都读到同一个旧值,各自加一,各自写回,中间那一次自增就没了。

行首那些十六进制地址是这一次编译和链接布出来的位置,换一次编译就换一套,咱们要看的只是那三条指令本身的形状。

## 同一份源码,`-O2` 编出来八次全对

同一个 `race.cpp`,咱们换成 `-O2` 再编一份,同样的八次。

```bash
$ g++ -O2 -pthread -o race-O2 race.cpp
$ for i in $(seq 8); do ./race-O2 2; done
```

```text
counter = 2000000 (expected 2000000)
counter = 2000000 (expected 2000000)
counter = 2000000 (expected 2000000)
counter = 2000000 (expected 2000000)
counter = 2000000 (expected 2000000)
counter = 2000000 (expected 2000000)
counter = 2000000 (expected 2000000)
counter = 2000000 (expected 2000000)
```

八次全是对的。源码一个字没改,两百万安安稳稳地躺在那儿。咱们再把 `-O2` 编出来的那一版 `worker` 反汇编出来看:

```text
$ objdump -d --no-show-raw-insn -Mintel race-O2 | sed -n '/worker.*>:/,/ret/p'
00000000000013c0 <_ZL6workerv>:
    13c0:	add    QWORD PTR [rip+0x2ca5],0xf4240        # 4070 <_ZL7counter>
    13cb:	ret
```

整个函数体只剩一条 `add QWORD PTR [rip+0x2ca5],0xf4240`,后头一条 `ret` 就完了。编译器把那一百万次的循环整个折没了,一次加上十六进制的 `0xf4240`,正好是一百万。刚才那三步的窗口,现在只剩一步。咱们前面数出来的那个空档,不是 `counter = counter + 1` 生来就有的东西,它是编译器在 `-O0` 下没做手脚才留下来的。

还有一件事咱们得抠一下:`0xf4240` 是十六进制,转成十进制就是 1000000,编译器把整个循环折成了“一次加一百万”。

## 想把错误跑出来,得同时看着三件事

到这里咱们就能回答那个最容易把人绕进去的问题:为什么我抄了同样的代码,跑出来却是对的?

**优化等级。**咱们得确认自己编的时候写的是 `-O0`。换成 `-O2`,连窗口都被折掉了,跑多少次都是两百万。

**线程数。**咱们把参数写成了一,那它永远精确,一百万就是一百万,一次都没有错过。两个、四个、八个都错,而且线程越多错得越狠。命令还是 `./race-O0`,后面那个参数就是线程数,每档跑三次:

```text
# 1 个线程:
counter = 1000000 (expected 1000000)
counter = 1000000 (expected 1000000)
counter = 1000000 (expected 1000000)
# 2 个线程:
counter = 1041223 (expected 2000000)
counter = 1012069 (expected 2000000)
counter = 1372192 (expected 2000000)
# 4 个线程:
counter = 1158712 (expected 4000000)
counter = 1139674 (expected 4000000)
counter = 1295967 (expected 4000000)
# 8 个线程:
counter = 1758443 (expected 8000000)
counter = 1921508 (expected 8000000)
counter = 1916995 (expected 8000000)
```

八个线程期望八百万,三次落在 `1758443` 到 `1921508` 之间,一次都没够着,别的档位也各有各的少法。这几行数和上面那八行一样,都是笔者机器上这一趟的读数。

**CPU 数。**咱们把整个进程按在一个 CPU 上,错误的频率会掉下来,可它没有消失。

```bash
$ taskset -c 0 ./race-O0 2
```

```text
counter = 2000000 (expected 2000000)
counter = 2000000 (expected 2000000)
counter = 2000000 (expected 2000000)
counter = 1000000 (expected 2000000)
counter = 2000000 (expected 2000000)
```

在这个被按在 0 号 CPU 上的进程里,五次有四次对,有一次是一百万,整整少掉了一个线程的量。您最好别把“按在一个 CPU 上就不复现”当成一条能靠的规律:笔者自己这边把同一份源码编了好几轮,对着它既跑过五次全对的样子,也跑过眼下四次对、一次错的样子。单核上能不能碰见,是概率问题,不是非黑即白。讲并发的那本教科书(OSTEP)里也留给我们一句,**even on a single processor, we don't necessarily get the desired result**,就算只有一个处理器,我们也不一定拿到想要的结果(第 26 章,书页 8)。

有一点咱们得说明:线程多的时候错得更狠,原因在于同一段时间里重叠的窗口更多,不是因为调度器偏心。调度怎么给线程挑人、隔多久换一次,是操作系统教材里调度章的地盘,咱们在这儿不展开。

## 锁:把那一行圈起来

修起来短得让人意外:那一行同样的时刻只准一个人进,数就不丢了。咱们把上面那段 `race.cpp` 复制两份,分别存成 `race_locked.cpp` 和 `race_mutex.cpp`,只改几行,别处照旧。

`race_locked.cpp` 那一版一共动四处:头上加一句 `#include <atomic>`,把 `static long counter = 0;` 换成 `static std::atomic<long> counter{0};`,循环里的自增换成 `counter.fetch_add(1, std::memory_order_relaxed);`,末尾打印的 `counter` 换成 `counter.load()`。咱们按同样的姿势编出新产物:

```bash
$ g++ -O0 -pthread -o race_locked-O0 race_locked.cpp
```

```text
$ for i in $(seq 5); do ./race_locked-O0 2; done     # std::atomic
counter = 2000000 (expected 2000000)
counter = 2000000 (expected 2000000)
counter = 2000000 (expected 2000000)
counter = 2000000 (expected 2000000)
counter = 2000000 (expected 2000000)
```

咱们在 `race_mutex.cpp` 那一版上动三处:头上加 `#include <mutex>`,在 `counter` 旁边加一句 `static std::mutex m;`,循环里用 `std::lock_guard<std::mutex> guard(m);` 圈住那一行再自增,别的地方与 `race.cpp` 一模一样。少一句 `<mutex>`,编译器第一条就报 `'mutex' in namespace 'std' does not name a type`,接着才轮到 `lock_guard` 不是 `std` 的成员,反正编不过。照样编出产物:

```bash
$ g++ -O0 -pthread -o race_mutex-O0 race_mutex.cpp
```

```text
$ for i in $(seq 3); do ./race_mutex-O0 2; done      # std::mutex
counter = 2000000 (expected 2000000)
counter = 2000000 (expected 2000000)
counter = 2000000 (expected 2000000)
```

两版都对,而且对得很干脆,每次都报两百万。锁买到的是正确性,但它不白给。同一个两百万次自增,咱们掐了一次表:两条路各跑一次,记下起止时刻再相减。

```text
$ T0=$(date +%s.%N); ./race-O0 2 >/dev/null; T1=$(date +%s.%N)
$ awk -v a="$T0" -v b="$T1" 'BEGIN{printf "race-O0        wall=%.3f s\n", b-a}'
race-O0        wall=0.004 s
$ T0=$(date +%s.%N); ./race_mutex-O0 2 >/dev/null; T1=$(date +%s.%N)
$ awk -v a="$T0" -v b="$T1" 'BEGIN{printf "race_mutex-O0  wall=%.3f s\n", b-a}'
race_mutex-O0  wall=0.102 s
```

两个数字都是笔者从自己机器上掐出来的墙上时间,它随着机器忙闲漂,您那边跑出来的不会一模一样。咱们要读的是那个量级:加了锁慢了二十几倍。这不能读成“锁很慢”,慢的原因是咱们亲手把那一段划成了不能并行的地方,本来两个人一起干的活,现在得排队。

## 线程在内核眼里,是两条几乎一样的克隆

咱们上面开出来的那些线程,内核那边到底看见了什么?它看见的不是一个人,是好几条一模一样的任务。咱们拿 `strace` 挂上去,只看克隆这一类系统调用,并且带上 `-f` 让它跟着子任务走。手册给 `-f` 的说法是 **Traces child processes as they are created**,跟着那些刚被创建出来的子任务,少了它就看不到克隆子任务写下的记录。屏里的进程号、`counter` 那个数、等号后面的地址和长度,都是这一趟跑出来的值,您那边全不一样,咱们要认的是那串 `flags` 里的名字。

```text
$ strace -f -e trace=clone,clone3 -o clone.trace ./race-O0 2; cat clone.trace
counter = 1283644 (expected 2000000)
386   clone3({flags=CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SYSVSEM|CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID, child_tid=0x74c57ffffce8, parent_tid=0x74c57ffff990, exit_signal=0, stack=0x74c57f7ff000, stack_size=0x7fff40, tls=0x74c57ffff6c0} => {parent_tid=[387]}, 88) = 387
386   clone3({flags=CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|CLONE_SYSVSEM|CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID, child_tid=0x74c57f7fece8, parent_tid=0x74c57f7fe990, exit_signal=0, stack=0x74c57effe000, stack_size=0x7fff40, tls=0x74c57f7fe6c0} => {parent_tid=[388]}, 88) = 388
388   +++ exited with 0 +++
387   +++ exited with 0 +++
386   +++ exited with 0 +++
```

`strace` 把 trace 落到文件里,程序自己那句 `counter = ...` 打在屏上,再 `cat` 出来就是上面这一段。咱们开两个线程,就换来两条克隆:两条的 `flags` 那一串与结尾那个 `88` 逐字相同,`stack_size` 也一样,各是各的是 `child_tid`、`parent_tid`、`stack`、`tls` 这四格,外加返回的那个进程号。`flags` 那一串名字逐字写着它们共享了什么:`CLONE_VM` 是地址空间,`CLONE_FILES` 是已打开文件的表,`CLONE_SIGHAND` 是信号处理方式,`CLONE_THREAD` 说明它是同一批线程。

咱们回头接一下前面两篇。上一篇里有一份进程花名册,一个进程号底下只住着一条记录,这会儿屏幕上多出来的正是它的第二条和第三条,同一张名册,换了个看法。再往前那一篇数过一个 hello 的系统调用面,一行一行记下程序跟内核说了哪些话,这里的两行是同一类记录,只是话头换成了给内核递一句“给我再开一条任务”。写完这段咱们得补一句:这一趟抓到的系统调用叫 `clone3`,是眼下这套 glibc 和内核走的路径,换个版本可能就报 `clone`。线程和进程共用同一套克隆开关,只是拨到不同的档位上。既然它们共享了地址空间,那就怪不得两个线程会去动同一个 `counter`。

这一屏只留了克隆这一类系统调用,旁边那些用来协调线程的调用咱们没抓,它们属于锁实现的地盘。

## 死锁的四个条件

并发里另有一类麻烦叫死锁:两个线程各自捏着一个东西,又都在等对方手里那个,谁都不松手。咱们看到的不是少个数,是程序一直停在那儿。

死锁要同时凑齐下面四条,咱们一条条看:

- **互斥**。这个东西同一时刻只准一个人用。
- **持有并等待**。手里已经捏着一个,还在等下一个。
- **不可剥夺**。别人手里那个你抢不走,只能等他松手。
- **循环等待**。等的方向连成了一个圈,甲等乙、乙等甲。

这四条里,咱们只要能拆掉一条,死锁就凑不起来。

这些条件咱们只列到这儿,不做现场。这一篇没有配一段能复现死锁的小程序,笔者也不打算凭印象写一段出来充数。等您在哪天见到一个卡住不动的程序,再回头拿它们四个挨个对。

## 跑不出错的时候

咱们得替您想到一桩:如果您照着上面抄完,连着跑却次次是两百万,那就回头核那三件事,`-O0` 写了没有、线程数是不是还停在一、有没有被 `taskset` 按在单个 CPU 上。这三件都占全了,每一次的错数也还是不同,想抓住它,咱们得多跑几轮。

还有一桩只当量级看:那两行墙上时间是笔者从自己机器上掐出来的。您想自己掐表,`time` 或者前后各记一次 `date` 都行,别把别人的数字当成自己机器上的标准。

## futex、屏障,和没搭起来的死锁现场

有几样东西咱们得在这儿交代清楚。锁在操作系统内部是怎么落地的,包括追踪线程协调时会出现、咱们这一篇故意没去抓的 `futex`,一个字都没有讲。屏障和内存顺序同样留着,连 `memory_order_relaxed` 那个参数咱们都跳了过去,那是编译器和处理器层面的东西。

死锁也只停在四个条件,没有真的搭一个两把锁反序去拿的现场给您看。

调度器怎么在一堆可运行的线程里挑下一个、隔多久换一次人,咱们这一篇只用到“窗口被打开”这件事,挑人那套策略一点没碰。

## 双视角锚点:同一行代码,两个人读到的不是一回事

咱们让两个人分头来看这一篇里的几件事。

**那一行。**应用程序员写下 `counter = counter + 1`,在源码里它就是一条语句,他读到的意思是“把 counter 加一”,天经地义。轮到咱们写内核的人,看到的还是上面那三步,而它们之间随时可以换人:`-O0` 下八次跑出八个不同的数,`-O2` 下那个空档被编译器折掉,八次全对。应用程序员以为自己写的是一条指令,内核作者知道那是三步。

**那把锁。**应用程序员手上,锁是一个开关,咱们按下去结果就对了,代价是那边慢一点。轮到内核作者,代价换了个名字:咱们是把“这一段不能并行”拿出去换回来的正确性,原本可以同时干的两份活,现在得在门口排队。同一件事,一边看到的是快慢,一边看到的是能并行和不能并行的分别。

**死锁。**应用程序员那边听到死锁,想到的是程序卡住了,重开一次就好。轮到咱们这些写内核的人,卡住是一种结构:四个条件同时在,谁都不肯松开手里那个。同一场停摆,一边看到的是症状,一边看到的是四个能逐个拆掉的支点。

末了收一句。咱们上面开出来的这些线程,落到内核的花名册里是两条并排的记录,它们能同时去动同一个 `counter`,正因为它们共用了同一张地址空间。
