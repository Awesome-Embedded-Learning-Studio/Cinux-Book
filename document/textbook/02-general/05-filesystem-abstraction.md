---
title: 05 · 文件系统抽象
---

# 05 · 文件系统抽象:一张脸,两张表,一堆名字

`welcome to cinux - this file was baked into the kernel image`。cat readme.txt,回您的就这一行。这行字有来历:关机重启一回,它还在。等会儿您 echo 进 note 的字就没这个待遇,机器一关就没了。两样待遇,两个住处,readme.txt 烤在内核镜像里,note 住在内存的堆上。

住处这件事,咱们再往深问一步。Cinux 的内核,手里一块盘都使唤不动:磁盘驱动整个不存在,那块引导盘把内核送进内存,差事交割完毕,此后再没人理它。所以字只能住内存,名字也只能立在内存。管名字的那一层,给字节起名字、把名字排成树、按树递字节,行话叫文件系统。咱们的做法有一句纲领,名字立于盘前,今天整章都在给它作注。

上一章收尾咱们记下的话,今天接住,任务会生、会跑、会让、会退了,可它们经手的活还落不下地:write 递出去的字,进了串口就没了影。字得有地方住。这一章把地方造出来,顺带给第 2 部收卷。

咱们一件一件办。头一件立脸,文件系统后端长什么形状,凭什么将来添一位后端,楼上楼下都不用改。再一件安家,挂载表怎么认门,一条路径进来,落到当家的后端头上。末一件开户,fd 表怎么发号,open、read、write、getdents 怎么一路走通。办完实况节起机喂十单,对照节请出 xv6 数七层。

量法这一章,三样家什三个来路。码,自家树里的 vfs.hpp、file_table.hpp、file_world.hpp 三个头文件,带一排每个 syscall 号一件的小文件,sys_read.cpp 就是这样一号一件,咱们逐件读。屏,自家起机,十单会话一条一条抄,一枚读数都不借别人的。书,xv6 翻到第 10 章,书页 89、90 两页逐字核过,咱们整段请。Linux 那头只认两笔自家注释写明的来往,号码随它排、errno 随它抄,其余不比。

> **一块盘都没有,名字凭什么立得起来?盘哪天来了,又往哪儿摆?**
>
> 立得起来,因为咱们把文件系统里跟盘无关的那一半做全了。名字、树、路径、读写,全是形状,没有一样生来要盘。盘来了,往脸后头摆。vfs.hpp 的头注里,笔者给脸面写过一句期票,`ramfs today, ext2 one station later, behind one face`,今天住 ramfs,ext2 进门那天,脸还是同一张。这一章咱们看的就是它,脸怎么立,名字怎么住进去。

## 一张脸:九个方法,编译期点名

咱们从后端说起。后端,行话 backend,真正存字节、记名字的实现。眼下只有一位,ramfs,一棵长在堆字节里的树:目录是节点,文件也是节点,文件的字节住在节点挂着的缓冲区里,要长就在谁身上长。它是个普普通通的 C++ 类,您翻 ramfs.hpp,通篇找不着一枚函数指针,擦成表那一步不归它管,它也不知道自己将来会被挂进表里。

可柜台那侧的 syscall 不认得 RamFs 这个类名,也不该认得。今天住 ramfs,ext2 哪天进门,要是 open 的路里写死了 RamFs 的调用,每换一位房客,柜台里就得改一遍。所以中间得立一张双方都认的脸:后端按自家的样子实现,柜台按脸的样子调用。立脸分两步,咱们一步看一步。

第一步,定资格。C++ 给了个现成家什,行话叫 concept,编译期的资格检查。您声明一个类型当文件系统后端,编译器在编译那一刻把九个方法的原型逐个对一遍,缺一个、错一个签名,编译当场不过。vfs.hpp 里它长这样:

```cpp
template <typename Fs>
concept FileSystem = requires(Fs& backend, std::string_view path, std::string_view name,
                              FsNode node, unsigned long offset, void* sink, const void* source,
                              unsigned long count, FsDirent& entry) {
    { backend.lookup(path) } -> std::same_as<base::Result<FsNode>>;
    { backend.make_file(path) } -> std::same_as<base::Result<FsNode>>;
    { backend.make_dir(path) } -> std::same_as<base::Result<FsNode>>;
    { backend.unlink(path) } -> std::same_as<base::Result<void>>;
    { backend.read(node, offset, sink, count) } -> std::same_as<base::Result<unsigned long>>;
    { backend.write(node, offset, source, count) } -> std::same_as<base::Result<unsigned long>>;
    { backend.read_dir(node, offset, entry) } -> std::same_as<base::Result<bool>>;
    { backend.retain(node) } -> std::same_as<void>;
    { backend.release(node) } -> std::same_as<void>;
};
```

九个方法,咱们挨个报名。lookup,按路径找门,找着了回一枚节点门牌。make_file 与 make_dir,造,造文件、造目录。unlink,拆,拆文件,也拆空目录。read 与 write,按 offset 读写字节,写越过了文件尾,文件自己长。read_dir,按序号领孩子,一回领一个。末了 retain 与 release 是一对看护:开户时 retain 把节点看住,关号时 release 松手,名字被拆了而还有人开着的节点,靠它们活到最后一枚 release。九个全是普通成员函数,RamFs 实现它们的时候,既不知道 concept 在楼上,也不知道挂载表在楼上,头注里写得明白,资格检查与擦表都住上一层。

第二步,咱们擦。概念验完,RamFs 还是 RamFs,类型各是各,表里存不下类型。EraseFs 干的就是把类型擦掉。九个不捕获的 lambda,每个转调真身的对应方法,编译器把每个 lambda 各放成一枚函数指针,真身的地址另走一枚 self 指针,调用的那一刻递过去。头一件转发的样子:

```cpp
template <FileSystem Fs>
[[nodiscard]] constexpr FsOps EraseFs() {
    return {
        [](void* self, std::string_view path) { return static_cast<Fs*>(self)->lookup(path); },
        // (余下八针,同一副擦法)
    };
}
```

擦出来的成品是 FsOps,九枚函数指针,咱们把针脚摆开看:

```cpp
struct FsOps {
    base::Result<FsNode> (*lookup)(void* self, std::string_view path);
    base::Result<FsNode> (*make_file)(void* self, std::string_view path);
    base::Result<FsNode> (*make_dir)(void* self, std::string_view path);
    base::Result<void> (*unlink)(void* self, std::string_view path);
    base::Result<unsigned long> (*read)(void* self, FsNode node, unsigned long offset, void* sink,
                                        unsigned long count);
    base::Result<unsigned long> (*write)(void* self, FsNode node, unsigned long offset,
                                         const void* source, unsigned long count);
    base::Result<bool> (*read_dir)(void* self, FsNode node, unsigned long index, FsDirent& entry);
    void (*retain)(void* self, FsNode node);
    void (*release)(void* self, FsNode node);
};
```

您留意这副两件套的形状:一枚函数指针,配一枚 void* 的 self。指针里不存真身,每次调用真身的地址随头一件参数进门,进了门再转回真身干活。这副形状您不是头一回见,2.4 压轴那座调度器存下的就是四个指针加一个地址,同一副骨架。vfs.hpp 的头注自己也点了名,`the same pair-shape the tick service uses one floor down`,楼下的 Tick 服务用的正是它。文件系统这层的新东西是排场,头一回成建制,九枚指针排满一张脸,后端千差万别,进了表一律长成它。整套活没有一行人手写的表,新后端想进门,写个类、过 concept,擦法 EraseFs 一处包办,头注管它叫 zero boilerplate,零份样板。

脸上还差一件家什,节点门牌 FsNode。后端替您找到门了,得给柜台一张凭条:

```cpp
struct FsNode {
    void*         opaque;  ///< Backend-private node pointer.
    InodeType     type;    ///< File or directory.
    unsigned long ino;     ///< Backend-assigned inode number.
    unsigned long size;    ///< File bytes; zero for directories.
};
```

四格里最有讲究的是头一格,咱们细看。opaque,不透明指针,指向后端自家的节点,谁铸的谁认得,别人拿到也只能原样递回,ramfs 的节点与将来 ext2 的节点长法各是各,凭条上不写也写不了,void* 里只装一个地址。type 说文件还是目录,ino 是后端愿意报的节点号,size 是字节数。凭条归凭条,真身长什么样,只有后端自己知道。

## 挂载表:前缀最长者赢,余路归它

脸立好了,安家。挂载,行话 mount,把一位后端安在一条路径前缀上。内核的家当是 Vfs 的挂载表,章程咱们挨条念:前缀必须斜杠打头,不许拖尾斜杠,除非整个就是根。一条前缀只许挂一家。表统共 8 席。犯规各有名目,前缀形状不对 kInvalidArgument,重挂 kAlreadyExists,表满 kOutOfMemory,这些名目到了柜台会翻成 errno,咱们留到讲 fd 表时再认。

mount 办事只费一举,擦表就发生在这一举里,每家后端一生一次。擦好的 FsOps 连同真身指针与前缀一起入席,咱们看入席那两行:

```cpp
    static constexpr FsOps kErased = EraseFs<Fs>();
    Slot                   slot{.ops = &kErased, .self = &backend, .prefix_len = prefix.size()};
```

住进来了,咱们接着看路怎么走,这是 resolve 的活。客人递来一条绝对路径,resolve 扫一遍全表,找盖得住它的最长前缀,交还三件,胜家的脸、胜家的真身、余路。余路是挂载点之后的部分,不带前导斜杠,空串就是挂载根本人。章程里最要紧的一条,前缀得断在斜杠上,vfs.cpp 的写法:

```cpp
        const std::string_view kPrefix(slot.prefix, slot.prefix_len);
        if (!path.starts_with(kPrefix)) {
            return;
        }
        if (kPrefix.size() > 1 && path.size() > kPrefix.size() && path[kPrefix.size()] != '/') {
            return;
        }
```

咱们拿挂 /tmp 打个比方:它管 /tmp,也管 /tmp/build,偏偏不管 /tmpx。tmpx 是一整个名字,不归 /tmp 的房客,前缀匹配认边界,不认字母。眼下表里只住着一家,前缀就是根,一根到底,可章程是照多家住户立的,多家来的时候它已经站在那儿了。

resolve 交还的东西里,没有住户的名字。ops、self、rest,三件里没有一件能告诉您今天当差的是 ramfs,脸是 FsOps,真身是 void*。头注里有一句原文,`Consumers never learn a backend's name`,消费者永远学不到后端的名字。这不是疏忽,是设计里最要紧的一手:名字不出去,换房客就惊不动任何楼上。今天 ramfs 在,ext2 来了,open 那几行一个字不用改。

咱们内核里,挂载表连同它的根住户只此一套,FileWorld,singleton 家族的新成员,行话 singleton,全局只此一份,排在 Pmm 与 Tick 后头。它的 init() 办三件事,一件比一件实:new 一个 RamFs,mount,前缀就是根,然后把随身的行李搬进树里。

行李,行话 initrd,initial ramdisk 的缩写,实物是一份 tar 档,构建那一步打出来,链接期烤进内核镜像,成了镜像里一段只读字节。行李这词上一章也用过,2.4 数过的五件行李是任务随身的家当,一件一件住在 Task 里。本章往后,行李只认镜像里烤着的 tar 档。咱们烤进去三件,readme.txt 一件,docs 一家带一件 note.txt。起机这会儿,UstarReader 一户一户地走,目录 make_dir,文件 make_file 再把字节 write 进去,名字、类型、个头、内容,全从档里来。ustar 是 tar 家族里老资历的一种格式,512 字节一块,咱们不逐字段细读,您只消认下一件事:它是行李的打包法,解包只有起机这一次,进了树,行李就再没人碰。

**停一停,替期票排一场戏。** 期票上写 ext2 要来。它真来的那天,ramfs 为它改几行?挂载表又得添什么手续?您拿着 mount 与 resolve 的章程推一推,再往下对。

咱们排一遍:ramfs 一行不用改。ext2 写成类、过 concept、擦成脸,进门的路跟 ramfs 走的一模一样,手续就是再 mount 一回,前缀随它挑。想让它坐根位也不难,动的只是 FileWorld 里接线那几行,脸面与 ramfs 都不惊动。尺寸的旋钮也备好了,名字上限如今 32 字节,fs_config.hpp 的头注写着,ext2 来的那天要拧的就只有其中一枚。期票之所以敢立,靠的就是前面这些。

## fd 表:3 号起发,空号回收再使

脸立了,家安了,轮到您用。ring 3 想开一个文件,递路径,内核发还一枚小整数,行话文件描述符,fd。它只是号,不是文件。拿着号来,柜台在一张表里按号找票根。表,FileTable,咱们看票根长什么样,file_table.hpp 里的 FileSlot,四件:

```cpp
struct FileSlot {
    const FsOps*  ops     = nullptr;  ///< The backend's erased face.
    void*         backend = nullptr;  ///< The backend instance.
    FsNode        node{};             ///< Which node of that backend.
    unsigned long offset = 0;         ///< Bytes read so far, or the directory entry ordinal.
};
```

三件是认路的:哪张脸、哪家后端、哪个节点。第四件 offset 是走的进度,也归它最有戏,一枚数字,两副差事。开的是文件,它数字节,读到哪儿、写到哪儿,下次调用从哪儿接着。开的是目录,它数孩子,现在数到第几个,下一回 getdents 领哪个。差事不写在这枚数字上,写在 node 的 type 里,表只管存数,后端按节点的身份解读。这一枚数字够咱们停一停细想,题在节末。

头三号有主,而且是终身制:0 号键盘进,1 号控制台出,2 号控制台错。表从来不发它们,read 与 write 在柜台那侧直接特判,fd 0 走键盘,fd 1 与 2 走控制台,压根不进文件系统。2.3 记过 write 只认 1 号的窄处,fd 2 只能挨 -9,这一版宽到 2,当初应下的放宽手续,这就兑现了一笔。所以表发出去的头一枚号是 3,kFirstDescriptor。头注给了一句交代,`the shape every Unix program grew up expecting`,Unix 世界长大的程序,睁眼拿到的头一枚 fd 从来就是 3,咱们照这副形状长,客人搬进来就不用换习惯。

发号的章程,咱们一句话念完:挑最低空闲,空号回收再使。close 还回来的号,下一回 open 头一个拿去补。表的封顶 8 席,kFileTableMax,头注把预算摊开了念,`Three reserved standard streams plus five open files is a teaching kernel's honest budget`,三条保留流加五个开着的文件,教学内核的老实话预算。满了怎么办?alloc 回 kOutOfMemory,柜台翻成 errno 12,老老实实拒,不偷偷挤掉谁。

表住哪儿,2.4 交代过任务的家当,如今 task.hpp 里又长出一件:files,一张 FileTable 按值住在 Task 身上,头注原话 `Lives inside the Task by value`。2.3 立过一条老话,帧跟着任务走,不跟着 CPU 走,fd 表走的是同一条理,一人一张,您开您的 3 号,隔壁任务开它的,谁也看不见谁的。任务退场那天,表跟着析构,close_all 把还开着的号一枚一枚关掉,每关一枚,release 松一道引用,干净散场。

柜台这次也翻了门面,咱们指认一回,不重开课。2.3 立柜台时,词典 61 格,开张四号。这一版词典长到 218 格,kSyscallCount,号排到 217,开张九号:read 0,write 1,open 2,close 3,yield 24,exit 60,mkdir 83,unlink 87,getdents 217。新开张的五号全在文件系统当差。三条约定没动,号还是随 Linux 报,话还是走 rdi、rsi、rdx,错还是回负 errno。

errno 的名册厚了。咱们手里还是 2.3 备的那三枚,-38 空号、-9 错门、-14 越界。这一版后端进来了,后端说自家的话,KernelError,查无此人 kNotFound,名字已有人用 kAlreadyExists,路径中段站着个文件 kNotADirectory,拿目录当文件使 kIsADirectory,目录还有孩子 kDirectoryNotEmpty。柜台那侧 sys_common.cpp 里立了一座翻译台 ErrnoFrom,把它们挨个翻成客人的话:2、17、20、21、39,兜不住的一律 22。挂载表立章程时报过的 kInvalidArgument,到这儿也有了下落,它没排上专格,落的就是兜底的 22,而 22 在 Linux 里的本名正是 EINVAL,非法参数,兜得不冤。号码照 Linux 抄,syscall.hpp 的注释写明 Linux errno values,跟号码词典同一个道理,将来 musl 的程序来敲门,报错就认。拒门 -9 也添了新差事,getdents 拿 0、1、2 来问目录,回的还是它,门不对,跟文件不文件无关。

咱们把 open 的路整条走一遍,九号里它最见工夫。头一步 CopyUserPath,把客人递的路径搬进内核,越界 -14,空串或超长 -22,这是 2.3 验指针那道岗的翻版。第二步 resolve,找脸。第三步 lookup,拿余路找门。查无此人怎么办,看旗:带了 kOpenCreat,值 0x40,跟 Linux 的 O_CREAT 同一个数,就 make_file 现造一个,没带,错回。末一步 alloc 发号,retain 把节点看住,3 号递出去。cat 一声 nope,走到 lookup 就折了,kNotFound 翻成 2,实况节屏上见。

目录怎么读,getdents 一号独挑,咱们把核心请出来:

```cpp
    const base::Result<bool> kOne =
        kDir->ops->read_dir(kDir->backend, kDir->node, kDir->offset, entry);
    if (!kOne.ok()) {
        return -ErrnoFrom(kOne.error());
    }
    if (!kOne.value()) {
        return 0;
    }
    kDir->offset++;
```

一回一枚,这是它跟 Linux 的分岔:人家一次调用打包一整块缓冲区,咱们一回发一件,头注自己交代了分期,号码与语义照旧,打包的功课等 busybox 需要那天再补。read_dir 按序号领孩子,领到 false 就是到头,回 0。领到一枚,offset 进一,名字与类型写进客人递的缓冲区,type 1 是目录,0 是文件。壳的 ls 就是它养的,open 一回,getdents 数到 0,close 收工。

**停一停,给一枚数字派两回差。** 同一个目录 open 两回,拿回两张票根。头一张数到第 2 个孩子,第二张现在数到第几?再深一层:同一张表里,3 号开着文件,4 号开着目录,一枚字段两种单位,凭什么不打架?您推一推,再往下对。

咱们对:两张票根,两个 offset,各数各的。第二张还从 0 起,头一张数到第 2 个孩子跟它不相干,一人一张表,一个槽一个数。单位也不打架,offset 不解释自己,node 的 type 才解释。3 号槽里的节点是文件,它的 offset 是字节,4 号槽里是目录,同一枚字段就是序号。表存数,后端按身份读数,职责切开,数字就不串门。

## Code: Cinux 实况:起机、十单、随身的行李

实况锚 r14_vfs_ramfs_initrd,文件系统立起来的那一版。切版的老话照旧,git switch --detach 切过去,行号都按这一版数,看完 git switch - 回 main。镜像的老话也照旧,改完代码 boot_image 得显式重打。喂键的法门 2.3 交过底,monitor 接 stdio,字母原样,空格 sendkey spc,回车 sendkey ret,十单照此喂。这一版的壳还有个新脾气,咱们一句话交代:单字命令它一声不吭地吞掉,所以咱们每一单都带参,收场交给 timeout。

```bash
git switch --detach r14_vfs_ramfs_initrd
cmake -B build && cmake --build build --target boot_image
timeout 180 qemu-system-x86_64 -accel tcg \
    -drive format=raw,file=build/boot/cinux.img \
    -display none -no-reboot \
    -serial file:build/console.log \
    -monitor stdio
```

TCG 起机要等一阵,console.log 您另开一窗 tail -f 盯着,长到 cinux> 露头就能喂。面板从 pmm 行起抄,五格:

```text
[kern] pmm: 32309 pages free (probe 113000 ok)
[kern] vmm: address space up, magic 114514
[kern] heap: new/delete live (probe 114514)
[kern] irq on, tick 100Hz
[kern] keyboard on, type to echo
```

老面孔全在,新数两处,pmm 32309,比 2.4 那屏的 32336 又小了。2.4 立过的老话还作数,内核长个头,占地就多,余页就少。个头长在哪儿,您那窗全量日志里写着一行,`kernel at 200000 size 976C0`,比 2.4 那屏的 8F090 长出一截,长出来的就是文件系统这一层,从挂载表到 tar 解包,全在镜像里。vmm、heap、irq、keyboard 四行,2.3 认过名,irq 的来路是 1.6 追的,不重叙。再往下六位任务的三场拉练照旧流水,2.4 逐场读的,咱们直接翻到会话。

会话开张之前,咱们还有一行报幕要停半拍,kernel.cpp 里:

```cpp
    Println("[kern] launching shell in ring 3");
    cinux::fs::FileWorld::self().init();
```

launching 之后那行,就是世界起底的一行:FileWorld 单例起用,里头 new RamFs、mount 在根、行李入树,三件事一次办完。从这一拍起,机器有名字了。壳在 ring 3 里开张,头一单咱们就问它要行李里的东西。

```text
cinux> cat readme.txt
welcome to cinux - this file was baked into the kernel image
cinux> echo fs lives here > note
cinux> ls /
note
docs/
readme.txt
```

单一,cat readme.txt。您敲的是裸名字,壳补了道手续,make_path 见您没带斜杠,头一个字符替您补上,递给柜台的是 /readme.txt。内核只认绝对路径,这一版没有当前目录一说,认路全从根起,补斜杠的活归壳。open、read、close 一路走通,那行 welcome 就是行李里 readme.txt 的本尊字节。单二,echo fs lives here > note,壳见着 > 号,拿 open 带 0x40 造出 note,发 3 号,这架机器发出去的头一枚 3 号,把 fs lives here 连换行写进去,close 还号。单三,ls /,壳 open 根目录,getdents 一枚一枚领到 0。念出来的次序有讲究:新孩子进链头,note 最新,排头,readme.txt 进门最早,垫底,docs 夹中间。ls 不排序,树里怎么躺,屏上怎么念。

```text
cinux> cat note
fs lives here
cinux> cat nope
open failed: errno 2
cinux> mkdir docs
mkdir failed: errno 17
cinux> mkdir own
```

单四,cat note,单二写进去的字,咱们从堆里原样读回来,写与读对了个正着。单五,cat nope,lookup 查无此人,kNotFound 过翻译台变 2,壳印 open failed: errno 2。单六,mkdir docs,这名字已经有主,kAlreadyExists 变 17。2.3 说拒得各有名目,名目厚起来就是这个意思:同名拒 17,查无拒 2,各回各的数,客人一看就知道差在哪儿。单七,mkdir own,成了,屏上静悄悄。静默是设计,不是故障,成功的 mkdir 没有一句废话。这会儿 own 已经进了链头,下一单见分晓。

```text
cinux> ls /
own/
note
docs/
readme.txt
cinux> rm note
cinux> ls /
own/
docs/
readme.txt
```

单八,own/ 果然排头。目录名尾巴那道斜杠,是壳看 type 印的,type 1 目录,补个斜杠让您一眼分得清文件与房子。单九,rm note,unlink 拆名,又是静默。单十,note 没了,树里少一门。十单走完,咱们把住处点一遍:readme.txt 与 docs 一家住得稳,起机从镜像搬进树,关机也不怕,行李还在镜像里,下回起机再搬一回。note 就这一世,堆上的字节,机器一关就还给内存。要它活得过重启,得等它搬进一块真的盘,那是第 5 部的戏。十单之外咱们还补发了一发 exit,单字命令,也在被吞之列,屏上只见命令自己回显,接一行新提示符,再无下文。这一发是收尾的探针,顺手给单字被吞留了个实证。timeout 到点,收场。

## 对照:同一件事,别的机器怎么做

xv6 翻到第 10 章。书页 89 起的那段,咱们整段请出来,自下而上一层一层数,底下三层:

> “The xv6 file system implementation is organized in seven layers, shown in Figure 10.1. The disk layer reads and writes blocks on an virtio hard drive. The buffer cache layer caches disk blocks and synchronizes access to them, making sure that only one kernel process at a time can modify the data stored in any particular block. The logging layer allows higher layers to wrap updates to several blocks in a transaction, and ensures that the blocks are updated atomically in the face of crashes (i.e., all of them are updated or none).”

咱们把同一段翻过页,inode 那句在 89 页的末行起了头,到 90 页才写完,顶上四层接着排:

> “The inode layer provides individual files, each represented as an inode with a unique i-number and some blocks holding the file's data. The directory layer implements each directory as a special kind of inode whose content is a sequence of directory entries, each of which contains a file's name and i-number. The pathname layer provides hierarchical path names like /usr/rtm/xv6/fs.c, and resolves them with recursive lookup. The file descriptor layer abstracts many Unix resources (e.g., pipes, devices, files, etc.) using the file system interface, simplifying the lives of application programmers.”

七层数完,回头数自家。Cinux 站着的只有三层:顶上一层 fd 表,中间一层挂载表带 resolve,底下一层 ramfs,唯一住户。顶上四层咱们挨个对过来,pathname 对咱们的挂载表寻路,file descriptor 对咱们的 fd 表,连 0/1/2 的行话都同源。directory 那层,人家说它是装着一串目录条目的特殊 inode,咱们的对应物就长在底下一层,ramfs 的名字树,目录是节点,孩子挂在链上。inode 没对上,咱们的 FsNode 只是凭条,顶不了它的差,只好把它归进缺的那摞。这样缺的凑成四层,disk、buffer cache、logging、inode,咱们一层都没有。名字立于盘前,形状立于介质前。盘以下那一摞,等第 5 部的《文件系统实现》开讲,块设备、崩溃一致性,也都在那一部名下。这一章咱们盖的是上面三层,盘来之前它们已经能整套当差。

人家书里的 inode 一词,咱们轻点一下它的一语两说:盘上一份,是数据结构,记着数据块的号,内存里一份,是活动副本,原文里 `represented as an inode with a unique i-number` 说的就是 inode 这个词。咱们的 FsNode 不是 inode,也不装作它,一枚凭条,ino 一格是后端愿意报的号,opaque 里装的什么,只有铸它的后端认得。ramfs 报的 ino 是自家发的流水号,根领 1 号,后来者挨个往上。同名不同物,咱们读人家书的时候,把这个词在心里译成节点凭条就对了。

fd 表两边的形状拿来对一对也有味,xv6 那头,表也是每进程一张,0、1、2 常开。咱们这边 0/1/2 固定意义、3 号起发、八席预算,按值住进 Task,2.4 交代过的那间屋,如今又添了一口人。Linux 那头,咱们报得出口的只有两样,都是自家注释里的交代:号码随它排,errno 随它抄。它家 VFS 的全本怎么写,咱们一行手册没翻过,不比,留给将来的书。

自家的窄处,咱们挨件交底。名字最长 32 字节,路径最长 96,挂载点 8 席,每任务 fd 8 席,全是教学预算,各有各的旋钮,名字、路径、挂载点三枚都住在 fs_config.hpp,fd 的席数一枚,住在 file_table.hpp。offset 不能拨,没有 lseek,读到哪儿算哪儿。目录条目一回一枚,打包的功课记在 busybox 名下。当前目录没有,认路全从根起。几笔都是记了名的债,到期各有债主。

## 这一章留下了什么

收官之章,咱们把本章的家什点一遍:一张脸,FsOps 九针,concept 编译期验资格,EraseFs 机械擦成,后端不报名字。一张挂载表,前缀最长者赢,边界断在斜杠上,余路不带前导斜杠,空串即根本人。一张 fd 表,0/1/2 固定,3 号起发,最低空闲回收,八席,按值住 Task,一人一张。柜台词典长到 218 格,开张九号,errno 名册翻成 Linux 号码。行李一份,ustar 三件,起机入树,readme.txt 关机不怕,note 只活一世。

开部那会儿,机器只会一口气把话讲完,提示符背后站着什么,咱们两眼一抹黑。后来它分清了主客,门牌一换,执行流下了楼(2.1)。疼了,它把名姓喊出来,一张表管两类来路(2.2)。楼下有了常客,柜台立起号、话、错三条约定(2.3)。任务多了,它们会生、会让、会退,轮着上场(2.4)。今天,字落了地,名字成了家。一部收在这儿,机器从裸机长成了有户口、有机关、有差事的系统,手里还是一块盘都没有。

下一部,咱们进《内存》。地址空间、分页与 TLB、缺页与按需分页,都排在那一部名下,2.3 欠下的低半区试验、2.1 照过面没讲的堆,也一并记在它那儿。字有了住处,可住处本身还借住在堆上,内存自己的正戏,下一部开锣。

| 术语 | 一句话口径 |
|---|---|
| 后端 | 真正存字节记名字的实现:过 concept 九法,擦成 FsOps 进表 |
| 挂载 | 把后端安在路径前缀上:一生擦一次,一条前缀只许一家 |
| FsOps | 脸面九针:函数指针配 self,后端换人不换脸 |
| FsNode | 节点凭条:opaque 谁铸谁认,ino 与 size 是可读的档 |
| fd | 一枚小整数当票根:0/1/2 固定,3 号起发,最低空闲回收 |
| offset | 走的进度:文件数字节,目录数孩子,身份写在 node 的 type |
| initrd | 随身的行李:ustar 档烤进镜像,起机入树 |

## 练一练

咱们留三道题,一道动手,一道推票根,一道读源码。

**题一,动手。** 实况十单您自己整场走一遍,喂完第十单别急着收,再补一单 rm docs。敲下去之前猜回执,敲完对底下的参考答案。

**题二,推票根。** 开同一个文件两次:open /note 拿到 3 号,不关,再 open /note 一回。第二回拿到几?接着经 3 号写 fs lives here 加换行,经 4 号写 ok 加换行,note 里躺着什么?您拿 offset 的差事推一推,再对答案。

**题三,读源码。** 假设内核把一位新后端挂在 /tmp 上,`resolve("/tmpx")` 会交给谁,余路是什么?您拿 vfs.cpp 那两道前缀检查推一推。再追一层:这一版的内核里,挂载表真正住着几家,谁?

### 参考答案

题一:前十单与实况节的屏一致,次序都不差。rm docs 回的不是静默,是 rm failed: errno 39。docs 里还躺着 note.txt,kDirectoryNotEmpty 过翻译台就是 39,带孩子的房子不让拆。您要真想把它拆了,rm docs/note.txt,再 rm docs,两单都静默。

题二:您第二回拿到的是 4 号。3 号还开着,最低空闲轮不到它。写完之后,cat note 会看到两行,头一行 ok,第二行 lives here。3 号那笔从 offset 0 写了 14 个字节,offset 到 14,4 号也从 0 写起,ok 加换行 3 个字节,把头三个字符顶掉,o、k、换行换下了 f、s、空格,后头剩下的原样接上。两张票根两个游标,谁也不等谁,这就是没有 lseek 的世界,重开一回,等于重写一回的开头。

题三:交给挂在根上的 ramfs,余路是 tmpx。/tmp 盖不住 /tmpx,第二道检查看见 /tmp 后头跟着字母 x,前缀边界不成立,/tmp 出局,盖得住 /tmpx 的只剩根。根的余路是去掉头一道斜杠剩下的整条。至于第二问,这一版表里真正住着的只有一家,根上的 ramfs,挂 /tmp 是咱们给这题搭的戏台。边界检查不认字母,认斜杠。

## 延伸

- xv6 书第 10 章(书页 89 到 90):本章整段借的就是这两页。往后翻,盘面分区的图,boot、super、log、inodes、bit map、data,咱们一步没进,logging 那节讲崩溃一致性,也一步没进,它们归第 5 部当面学。您拿着正文里引的原句去核,借得准不准,您说了算。
- 正文《无盘起家:名字、内存里的盘与随身的行李》一卷:本章的九针、挂载表、fd 表与行李,那一卷一步一步搓出来,第一棵树怎么长、锁怎么排,全本都在那边。教科书层把它们收拢成一副读法,您要现场,那一卷的门开着。
