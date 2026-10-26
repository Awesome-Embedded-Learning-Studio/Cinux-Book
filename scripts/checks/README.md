# 必跑验收

代码最终验收必须运行普通构建与测试、ASan+UBSan、TSan，以及开启 lockdep 和内核 UBSan 的构建与测试：

```sh
CINUX_CHECK_JOBS=4 scripts/checks/acceptance.sh
```

每段内部遇错停止，四段全部运行后汇总失败；不得用关闭检查、忽略退出码或跳过失败用例来报“验收通过”。QEMU 必须有可访问的 `/dev/kvm`，不回退 TCG。沙箱或容器若阻止 sanitizer 运行，应在允许的宿主环境重跑；未运行成功必须明确记录阻塞项。KVM 用例串行运行，host 用例可并行。

设置 `CINUX_CHECK_LOG_DIR=build/station33-acceptance` 可在成功时也保留四段完整日志和状态文件；未设置时，失败日志留在临时目录，成功后清理。

| 构建目录 | 检查 | 覆盖范围 |
|---|---|---|
| build | 常规编译及 host/KVM 测试 | 当前全部自动用例；刷新 boot_image |
| build-asan | `CINUX_HOST_ASAN=ON`、`CINUX_LOCKDEP=ON` | host 用例及其链接的 base/kernel 逻辑；ASan、UBSan、泄漏检查，UB 不恢复执行 |
| build-tsan | `CINUX_HOST_TSAN=ON`、`CINUX_LOCKDEP=ON` | 同一 host 测试树；Tick 的两生产线程与并发读者用例；遇到数据竞争失败 |
| build-checks | `CINUX_UBSAN=ON`、`CINUX_LOCKDEP=ON` | 普通 host 测试和全部 KVM 机内测试；内核 UB 触发 trap，异常导致测试失败 |

ASan 与 TSan 不能放在同一次构建，配置时同时开启会失败。Host sanitizer 选项不传给 boot、user 或 freestanding kernel；内核 UBSan 使用编译器 trap，无需链接宿主 sanitizer runtime。普通 shell 镜像与内核检查镜像均生成；涉及用户态交互的改动，还必须分别实弹验证对应镜像，自动模块测试不能代替该步骤。

lockdep 沿用 legacy 的锁顺序图思路，适配当前单核、关中断的 SpinLedger。它检测历史 AB/BA、传递环、重复获取、错误释放和容量耗尽；睡眠时持有自旋锁的检查继续保留。通用图、位掩码、定长栈、稳定槽表和引用计数在 base。开关由 CMake 生成 constexpr 配置，不引入产品宏，也不改 legacy 分支。

锁身份按地址记录。默认账本中的临时锁在存储回收前必须调用 `Spinlock::retire()`，或使用随测试世界一起销毁的独立 SpinLedger；RamFs 析构已接入 retire。常驻内核锁保持平凡析构，以满足常量初始化单例的约束。图最多记录 64 把锁、16 层同时持有，达到上限会失败，不能静默丢弃边。

当前 lockdep 覆盖自旋锁获取顺序，不代表完整 Mutex 持有顺序验证或 SMP 检查。Host TSan 也不检测真实 IRQ/汇编上下文切换。这些边界必须与结果一起说明；后续真实并发逻辑应新增相应并发用例。

站 34 的完整内核自编译实弹使用独立磁盘，先组装源码卷，再启动 KVM：

```sh
cmake -S . -B build
cmake --build build --parallel 4 --target kernel_selfhost_volume
python3 scripts/test_station34_kernel_selfhost.py
```

宿主导出当前 CMake 配置的编译、链接命令，准备 GCC/G++、binutils、Python 运行环境、源码、生成配置和引导器。源码卷不携带内核对象或已链接内核。Cinux 从空输出目录编译全部内核源文件，在机内生成资源对象和 kallsyms、完成两轮链接、核验镜像头并封装启动盘，经 `fsync` 落盘。验收脚本关闭 QEMU 后提取产物，核对机内 SHA-256，再用这个产物启动，并重新编译、运行 hello。

产物是 `build/boot/cinux-selfhost.img` 和 `build/kernel/cinux_kernel_selfhost`；编译及重启日志分别为 `build/station34-kernel-selfhost.serial.log`、`build/station34-kernel-selfhost-reboot.serial.log`。源码卷为 256 MiB，guest 内存为 512 MiB。该步骤编译完整内核；引导器和用户态工具链作为自举输入保留。`build-checks` 可用同样的目标和 `--image build-checks/boot/cinux.img` 验证内核检查配置。全部构建树与 QEMU 仍须串行运行。

第一批性能优化将根文件系统块缓存从固定 32 KiB 改为按初始化时空闲 RAM 的 1/32 预算、最大 64 MiB，缓存页按需分配，使用哈希索引与 CLOCK 淘汰。2026-10-26 在相同 KVM、2 vCPU、512 MiB、串行编译及原有 `-g -O2` 参数下，前后匹配的 138 个编译单元累计耗时为 834.685s / 281.288s，约快 2.97 倍；完整 188 次编译、两轮链接及镜像封装共 369.811s，产物启动与 GCC 编译运行通过。旧基线在后续目录容量限制处失败，因此只比较双方完成的相同单元，不作完整构建耗时对比。ext2 目录按需增长、动态描述符表及 `RLIMIT_NOFILE` 同时保证完整链接能够完成；统一文件页缓存、目录项缓存及并行构建留待后续测量。
