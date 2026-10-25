---
title: "25 · 把后半场交给解释器:ELF 动态链接"
description: "假文件与真终端那一卷收卷时把话撂下:动态的戏排在后面开锣,这一卷开锣。静态与动态差的只是程序头表里的一条登记,内核照 Linux 的分法只做三件事:扫出解释器的路径、把它装进 256MiB 一个确定的家、改入口再把一桌 auxv 情报摆上栈页。musl 的期待面咱们翻源码逐条核过,栈页二百六十四字节的一笔数算在了动刀之前。实弹首弹炸出两桩连破的案:号位表上对调多年的两个座,mprotect 拒劈段的旧章法,都在第一个照真 ABI 敲门的客人手里现了形。盘上开出 /lib,烤盘默认自带解释器,ash 坐进登录座,开机提示符换了相,裸名敲出动态的问候。"
chapter: 25
order: 0
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - elf
  - dynamic-linking
  - ldso
  - musl
---

# 25 · 把后半场交给解释器:ELF 动态链接

假文件与真终端那一卷收卷的时候,咱们把话撂在了明处:客人在静态世界里的家什齐了,剩下那场动态的戏,链接器、装载器与库怎么三方搭台,排在后面的路上开锣。这一卷就是来开锣的。

咱们照例盘家底。手里有的:开门迎客那一卷编好的 musl 工具链与烤盘的管线,生与收那一卷立下的 execve 与装载器,懒到家那一卷的登记册与 mprotect,认字那一卷铺好的 ext2 地界一样都在。缺的咱们也一片一片数:解析器只认静态的可执行形,位置无关的身子进来就被拒在门口。装载器连基址的概念都没有,虚地址就是绝对地址。栈页上的 auxv 是一桌空席,问了也不答。咱们盘上没有 /lib 这个目录,exec 认识的登记里没有 PT_INTERP,号位表上 munmap 与 mprotect 还坐着当年换错的座。家底数完了,活就都现了形。

咱们进站时,系统认得的只有静态的 ELF:一具由内核整读整装的自足身子,进了门就能跑。咱们出站时,系统认得带 PT_INTERP 的动态程序:内核扫出解释器的名字,把 822KB 的它装进 256MiB 一个确定的家,栈页上摆好了一桌情报,后半场的收尾交给解释器自己,盘上的 ash 裸名敲出动态的问候。

这一卷的活,咱们摊开报数。头一件把边界划清:静态与动态差的那一下,以及内核只做三件事的边界。第二件解析器分两张座位:程序座收固定地址的形,解释器座收位置无关的形,换了座就拒。第三件给解释器一个固定的家:基址进了装载的世界包,选址的道理与堆的旧债一起摆开。第四件栈页上摆一桌真的 auxv:musl 等着什么,咱们翻源码逐条数,当然不猜。第五件动刀之前的一笔数:二百六十四字节的字区,算清了再改偏移。第六件工具链与装盘:三面旗铸出动态的 hello,烤盘默认自带了解释器,一句说谎的探测被魔数拦下。第七件实弹与两案连破:号位对调的陈年旧座,protect 拒劈段的旧章法,探针的零打印半刻定案。第八件 ash 坐进登录座:init 直拉的暗口补上路。第九件当年的考古:大个子解释器挤爆文件上限,当年怎么绕的路。末一件收尾走的还是验收。

当年的旧话咱们也照例交代:工程线上,这一段当年的活是一天发完的,连改块大小绕行都是同一天的事。重走这一遍的活也是一天落的地,可讲的时候咱们按件分了十章,一件一章地排,这是幕间立下的节奏家法:第一遍的读者被单卷的陡度喷倒过,咱们这一遍不许再犯。旧线上这一卷最大的坎是 ext2 的文件上限,咱们这一遍因为认字那一卷提前把双重间接做掉,连坎都不剩了,只在考古那一章留了一页插图,这是把当年最陡的一段拆成三卷给咱们的红利。

<StationPanel from="r24_devfs_real_tty" tag="r25_dynamic_loading" build="cmake -B build && cmake --build build --target run" />

<ChapterNav variant="sub">
  <ChapterLink num="1" href="01-the-one-line-difference" desc="静态与动态差的那一下:程序头表里多一条 PT_INTERP,一句指名解释器的路径。启动拆成前后两半,前半场内核只做三件事,后半场全是解释器在用户态的活。GOT 与 PLT 是它的学问不是咱们的,PIE 与基址随机化留给讲安全的那一卷">差的那一下</ChapterLink>
  <ChapterLink num="2" href="02-two-seats-one-parser" desc="ParseElf64 长出第二个参数:座位。程序座收类型 2 的可执行,解释器座收类型 3 的共享对象,拿错座位一律拒。ElfImage 新报头表落点,解释器的路径从整读在手的 buffer 直接拷,宿主案子从四案长到十案">一台解析器,两张座位</ChapterLink>
  <ChapterLink num="3" href="03-a-fixed-home-for-the-interpreter" desc="LoadWorld 收编座位与基址两词,主程序基址为零,解释器平移到 256MiB。用户半区地图摊开论证为什么求确定,堆扩张不查重叠的旧债登记在案,段权限进登记册这一步为实弹埋伏笔">给解释器一个固定的家</ChapterLink>
  <ChapterLink num="4" href="04-the-real-auxv" desc="musl 1.2.5 的源码一条条核:缺了就崩的硬需五味,缺了能活但可惜的营养组,AT_RANDOM 的缺席这些年一直由可预测的确定性回退硬扛。十四枚号全锁给宿主的 elf.h,画像拆成宿主可链的独立件,四笔测试的错记下">真正的 auxv</ChapterLink>
  <ChapterLink num="5" href="05-two-hundred-sixty-four-bytes" desc="十四对 auxv 摆上栈页之前先算数:0x108 的字区二百六十四字节,init 直拉那一路三十三词贴着顶,余量检查会把 init 自己拒掉。抬到 0x208,除十六余八的 movaps 形态保住,极端的满 argv 满 envp 仍诚实答 E2BIG">二百六十四字节,与一个新偏移</ChapterLink>
  <ChapterLink num="6" href="06-a-toolchain-and-a-lying-answer" desc="musl 的 libc.so 一具身子两份差事。三面旗铸出 15KB 的动态 hello,装盘头一回开 /lib,依赖表里的单名靠 ld.so 自识别就够。-print-file-name 答的是宿主 glibc 的链接脚本,魔数校验一步拦住假货,烤盘从此默认自带解释器">动态的工具链,与一句说谎的回答</ChapterLink>
  <ChapterLink num="7" href="07-the-first-guest-that-knocked-by-the-book" desc="实弹首弹 RELRO protection failed,炸出两桩连破的案。号位对调:裸号十敲进了 munmap 的门,自家用符号常量的测试遮了这么多年,探针的零打印半刻定案,修完拿宿主 syscall 表锁四个号。protect 拒劈段:整吞的旧门换成逐页覆盖走查。两案都在,缺一跑不通">头一个照本敲门的客人</ChapterLink>
  <ChapterLink num="8" href="08-ash-takes-the-login-seat" desc="教学 shell 能启动程序的动词只有 run,回回摩擦,登录座换血:kernel_init 仍是 pid 1,盘上 /bin/sh 指到 busybox 的 ash。init 直拉不经 exec 的默认铺路,得亲手递 PATH。提示符从 cinux> 换成 / #,裸名 hello-dyn 一次验完查找与链接">ash 坐进登录座</ChapterLink>
  <ChapterLink num="9" href="09-a-guest-too-big-for-the-blocks" desc="822368 字节的解释器在咱们这一遍读得毫无戏剧性,当年不是:1024 字节的块,直接加单间 268KiB 就是全部,读到第 274432 字节处断流,正是双重间接的第一块。当年把盘改成 4096 块绕行,块大小换文件上限的取舍慢慢算">大个子客人,与当年的绕行</ChapterLink>
  <ChapterLink num="10" href="10-acceptance" desc="面板一行没添。宿主五十七件不动,件里的案长厚,机内十五台不动,六段验收 exit 零。实弹三场逐字抄:动态问候,静态回归,盘面清点。复跑脚本的手稿两笔记下,边界一条一条挂着,合卷的话撂给请第二颗脑子的那一卷">验收:后半场交得干净</ChapterLink>
</ChapterNav>

合卷的时候咱们再点一遍手里的新话:解析器认得两具身子的形了,装载器学会了基址,栈页上的一桌 auxv 摆齐了十四味,盘上有了 /lib,烤出来的盘自带会动的家什,号位表跟真 ABI 对了表,mprotect 学会了子区段的落笔,ash 在登录座上接了客。咱们欠下的也一并列着:堆扩张不查重叠的旧债,PIE 与基址随机化的留白,符号链接的形态,busybox 与 musl 的源树在开发机本地,还有上一卷接着挂的老欠。咱们的机器到今天还是一个脑子在转,把第二颗请上线的那场戏,排在后面的路上,这一卷就讲到了这里。
