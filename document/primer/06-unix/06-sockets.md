---
title: 06 · 套接字
---

# 06 · 套接字:它长得像一个文件,用起来也像一个文件

套接字是文件。这句话听着不太可信。前面几篇咱们已经在文件系统里见过不少名字,管道、终端,各有各的模样。套接字也住在那一堆名字里,而且住得不动声色。

## 一个 `s`,就在咱们列目录的时候

咱们把 `nc` 喊起来,让它在一个名字上等着,手里不必有那个名字,它自己会去建。`nc` 和后面要用的 `socat` 都得您自己装上,发行版里各自是一个包。这会儿用的 `nc` 得是 openbsd 那一版,理由在下面讲旗子的时候说。片刻之后,再拿 `ls -l` 去看它建出来的东西:

```bash
rm -f sockA; (sleep 2 | nc -lU sockA &) ; sleep 0.4; ls -l sockA; stat -c "%F  权限=%a" sockA
```

```console
srwxr-xr-x 1 ‹您的用户名› ‹您的用户名› 0 Oct  2 16:09 sockA
socket  权限=755
```

`sockA` 是随手挑的名字,挑在哪个目录也有讲究:套接字这个名字得建在一个能写的地方,咱们跟前这个环境的根目录是只读的,所以这几条命令得在一个能写的目录里敲,您那边随手找个地方就行。命令里那个 `-lU`,`-l` 是等着接,`-U` 说的是这回走文件系统里的名字,不走网络那一套。

第一行开头的 `s` 就是咱们要找的答案,它表示套接字。字母 `s` 平常极少露面,所以第一次看见容易当成手滑打出来的。后面那些权限位与属主,您在自己机器上看到的会是另一套,它们说明的是谁建的这个名字、谁能碰它。`Oct  2 16:09` 那个时间戳也是这一趟跑出来的:您那边换一个时刻跑,写出来的就是您跑的那一刻。

`stat` 报的文件类型是 `socket`,`ls -l` 那一行的字节数写的是 0,它里面没有任何内容。这就有点意思了:文件系统里明明白白站着一个名字,它却什么都不存。您要是拿 `cat` 去读它,它不会给咱们一个字,而是当场报一句 `No such device or address`。这是笔者在这个环境里试出来的。这个名字在文件系统里有位置,可它不对普通读写开门。它的用处不在这儿,而在那个名字本身:谁拿起这个名字,谁就能接上背后那根通信的两头。

## 这台机器上的三个前提

咱们把下面三条摆在前面,后面凡是数字对不上的地方,多半就是它们造成的。

| 咱们这台机器的现状 | 您那边可能不一样 |
|---|---|
| 当前用户的 uid 是 1000,没有 root,根目录 `/` 挂成只读 | 您可能随时能 `sudo`,写系统目录也不费事 |
| 有自己独立的 PID 名字空间,`ps -e` 只列出 4 到 6 条 | 您那边通常是几百条 |
| 没有任何控制终端,`tty` 报 `not a tty` | 您多半正坐在一个真终端前 |

第三条还会顺带改掉几个数字。没有终端的时候,进程组号和会话号被压成了 0,`TPGID` 报 -1。前面讲 `ps` 那七列的时候,咱们看到的就是这副样子。

## 两个进程,一根连接

现在咱们让它真的说上话。在咱们跟前这个环境里,`socat` 在 4568 上候着,它每一次接到连接就开一个 `/bin/cat`,您发过去什么,它原样吐回来:

```bash
socat TCP-LISTEN:4568,reuseaddr,fork EXEC:/bin/cat & sleep 0.4
printf "ping\n" | nc -q1 127.0.0.1 4568
```

```console
ping
```

`-q1` 问的是 stdio 那头的事:标准输入收完之后再等 1 秒就收手,咱们要的回音正好来得及回来。没有它,`nc` 会一直挂在那儿等,命令行再也回不到您手上。引屏的时候,这一屏的末尾还挂着一行 `socat[287] W exiting on signal 15`,那是笔者收摊时 `kill` 掉 `socat`、它自己报的收场话,方括号里那个号与它前头的时刻都是笔者这一趟的,不是实验数据,咱们把它摘掉了。

下面换掉客户端那一头。咱们写一个最小的客户端,替下刚才那句 `nc`,服务端那一头还是 socat:

```c
/* 最小客户端:拨到本机的一个端口,说一句,等回话 */
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    int port = argc > 1 ? atoi(argv[1]) : 4567;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    printf("socket(AF_INET, SOCK_STREAM, 0) = %d\n", fd);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, host, &sa.sin_addr);
    int r = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    printf("connect(..., %s:%d) = %d\n", host, port, r);
    if (r < 0) { perror("connect"); return 1; }
    const char *msg = "ping\n";
    write(fd, msg, strlen(msg));
    char buf[64];
    ssize_t n = read(fd, buf, sizeof buf);
    printf("read 回来 %zd 个字节:%.*s", n, (int)n, buf);
    close(fd);
    return 0;
}
```

六行 `#include` 全来自 libc,一个第三方依赖都没有,所以它短得可以当场抄完。咱们把它存成 `sockcli.c`,在同一个目录里编出来:

```bash
gcc -o sockcli sockcli.c
```

这一次两端都落在 4567:服务端还归 socat,客户端是一段自己写的代码。两头起落的次序不能反,咱们把 `socat` 那一头和编好之后要跑的那一句摆在一起:

```bash
socat TCP-LISTEN:4567,reuseaddr,fork EXEC:/bin/cat & sleep 0.4; ./sockcli 4567
```

```console
socket(AF_INET, SOCK_STREAM, 0) = 3
connect(..., 127.0.0.1:4567) = 0
read 回来 5 个字节:ping
```

这几行里藏着几件事。`socket()` 交回来的那个整数是 3,您那边可能是别的号,它是内核给这根连接发的一格文件描述符,跟打开一个文件拿到的号是一回事。开头那两行是咱们自己 `printf` 出来的,不是内核打的,这一点得说清。最后,连上之后咱们做的事只有 `write` 与 `read`,与读写一个文件没有任何分别。

端口号 4567 与前面那个 4568 都是笔者随手挑的。您那边要是已经被别的程序占着,换成别的号就行,两个号都换,别跟自己的机器过不去。

## 内核那边多看了两眼

同一个客户端,咱们这回让 `strace` 跟在后面,把 `socket`、`connect`、`sendto`、`recvfrom` 四路调用记在过滤表上。上一节起的那个 `socat` 要是还占着 4567,照下面给出的命令再起一个会被端口挡回来,多出一句报错,那句不是实验数据,旧的那一头照样接得上,屏上那六行不受影响。

```bash
socat TCP-LISTEN:4567,reuseaddr,fork EXEC:/bin/cat & sleep 0.4; strace -f -e trace=socket,connect,sendto,recvfrom ./sockcli 4567
```

表里写了四个名字,这一趟真正掉下来的只有 `socket` 与 `connect` 两路。咱们那个程序连上之后走的是 `write` 与 `read`,不在过滤表里,所以 `sendto` 与 `recvfrom` 一次都没有露面。下面这一屏里两个调用的说法各自齐了:

```console
socket(AF_INET, SOCK_STREAM, IPPROTO_IP) = 3
connect(3, {sa_family=AF_INET, sin_port=htons(4567), sin_addr=inet_addr("127.0.0.1")}, 16) = 0
socket(AF_INET, SOCK_STREAM, 0) = 3
connect(..., 127.0.0.1:4567) = 0
read 回来 5 个字节:ping
+++ exited with 0 +++
```

这一屏是两条流交错着下来的,咱们要分开读。头两行是内核这一侧记下的,它记的是 `socket(AF_INET, SOCK_STREAM, IPPROTO_IP)` 与那个带花括号的 `connect()`。第 3 行到第 5 行是程序自己 `printf` 出来的,同一个 `socket()` 调用到了它手里写成 `socket(AF_INET, SOCK_STREAM, 0) = 3`。末一行又是内核那侧记的收场话。第 1 行与第 3 行说的是同一个调用,第 2 行与第 4 行也是,两边交回来的地址都落在 `127.0.0.1` 与 4567 上。

两行 `socket()` 里数下来是同一组东西:地址族 `AF_INET`,类型 `SOCK_STREAM`,第三个参数表示协议。程序那边写的是 0,`strace` 把这个 0 按名字印成了 `IPPROTO_IP`(它在头文件里就等于 0)。搁在 `AF_INET` 加 `SOCK_STREAM` 这一组上,内核给咱们最后选定的是 TCP。`connect()` 那一段里,`sin_port=htons(4567)` 与 `inet_addr("127.0.0.1")` 分别是端口与地址,末尾那个 16 是这段地址结构的长度。程序把自己搭的那个 `struct sockaddr_in` 交给内核,内核把接在手里的样子记成花括号里那几格,长度还是 16。

`+++ exited with 0 +++` 那一行在说咱们盯着的那个程序正常退场了。

这一段路上咱们跳过了太多东西。内核到底怎么把连接建立起来,服务端要在哪个调用上把门打开,端口号为什么要过一遍 `htons` 才交出去,这些都不是这一篇能装下的。今天咱们只认住那个形状:一个整数,一段地址,一次成功的通话式调用,然后就是 `read` 与 `write`。

## 换一个名字,照旧是 `read` 与 `write`

咱们把地址换成文件系统里的一个名字,再看一遍。这回两头都是 `nc`,只是拨号的那一句换成了文件名。名字本身值得咱们单看一屏:

```bash
rm -f sockA; (sleep 2 | nc -lU sockA &) ; sleep 0.4; ls -l sockA; stat -c "%F  权限=%a" sockA
```

```console
srwxr-xr-x 1 ‹您的用户名› ‹您的用户名› 0 Oct  2 16:09 sockA
socket  权限=755
```

这一屏与开头那一屏是同一次运行留下来的,连时间戳都一样。您那边换个时刻再跑,写出来的就是您跑的那一刻。

咱们接着隔着这个文件说一句:

```bash
rm -f sockB; (sleep 2 | nc -lU sockB > gotB.txt &) ; sleep 0.4; printf "过来说话\n" | nc -q1 -U sockB; sleep 0.5; cat gotB.txt
```

```console
过来说话
```

屏幕上这一行就是咱们发过去的那几个字。送出去的字不会自己冒到屏幕上,是服务端那头把它们收下来、写进了 `gotB.txt`,末尾那句 `cat` 又把文件里的字打回屏幕上。开头那个 `s` 在刚才那一屏里也出现过,它还是那个意思:这个名字不通向网络,它自己就是那头。

`nc` 的 `-lU` 与 `-U` 是 openbsd 版的写法,这就是开头让您留意的地方。您那边的 `nc` 要是另一种包装,旗子的名字会不一样,查一下手边 `nc` 的说明再敲。送字的那句带 `-q1`,跟上面 TCP 那一趟同一个道理。名字用完了您得自己动手删:`sockA` 那一趟跑完,进程走了,名字还立在文件系统里,下一个程序再想用它,得把这个空壳挪开。

## 这一篇没有做的部分

今天咱们只搭了一个最小的模型:一个整数,一段地址,一句问,一句答。下面这些,咱们这一轮没有做,咱们如实记着,不假装见过:

- 三次握手的细节,内核在两个进程之间到底交换了什么
- 服务端那一侧的 `bind`、`listen`、`accept` 三件套,以及它怎么把门开在某个地址上
- 端口与地址的字节序,那个 `htons` 为什么非写不可
- 两台机器之间的连接,今天走的一直是本机

咱们把这一篇的东西收起来。在咱们跟前这个环境里,套接字长得就是一个文件:第一个字母是小写的 `s`,名字住在文件系统里,拿 `cat` 去读它只会换来一句 `No such device or address`。真的说上话是两件事:一头的整数、一段 `127.0.0.1:4567` 的地址,一句问、一句答。换成文件系统里的名字之后,两头还是 `nc`,只不过另一头拨的是那个名字本身。名字用完不会自己消失,得您亲手把它删掉。
