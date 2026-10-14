# Cinux GDB MCP

`gdb_server.py` 通过 stdio 提供 MCP 工具，管理一台 QEMU 和一个持久 GDB/MI
会话。运行需要 `gdb`、`qemu-system-x86_64`、可用的 `/dev/kvm` 和构建好的镜像。
生产调试仍要求 KVM，不会自动切换到 TCG。

更新脚本后重启 Cinux-gdb MCP 服务，加载新的工具列表。

## 常用流程

1. `debug_boot {"stop_at":"KernelEntry"}`：启动并尝试停在内核入口。
2. `debug_break {"location":"cinux::loader::LoadElfIntoSpace"}`：设置硬件断点，
   返回断点编号和 GDB 解析结果。
3. `debug_continue {"timeout":5}`：继续并等待最多 5 秒。
4. 返回 `running` 时，会话和虚拟机仍然保留，可以直接：
   - `debug_serial {"lines":30}` 读取串口；
   - `debug_status {}` 查看运行状态、进程和日志路径；
   - `debug_wait {"timeout":5}` 等待断点命中；
   - `debug_interrupt {}` 暂停，然后读取寄存器、栈和内存。
5. `debug_step {}`：单步一条指令，等待这次单步的停止事件。
6. `debug_quit {}`：关闭 GDB 和 QEMU，返回串口尾部；重复关闭也可用。

`debug_boot`、`debug_continue`、`debug_wait`、`debug_interrupt`、`debug_step`
的 `timeout` 默认 10 秒，范围 0–30 秒。`debug_continue {"timeout":0}` 启动执行
后立即返回；目标已经运行时，再次 continue 只等待，不重复发送执行命令。
读寄存器、内存和栈之前使用 `debug_interrupt` 暂停运行中的目标。

硬件断点适合在内核尚未从磁盘加载时设置；早期软件断点可能被加载过程覆盖。
停止摘要包含原因、指令地址、函数、源码位置和断点编号（取决于 GDB 提供的字段）。

## 错误与恢复

普通 GDB 错误、无效断点和参数错误以 MCP `isError` 工具结果返回，保留会话。
等待停止超时属于正常结果：返回 `running`，不会销毁虚拟机。

`debug_eval` 支持带引号和反斜杠的控制台命令，例如：

```json
{"command":"printf \"rip=%#lx\\n\", $rip"}
```

控制台命令默认限时 5 秒，可以指定 `timeout`。常见的 `continue/c`、`stepi/si`、
`interrupt` 命令会使用异步执行路径；其他阻塞命令超时后尝试发送 SIGINT 取消。
若 GDB 没有确认取消，先使用 `debug_reconnect`，避免后续命令与尚未完成的命令混在一起。

GDB 退出或连接不可用时，`debug_serial` 和 `debug_status` 仍可调用。
`debug_reconnect {}` 更换 GDB 并重新连接现有 QEMU，不重启虚拟机，恢复通过
`debug_break` / `debug_boot` 创建且尚未删除或命中的断点。断点编号可能改变。
通过 `debug_eval` 创建的断点、修改的断点条件及其他 GDB 设置需要手动重设。
如果 QEMU 已经退出，查看日志后使用 `debug_boot` 启动新机器。

每个会话使用独立的 `/tmp/cinux-gdb-*` 日志目录和仅监听 localhost 的临时端口，
不会用 `pkill` 清理其他会话。串口和 QEMU 错误日志在关闭后保留，路径见 `debug_status`。
MCP 服务正常退出或收到 SIGTERM 时会清理自己启动的子进程。

## 验证

协议测试不需要 KVM 或监听网络端口：

```sh
python3 -m unittest discover -s scripts/mcp -p 'test_gdb*.py' -v
```

真实 GDB/QEMU 集成测试可选开启。它使用临时启动扇区和 TCG，验证超时、串口、
暂停、单步和 GDB 重连，不修改项目镜像：

```sh
CINUX_GDB_QEMU_TEST=1 python3 -m unittest discover -s scripts/mcp -p 'test_gdb*.py' -v
```

MI 驱动遵循 GDB 的[异步执行模式](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Asynchronous-and-non_002dstop-modes.html)
和[执行命令](https://www.sourceware.org/gdb/current/onlinedocs/gdb.html/GDB_002fMI-Program-Execution.html)：
命令确认和目标停止分别处理，不用 `(gdb)` 提示符判断指令是否执行完毕。
