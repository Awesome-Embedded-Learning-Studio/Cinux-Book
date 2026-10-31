---
title: "07 · /lib 里只剩一家"
description: "整具 busybox 换血上桌。同一棵 BusyBox 1.38 的源树,同一份配置,连编译的姿势都是老的,换的只有掌勺的编译器:开发机自带的 gcc 顶掉 musl-gcc,配置再翻三个开关,静态翻掉,编译与链接各关一个 PIE 的旗,出炉 glibc 血统的动态身子,ET_EXEC 落址固定,链接旗里还把解释器的家写死成 /lib64、给 rpath 指到 /lib。busybox 牵着 libc、libm、resolv 三件库,三库全装,烤盘时 libm 与 resolv 缺一件就大声失败。造盘脚本添换血模式:busybox 换座,musl 对下盘谢幕,盘从 16M 扩到 64M,种子添户口本两册与当班表一件,骨架目录补齐,/lib 里落东西的地方只剩三处,一家的血验讫。两模式并存,旧 musl 模式照旧能出,默认暂不翻,作坊那一章统一翻,中间不偷偷换口径。换血不重写家谱,当年盘上的动态 busybox 本就是 musl 动态,glibc 化是这一遍多做的一步,开门迎客那一卷的叙事一字不改。fd 从 0 起、头一个进程显式装三座 stdio 的座序,libc 无关件点名沿用。"
chapter: 33
order: 7
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - ecosystem
  - glibc
  - musl
  - busybox
  - rootfs
  - ext2
---

# /lib 里只剩一家

整具 busybox 的换血,动的地方小得出奇。同一棵 BusyBox 1.38 的源树,同一份配置,连 make 的姿势都是老的,换的只有掌勺的人:开发机自带的 gcc 顶掉 musl-gcc,出来的货,血统就是 glibc 的。再往配置里翻三个开关,busybox 就从静态的自给自足,换成了吃饭要靠解释器端上来的动态身子。翻开关的活,由编 busybox 的脚本替咱们做,翻的是三行:

```bash
python3 - "$work/.config" <<'EOF'
import sys
path = sys.argv[1]
with open(path) as handle:
    lines = handle.readlines()
with open(path, "w") as handle:
    for line in lines:
        if line.startswith("CONFIG_STATIC=y"):
            handle.write("# CONFIG_STATIC is not set\n")
        elif line.startswith("CONFIG_EXTRA_CFLAGS="):
            handle.write('CONFIG_EXTRA_CFLAGS="-fno-pie"\n')
        elif line.startswith("CONFIG_EXTRA_LDFLAGS="):
            handle.write('CONFIG_EXTRA_LDFLAGS="-no-pie -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 -Wl,-rpath=/lib"\n')
        else:
            handle.write(line)
EOF
```

三行,三个开关。头一行把静态翻掉,busybox 从此是动态的。中间一行关编译的 PIE,末一行关链接的 PIE,两道旗各管一段,您扫一眼就分明,编译的管代码怎么生成,链接的管出品是什么型。位置无关的可执行件(PIE),加载到哪儿都能活的那类身子,咱们的机器还没有它的座,这是把后半场交给解释器那一卷挂出的留白,欠单上有名,后面专门的卷再还。今天上桌的 busybox 只好按老礼数出 ET_EXEC,链接的时候落址就定死。末一行还捎了两件要紧的东西。头一件,--dynamic-linker 把解释器的家,写成 /lib64/ld-linux-x86-64.so.2。第二件,rpath 把寻库的路指到 /lib。解释器找库的时候,除了自家惯常的搜法,还会看写进二进制里的指引,盘上的库住哪儿,它照着找。

开卷摸客人吃法的时候,咱们摆过一张三件座位表:解释器住 /lib64,libc 住 /lib,客人自带二进制。那会儿坐上去的是 hello,如今整具 busybox 照着同一张表落座,本尊照旧进 /bin。busybox 动态的身子牵着的东西也不止 libc 一件,它还牵着 libm 与 resolv,三件库,一件不能少。

盘这边,咱们跟着开换血模式。烤盘的脚本添了一个模式字 CINUX_LIBC,取 glibc 或 musl,写别的值它当场大声失败。换血模式一开,busybox 换座,坐进 /bin/sh 的从此是 glibc 版的那具,一串小程序名跟着它走。musl 对下盘谢幕,脚本里给 musl 解释器留的那一行落座,门口挂的条件是模式字不为 glibc,换血盘上它压根不执行。盘从 16M 扩到 64M,给 glibc 一家腾地方。种子跟骨架也补齐,/etc/passwd 与 /etc/group 两册户口本、/etc/inittab 一张当班表上了盘,/root、/sbin、/tmp、/var 几个目录立起来,户口本上 root 的家安在 /root,当班表往后要用的路,都有处可落。

落进 /lib 的三件库,一段就管完,您顺着循环读:

```bash
    for library in libm.so.6 libresolv.so.2; do
        library_path="$(gcc -print-file-name="$library")"
        if ! is_elf "$library_path"; then
            echo "[ext2-disk] missing runtime library: $library" >&2
            exit 1
        fi
        debugfs -w -R "write ${library_path} /lib/${library}" "$image" >/dev/null 2>&1
    done
```

libc 本尊在上一行已经落座,这个环里补的是 libm 与 resolv。咱们看重的是它失败的样子:库要是缺了,脚本不糊弄,exit 1 大声报 missing runtime library。换血盘烤出来验讫,坐 /bin/sh 的是 glibc 版的 busybox,/lib 里数得出来的就三件,一家三口,解释器住在隔壁 /lib64。您要自家核一遍也容易,把烤盘脚本从头读到尾,换血模式里往 /lib 落东西的地方,一只手数得过来。

换血动静这么大,口径反而要收得干净。咱们把模式字一分为二,旧的 musl 模式照旧能出,想烤老盘,把模式字写回 musl 就是。默认这会儿还不翻,翻默认的日子排在作坊那一章,到时候公开地翻,中间谁也不偷偷换口径。留着的旧模式还有活干:堆里砌护墙那一章结案,musl 的全链路复绿,就是在它的产物上验回来的,对照的家底还在。

换血还有一条界,咱们按老尺子量:能解释今天手里这套机制的进正文,来路一字不涂。当年第一遍,笔者把 Buildroot 的 musl 工具链出的动态 busybox 放上盘,它从进门那天起喝的就是 musl 的血。把它换成 glibc,是重走这一遍多做的一步,当年没有这笔判例。所以开门迎客那一卷请 busybox 进门的叙事,把后半场交给解释器那一卷跑通 musl 动态的那段路,咱们一个字不改。客人还是原来的客人,身份不变,变的是血。换血的事单独成章,就立在这儿。

末了一件顺手的沿用,咱们点个名。glibc 的 busybox 一上桌,用的就是家里现成的座序:fd 从 0 起数,头三号是真座,内核起头一个进程的时候,显式把 0、1、2 三座 stdio 装齐。这一件属于当年判例里 libc 无关的那部分,讲占址重建的那一章把三组语义点过名,它在其中,章法已立,照用,不重做。

户口本与当班表都上了盘,可当家的还没坐进一号座。十四位客人的点名考,咱们也该开了。
