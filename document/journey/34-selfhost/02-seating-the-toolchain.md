---
title: "02 · 请师傅进门:闭包与门牌"
description: "师傅进门,头一件活是算行李。ldd 对着二进制逐个报名,动态库家属一个不落,合起来才叫闭包。crt 一家是 C 运行时祖传的家什,gcc 有问必答,照 specs 的路径布局在开发机上凑一棵 staging 树,每样东西按绝对路径原样安座。门牌是硬的:PT_INTERP 写谁就装谁,ld-linux 装成普通文件,坐在 /lib64/ld-linux-x86-64.so.2 分毫不差的路径上,差一个字师傅不进门。烤盘换了路:造盘脚本从 debugfs 逐件写改成拼整棵目录树、mkfs.ext2 的 -d 一口烤进盘,硬链与 SONAME 软链原样保住,musl 老盘原路保留,口径起步 128 MiB、8192 个 inode。断链案立判例:libc.so 是 GNU ld 的脚本,GROUP 指名 /usr/lib 下的 ldso 副本,看着冗余的其实是特性,过滤即断链。指纹缓存与作坊那一章同族。烤盘路上两笔工程笔记照实记:修改时间与指纹两套缓存不同步,动脚本须强制重烤。烤盘之前树在开发机上手工把汇编链接走一遍,盘内问题与命令行正确性分家,省 QEMU 轮次。当年真容也照实念:as 与 ld 两个二进制带七样动态库加 crt,盘 8 MiB 扩到 128 MiB,cc1 约 47 MiB,g++ 时代 192 MiB、16384 个 inode。盘烤好了,下一章两位老师傅上工,交头一件活。"
chapter: 34
order: 2
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - toolchain
  - gcc
  - ext2
  - rootfs
---

# 请师傅进门:闭包与门牌

师傅进门的头一件活是算行李。as 在开发机上看着是一个老老实实的可执行文件,真要搬家的时候,它从来不落单:咱们拿 ldd 对它一问,随行的动态库一个个报出名字,打头的是 libbfd,后头还跟了一串。漏带了一个,进了盘也起不来,解释器找不到要的库,当场把程序轰了出去。所以行李要的是一家人全须全尾,全家老小得一个不落地带上,这样凑齐的整份名单,行话里的名字是闭包。

名单不由咱们手抄,交给 ldd 现场点:对着二进制一个一个地问,回答里只认带落点的那几行,把路径取了出来,排好序去了重,闭包的正本就出来了。开发机上的家属是谁,盘上住的就是谁,多带了一个不行,少带了一个也不行。

家属怎么搬,咱们交给脚本里一个小函数去办:

```bash
cp_lib() {
    local src="$1"
    [ -f "$src" ] || return 0
    local base soname
    base="$(basename "$src")"
    cp -aL "$src" "$work/usr/lib/"
    soname="$(readelf -d "$src" 2>/dev/null | sed -n 's/.*SONAME.*\[\(.*\)\].*/\1/p' | head -1)"
    if [ -n "$soname" ] && [ "$soname" != "$base" ] && [ ! -e "$work/usr/lib/$soname" ]; then
        ln -sf "$base" "$work/usr/lib/$soname"
    fi
}
```

您贴着中间那两行看:readelf 问出库自报的 SONAME,也就是它对外应答的小名。有的库文件名挂着版本尾巴,别人找它的时候只认小名,所以本尊复制过去之后,还得替小名补一条指回本尊的软链。软链是跟着本尊一起走的,进盘的时候,两样少了一个都不行。

crt 一家也是要随行的。这些带 crt 字头的小文件是 C 运行时祖传的家什,程序落座起跑之前的那一段热身,全由它们替咱们张罗,libgcc 则是 gcc 自家的后勤库。名单咱们不用背,gcc 是有问必答的。咱们拿 `-print-file-name=` 问一声 `crt1.o` 住哪,它答出来的路径分毫不差,脚本照它答的数取件。凑出来的整棵树行话叫 staging 树,照的是 gcc 的 specs,一张写死寻路答案的清单:每样东西在开发机住哪个绝对路径,树上住的就是哪个路径,两位老师傅问路照的是老习惯,答案是一字不差的。

解释器的座位另有一张门牌。把后半场交给解释器那一卷立下的礼数,咱们一直守着:PT_INTERP 写谁就装谁。生态那一卷给 busybox 换血的时候,--dynamic-linker 把解释器的家,写成 `/lib64/ld-linux-x86-64.so.2` 这个地址,打那天起盘上的解释器就住这个门牌。师傅进了门,门牌是一个字都差不得的,落座的路数就三行,行行写的都是路径:

```bash
cp -aL "$as_bin" "$work/usr/bin/as"
cp -aL "$ld_bin" "$work/usr/bin/ld"
cp -aL "$ldso_bin" "$work/lib64/ld-linux-x86-64.so.2"
```

这三行咱们挨个看,清清爽爽:as 与 ld 两位进的是 /usr/bin,解释器坐在分毫不差的 /lib64 门牌下,而且是一份实打实的普通文件。软链的另一头要是留在开发机上,进了盘就成了断头路,师傅按门牌敲了门,门是敲不开的。

树摆好了,还得记得自己摆过了。作坊那一章给 busybox 立的三道章程,第三道就是指纹缓存:进过一回的料记一笔总册,册页没变的话,咱们就不重出工。同一门章法咱们原样搬进工具链的院落:

```bash
fingerprint="$({ collect_inputs | sort -u | xargs sha256sum; sha256sum "$script_abs"; } | sha256sum | cut -d ' ' -f 1)"
if [ -d "$out" ] && [ -f "${meta_dir}/.inputs" ] &&
   [ "$(cat "${meta_dir}/.inputs")" = "$fingerprint" ]; then
    echo "[toolchain-staging] cache hit, keeping ${out}"
    exit 0
fi
```

入册的每一页,咱们一笔一笔数:两位老师傅的本尊,ldd 点出来的家属,随行的 crt 与 libgcc,备好的种子源码,外加脚本自己的一页,一页一页地过哈希,末了合成一笔指纹。指纹对得上旧册的话,树原样地留下,对不上的话,咱们就把整棵树从头再摆。换了一台机器,工具链变了,指纹跟着就变了,旧树是绝不冒充新货的。

盘的这一边,咱们把烤法整个换了一轮。生态那一卷烤 glibc 盘的时候,走的是 debugfs 逐件写,一件文件一条命令地写,塞进镜像的回回是一件。那时的件数少,这么写没脾气,如今进盘的是一整棵树,硬链软链成了片,逐件写的老路保不全。新烤法倒了个个儿:在开发机上把整棵目录树拼好,busybox 的本尊进 /bin,二十多位 applet 一个个地 ln 到同一具身子上:

```bash
    local applet
    for applet in cat ls stty script id uname ping ifconfig nslookup netstat \
                  echo whoami pwd true false sleep env hostname wc ps free \
                  mount umount mkdir sh; do
        ln "$assemble/bin/busybox" "$assemble/bin/${applet}"
    done
    ln "$assemble/bin/busybox" "$assemble/sbin/init"
```

一个文件顶了多个名字,占的是一个 inode。咱们把这棵树拼到末尾,师傅的整棵家当用一行 `cp -a "${staging}/." "$assemble/"` 原样叠上去,再让 mkfs.ext2 带上它的 -d,把这棵树一口烤进了盘:硬链进去还是一个 inode 挂几个名字,SONAME 的软链进去还是软链。musl 的老盘,烤法是一条没动的,照旧是能出的。构建图里新挂了 toolchain_staging 这个目标,烤盘照旧是一条命令的事。

咱们把起步的口径翻到 128 MiB,inode 给到了 8192。生态那一卷收在了 64 MiB,可师傅全家还在陆续地进门,口径从起步就按全家的身材定。

当年第一遍的真容,咱们照实念:as 与 ld 两个二进制,作伴的动态库有七样,开出来的名单是 libbfd、libctf、libjansson、libz、libzstd、libsframe、libgcc_s,再带上的是 crt 一家。8 MiB 的老盘一步扩到 128 MiB、inode 8192,心里装的已经是 cc1 那 47 MiB 上下的块头,轮到 g++ 上桌的时候,再扩成了 192 MiB、16384。这一遍起步的数字,跟当年的第一步正好对上。

行李里最容易看走眼的,是解释器的第二份。咱们把闭包过到树里,/usr/lib 底下躺着的是一份 ld-linux-x86-64.so.2,跟 /lib64 门牌底下的一份一模一样,是同一个文件占了两份。咱们头一回盘点,看它实在碍眼:门牌底下明明有一份了,它像搬运工多抖出来的冗余。咱们清掉了。再链接的时候,ld 当场就回了话:`cannot find /usr/lib/ld-linux-x86-64.so.2`。

好好的链接,怎么会去 /usr/lib 找解释器?这东西不是程序跑起来才进场的吗。线索在 libc.so 的身子里。您把它打开看,它压根不是什么二进制,肚子里是一段 GNU ld 的脚本,GROUP 的一行白纸黑字,指名要 /usr/lib/ld-linux-x86-64.so.2:链接的师傅按单取件,单上是有名的,架上偏偏无货,自然就翻脸了。把清掉的副本请回来,链接立马就过了。两份各有各的差事,/lib64 的一份管跑,/usr/lib 的一份管链。

判例立在了这儿:“看着冗余的,其实是特性”。当年笔者的 cp_lib 早就装了它,重走的这一遍,咱们差点把这课重修一回。往后咱们动行李,动手之前问这么一句,讨这件东西的是谁。

烤盘的路上还有两笔工程笔记,咱们照实记。头一笔记的是两套缓存不同步。staging 脚本自己看的是指纹,构建图那头看的是产物的修改时间:脚本改了,指纹变了,可旧树还好端端地躺着,时间戳是一动也没动的,脚本里喊了一声 cache hit,构建图这么一看,只当是天下太平了,烤进盘的还是旧树。WSL 上的时钟再一漂,修改时间愈发地不可信。所以章法定死:动了 staging 脚本,就把旧树删掉了重摆,疑心要是起了,连盘一起删了重烤,谁也别信谁的记忆。

第二笔是验货的路数。盘进 QEMU 之前咱们让这棵树在开发机上原样干一遍活:拿树上的 as,把树上备着的 hello 汇编汇成目标文件,再拿树上的 ld 链成成品。链要是断了,断的地方就在开发机上,哪一处断的一眼分明,改起来的话也快,到了 QEMU 里再出的毛病,那才是盘里的事、串口的事。两边的病分了家,盘内的问题与命令行本身的正确性从此分得清,省下的就是一回一回开机的轮次。

树验过了,盘烤好了,师傅一家在盘上坐定了,门牌是分毫不差的。下一章咱们请两位老师傅上工,交他们的头一件活。
