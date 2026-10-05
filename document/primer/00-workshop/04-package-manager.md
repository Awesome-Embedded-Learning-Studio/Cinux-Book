---
title: 04 · 包管理器
---

# 04 · 包管理器:一行命令,和一整条跟着进门的链子

您想在机器上装一件东西,手上那套 Windows 的老本事大概会冒出来。您打开浏览器,其实第一个念头就是到官网去看,挑对了版本把安装包下载回来,您双击它,一路的下一步下一步下一步,末尾还得小心地把那个勾去掉。Linux 这边不这么办事。您在终端里敲了一行命令,回了一趟车,它自己去找、自己去下载、也自己往系统里面装了,中间还得把别人家的几个零件搬进来。咱们这一篇就跟着这一行命令走一趟,看它怎么把一件小软件请进机器,又怎么亲手送走,看清楚它到底带了多少东西进来。

## 装东西这件事,为什么要有位管家

Windows 的那套老习惯,在 Linux 上真的用不上。咱们也不是说那套做法不好,只是它在这边派不上什么用场。那本讲命令行的书里讲得很直白,专有软件那边的做法,而常见的无非就是买介质,或者跑到官网去把安装包下载了回来,然后再跑了上一遍安装向导。而 Linux 走的并不是这样的老路子。这个系统上几乎所有的软件都住在互联网上,而且绝大多数由发行版厂商以包文件的形式提供。

仓库这边也分了门派,咱们认的是包文件的家族。Ubuntu、Debian、Linux Mint 这一类用的都是 `.deb`,红帽、Fedora、CentOS 那一类用的就是 `.rpm`。包文件当然不只是个压缩包,它里面除了程序本体,还揣着自己的名片,上面写着叫什么、什么版本、多大、干什么用。咱们再看一句更有分量的话,其实程序很少是单打独斗的,它们多半要借用别人家的零件,比如一段公共的库。书里给这层关系起了个名字,就叫做依赖了。所以现代的包管理系统会替您算清楚该跟着要哪些包,再把它们一并取了回来。

管这些事的工具分上下两层。低层的那几位只管动手,装文件、拆文件、列清单这些活都归了它,而依赖关系上谁该跟谁一起进门,它一概都不管了。高层的那几位管的是关系,它们认识仓库里的索引,会替您搜,也会替您比版本高低,还会把依赖算明白了。Debian 这一派的低层叫 `dpkg`,高层里排着的是 `apt`,它身边还站了个 `apt-cache`。书上的作者写书的时候,通用的是 `apt-get`,您机器上两个都在,子命令的名字也一样。apt 自己带了一份说明书,上面写着它是给人敲的,正经的脚本该去用那几个老工具,老教程里之所以有一串 `apt-get`,就是这么来的。这一篇的动手命令,咱们都按您顺手敲的 `apt` 来。

> 咱们把包名和包文件分开来看,包名是您点单时说出来的话,包文件是仓库货架上那个具体的东西。您可以报上名字让管家替您去取,也可以自己下载了一个包文件直接塞进去,后面的做法属于另一套手法,咱们后面单说。

## 找它、看它、把它请进门

仓库并不在您家的硬盘上,您手里拿着的只是它的一角。索引就是一份名单了,上面写着这个仓库里都有什么。所以在动手之前,咱们得把索引更新一回。咱们敲的 update 要动系统级的地方,所以前面挂了 `sudo`,它是干什么用的,咱们后面有一节专门说:

```bash
sudo apt update
```

咱们在 Ubuntu 24.04 上跑的命令,回给咱们的就是下面这样的话。它是笔者在真正的 Ubuntu 24.04 里取到的一份回话。不是从书里抄来的,也不来自您眼前的机器。这一篇里凡是 apt 自己回的话,全都是这么来的(只有后面那段 sudo 对话例外,到时候咱们另说)。输出里的 `noble`,说的就是 Ubuntu 24.04 这一版的代号,`noble-updates`、`noble-security`、`noble-backports` 则是它另外三个补充仓库的名字。您那儿敲出来的行数、包名、版本都会不一样,因为索引里的这些东西,都是跟着日子往前走的。

```text
Hit:1 http://archive.ubuntu.com/ubuntu noble InRelease
Hit:2 http://archive.ubuntu.com/ubuntu noble-updates InRelease
Hit:3 http://security.ubuntu.com/ubuntu noble-security InRelease
Hit:4 http://archive.ubuntu.com/ubuntu noble-backports InRelease
Reading package lists...
```

字头那个 `Hit` 说的是“本地索引已经是最新的,不用重下”。您把本地索引清空了之后再跑上一遍,这些字头就全换成了 `Get`,那才叫真正的下载。您冷启动那一趟的回话里,十九条 `Get:` 拉回来的三十多兆里头并没有哪一个软件,而是 `Packages`、`InRelease` 这一类索引文件,最大的一份有 19.3 MB。当然,这些数目只属于当天的最小镜像,您那儿拉回来多少,看您这些仓库有多久没更新。您敲的命令叫 update,它干的活只是刷新本地索引,它跟装东西之间其实还隔着一层。这几行的次序,各家机器上都是不同的。

索引到了手,咱们接下来就该找东西,您拿 `apt-cache search` 递一个词过去:

```bash
apt-cache search cowsay
```

```text
cowsay - configurable talking cow
cowsay-off - configurable talking cow (offensive cows)
xcowsay - Graphical configurable talking cow
```

回话里放着的一共三行,一行写着的是一个包名,再加上一句话的描述。咱们顺带交代一句它找东西的脾气,它的眼睛并不只是盯着包名看,描述里带上这个词的也一并捞了上来,`xcowsay` 就是这么被它找着的。您要是已经知道了名字,就拿 `^` 和 `$` 把两头给夹住了,回话就收窄成一行的。

咱们要请进门的就是 `sl`,一个专治手滑的小玩具。您哪天把 `ls` 敲成了 `sl`,它就替您把那个错纠正一遍。咱们在请它进门之前,得看看它关于自己的介绍:

```bash
apt-cache show sl
```

```text
Package: sl
Architecture: amd64
Version: 5.02-1
Installed-Size: 59
Depends: libc6 (>= 2.2.5), libncurses6 (>= 6), libtinfo6 (>= 6)
Filename: pool/universe/s/sl/sl_5.02-1_amd64.deb
Size: 12708
```

咱们顺着名片往下看。这里贴出来的只是节选,完整的自述里还夹着维护者、分区、包的级别这些行。`Depends` 那一行您也看见了,这么小的一个玩具站不住,当然得靠 `libc6`、`libncurses6`、`libtinfo6` 这几位在下面撑着它,它们都是系统里的库包,您看到的就是依赖落在纸上的样子。`Filename` 给咱们的东西更有意思,它写出来的是这个包在仓库货架上待着的具体位置。`universe` 说的是它在仓库里所处的那一块,`amd64` 说的是它给 64 位 x86 机器用,后面清单里还会见到的 `all`,说的则是这个包不分机器架构。版本号和大小也是会变的,您那儿看到的内容跟这一屏不会一样。那咱们就装吧:

```bash
sudo apt install -y sl
```

咱们在命令尾巴上加的那个 `-y`,说的就是“别问了,直接干”这样的意思。咱们在 Ubuntu 24.04 上跑它,它回话的头一段就是这个样子,咱们一起看看:

```text
Reading package lists...
Building dependency tree...
Reading state information...
The following NEW packages will be installed:
  sl
0 upgraded, 1 newly installed, 0 to remove and 4 not upgraded.
Need to get 12.7 kB of archives.
After this operation, 60.4 kB of additional disk space will be used.
```

您点的只有 `sl` 这么一个名字,它就把要做的活摊开来告诉您,这次新装的只有一个,下回来的是 12.7 kB,占您 60.4 kB——笔者跑的是一份干净的最小系统,您那儿的数量比这个大得多。把这些都摊完了,它这才动手的:

```text
Get:1 http://archive.ubuntu.com/ubuntu noble/universe amd64 sl amd64 5.02-1 [12.7 kB]
Unpacking sl (5.02-1) ...
Setting up sl (5.02-1) ...
```

下载、解包、配置——咱们就这么看着它一路走完了下来,您自己什么也都没再操心过。它到底落到哪儿去了?咱们回头问一句 `which sl`,回来的就是一行 `/usr/games/sl`。装好的命令通常落在 `/usr/bin`,这个玩具却被放进了 `/usr/games`。游戏类的包有这个去处,您敲命令的时候照样喊得动它,并不是非得去 `/usr/bin` 里找的。

## 更新、卸载,和它那份“我告诉您,但我不替您决定”

装完了别急着走,刚才那段回话里有半行您可能一扫而过了:`0 upgraded, 1 newly installed, 0 to remove and 4 not upgraded.`。那个 1 是这次的活,`4 not upgraded` 是它顺口交代的另一件事,您这系统里还挂着几个包该升没升的,它看见了,也记下了,这次的它并不去碰。它装一个 `sl` 的时候,管的就是 `sl` 这一件事,别的活它不替您代劳。剩下的四个是谁?它自己会说:

```bash
apt list --upgradable
```

```text
WARNING: apt does not have a stable CLI interface. Use with caution in scripts.

Listing...
libaudit-common/noble-updates 1:3.1.2-2.1ubuntu0.1 all [upgradable from: 1:3.1.2-2.1build1.1]
libaudit1/noble-updates 1:3.1.2-2.1ubuntu0.1 amd64 [upgradable from: 1:3.1.2-2.1build1.1]
libssl3t64/noble-updates,noble-security 3.0.13-0ubuntu3.16 amd64 [upgradable from: 3.0.13-0ubuntu3.15]
perl-base/noble-updates,noble-security 5.38.2-3.2ubuntu0.6 amd64 [upgradable from: 5.38.2-3.2ubuntu0.4]
```

顶上那行警告是 apt 自己打出来的,它提醒您别拿它写脚本,跟前面 apt 自己的说明书一个口气。方括号里的那半句把新旧的版本号双双写了出来,`[upgradable from: …]`,它写的就是“升级”两个字落到纸上的那副模样。您那儿是哪些包、有几个,全看您多久没更新过。您要是还想看得更细,那就请它把动作给您过上一遍,不过一件都别做:

```bash
apt --just-print upgrade
```

```text
The following packages will be upgraded:
  libaudit-common libaudit1 libssl3t64 perl-base
4 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.
```

这个 `--just-print` 开关是挺好用的,它把要做的每一件都摊开来念给您听,念完拍了拍手就走,一个字节都不动的。这里还有一件容易混的事,得跟您交代清楚,咱们说的更新,其实并不是一条命令。`apt update` 刷新的是索引,只有 `apt upgrade` 才会真的去动软件,书里把两条命令并排写在了一起。

那咱们现在就把 `sl` 送走:

```bash
sudo apt remove -y sl
```

```text
Reading package lists...
The following package was automatically installed and is no longer required:
  libncurses6
Use 'apt autoremove' to remove it.
The following packages will be REMOVED:
  sl
0 upgraded, 0 newly installed, 1 to remove and 4 not upgraded.
After this operation, 60.4 kB disk space will be freed.
Removing sl (5.02-1) ...
```

回话里写得清清楚楚的,它要拆的只有 `sl` 这么一个。它机灵地提了一句 `libncurses6`,那是前几回在系统里被自动装进来的零件,现在没人要了,它告诉您该拿 `autoremove` 去收,自己却一动也不动的。它替您看明白,却不替您动手,那 4 笔升级的活也是同一个意思。您再点一次它的名字,这回请出低层的 `dpkg`,尾巴上的 `-l` 就是列清单的意思:

```bash
dpkg -l sl
```

```text
dpkg-query: no packages found matching sl
```

`dpkg-query` 就是 `dpkg` 负责查询的那一半,它并没有报什么错,只是回了咱们一句查无此包。刚才还在的东西,这会儿连名字都不认了。进的时候是一个名,出的时候是一个名,中间那些自动跟来的零件,它一样都不替您做主。

## 它动不动就要 sudo,这一节说清楚

上一篇收尾的时候,咱们答应过 `sudo` 的事情要等到装东西的时候一起说,这会儿就是来还它的。前面出现的那些命令,`update`、`install`、`remove` 的身上都能看到 `sudo` 的影子。您要是把它敲漏了,这活就办不成的,书里那句话其实写得很清楚,要动别人家目录之外的文件,就得有超级用户的权限的。`sudo` 要动的是系统级的目录,而系统级的目录不归您一个人管。这话书里有个同型的场景,咱们看看原话(bill 是书里的示例用户):因为 bill 动的是他家目录之外的文件,所以需要超级用户的权限。那它跟 root 是什么关系?书里数了三条换身份的路,咱们挨个来看。头一条是退出当前的身份、再以另一个用户登录,第二条用的是 `su`,第三条用的就是 `sudo`。`sudo` 走的是另一条路,管理员在 `/etc/sudoers` 里规定了,谁能以什么身份执行哪些具体命令。而 Ubuntu 那边,人家干脆就不设 root 的密码。一切管理权限都交在 `sudo` 的手上。这就是为什么别人家的 Linux 会跟您要 root 的口令,而您手上的机器,从来只跟您要自己的。

问到您自己的密码,这里头是有个道理在的,书里那句话就是最好的答案,用 sudo 并不要您拿到超级用户的密码,它验证的就是您自己的密码。书上配了个例子,长的是这个样子:

```text
[me@linuxbox ~]$ sudo backup_script
Password:
System Backup Starting...
```

这一段是笔者从书上抄来的例子,不是这一篇取到的实录。笔者那一批 apt 回话,是在一个以 root 身份运行的 Ubuntu 环境里取的,全程都绕开了 sudo,所以密码这一句问话长什么样子,笔者也没亲手见过,只能请书上的例子替咱们作个示范。您那儿敲下去大概也是这么个意思,提示符后面就等着一个 `Password:` 了,您敲自己的密码,按了回车,这活就干上了。输完了这一回,您再连着敲一条带 sudo 的命令,它多半就不问了,书里说这是 sudo 在多数配置下把咱们“信任”了几分钟的缘故,不过那个计时器一旦走完,它就又要问了。

> 您要是好奇自己的机器被授予了哪些权限,`sudo -l` 会列给您看,书上的例子回来的是两行,头一行说的是这个人在本机可以跑哪些命令,第二行 `(ALL) ALL` 说的是什么都能干。

## 它说不认识这个名字

装东西最容易遇到的一句话,是它回您:**找不到这个包**。听上去像是它坏了,其实九成是您下面这地方出了岔子。

一种是把 `sudo` 敲漏了。命令您敲全了,它偏说您权限不够,这就是书里讲的另一种情形,动系统级的目录需要超级用户的权限,您没把自己的身份出示给系统,回头把 `sudo` 补上再办的。

还有一种情况是名字看走了眼的。您自以为记得它叫什么,敲下去人家偏偏没听说过,这时候还硬猜什么?咱们回头拿功能词去问一遍索引,`apt-cache search 关键词` 这一手在这里是最管用的,因为描述也成了搜索的线索。名字忘了也能找回来,您手上记得住的东西,通常都不是什么包名,您记下来的是它到底能干哪些活。另外还有一件小事咱们也需要算上,它手里的索引同样得是新的,您要是很久都没 `update` 过,再对的名字也可能扑空。

## 这一章只碰了仓库里的货

咱们这一篇走的是“从仓库里取货”的这段路,仓库外面还有几件事咱们没碰。低层的 `dpkg` 也能直接吃一个包文件,那是您自己下载来的东西,可 `dpkg` 是不做依赖解析的,缺的那口气它会当场报出来,但不会替您把缺的装上。名片上那几行校验和是下载之后验货用的,咱们看了一眼就放下了。红帽那一派的低层叫 `rpm`,管的就是 `.rpm` 这一类包文件,高层那边排着的是 `yum` 和 `dnf`,跟咱们这边正好是两套。第三方仓库怎么写、喂给 apt 的源列表放在哪儿,这一篇咱们都只认了脸。`--just-print` 这个开关的正式名字叫试运行,跟 `upgrade` 长相相似的 `full-upgrade` 有什么区别,咱们留到真要用的时候再说。`apt` 提了一句的 `autoremove`,咱们这次没敲,以及从源码编译安装、那些不在仓库里的设备驱动,往后都有各自的场合。

临走之前还有一件事要办了,您也可以顺手把它办了,敲命令的时候允许它把动作念给您听,您自己一件都不做。往后凡是拆东西、升东西的时候,咱们都该请它念上一遍。

回头看这一趟的活,咱们从仓库里请进门的东西,最后也只走了个干干净净。可咱们最该多看的一眼,还是留给开头那一屏的,那一趟拉回来的三十多兆,结果一个软件都没有。往后您再敲 update,心里就该有数了。咱们接着往下走,下一篇轮到的是编辑器。