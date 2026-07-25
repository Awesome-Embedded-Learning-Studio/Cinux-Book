---
title: 03 · 三个配套小修:instance、launch_first_user、main 清理
---

# 三个配套小修:instance、launch_first_user、main 清理

## 三个配套小修

线程化不是孤立的一刀,它逼出了三处连带修改。

### 一、AHCI 实例怎么从 init.cpp 拿到:instance()/set_instance()

`kernel_init_thread` 在 `init.cpp` 里,而 `init.cpp` 属于 `big_kernel_common`(被生产内核和测试内核共用);那个 `static AHCI ahci` 实例却定义在 `main.cpp` 里。如果 init 线程直接引用一个 `main.cpp` 的全局变量,测试内核(没有那个定义)会链接失败。

解法是给 `AHCI` 加一对静态访问器,把「实例在哪」这件事封装进类自己:

```cpp
// ahci.hpp
static AHCI& instance();
static void  set_instance(AHCI* ahci);
// ahci.cpp
AHCI* AHCI::s_instance_ = nullptr;
AHCI& AHCI::instance()       { return *s_instance_; }
void  AHCI::set_instance(AHCI* a) { s_instance_ = a; }
```

`main.cpp` 初始化完 ahci 就 `AHCI::set_instance(&ahci)`,`init.cpp` 用 `AHCI::instance()` 拿。比暴露一个全局变量干净,也顺带修了跨翻译单元的链接问题。

### 二、launch_first_user 不再手搓 Task:复用调度器的 current

008 的 `launch_first_user` 里有一行很可疑的代码:它手动 `static Task shell_task{}`——一个零初始化的、没有内核栈、`tid=0`、不在任何运行队列里的「假 Task」。重构前凑合能用,是因为那时调度器还没真正接管它;但一旦启动路径线程化,这个假 Task 就成了定时炸弹:

- `sys_exit` 会 `Scheduler::current()` 拿到这个假 Task,标记 Dead 后 `yield`;可它根本不在运行队列里;
- 它没有有效的 `CpuContext`,调度器要是尝试切回它就直接崩。

修法是**别再造 Task**:`launch_first_user` 现在跑在 `kernel_init` 线程里,直接用 `Scheduler::current()` 拿到这个真 Task,只更新它的 `addr_space` 和 `cwd`:

```cpp
auto* current = Scheduler::current();
current->addr_space = user_space;
current->cwd[0] = '/';
current->cwd[1] = '\0';
Scheduler::set_current(current);
```

还有一个连带的小坑,分两层。008 里 `user_space` 是 `launch_first_user` 的一个**栈上局部变量**(`AddressSpace user_space;`),函数一返回就析构;但线程化之后,它要被存进 `current->addr_space`、由调度器长期持有,函数返回时不能析构,所以必须把它的生命期提升到**静态存储**。可 `AddressSpace` 带析构器,一个静态的 `AddressSpace` 对象会触发 `__dso_handle` 之类的全局析构登记,在 freestanding 内核里链接报错。解法是 **placement new**——在一块 align 好的静态 `uint8_t` buffer 上构造它,既拿到静态生命期,又绕开析构器登记:

```cpp
alignas(alignof(AddressSpace)) static uint8_t user_space_storage[sizeof(AddressSpace)];
auto* user_space = new (user_space_storage) AddressSpace;   // 不触发全局析构器
```

### 三、main.cpp 删掉 stress 调用和键盘轮询

`run_concurrent_stress()` 是 008 的阶段性脚手架,兼着「验证并发 + 顺带起 shell」两件事。009 把启动职责交还给 `kernel_init` 后,这个脚手架就整个删掉了(`kernel/stress/stress_test.cpp` 连文件一起移除)。`kernel_main` 末尾也不再是键盘轮询循环,而是让 `boot_task` 进入调度器、由 idle task 兜底。
