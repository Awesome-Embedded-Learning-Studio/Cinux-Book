---
title: "09 · 收官:伞、盘上的 shell、respawn"
description: "Own 家族入库,持有的东西出界即还,AddressSpace 析构自拆,fork、execve、init 全线收伞。内核镜像里的 .user 行李整段拆除,shell 改出 ELF 搬上盘,kernel_init 从 /bin/sh 起用户世,wait 到退出就重拉——上一卷借来的称呼全部还清。run 命令的全链在真正的机器上走通,孩子的 pid 是 3。"
chapter: 17
order: 9
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - process
  - shell
---

# 收官:伞、盘上的 shell、respawn

破了三案,链路通了,咱们收官的活是三件:把内核里的裸指针收进伞,把 shell 的身子搬去盘上,让 init 学会等和重拉的手艺。

伞的家规,咱们拍得直白:内核里不许 new 和 delete 裸奔。这一卷里 fork、execve、init 连着写了下来,错误路径一条比一条地深,哪条忘了还,哪条就漏——人是会忘的,类型倒是不会。所以在 base 里立起 Own 一家:Own<T> 持有一个对象,出了界就 delete。Own<T[]> 持有的是一段数组,MakeOwn、MakeOwnArray 是造它们的工厂,转移只有显式的 move 和 release 两路,想悄悄复制的话,编译期就把你拦下。AddressSpace 的析构函数接上 teardown——delete 即回收,世界随着尸身拆了。fork、execve、init、task_runtime 全线换了伞,错误路径从此只剩一句提前的 return,手里的东西自己会还。文档咱们也写得如实:内核堆的 new 耗尽是响而不是返空,MakeOwn 的失败语义按真话讲。同一天进家的还有 Shared 和 Weak 两位,把文件系统节点那套数着引用过日子的纪律写成类型,Weak 替咱们盯着对象,对象没了自动落空,是不会悬垂的。名字系统已经这么活了半程,现在轮到类型替咱们守。

第二件是 shell 的搬家。下楼开张那一卷收卷的时候咱们交过底:echo 打出来的字是您敲的,help 报的菜名是编进镜像里的,程序自己睡在内核镜像的怀里,盘上什么都没有。认字那一卷把盘立成了根,可 shell 的铺盖还留在镜像里。这一卷请它出门:构建链里 objcopy 抽纯二进制的那道工序,链接输入里的 user_blob.o,内核里的 g_user_shell 那对符号,整段都拆了。shell 改出了 ELF,照 hello 的样子,配上 -nostdlib -static 的参数,造盘脚本把它写进了 /bin/sh。从此机器上剩下的只有一台装载器——kernel_init 起 shell、execve 起程序,走的是同一张脸,跑的是同一套头校验、同一个段循环。kernel_init 的主流程函数 init_thread 重写成分层的四步:读卷上的 /bin/sh 整份进 Own 缓冲,新世界的装载加一页栈,造出的任务下放 ring3,然后咱们 wait。shell 退了,打印了退出码,重新拉一个新的。respawn 的循环,就是 init 的余生。

这个循环这么一转,上一卷的一笔旧债就清了。上一卷交底:PID 0 和 PID 1 都是借来的称呼,fork 和 wait 的影子都没有,借的只是姿势。这一卷 fork 有了,wait 有了,PID 1 成了名副其实的一家之主——它的每个孩子退场,真的有一双手收下退出码。您算算机器的 pid 流水:boot 是 0,kernel_init 占的是 1,shell 占的是 2,shell 里 fork 出的头一个孩子是 3。验收的时候,您会在屏幕上亲眼见到这个 3。

第三件的 run 命令,是 shell 的门面。逻辑您已经全认识:fork 一声,孩子拿到的是 0,转头 execve 路径指的程序,办不成的话就报一声、退场带 1。父亲拿到的是 pid,wait4 等它的退场,收据到了手,把 pid 和退出码报给敲命令的人。help 菜单添上了 run,一口气到了十四样。

还有一桩遗留的事,咱们记在明处:孤儿从孩子名册里摘掉的只是亲缘,链结的那一截还挂着——respawn 长跑,名册慢慢沉下来一群没爹的行,功能性是对的,可这是个慢性的性能漏。摘链的活,同样并给了按需分页那一卷,那边要动等待和名册的形态,咱们一次收拾。

真正的机器上,run /bin/hello 全链走通:盘上的 shell 发起 fork,孩子换上了 hello,问候打了出来,Exit(42) 交了码,父亲收了尸,提示符回来了。逐字的会话,咱们留到验收时端上来。这一卷的主体到这儿完工,后日谈里还有一桩当年的名案要讲,讲完了再上秤。
