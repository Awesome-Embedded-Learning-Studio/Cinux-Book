---
title: "24 · 假文件与真终端:/dev、/proc 与伪终端"
description: "开门迎客那一卷收卷时,客人指名的下一批东西都在这一卷兑现:名字里的家什 /dev 与 /proc 造了出来,设备坐进名字,进程的事实照进镜子。终端的单例拆成了可造的类,termios 的开关面立起,线格式量准了三十六字节。伪终端成对铸出,fd 头三号的座位从假座换成了真座,前台组领走了键盘,^C 与 ^Z 在真客人身上走通,四桩悬案请外面的眼睛一并破掉,检查镜像带盘启动零 trap。"
chapter: 24
order: 0
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - devfs
  - procfs
  - pty
  - tty
---

# 24 · 假文件与真终端:/dev、/proc 与伪终端

开门迎客那一卷收卷的时候,咱们把客人的话记在了明处:它住下了,伸手指名的下一批东西也现了形。终端要的是开关与伪终端,作业控制的整台戏等着锣,还有几样名字里的家什,照得着面的日子排在下一卷。这一卷就是来兑现的。客人把 /dev/null、/proc/self/exe 都写进了代码,咱们盘上却只有 ext2 一棵真树。名字咱们都认得,东西咱们一件也拿不出来。

咱们照例盘家底。手里有的是无盘起家那一卷立的文件系统后端接口,一张擦掉脸面的约定,ramfs 与 ext2 各穿了一回,开门迎客那一卷的管道又穿着它混进了文件表。挂载表咱们立了八座,ext2 占了根,空着的还有七座。终端这头咱们也盘了盘:行规程在上一卷住进了内核,可它是全机器只此一台的单例,回显是常开的,开关是没有的,伪终端这个词咱们连影子都没有。号位表上 ioctl 咱们只答得出两问,ppoll 收下的掩码没有验,七号的 poll 门牌还空着。还有一处咱们自己心里有数的虚火:fd 的零一二号是三张特权假座,read 与 write 到了系统调用里就直接认号不认表,真正的客人哪天较起真来,假座是要现形的。

这一卷的活,咱们摊开报数。头一件造假文件的第一种:设备目录 /dev 落地,null 与 zero 两个纯计算设备头一批住了进去,console 也跟着住了进来,挂载点在盘上占好了位。第二件拆终端的单例:一台行规程变成可以造出任意多台的类,接线四座各归各的,termios 的开关面照着线格式立起来。第三件是伪终端:ptmx 一开就铸出了一对,pts 的座次动态解析,fd 头三号的假座同时换成真座,七号的 poll 补上。第四件造假文件的第二种:进程名册 /proc 落地,事实在读取的那一刻现场生成,活着的进程才在册。第五件开锣唱作业控制:控制终端、前台组、^C 与 ^Z 的翻译投递一整条链,ppoll 的掩码真验了,sleep 被打断的实弹要打得干净。中间还夹了四桩悬案,咱们把案卷送出去,请外面的眼睛看了一遍,回来的时候四案并破。末一件收尾:ACPI 的表头读取改字节级,检查镜像带着数据盘一路启动到了 shell,咱们连一声 trap 也没有听到。收尾照旧走的是验收。

当年的旧话也照例交代。工程线上这一段的几件活,设备目录、进程名册、伪终端是接连几天打完的,咱们一件一件看,倒是哪件都不算最大,合在一处却是一堵新的陡坡。幕间立下的家法咱们记得牢,这一遍咱们按件消化:跑起来是上一卷,假文件与真终端归了本卷,动态链接是再后面的一卷。本卷咱们一句不越界,链接器与动态库的三方协议,咱们一个字不碰。命名管道的名目咱们也交代清楚:管子上一卷已经从号位里出生了,不过给管子起名字挂进目录是另一回事,咱们不排进本卷。

<StationPanel tag="r24_devfs_real_tty" build="cmake -B build && cmake --build build --target run" />

<ChapterNav variant="sub">
  <ChapterLink num="1" href="01-the-furniture-in-the-names" desc="客人代码里的 /dev 与 /proc 只闻其名,假文件的两种造法在卷首定谱:名字是真的,内容是活的。终端缺开关与影子,fd 头三号的假座心里有数,单子一片一片数出来">名字里的家什</ChapterLink>
  <ChapterLink num="2" href="02-a-directory-that-is-not-on-the-disk" desc="设备文件是没有介质的文件。设备表八座,注册制而非出生制,null 与 zero 纯计算头一批住,console 接线另住一层,四条缝从 inode 类型一路补到 stat 的 rdev。挂载点在盘上预建空目录,ls / 从此见得到 dev 与 proc">不在盘上的目录</ChapterLink>
  <ChapterLink num="3" href="03-one-discipline-many-terminals" desc="行规程剥掉单例,一台变成可以造的类:接线四座带上下文,一份纪律代码喂多台终端。构造零动作化配懒挂线,constinit 家法两连,首版函数内带构造的 static 翻车如实记,纯逻辑化之后宿主第一次直测终端">一门纪律,多台终端</ChapterLink>
  <ChapterLink num="4" href="04-thirty-six-bytes-across-the-line" desc="termios 的开关面立起:TCGETS 四问落地,ECHO 与 ICANON 行为全生效,ISIG 存位待后。线格式三十六字节,咱们量库那一层量出六十,写穿了边界,当卷更正回线格式,断言改锁线上尺寸。ttydemo 读一行零回显的秘密,全链走通">线上三十六字节</ChapterLink>
  <ChapterLink num="5" href="05-eight-bytes-lost-and-regained" desc="ttydemo 一声一般性保护异常,反汇编对回入口的 movaps。编译器认的是进场栈顶除十六余八的约定,懒到家那一卷修过的减八,开门迎客那一卷立新常量时丢了,0x100 改回 0x108。busybox 不炸是库自己校正,老 demo 不炸是优化恰好没选 SSE">丢了又找回来的八字节</ChapterLink>
  <ChapterLink num="6" href="06-a-device-that-mints-pairs" desc="伪终端是一对环夹一台真行规程:master 写进 raw 环,slave 的纪律在环上跑,输出汇进 master 的回读环。ptmx 一开就铸一对,pts 动态解析,后端接口添三钩子,设备表五张脸。busybox script 跑通三层回显,三个终端实例各司其职。座位的当卷更正:只铸新不回收">一开就铸一对的设备</ChapterLink>
  <ChapterLink num="7" href="07-three-real-seats-for-the-first-three" desc="script 的孩子关掉零号再开新座,POSIX 答应它拿回零号,咱们的零到二号是特权假座,当场引爆。找座改成最小空闲含零,shell 预置 console 三座,九个特权分支全拆,表从八座添到十六座,七号 poll 补上。头三号与任何 fd 同路,是这一卷才有的新形态">头三号的真座位</ChapterLink>
  <ChapterLink num="8" href="08-a-mirror-of-the-living" desc="别的后端答介质上的字节,procfs 答此刻的事实:七座注入,读取那一刻现场生成,活进程才在册。快照锁内拷字节不拷指针,当年那桩竞态按单核窗口讲。self/exe 委托真文件,readlink 开八十九号,sysinfo 补九十九号,free 念出真数">活着的镜子</ChapterLink>
  <ChapterLink num="9" href="09-keys-that-belong-to-a-group" desc="控制终端、会话、前台组,一台终端的键有了归属。ISIG 按出生的 c_cc 表翻译 ^C 与 ^Z,读被打断答 EINTR,设备两侧各管各的前台组,登录 shell 预置。ppoll 的掩码真验真恢复,nanosleep 与 tgkill 补号。ash 从 console 起整套会话无降级,sleep 被 ^C 打断零 unserved">键是哪一组的</ChapterLink>
  <ChapterLink num="10" href="10-four-verdicts-from-outside" desc="四桩悬点的案卷送出去,外面的眼睛回来四案并破:fork 把父表的座位折了号,十号悬空;键盘只认 Shift,Ctrl 出生缺位;ps 要十一次赋值才认 stat;开机的 init 没落父,名册只收有父的。四案同一条病根:绿的测试没造出真客人的形态">外面回来的四纸判决</ChapterLink>
  <ChapterLink num="11" href="11-the-last-ud2" desc="检查镜像带数据盘启动,ACPI 表头奇地址上直接解引用,HPET 表长的读取是最后一个 ud2 现场,全部改字节级清完。双重回显随开关面销案,送键的桥修得不再报假成功,检查镜像一路进 shell 零 trap">最后一个 ud2</ChapterLink>
  <ChapterLink num="12" href="12-acceptance" desc="面板安静,家当全在运行时。宿主五十三件长到五十七件,四件新住户点齐案名,机内十五台照旧,六段验收 exit 零。整站复阅十处发现逐项过,边界如实登记:无读者的信号推进、停止状态的回报、PTY 座位与 EOF,一样一样挂明处">验收:门里门外都是真话</ChapterLink>
</ChapterNav>

合卷的时候咱们再点一遍手里的新话:名字里住进了设备,名册里照出了活进程,一台终端变成了可以成群造的类,伪终端一开铸出了一对,头三号的座位是真的了,键盘的键归了前台的一组人。欠的也照旧挂在明处:写往空管道的那对事件,信号侧的 SIGPIPE 与 errno 侧的 EPIPE,词还是备着的,挂点还是没有接上的,PTY 的座位只铸不还,行池还认不出 EOF 的边界,script 的会话收尾不再应声,是收官之后新冒的疑点。客人在静态世界里的家什,这一卷要齐了。剩下那场动态的戏,链接器、装载器与库怎么三方搭台,排在后面的那一卷开锣,咱们在这里把话撂下,这一卷就收了。
