---
title: "11 · 加冕:STL、异常与三个 PIE"
description: "默认 PIE 在这一章落成常态。预编的 hello.s 拿掉了按住位置无关的选项,响应文件里的祖传家什换成带 S 的三件,头一行添上 -pie,领班 gcc 与 g++ 上场也不再带禁 PIE 的选项,座位认得 ET_DYN 那一章开的座,迎来了默认形态的客人。产物是不是真 PIE,咱们不问编译器,盘上 busybox 自带的 od 在串口壳里亲手读字节,三位产物偏移 16 起的两个字节拼出来都是 e_type 的 3,ET_DYN。C++ 的成色断言两行都要,vector、string、iostream 印出问候,throw 与 catch 走通一趟,exception: caught 落屏,少一行都不算数。裁判手里立着四条章程,等待只认本条命令的回话,状态不合预期当场判负,串口每读一段记录就重写一回,任何 unserved 都让整轮作废。首轮完整自举登记在案,三个 PIE、C++ 的 throw 与 catch、execprobe、spawn 失败回收,外加零 unserved。自举链合龙,成色如何,下一章从空缓存再走一遍。"
chapter: 34
order: 11
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - selfhost
  - gcc
  - pie
  - cpp
---

# 加冕:STL、异常与三个 PIE

烤盘脚本里预编 hello.s 的那一行,如今就是一句光溜溜的 `gcc -S`。从头一件活的那一章起,这行命令一直带着禁 PIE 的选项,交给两位老师傅的底稿是固定地址的老实身子。这一回咱们把选项整个拿掉,师傅默认怎么排,底稿就怎么落,生出来就是位置无关的身段。

响应文件也跟着换了班底,咱们把烤盘脚本末尾那一段原样请出来:

```bash
{
    printf '%s\n' -pie -o /hello
    printf '%s\n' -dynamic-linker /lib64/ld-linux-x86-64.so.2
    printf '%s\n' /usr/lib/Scrt1.o
    printf '%s\n' /usr/lib/crti.o
    printf '%s\n' "${gcc_lib_dir}/crtbeginS.o"
    printf '%s\n' /hello.o
    printf '%s\n' -L/usr/lib
    printf '%s\n' "-L${gcc_lib_dir}"
    printf '%s\n' -lgcc
    printf '%s\n' -lc
    printf '%s\n' "${gcc_lib_dir}/crtendS.o"
    printf '%s\n' /usr/lib/crtn.o
} > "$work/hello.ldargs"
```

咱们从上往下念。领头的是头一行 `-pie`,它上来就把话告诉了 ld:这一单要按位置无关的动态形态交货。三件祖传家什换成了带 S 的兄弟 `Scrt1.o`、`crtbeginS.o`、`crtendS.o`。分工就写在它们的名字上,带 S 的几位伺候位置无关的身子,不带 S 的守着固定地址的老家。您细看还会注意到 `crti.o` 与 `crtn.o` 两位原地没动,俩人都不挑地址,新身子旧身子您给哪个都伺候得起。中间那行 `-dynamic-linker` 点的还是老门牌 `/lib64/ld-linux-x86-64.so.2`,师傅进门的时候只认它,差一个字都是不收的。

领班 gcc 与 g++ 上场的时候,考的是默认形态自己站不站得住。自举的那一轮里,给两位的命令就这么短:`gcc /hello.c -o /hello_g` 与 `g++ /hello.cpp -o /hellopp`,禁 PIE 的选项一个没递,产物落成什么形态,全凭师傅的默认。前面座位认得 ET_DYN 的那一章里,咱们给主程序的座添了收留 ET_DYN 的本事,还让它学会了报装载偏置。生态那一卷领进门的 glibc 动态客人,早把解释器的座位坐热了,主程序自己的位置无关身子,到这一卷才有了落座的地方。默认的 PIE 一放开,主程序的座迎来的就是天天上门的常客。

活交上来了,可它真是 PIE 吗?这话咱们不问编译器。急着高兴的话,证据就还欠了一截,师傅交完活就闭了嘴,从不自报交的是什么形态。想知道的话,咱们就得自己读字节,验货的地方就选在串口壳里,盘上的 busybox 正好带着 od,咱们就让三位产物挨个过。

```python
        for product in ("hello", "hello_g", "hellopp"):
            output = command(f"/bin/busybox od -An -tu1 -j16 -N2 /{product}; echo PIE_{product}_$?",
                             f"PIE_{product}_0")
            if not re.search(rb"\r?\n\s*3\s+0\s*\r?\n", output):
                raise RuntimeError(f"/{product} is not ET_DYN")
```

od 是按字节翻文件的老工具。`-j16` 跳过的是开头 16 个字节,那是 ELF 头里自报家门的 e_ident,紧随其后的两个字节才是标形态的 e_type,`-N2` 便只取了它们。您把镜头对准屏上,给出来的是 3 和 0 两个数,x86 玩的是小端字节序,排在前头的是低位,咱们把两个数拼回去,拼出来的 e_type 就是 3,ET_DYN 的编号,位置无关的动态形态。三位产物各查各的,`/hello` 是两位老师傅手链交的活,`/hello_g` 是 gcc 交的,`/hellopp` 是 g++ 交的,哪一位读不出 3 的话,咱们就把这一轮当场作废。

C++ 那边的成色,断言压的是两行输出。烤盘带给 g++ 的源码不长,咱们把它一字不差地铺开。

```cpp
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    const std::vector<std::string> kWords{"Hello", "from", "G++!"};
    for (unsigned long index = 0; index < kWords.size(); ++index) {
        if (index != 0) {
            std::cout << ' ';
        }
        std::cout << kWords[index];
    }
    std::cout << '\n';
    try {
        throw std::runtime_error("caught");
    } catch (const std::exception& error) {
        std::cout << "exception: " << error.what() << '\n';
    }
    return 0;
}
```

程序拿的是 vector,装的是 string,咱们让 iostream 一个词一个词往外送,屏上印出来的就是 `Hello from G++!`。接着 throw 把 runtime_error 扔了出去,catch 在外层把它接住了,印出来的是 `exception: caught`。这两行就是咱们的判据,缺了哪一行都不算数。头一行证明 STL 的家当真跑过了,后一行证明异常走的路也是通的,异常要靠运行库把栈一层一层地退回来,再把控制权递给接住的那一层,throw 与 catch 缺了哪一样,咱们都不认这次加冕。

主持这一轮的脚本,咱们就当它是裁判,它手里立着的是四条章程。头一条说的是,等回话只认本条命令自己的。每条命令进场的那一刻,卷宗把刻度记了下来,往后的搜寻全从这个刻度起步。等回话的函数住在裁判的 main 里,所以它带着一身缩进出场。

```python
    def wait_for(pattern, start=0):
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            match = re.search(pattern, transcript[start:])
            if match:
                return match
            if process.poll() is not None:
                raise RuntimeError(f"QEMU exited: {process.returncode}")
            if select.select([process.stdout], [], [], 0.1)[0]:
                transcript.extend(os.read(process.stdout.fileno(), 65536))
                log.write_bytes(transcript)
```

`transcript[start:]` 里的那个 start,就是命令自己带进来的刻度,咱们等的是本条自己的话音,上一条命令的残话,冒充不了本条的回话。

第二条管的是状态。咱们让每条命令末尾都缀一句 echo,把退出码报成带名字的记号,裁判等到了记号才去核对数字,对不上的时候当场就收场,没有重试这么一说。核对用的三行,就是裁判手里的那杆秤:

```python
        match = wait_for(rb"(?:\r?\n)" + prefix.encode() + rb"_\d+\r?\n", start)
        if not re.search(rb"(?:\r?\n)" + marker.encode() + rb"\r?\n", transcript[start:]):
            raise RuntimeError(f"command returned an unexpected status: {text}")
        wait_for(rb"/ #", start + match.end())
```

第三条管的是卷宗。`log.write_bytes` 挂在每次读取的身后,咱们每从串口收到一段就把整份记录重写一遍,机器中途翻了车,留下的也是翻车那一刻的现场,咱们回头排案,拿到手的永远是最新的一页。

压得最重的是第四条,它管的却是整轮的去留。等全部命令跑完了,裁判在整份记录里还要搜的是 unserved,任何一处有人喊了名字而没人应声,咱们就整轮作废,活干得再齐也是不行的。明说不办的号位不算在内,那些有人应声、答的是不办,念名单的那一章把这两码事分清过了,装聋才是这里要抓的。

登记册上还有两位陪跑的。咱们把 execprobe 拉进来再跑一遍,它是交接那一章按正本铺参数的实弹,这一遍要证明的,是交接的章程没被后来的活碰歪。另一位专考的是失败,裁判递给 gcc 的是 `-wrapper /absent`,让领班去唤一个不存在的包装程序,spawn 只得走失败的路子。失败了以后怎么收场,章程在师傅怎么开工那一章立过,子进程的旧世界不毁,glibc 把 errno 写了回去,进程拿非零的码退场。所以裁判在这里等的记号是 `SPAWN_FAIL_1`,真等来的是 0,反而是输。

首轮的完整自举,咱们看着它跑完了。登记在案的有三个 PIE,有 C++ 的 throw 与 catch,有 execprobe 的实弹与 spawn 失败回收,外加的是零 unserved。手链、gcc、g++ 三路产物查出来的都是 ET_DYN,STL 与异常双双落了屏,自举链在这里合了龙。成色怎么样,咱们下一章从空缓存再走一遍。
