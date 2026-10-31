---
title: "14 · 作坊:三层货与一本进料册"
description: "上一章撂过的话在这里接住:满屋家当件件是手装的,该有一间作坊把它们一条龙装出来。章法是考古考来的:当年第一遍笔者用真 Buildroot,三层分离,骨架一层,专有件一层,工具链闭包一层,末了拼进装配间再一次性烤成盘;重走的这一遍借范式不借工具,自家脚本一条龙。进料立三道章程:BusyBox 1.38.0 官方归档,SHA-256 固定校验,指纹缓存进册,源码,配置,脚本,编译器,libc 五样入册,料不变不出新货,命中即跳。作坊安家 user/rootfs,busybox 是院里一间屋,配置面是一张环境变量的清单。三个出口:build 出盘,run 装盘开机,verify 非交互验收,判据齐才绿。换血的默认在此翻正,musl 模式退作旧路的回归入口。空缓存加独立空目录,从下载校验到全量组装走通一遍,verify 落绿:把后半场交给解释器那一卷挂的源树旧欠,在这里还清,可复现从此不靠开发机的抽屉。作坊能出盘了,一条命令还差一步:作坊得长进构建图里,开张才算数。"
chapter: 33
order: 14
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - rootfs
  - busybox
  - glibc
---

# 作坊:三层货与一本进料册

把后半场交给解释器那一卷收卷的时候,欠单上有一条,念出来不太光彩:busybox 与 musl 的源树在开发机本地。这句话的分量,咱们这一卷走到今天算是全领教了。烤盘的脚本认的是开发机上的私藏:busybox 的源树躺在用户主目录的 `.local` 下面,血是当场从宿主编译器的行囊里舀的,客人程序是现编的。盘离了开发机,烤不出来:代码原样搬到另一台机器,那边的抽屉是空的,照样抓瞎。所以这一章咱们开一间作坊,把这件事一次了结。

作坊的章法,咱们从当年考古考来。当年第一遍走到这一段,笔者请的是真 Buildroot,路数叫三层分离。骨架一层,Buildroot 自己产,目录树,busybox,musl,都从这一层出。专有件一层,行话叫 overlay,Cinux 自家的当班表,测试脚本,冒烟用的源码,盖在骨架上头。第三层是工具链闭包,把 GCC 全家从开发机里整箱抽出来随行。三层货备齐,拼进一间过渡的装配间,末了 mkfs 一次性烤成盘。构建系统拿到的是成品,挂上去就开机,盘是怎么来的,它一概不问。这套章法笔者当年用得顺。重走的这一遍,范式收下,工具不请:真 Buildroot 不进咱们的树,三层货由自家的脚本一条龙装出来。

作坊安家,落在 user/rootfs 这个院落,busybox 是院里一间屋,构建脚本,配置,说明清单,还有点验家当用的探针,都住这间屋里。三层货,咱们照样分三层码。头一层,骨架与户口:目录树,加上 /etc 一家的种子,户口本,当班表,fstab,都在这一层。第二层,血的一整套闭包:从宿主编译器的行囊里点名取件,解释器进 /lib64,libc,libm,libresolv 进 /lib,盘上 glibc 一家的血,本尊就是这一层进的货。第三层,自家的客人:busybox 一具身子,加上 hello 这样的自编程序,一一入座。末了烤盘,debugfs 一件一件把三层货写进 ext2 的镜像。当年是装配间加 mkfs,咱们是 debugfs 逐件写,路数同源,家什换了。

咱们给进料立三道章程。头一道,料有正身:BusyBox 认 1.38.0 的官方归档,从 busybox.net 下来,不认开发机抽屉里来路说不清的源树。第二道,校验固定:归档的 SHA-256 校验值写死在脚本里。SHA-256 咱们不陌生,把一份文件的内容压成一段定长的十六进制,内容变一个字节,它就整个变了样。第三道,指纹缓存:进过一回的料记一笔总册,册页没变,不重出工。头两道落在脚本开头这几行:

```bash
    version=1.38.0
    checksum=34f9ea6ff8636f2c9241153b9114eefa9e65674a45318ae1ef95bb5f31c53bb2
    cache="${CINUX_ROOTFS_CACHE:-$(cd "${here}/.." && pwd)/.cache/rootfs}"
    archive="${cache}/busybox-${version}.tar.bz2"
    mkdir -p "$cache"
    if [ ! -f "$archive" ]; then
        curl -fL --retry 3 --output "${archive}.part" \
            "https://busybox.net/downloads/busybox-${version}.tar.bz2"
        mv "${archive}.part" "$archive"
    fi
    printf '%s  %s\n' "$checksum" "$archive" | sha256sum -c -
```

这几行把头两道章程走全了。缓存里没有归档,才去 busybox.net 下载,半成品落成 `.part` 文件,挪正名,随后拿写死的校验值过 `sha256sum`,一个字节对不上,当场报错退出,绝不带病进炉。校验过了才解包,源树就位。您留意 checksum 那一行:校验值跟脚本一起进版本库,谁来验,验的都是同一个值。料的身份从此不靠哪个网站说了算,靠这段写死的十六进制。

第三道章程是这间作坊的心脏,咱们把算法单独摆出来看:

```bash
fingerprint="$({ sha256sum "${here}/../user/rootfs/busybox/busybox.config" "$script_abs";
                 printf '%s\n' "$source_fingerprint";
                 sha256sum "$(gcc -print-file-name=libc.so.6)";
                 gcc --version; gcc -dumpmachine; } | sha256sum | cut -d ' ' -f 1)"
if [ -f "$out" ] && [ -f "${out}.inputs" ] && \
   [ "$(cat "${out}.inputs")" = "$fingerprint" ]; then
    echo "[build-busybox-glibc] cache hit, keeping ${out}"
    exit 0
fi
```

一笔指纹,五样东西收进同一册,咱们一笔一笔数。头一样是 busybox 的配置,就是 /lib 里只剩一家那一章翻过三个开关的同一份。第二样是脚本本尊。第三样压的是源码树,会参与编译的文件,从 `.c`,`.h` 到 `Makefile`,名字连内容逐个过一遍哈希。第四样是宿主编译器配的 libc。第五样是 gcc 的版本与目标机描述,两行合成一样。五样凑齐,整包再过一次哈希,落进产物旁边的 `.inputs` 文件,这就是进料册的本体。下一次开工,新算的册页跟旧页对得上,一行 `cache hit`,作坊收工,料一动不动。对不上,说明五样里有一样变了,重活从头再干,新页盖过旧页。

五样里最容易被当成外人的,是 gcc 与 libc 这两样。可咱们这一卷干的就是换血的活:盘上 glibc 一家的血,本尊取自宿主的编译器,同一份 busybox 源码,换一套 libc 编出来,血就换了。在别的工程里,编译器多半算工具,在这间作坊里,它的身份是料。料入册,理所应当。换一台机器,gcc 与 libc 一变,指纹立变,作坊绝不拿旧册冒充新货。

作坊开三个出口,咱们一条命令挑:

```sh
bash user/rootfs/busybox/build.sh           # 默认 run:装盘开机
bash user/rootfs/busybox/build.sh build     # 只组装镜像,不开机
bash user/rootfs/busybox/build.sh verify    # 非交互验收
```

咱们挨个看。run 是默认出口,咱们敲下去,镜像备齐,直接开机进 KVM,要的是一块可写的 `/dev/kvm`。build 只组装,内核,客人,busybox,盘,备齐就收摊,机器不开。verify 是非交互的验收:机器自己起来,把这一卷攒下的家当逐件点过,当家的坐在一号座上,十四位客人逐个交退出码,glibc 的 hello 出问候,fstab 与临时隔间,共家的储物间跨 fork,门内的传话带着 poll,判据到齐,绿灯才亮,哪一条不齐,作坊不替咱们圆场。

配置不另立文件,是一张环境变量的清单,立在 user/rootfs/busybox/README.md 里,一行一个,默认值写在旁边。`CINUX_BUILD_DIR` 管构建目录往哪儿出,`CINUX_ROOTFS_CACHE` 管进料缓存住在哪,`CINUX_BUSYBOX_SOURCE` 是一扇侧门,咱们设了它,就用自家的源树,跳过下载,`CINUX_GLIBC_LDSO` 与 `CINUX_GLIBC_LIBC` 管血的本尊从宿主的哪里取。开发机的家什也得点齐,gcc 与 g++ 要 14 往上,cmake,qemu,e2fsprogs 之类,完整清单 README 里写着,缺哪样,作坊会在对应那一步大声喊停。可复现说的是料有正身,人人领得到,空机器白手起家那回事,它可没许诺。

咱们还有一处口径要翻正。/lib 里只剩一家那一章留过话:两种造盘模式并存,musl 的旧路照旧能走,默认暂不翻,等作坊立起来统一。如今作坊立起来了,默认翻过来,造盘脚本里管这件事的是一行:

```bash
libc_flavor="${CINUX_LIBC:-glibc}"
```

环境变量没有设的时候,这一行取的就是冒号后头的 glibc,咱们空手跑一遍造盘,出来的直接是 glibc 一家的 64 MiB 盘。musl 的模式退居二线,只作旧路判据回归的入口。两种模式并存的过渡期到此翻篇,往后谁也不用再记哪种盘该传哪个变量。

成色得换个地皮验。咱们把 `CINUX_BUILD_DIR` 指向一个空目录,再把 `CINUX_ROOTFS_CACHE` 指向另一个空目录,缓存里一粒料都没有,构建目录里一砖一瓦都没有,然后从正门走进去,您看着作坊从头走一遍:下载,校验,解包,编译 busybox,编内核,烤盘,一路走到 verify,绿灯照样亮。这就是换一台机器重开张的预演。作坊的料不再是开发机抽屉里的私藏,是官方归档加指纹,任何机器,任何人都领得到,验得出,重得出同一张盘。把后半场交给解释器那一卷挂下的源树旧债,到这里还清了。

作坊能出盘了,可开张还差一步,差的那一步不在作坊身上,在构建图身上。构建图是 cmake 手里的依赖图,谁依赖谁,谁变了谁重跑,都记在图上,眼下它还不知道作坊的存在,咱们烤盘,还得记着去敲作坊的门。作坊长进构建图里,一条命令从干净的树直达开张的机器,那一章才算数。
