---
title: Lab 033b · 懒创建终端:让点 Shell 真的弹出窗口
---

# Lab 033b · 懒创建终端:让点 Shell 真的弹出窗口

> 这个实验对应主书 [033b · 懒创建终端](../../book/09-gui/033b-gui-lazy-terminal.md),接在 [Lab 033](lab-033-gui-desktop.md) 之后。我们不在 lab 里贴完整答案代码——你要自己把 033 那个"点图标只存了意图、还没人消费"的桌面,改成"点 Shell 真的弹出终端窗口"。
>
> 033 收尾时,点 Shell 图标只往 `pending_icon_action_` 槽里塞了一个 `OpenShell`,接下来就没人理了。这个 lab 要做的,就是给 tick 接上消费者,让那枚意图真的变成一个绑好管道的终端窗口。注意:这里只有 shell 能开窗,**Calculator 图标依然没有消费者**——那是下一个里程碑(fork/exec)的事,本 lab 诚实地不碰。

## 实验目标

把"开机就创建终端"的老路子改成"**用户点 Shell 图标才懒创建终端**"。

要点亮的是:`gui_start()` 从返回 `Terminal*` 改成返回 `void`(开机不再造终端)、模块级暂存 shell 的管道指针、一个 `create_shell_terminal()` helper 在点 Shell 时现场 new 出终端并绑上之前存好的管道、`window.hpp`/`terminal.hpp` 加一个 `is_terminal()` 虚函数让 tick 泛化地 poll 终端(而不是 unsafe 地把任意窗口 `static_cast` 成 `Terminal*`)、`init.cpp` 把管道接线顺序重排。最后会引出一个真实的时序坑:**shell 在 boot 时就起来了,而终端窗口要到点击才出生——shell 先写的输出在终端出生前去哪儿了?**

## 前置条件

- 跑通 Lab 033:WindowManager 的图标三件套(`add_desktop_icon`/`hit_test_icon`/`consume_pending_icon_action`)、`composite()` 在 clear 与窗口之间插的 `draw_desktop_icons`、`handle_mouse` 命中图标时把 action 存进 `pending_icon_action_`。
- 跑通 Lab 031b:内核管道 `Pipe`(4 KB 环形缓冲、`try_read`/`try_write` 非阻塞、满了/空了的阻塞语义)、`PipeReadOps`/`PipeWriteOps` 把管道伪装成文件、`FDTable::set`、`init.cpp` 把 fd0/fd1 接成管道。
- 知道 032 那条 `gui_tick_callback` 里对 `wm.focused()` 做 `static_cast<Terminal*>` 然后调 `poll_output()` 的 poll 路径。

## 任务分解

### 任务 1:`gui_start()` 改 void,开机不再造终端

032 的 `gui_start()` 返回 `Terminal*`,开机就 new 一个终端、`add_window`、把指针交还 init。这条路要拆掉。

- 返回类型从 `Terminal*` 改成 `void`。开机那行"new 终端 + add_window"删掉——它现在归点击逻辑管。
- `gui_start()` 现在的职责回归到"注册桌面图标(Shell/Calculator)+ 注册 tick 回调"这两件 033 已经做完的事。
- 想清楚为什么要拆:开机造终端意味着"桌面永远先有一个终端",而 033 的设计是"桌面先空着,终端按需出生"。返回 `void` 是为了让 init **没法**再拿到那个开机终端去接管道——逼着管道接线走新的暂存路径。

### 任务 2:模块级暂存管道指针 + `set_shell_pipes`

管道不能再像 031b 那样由 init 直接塞给某个开机终端了——因为那个终端还没出生。

- 在 `gui_init.cpp` 的匿名命名空间里加两个 `Pipe*` 模块级指针(初值 `nullptr`)。
- 新增 `void set_shell_pipes(Pipe* stdin_pipe, Pipe* stdout_pipe)`:把两个指针存进模块变量,打印一行 `[GUI] Shell pipes stored: stdin=%p stdout=%p` 方便确认。
- 头文件里前向声明 `namespace cinux::ipc { class Pipe; }`(取代原来的 `class Terminal;` 前向声明)。
- 这两个指针是"管道的占位符":init 先把管道建好、指针存进来,**等终端出生时**再让终端去用它们。

### 任务 3:`create_shell_terminal()` helper

在匿名命名空间里加一个 helper,点 Shell 时调用。

- 算窗口尺寸:宽 = `Terminal::COLS * 8`、高 = `Terminal::ROWS * 16`。位置先给个默认(左上偏移),如果 `g_screen` 非空且放得下就居中。
- `new Terminal(x, y, "Cinux Terminal")` + `set_font(g_font)`。
- 把任务 2 存好的两个 `Pipe*` 绑上去:`set_stdin_pipe(g_stdin_pipe)` / `set_stdout_pipe(g_stdout_pipe)`——**绑的是同一对指针**,不是新管道。
- `wm.add_window(term)`,打印 `[GUI] Shell terminal created and connected.`。
- 关键点:每次点 Shell 都 new 一个新终端,但它们**共享同一对全局管道指针**。这意味着本 lab 里只有一个 shell 进程(多 shell 要等 fork/exec)——这一点要在代码注释或报告里诚实标注。

### 任务 4:tick 接上消费者

改 `gui_tick_callback`。

- 回调开头:**先** `consume_pending_icon_action()` 取出 033 存进槽里的动作。取出即清零(`consume` 的语义是取出-清零,见 033)。
- 只有 `OpenShell` 一个分支:命中就 `create_shell_terminal()`。
- **不要写 `OpenCalculator` 分支**。这是诚实的:Calculator 没有对应的窗口类,也没有 helper。点 Calculator 的动作被 consume 取出来后,在 `if (OpenShell)` 里不匹配,静默落地——什么都不发生。

### 任务 5:`is_terminal()` 虚函数,替换嗅探式 downcast

032 的 tick 里是"拿到 `focused()`,无脑 `static_cast<Terminal*>`,然后调 `poll_output()`"。这在"桌面只有终端窗口"时凑合,但 033 之后桌面迟早会出现非终端窗口(虽然本 lab 还没有)——把一个普通窗口 unsafe downcast 成 `Terminal*` 再调虚函数,是定时炸弹。

- `window.hpp` 基类加 `virtual bool is_terminal() const { return false; }`。
- `terminal.hpp` 派生加 `bool is_terminal() const override { return true; }`。
- tick 的 poll 路径改成:`focused != nullptr && focused->is_terminal()` 为真,才 `static_cast<Terminal*>(focused)` 调 `poll_output()` + `render_to_canvas()`。
- 想清楚为什么用虚函数而不是别的:运行期由对象自己的 vtable 分发,基类默认 false、终端 override 成 true,调用方只问 `is_terminal()` 就知道能不能安全 downcast,不必"嗅探"对象类型。这是 C++ 虚函数的标准用法([cppreference: virtual](https://en.cppreference.com/w/cpp/language/virtual))。

### 任务 6:终端析构只清指针,不关管道

031b 的 `Terminal` 析构会 `close_writer`/`close_reader`——因为那时一个终端独占一对管道,终端死了管道也该关。懒创建下这个假设破了。

- 析构改成**只把两个 `Pipe*` 成员置 `nullptr`,不调 `close_*`**。
- 注释写明原因:管道由外部(init)持有,懒创建模式下同一对管道可能先后接多个终端(用户点 Shell、关掉、再点);如果析构关了端,shell 会误以为读端/写端走了,后续行为错乱。
- 这条和任务 3 的"共享管道"是同一个设计决策的两面。

### 任务 7:`init.cpp` 重排管道接线

031b 的 init 顺序是 `gui_start()` 拿 term → 建管道 → 绑 fd0/fd1 → `term->set_stdin/stdout_pipe` → `launch_first_user()`。`gui_start` 改 void 之后,中间那步没了。

- 建 stdin/stdout 两个管道、装进 fd0/fd1 的部分**不变**(`new Pipe` → `PipeReadOps`/`PipeWriteOps` → `Inode` → `File` → `set(0/1, ...)`,方向别装反:fd0 读、fd1 写)。
- **新增**:`cinux::gui::set_shell_pipes(stdin_pipe, stdout_pipe);`——把同一对 `Pipe*` 存进 gui 模块,等终端出生时用。
- 打印 `[INIT] Terminal-shell pipes connected: ...`。
- `gui_start();` 不再捕获返回值(它现在是 void),而且**必须挪到 `set_shell_pipes` 之后**。
- `launch_first_user();` 在最后,shell 起来时 fd0/fd1 已是管道。
- 删掉 031b 那行 `term->set_stdin_pipe/set_stdout_pipe` 和 `terminal.hpp` 的 include。

## 接口约束

- `gui_start()` 返回 **`void`**(不是 `Terminal*`)。
- `set_shell_pipes(Pipe* stdin_pipe, Pipe* stdout_pipe)` 存指针不持有所有权(不 `delete`),指针为 `nullptr` 时 `create_shell_terminal` 应跳过对应绑定(别解空指针)。
- `create_shell_terminal()` 尺寸:`COLS*8 × ROWS*16`(终端字符网格 8px 宽 16px 高);`add_window` 后才上屏。
- tick 消费顺序:**先 consume pending action 开窗,再 poll focused**;consume 命中 `OpenShell` 当拍就会 `create_shell_terminal()`(内部 `add_window` → `update_focus` 把新终端立刻设为 focused),紧接其后的 poll 分支**同一拍**就命中它,`poll_output()` 抽干管道缓冲里之前积压的 shell 输出、`render_to_canvas()` 出帧——所以新终端一出生就带着 shell 的欢迎信息,不存在"要等下一拍才 poll"的延迟。
- `is_terminal()`:基类 `false`、`Terminal` override `true`,`const` 且无副作用;**只用在 gui_init 的 poll 路径**,不是给 WM 内部用的类型判断。
- 终端析构:**不关管道**,只置指针为 `nullptr`。
- `init.cpp` 顺序:`set_shell_pipes` 必须在 `gui_start` 与 `launch_first_user` **之前**。

## 验证步骤

**第一步:host 单元测试**(ctest 名 `desktop`,镜像了 WM 的图标层逻辑):

```bash
ctest --test-dir build -R "desktop" --output-on-failure
```

预期:聚焦消费行为的关键测试过——`"desktop: icon click sets pending_icon_action"`、`"desktop: different icons set different actions"`、`"desktop: consume_pending_icon_action returns None when empty"`、`"... resets to None"`、`"desktop: multiple consumes without click all return None"`。注意 host 测试是 mock 重新实现的镜像(不链 kernel 代码),它验证的是 WM 图标层的契约,不是懒创建本身——懒创建的端到端验证靠下面两步。

**第二步:QEMU kernel 测试**(入口 `run_desktop_tests()`,`TEST_SECTION("Desktop Tests (033_gui_desktop)")`):

```bash
cmake --build build --target run-big-kernel-test
```

预期:`test_desktop_click_sets_and_consumes_action`(MouseDown 命中 → consume 得 `OpenShell` → 再 consume 得 `None`)、`test_desktop_consume_no_pending`、`test_desktop_click_no_icon_no_action`、`test_desktop_click_window_no_icon_action`、`test_desktop_full_scenario` 等过,QEMU 退 1。

**第三步:端到端视觉验证**:

```bash
cmake --build build --target run
```

预期串口的相对顺序(注意 `set_shell_pipes` 在 `gui_start()` 之前,所以前两行打在 milestone 之前):
```text
[GUI] Shell pipes stored: stdin=0x... stdout=0x...
[INIT] Terminal-shell pipes connected: stdin_pipe=0x... stdout_pipe=0x...
...
[GUI] ===== Milestone 033: GUI Desktop =====
[GUI] Desktop icons registered: Shell, Calculator.
[GUI] GUI tick callback registered on PIT.
```

开机进 GUI,看到桌面两个图标。**点 Shell** → 串口出现 `[GUI] Shell terminal created and connected.`,屏幕足够大时居中弹出终端窗口(否则落在兜底位置 `(80,60)`),窗口里出现 shell 提示符(开机到现在 shell 往管道里写的东西被这次 tick 的 `poll_output` 抽干落屏)。**点 Calculator** → 串口无新行、桌面无新窗、什么都没发生——这就是本 lab 的诚实边界。

## 常见故障

- **开机就有一个终端窗口、不是点 Shell 才出**:任务 1 没做干净,`gui_start()` 里还留着开机 new 终端 + `add_window`,或者 init 还在用 `gui_start()` 的返回值。开机桌面应该是**空的**(只有两个图标)。
- **点 Shell 出窗了但窗口空白、没有 shell 提示符**:任务 7 的 `set_shell_pipes` 没在 `launch_first_user` 之前调,或根本没调——shell 起来时管道指针还没存进 gui 模块,`create_shell_terminal` 绑到的是 `nullptr`,终端和管道没接上。检查串口有没有那两行 `Shell pipes stored` / `Terminal-shell pipes connected`。
- **shell 输出丢了、只看到点 Shell 之后敲的命令**:`poll_output` 用了阻塞 `read` 而非 `try_read`,或者懒创建下 shell 在终端出生前写满 4KB 管道**阻塞了**。这是真实的时序陷阱:生产者(shell,boot 时起)先于消费者(Terminal,点击才生)启动,shell 的早期输出全暂存在管道环形缓冲里([pipe(7)](https://man7.org/linux/man-pages/man7/pipe.7.html):管道写满则 write 阻塞);缓冲满之前 shell 不会卡,终端出生后 `poll_output` 抽干就恢复。若 shell 启动横幅太长把缓冲写满、用户又迟迟不点 Shell,shell 会自旋等——这是有界缓冲的固有代价,见 031b 的 `sti;hlt` 自旋语义。
- **关掉终端窗口后再点 Shell,新窗没输出 / shell 卡死**:任务 6 没做,析构还是 `close_writer`/`close_reader`,第一次关窗就把管道端关了,shell 误以为对端走了。懒创建下管道生命周期独立于终端,析构只能清指针。
- **点 Calculator 弹了个奇怪的东西 / 报段错误**:别给 `OpenCalculator` 加分支。本 lab 没有 Calculator 窗口类,加分支只会调到不存在的类或乱 `static_cast`。正确的现象是点 Calculator **没反应**,这是诚实的。
- **桌面冻住 / tick 卡死**:`create_shell_terminal` 里某个步骤阻塞了,或 poll 路径忘了 `is_terminal()` 判空就 `static_cast` 到一个非终端窗口(虽然本 lab 桌面只有终端,但养成判 `is_terminal()` 的习惯能避免后面引入新窗口类型时翻车)。

## 通过标准

- [ ] host `ctest -R "desktop"` 全绿,`test_host` 整体不回归。
- [ ] `run-big-kernel-test` 里 `run_desktop_tests` 通过(尤其 `test_desktop_click_sets_and_consumes_action`),QEMU 退 1。
- [ ] `gui_start()` 返回 void、开机桌面只有图标无终端;`set_shell_pipes` 暂存指针;`create_shell_terminal` 点 Shell 时 new 终端 + 绑同一对 `Pipe*` + `add_window`。
- [ ] tick 先 `consume_pending_icon_action` 再 poll;`if (OpenShell) create_shell_terminal()`,**无 OpenCalculator 分支**。
- [ ] `window.hpp` 基类 `is_terminal()` 返 false、`terminal.hpp` override 返 true;tick poll 路径用 `focused->is_terminal()` 守门再 downcast。
- [ ] `Terminal` 析构只清指针、不关管道。
- [ ] `init.cpp` 顺序:`set_shell_pipes` → `gui_start()`(void)→ `launch_first_user()`,管道在 shell 起来前就绪。
- [ ] 端到端:点 Shell 弹出居中终端窗、出现 shell 提示符;点 Calculator **无反应**。
- [ ] 在代码或报告里**诚实标注**两条边界:① 本 lab 没有独立 shell,多终端共享同一对全局 `Pipe*`(独立 shell 要等 fork/exec);② Calculator 图标在本 lab 没有消费者,点它什么都不会发生——这是有意的中间态,不是 bug。不把未实现的东西写成已工作。
