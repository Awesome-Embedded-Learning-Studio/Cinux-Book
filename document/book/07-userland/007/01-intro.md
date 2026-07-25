---
title: 01 · PTY:伪终端把 console 单例变成多路终端
---

# PTY:伪终端把 console 单例变成多路终端

> 前面(062)把 console TTY 做成了一个**单例**——系统就一个终端,键盘当输入、串口当回显。够 shell 自己用了,可一旦你想让「多个程序各跑在各自的终端里」(比如 GUI 里每个 shell 窗口、或者一个终端模拟器后面拖一个 shell 进程),单例就不够了。这一章立 **PTY(伪终端)**:一对 master/slave,slave 对程序来说**表现得像一个真终端**(有行规范、能 ioctl termios、能收 Ctrl+C 信号),master 是另一端(终端模拟器连它,喂输入、读输出)。PTY 的关键性质是它能**多路**——开一对就是开一个新终端,要几个开几对。这一章把 062 那个单例 console TTY 升级成 Linux 风格的多路 PTY。
>
> 这一章依赖两块前序:062 的 `TTY` 行规范(PTY 的 slave 直接复用它,不重写)、064 的 DevFS(PTY 节点必须是设备 inode,挂在 `/dev` 下)。所以 062 末尾那句「PTY 留 DevFS 之后」到这儿兑现。A 档:punchline 是 Linux PTY ABI 跑通——`open("/dev/ptmx")` 克隆出一对、`ioctl(fd, TIOCGPTN)` 拿 pty 号、`open("/dev/pts/N")` 拿 slave、master↔slave 数据往返、`TIOCSCTTY` 挂控制终端。一条诚实的边界先说在前头:这一章把 PTY 的**机制**(数据通路 + 设备节点 + 控制终端)做全了,但「真用户程序跑在 PTY 里」的全闭环留后面——那需要 `dup2`(把 slave 重定向成子进程的 stdio)和一个会主动 `open("/dev/ptmx")` 的 shell,是 CFBox 那条线的事。

## 这章咱们要点亮什么

1. **PTY 是一对 master/slave**:slave 对程序像真终端(行规范 + termios + 信号),master 是模拟器那一端。开一对 = 开一个新终端,能多路。
2. **slave 复用 062 的 `TTY` 行规范**:PTY 的「终端感」全来自 slave 侧的行规范,不重写——把 062 那块纯逻辑直接拥有。
3. **四条数据路径**:master 写 → slave 行规范 → slave 读;slave 写 → master 读;回显经 echo sink 路由回 master 读侧(模拟器看到自己敲的字)。
4. **两条新接缝让 PTY 接进 fd**:`InodeOps::ioctl` / `open` 两个虚函数(对齐 Linux fops),`sys_ioctl`/`sys_open` 对 fd>2 走 `fd→File→Inode→ops` 派发。
5. **`/dev/ptmx` 的克隆 open** + `/dev/pts/N` 的动态查找:`open("/dev/ptmx")` 分配一对新 PTY、返回 master;`/dev/pts/N` 按号查到 slave。
6. **控制终端**:`TIOCSCTTY` 让 session leader 挂一个 PTY 当控制终端,`/dev/tty` 是「当前控制终端」的每进程别名。

## PTY 是什么:一对扮演终端的管子

先说清 PTY 到底是什么。一个真终端硬件(键盘 + 屏幕)对应一个 console TTY(062 那个)。PTY 是「软件假装的终端」:它有一对端——slave 端表现得像一个真终端(连上它的程序以为自己在跟真终端说话,有行规范、能设 termios、收 Ctrl+C 会触发信号),master 端是另一头(谁连 master,谁就在扮演「键盘 + 屏幕」——往 master 写等于敲键盘,从 master 读等于看屏幕输出)。

为什么要这么个东西?因为它把「终端的行为」和「终端的物理后端」解耦了。一个终端模拟器(比如 xterm,或者 GUI 里的 shell 窗口)连 PTY 的 master,它画的窗口就是「屏幕」、键盘事件喂给 master;一个 shell 连同一对 PTY 的 slave,它以为自己在跟真终端交互。这样每个模拟器窗口能拖一个独立 shell,互不干扰——这就是「多路」。console TTY 是单例,做不到;PTY 开一对就是一个新终端。
