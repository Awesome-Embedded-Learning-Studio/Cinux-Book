---
title: 01 · 装什么
---

# 01 · 装什么:把工具装齐,逐个点亮

劝退人的差事从来轮不到操作系统来干,真正的拦路虎,是动手之前的装机。多数教程的开头甩过来一堵命令墙,您照着敲完,屏幕里滚过几百行的输出,装了什么、哪些用得上,您是一概不知道的。更冤的还在后面:漏装了一件,过几天写代码的时候,偏偏来了一句 command not found,您还以为是自己的代码出了毛病,查了半天,查到的是装机那天欠下的。

咱们这一篇反过来装:每装一件,咱们就当场点亮一件,装完的每一件,都被您亲手验过。等您合上它,机器上会有一套做内核要用的工具,还有一个您亲手编译、亲眼看着跑起来的小程序,它就是咱们后面拿去解剖的标本。

## 咱们的机器长什么样

咱们动手之前,看一眼自己的脚下。两条命令就够了,一条看的是发行版,一条看的是内核:

```bash
cat /etc/os-release | head -4
uname -r
```

笔者把命令跑了,交回来的实录是这样的:

```text
NAME="Arch Linux"
PRETTY_NAME="Arch Linux"
ID=arch
BUILD_ID=rolling
6.18.33.2-microsoft-standard-WSL2
```

内核名里的 `microsoft-standard-WSL2` 暴露了实情:笔者干活的地方,就是 Windows 里的 WSL2,您拿 Linux 子系统做咱们这些事情,是完全够用的。发行版不一样的地方,只在装东西的命令上:笔者的 Arch 用 pacman,后面的实录也都是它。您要是 Ubuntu 或 Debian 一系的 apt,装法咱们就双轨都给。apt 的一轨,笔者是没跑过的,您照着敲就好。

## 装什么:清单一过目

打头的是编译器 `g++` 和 `gcc`,您写的 C++ 和 C 都交给它们去翻。底线咱们不背版本号:认得 `-std=c++23` 这个写法的就行,咱们马上要靠它过两道编译链接的闸,过不了闸的,咱们一律算它太老。

接着请干粗活的几家进组:binutils 一家管汇编、链接、查看和抽取,`as`、`ld`、`objdump`、`objcopy` 四位都是门下的人,同门的还有看符号的 `nm`、读 ELF 头的 `readelf`,后面的章节随用随认。`make` 和 `cmake` 管构建的编排,咱们后面的工程要靠它们长大。binutils、make、cmake 几家是不挑版本的,近年的发行版自带的都够用。

模拟器咱们要 `qemu-system-x86`:它能在您的电脑里模拟出一台 x86 机器,咱们写出来的东西以后都送进它肚子里跑,8.0 起的版本就稳妥了。

调试器 `gdb` 替咱们钻进机器里看寄存器、一步一步走。看十六进制的 `xxd`,咱们验镜像的时候天天都请它出场。

可选的一位是 `clangd`,编辑器里的跳转和补全归它管,咱们就算没有它,也不耽误装机的事。

装法咱们分两轨来给。笔者的机器是 Arch,实录是这样的:

```bash
sudo pacman -S --needed gcc binutils make cmake qemu-system-x86 gdb vim clang
```

列表里咱们还捎上了两位顺风客:Arch 的 `xxd` 是跟着 `vim` 走的,`clangd` 是跟着 `clang` 走的。两位的户口,咱们是拿 `pacman -Qo` 亲手查过的,错不了的。apt 那一轨的命令长这样:

```bash
# Ubuntu / Debian 系(这一轨笔者没跑过,照着敲就好)
# 旧一些的 Ubuntu 没有独立的 xxd 包,它在 vim-common 里
sudo apt update
sudo apt install -y build-essential binutils cmake qemu-system-x86 gdb xxd clangd
```

## 装一件,点亮一件

装完了不点灯,跟没装是一样的。咱们拿一串命令挨个验,每件工具亮一下自己的版本:

```bash
g++ --version | head -1
as --version | head -1
make --version | head -1
cmake --version | head -1
qemu-system-x86_64 --version | head -1
gdb --version | head -1
xxd -v
clangd --version | head -1
```

```text
g++ (GCC) 16.2.1 20260810
GNU assembler (GNU Binutils) 2.47
GNU Make 4.4.1
cmake version 4.4.3
QEMU emulator version 11.1.1
GNU gdb (GDB) 17.2
xxd 2026-06-16 by Juergen Weigert et al.
clangd version 22.1.8
```

binutils 家的四位报的版本都一样,咱们点亮了一位,就算点亮了一家。您的数字跟笔者的对不上,也不要紧的,咱们只问两件事:它在家吗?版本过线了吗?

## 两道闸:编译一道,链接一道

咱们把版本问出来了,工具也还只算是在家的。真要验它的本事,咱们就得让它真干一点活。咱们拿一个小到不能再小的程序过两道闸,它就是开篇说的那个标本:

```bash
cat > hello.cpp <<'EOF'
#include <cstdio>

int main() {
    std::printf("the toolchain is alive\n");
    return 0;
}
EOF
```

头一道咱们考的是编译,只让 `g++` 干编的活,链接的活它不许碰:

```bash
g++ -std=c++23 -c hello.cpp -o hello.o
file hello.o
```

```text
hello.o: ELF 64-bit LSB relocatable, x86-64, version 1 (SYSV), not stripped
```

请您抓一个词:relocatable,翻译过来就是“可重定位”的意思。翻成人话:东西是真的了,住址是还没定的。这是编译器单独交出来的半成品,咱们把这个身份记下,链接那边的课,要回来对它的。

过了编译,另一道咱们考的就是链接了,咱们让它把 `hello.o` 变成可执行文件:

```bash
g++ hello.o -o hello
file hello
```

```text
hello: ELF 64-bit LSB pie executable, x86-64, version 1 (SYSV), dynamically linked, interpreter /lib64/ld-linux-x86-64.so.2, BuildID[sha1]=cfbc8aef19a55db2ce1f64c333f64b4af2bd3066, for GNU/Linux 4.4.0, not stripped
```

relocatable 换成了 executable,翻过来就是“可执行”了。是谁给换的身份?链接器。咱们一个字节都没动它,链接给它换了身份。过了这道闸,咱们的装机就算成了。

## 首胜:它活了

```bash
./hello
```

```text
the toolchain is alive
```

它活了。咱们从编译器、汇编器到链接器的一整条流水线,在您的机器上跑通了。

## 配 clangd,可选的五分钟

clangd 咱们在点亮清单里已经验过了。它吃的东西叫 `compile_commands.json`,一份“每个文件用什么命令编译”的清单,咱们眼下还没有,得等构建系统上场替咱们生成。所以您知道有它就好,等正文里真用上了构建,咱们再来点亮它。

## 装不上的话,按最常见的排

咱们头一个遇到的多半是 `command not found`。十有八九的情况下,是名字敲错了,或是装机那天漏掉的:您手滑写成 `obj-copy`,它就回您这么一句。漏装的样子,跟它是一模一样的,您翻回开头“装什么”一节,在您自己那轨的命令里找包名,补上就消了。还有更迷糊的一档:您把 PATH(就是 shell 找命令时翻的地址簿)改在了配置文件里,当前这个终端却还是按老地址找人的。您重开一个终端就好,新开的终端会把配置重新读一遍。您要是挪过装好的工具,还可能碰上 shell 记着的旧地址,拿 `hash -r` 清一下就好了。

再有一种是咱们把自己的 PATH 改坏了。咱们在子 shell 里安全地复现一次:

```bash
env PATH=/tmp/definitely-empty /bin/bash -c 'objcopy --version'
```

```text
/bin/bash: line 1: objcopy: command not found
```

咱们玩的是一次性的子 shell,关掉它就恢复了。您要是真把自己的 PATH 写坏了,症状跟它是一样的:工具明明是装了的,却哪里都找不到它了。咱们自查就用 `echo $PATH`,要修的就是那句 `export` 的写法,等号右边必须带上原有的 `$PATH`,咱们照着 `export PATH=$PATH:$HOME/bin` 的样子写。少了那一段的写法,是把老家整个丢了。

还有一种是 WSL2 独有的:Windows 睡了一觉醒来,装包开始报校验或证书的错,咱们一看系统时间,差出去好几个小时了。根子是睡眠之后 WSL 的时钟漂了,咱们拿 `sudo hwclock -s` 把时钟一拽,它就回来了。咱们现场复现不了它,笔者的钟眼下是准的。不过它是 WSL2 上出了名的常客,您就是碰上了,也不用心慌的。

最后要提的一种情况是老版本:旧发行版自带的 gcc,可能认不得咱们用的 `-std=c++23`。您拿两道闸一过便知,真老了,您就升发行版,或者装您发行版提供的新版工具链包,咱们不在这里铺开。

## 债:工具的原理与别的系统

咱们这一栏记的,是本篇欠下、后面要还的东西。这一章咱们只管装与点亮,每件工具肚子里是怎么转的,咱们一个字都不教:`g++` 的内幕是下一章的正题,链接器和 `objcopy` 的活,这一卷后面讲链接的那一篇里就接上,QEMU 的门道,咱们等真起镜像了再讲。系统方面咱们只陪 Linux 一家:macOS 和 Windows 的原生是不在服务范围里的,WSL2 是算在 Linux 里的,笔者自己就是在上面干活的。包管理器咱们只讲 pacman 与 apt 两轨,别的系请您自行对应。

## 标本就位

您敲的 `g++`,看着只有几个字母的样子,背地里却指挥了编译、汇编、链接一整串人马。到了下一章,咱们就看看它到底指挥了谁。
