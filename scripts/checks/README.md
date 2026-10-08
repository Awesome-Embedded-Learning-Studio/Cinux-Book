# 必跑验收

代码最终验收必须运行普通构建与测试、ASan+UBSan、TSan，以及开启 lockdep 和内核 UBSan 的构建与测试：

```sh
CINUX_CHECK_JOBS=4 scripts/checks/acceptance.sh
```

任何一步失败都会停止；不得用关闭检查、忽略退出码或跳过失败用例来报“验收通过”。QEMU 必须有可访问的 `/dev/kvm`，不回退 TCG。沙箱或容器若阻止 sanitizer 运行，应在允许的宿主环境重跑；未运行成功必须明确记录阻塞项。

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
