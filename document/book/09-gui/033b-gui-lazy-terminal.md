---
title: 033b · 懒创建终端:把桌面图标接成活的东西
---

# 033b · 懒创建终端:把桌面图标接成活的东西

> 033 让桌面长出了两个图标,点 Shell 图标也已经能弹出终端——`gui_tick_callback` 消费到 `OpenShell` 就调 `create_shell_terminal()`。可这个终端是怎么"出生"的?它不是开机就造好的,而是点图标那一刻才 `new` 出来的——这就是"懒创建"。033b 要细看的就是这套懒创建时序:把 031b 那套"开机就建终端"的接线改掉,让终端的诞生时机从 boot 推迟到 click;管道指针先存进 GUI、点击时才绑到新终端上;shell 从开机起就往管道写,终端出生前那些字节先在 4 KB 缓冲里排队。所以这一章的两条线其实是同一件事的正反面:消费 pending 动作开出终端,以及让这个终端是"点图标才出生"的懒创建终端。

## 这一章我们要点亮什么

开机进 GUI,桌面是暗青底色加两个图标(Shell / Calculator)。你把鼠标挪到 Shell 图标上点一下,屏幕足够大时居中弹出一个 640×400 的终端窗口(否则落在兜底位置 `(80,60)`),里面立刻出现 Cinux shell 的提示符——你敲 `help` 回车,命令列表刷在窗口里。再点一下 Shell 图标,又弹一个。这条"点图标→终端出生→终端已经在跑 shell→输出回显"的链路,033 已经让它跑通(点 Shell 就 `create_shell_terminal`);033b 要讲清的是这个终端怎么"懒创建"出来——点图标那一刻才 new,管道先存后绑,shell 输出在终端出生前先在缓冲里排队。

它由三股改动拧成。第一股在 [gui_init.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/gui_init.cpp):`gui_start()` 不再开机造终端(返回类型从 `Terminal*` 变 `void`),管道指针被两个模块级变量收着,等点击时才由 `create_shell_terminal()` 把它们绑到一个新建的 `Terminal` 上。第二股是 `gui_tick_callback` 的改写:每个 tick 先 `consume_pending_icon_action()` 把 033 存的意图取出来,若是 `OpenShell` 就 `create_shell_terminal()`;接着只有当 focused 窗口"真的是终端"时才去 `poll_output()` 抽 shell 的输出——为此我们给窗口基类加了一个 `is_terminal()` 虚函数,替代原先硬邦邦的 `static_cast<Terminal*>` 嗅探。第三股在 [init.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/init.cpp):管道照旧在开机时建好并装进 fd 0/1,但不再 `term->set_stdin_pipe(...)`,改成 `set_shell_pipes(stdin_pipe, stdout_pipe)` 把指针先存进 GUI 子系统,留给以后那个"延迟出生"的终端。

还有一条贯穿全章的暗线:shell 进程从 boot 那一刻起就在往 stdout 管道里写字节,可终端窗口要等用户点图标才会出现。那段时间里,shell 的输出住在哪里?住在管道的 4 KB 环形缓冲里。终端一出生,`poll_output()` 用非阻塞的 `try_read` 把它们抽干、画到屏幕。这就是"生产者(shell)先于消费者(终端)启动、有界缓冲暂存"的真实时序——031b 的管道语义在这一章终于迎来了它最不平凡的一次使用。

## 为什么现在需要它

031b 给我们的接线是这样的:开机进 `kernel_init_thread`,先 `gui_start()` 拿到一个 `Terminal*`,立刻建管道、绑到 fd 0/1、`term->set_stdin_pipe(...)` 把这个终端和管道焊死,然后 `launch_first_user()` 起 shell。那个终端是 boot 时就造好的,开机即占据桌面,管道也从第一拍就和它绑在一起。

这套在"桌面 = 一个终端"的年代没问题。可 033 把桌面变成了"有一排图标、点哪个开哪个"的模型——终端不再是开机默认出现的那个窗口,而是"点 Shell 图标才该出现"的窗口之一(以后还有 Calculator,以后还有别的)。于是矛盾来了:如果还按 031b 那样开机就造终端,那桌面一进来就有一个终端杵在那儿,完全违背了"点图标才开"的交互;可如果开机不造终端,shell 又是开机就起的(`launch_first_user` 没法推迟到点击之后——shell 是 ring-3 的第一个用户进程,init 线程的本职工作就是拉起它),它一跑起来就往 stdout 写,谁来接?

答案是把"造终端"和"起 shell"解耦:shell 照旧开机起,它的 fd 0/1 照旧在开机时被装成管道;但管道的另一端不立刻绑终端,而是把管道指针先存进 GUI 子系统的一个角落。用户点 Shell 图标那一刻,GUI 才 `new` 一个终端,把存好的那对指针绑上去,`add_window` 推上桌面。在终端出生之前,shell 写出的字节都老老实实待在管道缓冲里排队。这套"懒创建"的好处是交互上干净(桌面初始是空的,图标就是启动器),代价是我们得认真对待"生产者先于消费者"的那段时间——这正是后面"调试现场"要细讲的地方。

## 设计图

把 031b 的"开机即建"和 033b 的"点击才建"放在一起对比,差别一眼就看清楚:

```text
=== 031b(旧):终端开机即建 ===
boot 时:
  gui_start() ────────────▶ new Terminal (开机就造)
                             │
  建 stdin/stdout 管道        │ term->set_stdin_pipe(stdin_pipe)
  装进 fd 0/1                 │ term->set_stdout_pipe(stdout_pipe)
                             ▼
  launch_first_user() ───▶ shell 起,fd0/fd1 已是管道,立刻有终端接它的 I/O

=== 033b(新):终端懒创建 ===
boot 时:
  建 stdin/stdout 管道,装进 fd 0/1          (管道照旧 boot 时建)
  set_shell_pipes(stdin, stdout) ──▶ g_stdin_pipe / g_stdout_pipe (只存指针)
  gui_start() ──▶ 注册 Shell/Calculator 图标(不造终端)
  launch_first_user() ──▶ shell 起,立刻往 stdout 管道写
                              │
                              │ 终端还没出生!输出先在 4KB 管道缓冲里排队
                              ▼
... 用户点 Shell 图标(发生在某次 tick 的事件排空阶段)...
  handle_mouse: 命中 Shell 图标 → pending_icon_action_ = OpenShell
  同一拍 tick 的后半段(事件排空之后):
    consume_pending_icon_action() = OpenShell
    create_shell_terminal():
       new Terminal(居中 640×400)
       term->set_stdin_pipe(g_stdin_pipe)     ← 绑同一根已存指针
       term->set_stdout_pipe(g_stdout_pipe)
       wm.add_window(term) → update_focus 把新终端设为 focused
    focused->is_terminal()? → poll_output() try_read 抽干缓冲里的旧输出
                            → render_to_canvas() 画出来
    composite() 成帧
```

右半张图里有两处值得停一下。先看 shell 写出的字节——在终端出生前它们并不是无处可去,管道的 4 KB 环形缓冲充当了"蓄水池",终端出生后用 `try_read` 一次性抽干。再看"绑同一根已存指针"这句话,它的分量不轻:boot 时建的那对管道,和点击时建的终端,是用指针解耦的,指针先存、终端后绑,中间隔着任意长的时间。也正因为同一对管道可能先后接不同的终端(关一个再开一个),我们才不敢在终端析构时关管道端——这点后面专门讲。

## 代码路线

### gui_init.cpp:gui_start() 改 void、管道指针搬进模块变量

先看模块顶部多了什么。033b 在 [gui_init.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/gui_init.cpp) 的匿名命名空间里加了两个指针,外加一个把它们写进去的函数:

```cpp
namespace {
cinux::drivers::Canvas*  g_screen      = nullptr;
cinux::drivers::PSFFont* g_font        = nullptr;

// Shell pipe pointers set by set_shell_pipes() before gui_start()
cinux::ipc::Pipe* g_stdin_pipe  = nullptr;
cinux::ipc::Pipe* g_stdout_pipe = nullptr;
}  // anonymous namespace

void set_shell_pipes(cinux::ipc::Pipe* stdin_pipe, cinux::ipc::Pipe* stdout_pipe) {
    g_stdin_pipe  = stdin_pipe;
    g_stdout_pipe = stdout_pipe;
    cinux::lib::kprintf("[GUI] Shell pipes stored: stdin=%p stdout=%p\n", ...);
}
```

`g_screen`/`g_font` 是 030 就有的、给 tick 和 `create_shell_terminal` 复用的画布和字体。新来的 `g_stdin_pipe`/`g_stdout_pipe` 就是"管道指针的暂存处"——`set_shell_pipes` 由 `init.cpp` 在 boot 时调用,把建好的管道指针存进来,谁都不动它们,直到点击触发 `create_shell_terminal` 取用。头文件 [gui_init.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/gui_init.hpp) 那头也跟着改:前向声明从 `class Terminal;` 换成 `namespace cinux::ipc { class Pipe; }`,`gui_start()` 的返回类型从 `Terminal*` 改成 `void`。返回 void 这件事本身就是一句宣言——开机不再交付一个终端对象,调用方也别指望拿到它。

### create_shell_terminal():点击时才发生的事

`gui_tick_callback` 消费到 `OpenShell` 时调的就是这个 helper,它藏在匿名命名空间里:

```cpp
void create_shell_terminal() {
    auto& wm = WindowManager::instance();

    uint32_t term_w = Terminal::COLS * 8;   // 80 * 8 = 640
    uint32_t term_h = Terminal::ROWS * 16;  // 25 * 16 = 400
    uint32_t term_x = 80, term_y = 60;      // 兜底位置

    if (g_screen != nullptr) {              // 能放下就居中
        uint32_t sw = g_screen->width();
        uint32_t sh = g_screen->height();
        if (term_w + 80 < sw) term_x = (sw - term_w) / 2;
        if (term_h + 60 < sh) term_y = (sh - term_h) / 2;
    }

    auto* term = new Terminal(term_x, term_y, "Cinux Terminal");
    term->set_font(g_font);

    if (g_stdin_pipe  != nullptr) term->set_stdin_pipe(g_stdin_pipe);
    if (g_stdout_pipe != nullptr) term->set_stdout_pipe(g_stdout_pipe);

    wm.add_window(term);
    cinux::lib::kprintf("[GUI] Shell terminal created and connected.\n");
}
```

几件事按顺序发生。先是算位置:终端的尺寸由 `Terminal::COLS/ROWS` 定死(640×400),`create_shell_terminal` 只决定把它摆在屏幕的哪儿;兜底是 `(80, 60)`,如果屏幕够大就改成正中。然后 `new Terminal` 一个出来、`set_font`。接着是最关键的两行——**把存好的管道指针绑到这个新终端上**。注意是"绑同一根指针":`g_stdin_pipe` 就是 boot 时 `set_shell_pipes` 存进来的那根,不是另建一根。绑完 `wm.add_window(term)` 推上桌面——`add_window` 会调 `update_focus()` 把焦点立刻设到这个新终端上,所以紧接着同一拍 tick 的 poll 分支就会轮到它。

那两个 `if (... != nullptr)` 不是多此一举。它给了一种"管道还没存好就点了图标"的退化情况留了活路——终端照样能建出来、能摆上去,只是暂时没有 shell 输出可读。正常流程里 `set_shell_pipes` 总在点击之前就调过了,这两个判断基本恒真;但留着它们,`create_shell_terminal` 就不假定调用顺序,自己更健壮。

### gui_tick_callback:先消费图标动作,再按 is_terminal 轮询

整个改写的核心在 tick 回调的后半段。事件队列排空、`wm.handle_mouse` 喂完之后:

```cpp
// Check if a desktop icon was clicked
IconAction action = wm.consume_pending_icon_action();
if (action == IconAction::OpenShell) {
    create_shell_terminal();
}

// Poll the focused terminal for shell output (if it has a stdout pipe)
auto* focused = wm.focused();
if (focused != nullptr && focused->is_terminal()) {
    auto* term = static_cast<Terminal*>(focused);
    term->poll_output();
    term->render_to_canvas();
}

wm.composite();
```

前两行把点击链路接通:每个 tick 都去 WM 那里 `consume_pending_icon_action()`——这个调用是"取出并清零",有就拿到、没有就拿到 `None`。033 在 `handle_mouse` 里命中图标时往 `pending_icon_action_` 写了 `OpenShell`,这里就把它取出来,调 `create_shell_terminal()`。取出即清零,所以同一个点击只触发一次开窗,下一拍 tick 再 consume 拿到的就是 `None` 了。

下面三行是 poll 路径,也是 `is_terminal()` 出场的地方。032 的写法是 `if (focused != nullptr)` 直接 `static_cast<Terminal*>(focused)`,那时桌面上的窗口基本都是终端,这么硬转没出过事。可 033b 之后桌面会有图标、会有非终端窗口,对一个不是 `Terminal` 的窗口 `static_cast` 是一次未定义行为——编译期挡不住,运行期可能给你一个错位的指针。所以判断改成 `focused != nullptr && focused->is_terminal()`:先用虚函数问一句"你真的是终端吗",是才转、才 poll。

### is_terminal():为什么用虚函数,而不是 static_cast 嗅探

这里值得多停一下,因为它是一个"小改动、大动机"的典型。窗口继承体系在 [window.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/window.hpp) 里,基类加了这么一行:

```cpp
virtual void on_paint(cinux::drivers::Canvas& canvas) { (void)canvas; }
// ...
virtual bool is_terminal() const { return false; }
```

而 [terminal.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/terminal.hpp) 派生类里:

```cpp
bool is_terminal() const override { return true; }
```

基类默认说"我不是终端",`Terminal` override 成"我是"。于是 gui_init 不用 `#include` 任何窗口派生类的头文件、不用 `dynamic_cast`、不用在窗口里塞一个 `enum WindowType` 字段,只通过虚函数分发问一句就能安全区分。C++ 的虚函数正是为这种"基类指针、运行期才知道实际类型、想按类型分流"的场景准备的——派发发生在运行期,由对象的 vtable 决定走基类版本还是派生版本。

为什么不用 `static_cast` 直接转?因为 `static_cast` 不做运行期检查,它假定你转的类型是对的,错了也不报错。在"focused 可能是任意窗口"的泛化路径里,这种"无检查的强转"就是把炸弹埋在代码里——只要哪天桌面上 focused 的是一个非终端窗口,tick 就会拿一个被错位的指针去调 `poll_output()`,后果不可预测。`dynamic_cast` 能查,但它要求 RTTI、要求基类有虚函数(这里恰好有)、而且语义是"转失败返回 nullptr",对一个只想做布尔判断的场景来说太重。加一个 `is_terminal()` 虚函数,既安全又轻,意图也比"转一下试试"清楚得多——这就是它唯一的用途,全代码库只 gui_init 的 poll 路径用到。

### terminal.cpp:析构只清指针,不关管道

这是个容易被略过、但和懒创建深度配套的改动。032 的 `Terminal::~Terminal()` 会 `stdin_pipe_->close_writer()` / `stdout_pipe_->close_reader()`——关掉管道端,让对面的 shell 知道"终端没了,你可以退出了"。033b 改成:

```cpp
Terminal::~Terminal() {
    // Pipes are owned externally (by the process that created them).
    // Just clear our references — do NOT close the pipe endpoints,
    // since multiple terminals may share the same pipe pair.
    stdin_pipe_  = nullptr;
    stdout_pipe_ = nullptr;
}
```

只清自己手里的指针,不动管道。原因就写在注释里:懒创建模式下,boot 时建的那对管道是"外部持有"的,它不属于任何一个终端——用户可能开了终端 A、关掉、再开终端 B,A 和 B 共享同一对管道(都绑的 `g_stdin_pipe`/`g_stdout_pipe`)。如果 A 析构时把管道端关了,B 还没出生呢,管道就废了;更糟的是对面的 shell 会因为读端/写端关闭而以为"终端都死光了",提前退出。所以终端不再拥有管道的所有权,它只是"借用"指针,生死都和管道无关。管道的真正生命周期跟着 init 那个 boot 时 `new` 出来的对象走,谁建谁管。

这条改动的潜台词是:031b 那套"终端析构触发 shell 干净退出"的机制,在懒创建下不再自动生效。关一个终端窗口,shell 不会被通知,它还在 boot 时那对管道上继续跑;再开一个终端,接着读同一根管道里后续的输出。这是单 shell 多终端视图的必然代价——要真正每开一个终端就独立起一个 shell,得等能动态 fork 进程的 034。

### init.cpp:管道照建,绑法变了

最后看 [init.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/proc/init.cpp) 的 `kernel_init_thread`,GUI 那段(`#ifdef CINUX_GUI`)现在的顺序是:挂 VFS → 建 stdin 管道 → 建 stdout 管道 → 装进 fd 0/1 → `set_shell_pipes` → 打印 → `gui_start()` → `launch_first_user()`。建管道的代码和 031b 一模一样,变的是后面怎么用:

```cpp
// Bind fd 0 (stdin) to stdin_pipe read end
auto* stdin_file = new cinux::fs::File(stdin_read_inode, 0, cinux::fs::OpenFlags::RDONLY);
cinux::fs::g_global_fd_table().set(0, stdin_file);
// Bind fd 1 (stdout) to stdout_pipe write end
auto* stdout_file = new cinux::fs::File(stdout_write_inode, 0, cinux::fs::OpenFlags::WRONLY);
cinux::fs::g_global_fd_table().set(1, stdout_file);

// Store pipe pointers for the GUI subsystem
cinux::gui::set_shell_pipes(stdin_pipe, stdout_pipe);

cinux::lib::kprintf("[INIT] Terminal-shell pipes connected: stdin_pipe=%p stdout_pipe=%p\n", ...);

cinux::gui::gui_start();
// ...
cinux::arch::launch_first_user();
```

对照 031b,这段改动的脉络很清楚。头文件包含那行 `#include "kernel/gui/terminal.hpp"` 没了,因为 init 不再直接碰 `Terminal`。原先 `auto* term = gui_start();` 加上随之而来的 `term->set_stdin_pipe(stdin_pipe); term->set_stdout_pipe(stdout_pipe);` 整组删掉,换成一句 `cinux::gui::set_shell_pipes(stdin_pipe, stdout_pipe);`,把指针交给 GUI 子系统暂存。至于 `gui_start()`,它的返回值也不再被捕获——现在返回 `void`,本来也没东西可捕获。

顺序上有个不能搞反的点:`set_shell_pipes` 必须在 `launch_first_user` **之前**调好。因为 shell 一被拉起就开始往 stdout 管道写,而那根管道的"另一端消费者"虽然是延迟出生的终端,但管道对象本身必须先存在、先被 GUI 记下,这根管道才接得上。`gui_start()` 也排在 `set_shell_pipes` 之后,虽然 `gui_start` 本身不读管道指针(它只注册图标和 tick),但把这个顺序固定下来,逻辑上更清楚:先备好管道,再让桌面和 shell 各就各位。

## 调试现场

这一章没有 notes 踩坑记录,但回路能不能通,几个卡点都是真实的设计陷阱,值得拎出来讲。

### 懒终端时序:shell 先写,终端还没出生

这是全章最容易想当然、也最容易出问题的地方。shell 在 boot 时就被 `launch_first_user` 拉起,它一跑起来——比如打印欢迎信息、显示提示符——就调 `sys_write(1)` 往 stdout 管道写。可这时候终端窗口还没建(用户还没点图标呢),那这些字节去哪了?

它们全堆在 stdout 管道的 4 KB 环形缓冲里。031b 那根管道本来就是个有界缓冲,写满之前 `write` 都能立刻返回;`count_` 从 0 一路涨上去,shell 完全感知不到"对面没人读"。直到用户点 Shell 图标——这一次 `MouseDown` 在事件排空阶段被 `handle_mouse` 命中、`pending_icon_action_` 设成 `OpenShell`;同一拍 tick 的后半段 `consume_pending_icon_action()` 取出它、`create_shell_terminal` 把终端造出来并 `add_window`/`update_focus` 设为 focused;紧接着同一拍的 poll 分支 `focused->is_terminal()` 命中,`poll_output` 用 `try_read` 一次性把这堆积压字节抽干,`render_to_canvas` 画到屏幕——于是你看到终端窗口一出生就"已经有一屏内容了",那些都是它出生前 shell 写的。

那如果用户一直不点图标呢?shell 还在写,缓冲总有一天会满。满的那一刻,`Pipe::write` 走不进有空位的分支,进入 031b 讲过的"有界自旋"——释放锁、`sti;hlt`、等读端来取数据。可读端(终端)还没出生,没人会来取,shell 的 `write` 就这么自旋着,相当于被"反压"住了。shell 进程卡在 `sys_write` 里出不来,不再产生新输出,系统也不会崩——它只是在等一个永远不会到来的读者,直到用户终于点了图标、终端出生、`poll_output` 抽掉一拍数据,缓冲腾出空位,shell 的 `write` 才解阻塞继续跑。

这套"生产者先于消费者、有界缓冲暂存、满了生产者阻塞"是经典的生产者-消费者时序,只是这里消费者(终端窗口)的出生被推迟到了一个人为的交互动作之后。它不是杜撰的边界情况,而是懒创建 + 单管道 + 开机即起 shell 这三个设计决策自然逼出来的真实行为。031b 那套管道的"满则阻塞"语义,在这一章第一次被推到了前台。

### is_terminal 替代 static_cast:不是洁癖,是防呆

前面讲虚函数时说过动机,这里从"踩坑"的角度再强调一次。如果 033b 还沿用 032 的 `static_cast<Terminal*>(focused)`,只要桌面上 focused 的是一个非终端窗口——比如以后 Calculator 那种——tick 就会拿一个被错误解释的指针去调 `poll_output()`。`static_cast` 不检查、不报错,编译过得好好的,运行时是个定时炸弹。加 `is_terminal()` 虚函数的本质,是把"我相信 focused 是终端"这个**无凭据的假设**,换成"我问过 focused 了,它承认自己是终端"这个**有运行期证据的判断**。多了一次虚调用、多了一个基类虚函数,换掉的是一整类"错类型强转"的隐患,划得来。

### 诚实点:Calculator 图标还是死的

点 Shell 已经能弹出终端了(033 的 tick 回调 `consume` 到 `OpenShell` 就 `create_shell_terminal`),但别以为整个桌面都活了——Calculator 还是死的。看 `gui_tick_callback` 里那个 `consume_pending_icon_action` 的分支:

```cpp
if (action == IconAction::OpenShell) {
    create_shell_terminal();
}
```

只有 `OpenShell` 一个分支,没有 `OpenCalculator`。所以你点 Calculator 图标:033 的 `handle_mouse` 照样命中、照样把 `pending_icon_action_` 设成 `OpenCalculator`;tick 里 `consume_pending_icon_action()` 照样把它取出来;然后 `if (action == OpenShell)` 不匹配,什么都不发生,这个 `OpenCalculator` 动作被静默吞掉。而且不要指望"是不是某个我没看到的 Calculator 窗口类在别处被建了"——整个 033 tag 的代码里就没有 Calculator 窗口这个类,`IconAction::OpenCalculator` 这个枚举值除了被存进槽、被取出来,没有任何代码消费它。点 Calculator 真的什么都不发生,这是必须如实写明的"函数存在 ≠ 接线"——图标注册了、动作枚举定义了、点击能存槽,但槽的另一头没人接。要让它活,得有一个 Calculator 窗口类、得有 tick 里多一个分支,而那要等能动态做事的下一个 milestone。

### 终端析构不关管道:别让 shell 误以为终端死了

和懒创建配套的另一个坑上面提过,这里从"如果不改会怎样"的角度看。假设 033b 保留了 032 的析构逻辑——终端关掉时 `close_writer()`/`close_reader()`。那用户开终端 A、用完关掉,A 析构关掉了管道端。可这对管道是 boot 时建的全局管道,shell 还挂在上面!shell 的下一次 `sys_read(0)` 会因为 stdin 写端关闭而拿到 EOF(返回 0),shell 以为"输入没了",很可能就此退出;或者 `sys_write(1)` 因为 stdout 读端关闭而拿到 -1,shell 以为"输出没人看了"。一个用户的关窗动作,把整个系统唯一的 shell 弄死了。改成"只清指针不关端",终端的生死就和管道解绑,关一个终端只是少了一个读 shell 输出的窗口,shell 和管道都安好——代价是失去"关窗即通知 shell 退出"的便利,这在单 shell 模型下是可以接受的取舍。

## 验证

这一章的测试还是落在 033 那套桌面测试里(kernel 端 `run_desktop_tests`、host 端 ctest 名 `desktop`),因为 033 和 033b 共用同一个 tag。和懒创建最相关的几条是验证"点击 → 存槽 → 取出 → 触发动作"这条链的前半段。

**点击存槽、取出清零。** kernel 测试 `test_desktop_click_sets_and_consumes_action` 把这条链钉得很死:在 `(10,10)` 摆一个 Shell 图标,模拟一次 `MouseDown` 事件打在 `(20,20)` 上,`handle_mouse` 之后 `consume_pending_icon_action()` 第一次必须拿到 `OpenShell`,第二次必须拿到 `None`——取出即清零。这正是 tick 里"同一个点击只触发一次 `create_shell_terminal`"的保证。host 镜像里有对应的 `"desktop: icon click sets pending_icon_action"` 和 `"desktop: different icons set different actions"`,验证不同图标存不同动作进槽。

```bash
ctest --test-dir build -R "desktop" --output-on-failure
cmake --build build --target run-big-kernel-test   # 含 run_desktop_tests
```

**点 Shell 真出窗的视觉验证。** 单元测试覆盖不到"点 Shell 真的弹出跑着 shell 的终端",这件事得靠眼睛看:

```bash
cmake --build build --target run
```

预期串口的相对顺序是:`[GUI] Shell pipes stored: stdin=... stdout=...` → `[INIT] Terminal-shell pipes connected: stdin_pipe=... stdout_pipe=...` → `[GUI] ===== Milestone 033: GUI Desktop =====` → `[GUI] Desktop icons registered: Shell, Calculator.` → `[GUI] GUI tick callback registered on PIT.`。这是因为 `init.cpp` 在调 `gui_start()` 之前就先 `set_shell_pipes` 并打了 connected 那行,而 milestone/desktop icons/tick callback 三行是 `gui_start()` 内部才打的。开机后桌面是空的(注意:没有开机就冒出来的终端,这就是懒创建),只有两个图标。点 Shell 图标,串口多一行 `[GUI] Shell terminal created and connected.`,屏幕足够大时居中弹出 640×400 终端窗口(否则落在兜底位置 `(80,60)`),里面立刻有 shell 的提示符和欢迎信息——那些是终端出生前 shell 写进管道缓冲、出生后被 `poll_output` 抽干画上去的。敲 `help` 回车,命令列表正常回显。再点一次 Shell,再弹一个。点 Calculator 图标——**没有任何反应**,串口也不多一行,符合前面"OpenCalculator 没接线"的诚实判断。

## 下一站

点 Shell 图标弹出跑着真 shell 的终端——这条链路在 033/033b 已经接通,而且终端是懒创建的(点图标那一刻才 new)。可 Calculator 图标还死着,整个桌面只有"开 Shell 终端"这一种动态行为。要让桌面真正多样——点 Calculator 弹计算器、能开多个独立的程序——我们缺的不是 GUI 代码,而是一种能力:在运行时**动态拉起一个新的用户进程**。现在 shell 是 init 在 boot 时直接 `launch_first_user` 拉起的唯一一个用户进程,它和那对全局管道绑死;要开一个计算器,得能 fork 出新进程、给它独立的 fd、exec 进它的代码。

下一个 milestone 会把 fork/exec 的骨架搭起来。不过要提前打个预防针:那是个"搭好骨架、尚未通电"的阶段——fork/exec 的框架会立起来,但离"点 Calculator 真的弹一个独立计算器进程"还有距离,中间隔着调度器、内存管理、独立的 fd 表一堆没填的坑。所以我们不在这一章对它做任何"已经能 fork 出计算器"的承诺。033b 的收尾已经够实在了:点 Shell 真的能开窗、真的跑着 shell,而终端是懒创建出来的——诞生时机从 boot 推迟到 click,shell 先写进管道缓冲、终端出生后抽干,时序在管道里老老实实兜住了。这是一座接通的桥,哪怕桥那头还没铺满。

## 参考

- C++ `virtual` 成员函数与 `override` 运行期分发(支撑本章 `is_terminal()` 用虚函数替代 `static_cast` 嗅探的设计):https://en.cppreference.com/w/cpp/language/virtual
- Linux `pipe(7)` man page(管道读空则 `read` 阻塞、写满则 `write` 阻塞,支撑本章"shell 先写、终端出生前缓冲暂存、满则 shell 阻塞"的生产者-消费者时序对照):https://man7.org/linux/man-pages/man7/pipe.7.html
- 本库 [031b · 管道:让终端跑起真正的 shell](031b-gui-pipe.md)(管道的 4 KB 环形缓冲、满-阻塞的有界自旋、`try_read`/`try_write` 的非阻塞语义,本章懒终端时序直接复用)
