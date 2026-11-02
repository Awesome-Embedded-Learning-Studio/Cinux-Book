---
title: "12 · 共家的储物间"
description: "家里两位客人要搬一大块数据,走管道得一个字节一个字节地抄,写进盘上的文件得绕出去再读回来,最快的一条路是两家同看一页内存。SysV 共享内存进家,按本卷的叫法立一间共家的储物间:房间按号码条认,几家客人凭同一张号码条走进同一间房,货就摆在页里。四个号位一次配齐,号照 Linux 的表:shmget 二十九领房,shmat 三十挂靠,shmctl 三十一管事,shmdt 六十七退租,三个连号坐一排,一个坐得远。挂靠落册时装扮成匿名段,要紧的是 shared 那一栏把手,缺页照册子补,补进页表的就是储物间里的原件。柜台只认两个命令字:IPC_STAT 抄名册,IPC_RMID 注销名字。钥匙还了,货还在,名字抹掉之后原挂靠继续存活,最后一家人退租房才散架,旧号码条再挂靠答负数。fork 过来,共家房不走写时复制,父子页表指进同一张物理页。对照线程那一卷的 CLONE_VM,那里共的是整个世界,这里租出去的只有几页。长凳一案专数家数:劈段不改,克隆加一,清算减一,归零收摊。机内探针走全一单:孩子睁眼读到父亲写的 41、自己改写成 73,名字注销挑在孩子还挂在房里的时点,收尸后父亲读到的已是 73。管事只此两样,十六间房,每间至多一兆字节,教学尺寸摆在明处。"
chapter: 33
order: 12
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - ecosystem
  - ipc
  - shm
  - syscall
  - fork
---

# 共家的储物间

家里眼下住着的是好几位客人。两位客人之间要搬一大块数据的时候,要么走管道,一个字节一个字节地抄过去。要么写进盘上的文件,走绕出去再读回来的老路。行是都行的,但是都躲不开拷贝。最快的一条路是让两家看同一页内存:一边落笔写下的字,另一边转头倒是看得见。这样的设施,真系统里有叫 SysV 共享内存的现成老家当,当年 System V 那一脉 Unix 传下来的进程间通信家什,老到 POSIX 都把它收编进了标准。咱们按本卷的叫法给它安名:共家的储物间。房间是按号码条认的,几家客人凭同一张号码条走进的是同一间房,货是摆在页里的。

开门的号位一次挂齐,号照 Linux 的表:shmget 二十九、shmat 三十、shmctl 三十一,三个连号坐的是一排,shmdt 单独坐到了六十七,人家的表上它就住那么远。吃法咱们一句话念齐:shmget 领房,shmat 管的是挂靠,shmdt 管的是退租,shmctl 是管事的柜台。派发用的还是老章程,号落进按号索引的派发表,表里有人应咱们就派发,没人应答的就是未实现。

房间的尺寸咱们照实念:十六间,每间至多一兆字节的大小,满了答 ENOMEM,申报零字节或超限的答 EINVAL,尺寸是教学用的尺寸,咱们摆在明处。册子上的条目长这样:

```cpp
struct Segment {
    base::Shared<mm::SharedPages> pages;
    std::uint32_t                 key{};
    std::uint32_t                 uid{};
    std::uint32_t                 gid{};
    std::uint32_t                 mode{};
    std::uint32_t                 creator{};
    unsigned long                 bytes{};
    unsigned long                 identity{};
};
```

您顺着字段读,头一个 pages 装的是货,后头一群装的是名分,key 扛的是名字,uid 与 gid 扛的是户口,mode 扛的是门规,再往后的两位是字节数与身份号。名字递零的申请永远开新房,glibc 给这个零起了名,写的是 IPC_PRIVATE,客人拿到的号码条就是身份号,递给同伴当暗号用的。带名字的申请是寻房,册上已有同名的话,领的就是同一间。申报的尺寸比房里大,答的是 EINVAL。IPC_CREAT 与 IPC_EXCL 两面旗是一起递的,房里已有同名的话,答出去的是 EEXIST,这里是不肯重名的。没递 IPC_CREAT 而册上无名,答的是 ENOENT。门规位照老 Unix 的分法,主人、同组、旁人各占了三位,身份为零的 root 走哪间都不查票。

挂靠是 shmat 的活。客人往 RDI 里递的是身份号,RSI 里递的是期望地址,递零的话就请册子代劳,从 mmap 区的顶向下找空档,找空档用的还是 find_free_spot 那件老家什。旗认的只有两面,SHM_RDONLY 挂成的是只读,SHM_RND 把地址往下对齐到页,多出来的位答 EINVAL。落册那几行咱们接着念,这一段装扮成匿名段的模样,要紧的在末尾那栏:

```cpp
    region.kind     = mm::VmaKind::kAnonymous;
    region.readable = true;
    region.writable = (frame.rdx & kReadOnly) == 0;
    region.shared   = base::Shared<mm::SharedAttachment>(
        new mm::SharedAttachment(std::move(pages), region.start, region.end));
```

shared 那栏是非空的,这段就从自家私有地变成了租来的共家房。往后的路咱们交给按需分页:缺页来了照册子查,查到的是 shared 把手,补进页表的就是储物间里的原件,物理页的共享票顺手加一,几家补进去的都是同一张页。退租是要认门牌的,shmdt 递的地址必须正好是挂靠那天的起点,差一页也是不认的,答的是 EINVAL。真把房腾空的活,走的正是占址重建那一章收进来的内部缝,munmap 是缝的定义之家,shmdt 成了又一个消费者。

柜台眼下认的只有两个命令字,咱们把号跟 Linux 的宏对上:IPC_STAT 抄名册,IPC_RMID 是注销名字的,别的字进门一律 EINVAL,样子是不装的。注销的权查得严,认的是房主本人或 root,旁人答的是 EPERM。要紧的一段就这几行:

```cpp
        for (Segment*& slot : world.entries) {
            if (slot == kSegment) {
                slot = nullptr;
                break;
            }
        }
        delete kSegment;
```

注销删的是名字,货是不走的,票攥在挂靠的人手里。Segment 死的时候,它攥着的 pages 票还回去一份,可每家挂靠的时候,SharedAttachment 又各自攥了一份,引用计数没到零的话,物理页是一张都不散的。钥匙还了,货还是在的,咱们管这叫钥匙与货分家。旧号码条自此就作废了,新客人再拿它挂靠的话,册上是查无此房的,答的是负数。

fork 过来的时候,咱们跟着克隆世界的老路走:孩子的页表一页一页照着父亲画,私有的页两边都打写时复制的记号,谁写的谁来抄。画到 shared 把手非空的段,笔法换了:

```cpp
    if (kRegion != nullptr && kRegion->shared.holds()) {
        Pmm::self().retain_frame(base::PhysAddr{kTarget});
        return;
    }
```

咱们接着看这几行的上头,孩子页表里画进去的已经是同一张物理页,画页递的是同一个 kTarget。走到共享的段,只把物理页的共享票加一,写时复制的记号一笔不打。于是父子两家的页表指进同一张页,孩子落笔写下的字,父亲不用等谁的捎信,转头去看就是了。挂靠的家数同一时刻也在动:克隆是把册子一起抄的,新世界给自家立一份新的挂靠票,家数就加了一。哪一家清算的时候,自家的票减一,最后一家走完了,货这才散了架。

共享这件事的近亲,咱们在线程那一卷认得:共世界的旗是 CLONE_VM,组里几条流挤的是同一个世界,连册子都是共用一本的。储物间这边反了过来,两家各守各的世界、各翻各的册子,共的只有租出去的那几张页。同是共享:一层共的是整个世界,一层共的是几页货,两层各管各的。

长凳上咱们添了一案专数家数,数下来的每一步都对得上:挂一段共家房,数下的家数是一。把中段改成了只读,段劈成了两半,数下来的家数还是一,劈的只是段,同一家劈出的碎片合用一张票。克隆了一次,家数到了二。父亲的世界清算,家数回落到了一。孩子的世界也散,家数归了零。计数一路数到了底,这一案是绿着的。

咱们在盘里备了机内探针,把整套故事真跑了一遍。储物间的那一段,父亲起手把 41 写进了房里,柜台抄了名册给他核:facts.shm_segsz 是 4096,facts.shm_nattch 记的是一,挂靠家数那一栏落在名册偏移八十八的位置,读的正是活着的计数。正戏排在了后头:

```c
    const pid_t kChild = fork();
    require(kChild >= 0, "shm-fork");
    if (kChild == 0) {
        require(*kPage == 41, "shm-child-read");
        *kPage = 73;
        require(shmdt((const void*)kPage) == 0, "shm-child-detach");
        _exit(0);
    }
    require(shmctl(kSegment, IPC_RMID, NULL) == 0, "shm-rmid-live");
    int status = -1;
    require(waitpid(kChild, &status, 0) == kChild && status == 0 && *kPage == 73,
            "shm-child-writes-shared");
    require(shmdt((const void*)kPage) == 0, "shmdt");
    require(shmat(kSegment, NULL, 0) == MAP_FAILED, "shm-stale-id");
```

孩子睁眼读到的就是 41,看的是同一张页,所以不劳拷贝。孩子写下了 73,退了租走人。父亲这边挑的时点刁,值得您多看一眼:孩子还挂在房里,名字就注销了,那一行的考点是 shm-rmid-live,冲的就是钥匙与货分家。收完了尸再去读,读到的已经是 73,孩子写的字隔着一次注销送到了父亲眼前。父亲自己也退了租,这间房的票清到零,货才散了架。拿旧号码条再挂靠的话,门是不开的,答的是负数。整段咱们一环没断地走了下来。储物间开了,门内的传话也该立:客人要隔着墙说话,出门上网的绕路就省了。
