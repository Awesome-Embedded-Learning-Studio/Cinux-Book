---
title: 05 · 文件系统抽象
---

# 05 · 文件系统抽象:一张脸,两张表,一堆名字

`welcome to cinux - this file was baked into the kernel image`。您 cat readme.txt,回您的就这一行。这行字的来历经得起查:关机重启一回,回来的还是它。等会儿您 echo 进 note 的字就没这个待遇,机器一关就没了。所以待遇两样,住处也各有各的:readme.txt 烤在了内核镜像里,note 住在内存的堆上。

咱们把住处这件事再往深问一步。Cinux 的内核手里一块盘都使唤不动:磁盘驱动整个不存在,那块引导盘把内核送进了内存,差事交割完了,此后再没人理它了。所以字只能住内存,名字的立身之处也只能在内存。管名字的那一层:给字节起名字、把名字排成树、按树递字节,行话的名目是文件系统。咱们的做法有一句纲领,写的是名字立于盘前,今天整章作的都是它的注。

上一章收尾咱们记下的话,今天把它接住了,任务会生、会跑、会让、会退了,可它们经手的活还落不下地:write 递出去的字,进了串口就没了影。字得有地方住。这一章把地方造出来,顺带把第 2 部的卷收了。

咱们一件一件办。头一件立的是脸,文件系统后端长什么形状,凭什么将来添一位后端,楼上楼下都没了改动的必要。再一件安的是家,挂载表怎么认门,一条路径进了门,落到当家的后端头上。末一件开的是户,fd 表怎么发号,open、read、write、getdents 怎么一路走通。办完了实况节起机喂十单,对照节请出 xv6 数它的七层。

量法这一章的家什有三样,来路也各有各的。码的来路在自家树里:vfs.hpp、file_table.hpp、file_world.hpp 三个头文件,带一排每个 syscall 号一件的小文件,sys_read.cpp 走的就是这样一号一件,咱们逐件读。屏的来路是自家起机:十单会话咱们一条一条抄,一枚读数都不借别人的。书这边翻到了 xv6 的第 10 章:书页 89、90 两页咱们逐字核过,整段整段地请。Linux 那头咱们只认两笔自家注释写明的来往:号码随它排、errno 随它抄,其余的就不比了。

> **咱们一块盘都没有,名字凭什么立得起来?盘哪天来了,又往哪儿摆?**
>
> 咱们立得起来,因为把文件系统里跟盘无关的那一半做全了。名字、树、路径、读写:全是形状上的事,没有一样是生来要盘的。盘来了,往脸的后头摆。vfs.hpp 的头注里,笔者给脸面写过一句期票,`ramfs today, ext2 one station later, behind one face`,今天住的是 ramfs,ext2 进门的那天,咱们一点不动它的脸面。这一章咱们看的就是它,脸怎么立,名字怎么住进去。

## 一张脸:九个方法,编译期把关

咱们从后端说起。咱们说的后端:行话 backend,是真正存字节、记名字的实现。眼下的住户只有一位 ramfs,一棵长在堆字节里的树:目录是节点,文件也是同样的节点,文件的字节住在节点挂着的缓冲区里,要长就在谁的身上长。它是个普普通通的 C++ 类,您翻 ramfs.hpp,通篇找不着一枚函数指针的影子,擦成表的那一步不归它管,它其实也不知道自己将来会被挂进表里。

可柜台那侧的 syscall 不认得 RamFs 这个类名,也没有认得的道理。今天住的是 ramfs,ext2 哪天进了门,要是 open 的路里写死了 RamFs 的调用,每换一位房客的当口,柜台里的这些调用就得改一遍。所以中间得立一张双方都认的脸:后端按自家的样子实现,柜台按脸的样子调用。立脸分的是两步,咱们一步看一步。

第一步咱们来定资格。C++ 给了个现成家什:行话叫 concept,编译期的资格检查。您声明一个类型当文件系统后端,编译器在编译那一刻把九个方法的原型逐个对一遍,缺了一个、错了一个签名,编译当场不过。vfs.hpp 里它长的是这样:

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

九个方法咱们挨个报名。lookup 管的是按路径找门,找着了回一枚节点门牌。make_file 与 make_dir 管的是造:造文件、造目录。unlink 管的是拆,拆文件与空目录的活都接。read 与 write 管的是按 offset 读写字节,写越过了文件尾,文件就自己长了。read_dir 管的是按序号领孩子:一回领一个。末了 retain 与 release 是一对看护:开户时 retain 把节点看住,关号的时候 release 松手,名字被拆了而还有人开着的节点,靠它们活到了最后一枚 release。九个全是普通的成员函数,RamFs 实现它们的时候,concept 的事它不知道,挂载表的事它也不知道,头注里那句写明白了:资格检查与擦表都住上一层。

第二步咱们来擦。概念验完了,RamFs 还是原来的 RamFs,类型各是各的,表里其实存不下类型。EraseFs 干的就是把类型擦掉。九个不捕获的 lambda,每个转调真身的对应方法,编译器把每个 lambda 各放成了一枚函数指针,真身的地址另走一枚 self 指针,调用的那一刻递过去。头一件转发的样子:

```cpp
template <FileSystem Fs>
[[nodiscard]] constexpr FsOps EraseFs() {
    return {
        [](void* self, std::string_view path) { return static_cast<Fs*>(self)->lookup(path); },
        // (余下八针,同一副擦法)
    };
}
```

擦出来的成品是 FsOps:九枚函数指针,咱们把针脚摆开看:

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

您留意这副两件套的形状:一枚函数指针,配一枚 void* 的 self。指针里其实不存真身,每次调用真身的地址随头一件参数进门,进了门再转回真身干活。这副形状您不是头一回见,2.4 压轴那座调度器存下的就是四个指针加一个地址:同一副骨架。vfs.hpp 的头注自己也记了一笔,`the same pair-shape the tick service uses one floor down`,楼下的 Tick 服务用的正是它。文件系统这层的新东西是排场,头一回成了建制,九枚指针排满了一张脸,后端是千差万别的,进了表一律变成它的样子。整套活没有一行人手写的表,新后端想进门的话:写个类、过 concept,擦法 EraseFs 一处就包办了,头注给它的名号是 zero boilerplate,零份样板的意思。

脸上其实还差一件家什:节点门牌 FsNode。后端替您找到门了,咱们得给柜台一张凭条:

```cpp
struct FsNode {
    void*         opaque;  ///< Backend-private node pointer.
    InodeType     type;    ///< File or directory.
    unsigned long ino;     ///< Backend-assigned inode number.
    unsigned long size;    ///< File bytes; zero for directories.
};
```

四格里最有讲究的是头一格,咱们细看它的名字:opaque,不透明的指针,指向后端自家的节点,谁铸的谁认得,别人拿到了也只能原样递回,ramfs 的节点与将来 ext2 的节点长法各是各,凭条上不写也写不了,void* 里只装一个地址。type 说的是文件还是目录,ino 是后端愿意报的节点号,size 记的是字节数。凭条能给的也就这些了,真身的样子只有后端自己知道。

## 挂载表:前缀最长者赢,余路归它

脸立好了,该安家了。咱们叫它挂载:行话 mount,说的是把一位后端安在一条路径前缀上。内核的家当是 Vfs 的挂载表,章程咱们挨条念:前缀必须斜杠打头,尾斜杠是不许拖的,整个就是根的除外。章程里的另一条:一条前缀只许挂一家。表里的席位统共 8 席。犯了规各有名目,前缀的形状不对,报的是 kInvalidArgument,重挂报的是 kAlreadyExists,表满报的是 kOutOfMemory,这些名目到了柜台会翻成 errno,咱们留到讲 fd 表时再认。

mount 办的事只费一举,擦表就发生在这一举里了,每家后端一生一次的额度。擦好的 FsOps 连同真身指针与前缀一起入席,咱们看入席那两行:

```cpp
    static constexpr FsOps kErased = EraseFs<Fs>();
    Slot                   slot{.ops = &kErased, .self = &backend, .prefix_len = prefix.size()};
```

住进来了,咱们接着看路怎么走,这是 resolve 的活。客人递来的是一条绝对路径:resolve 扫一遍全表,找盖得住它的最长前缀,交还的是三件:胜家的脸、胜家的真身、余路。余路是挂载点之后的部分,不带前导的斜杠,空串说的就是挂载根本人。章程里最要紧的一条:前缀得断在斜杠上,vfs.cpp 的写法:

```cpp
        const std::string_view kPrefix(slot.prefix, slot.prefix_len);
        if (!path.starts_with(kPrefix)) {
            return;
        }
        if (kPrefix.size() > 1 && path.size() > kPrefix.size() && path[kPrefix.size()] != '/') {
            return;
        }
```

咱们拿挂 /tmp 打个比方:它管 /tmp,也管底下的 /tmp/build,偏偏不管 /tmpx。为什么?tmpx 是一整个的名字,不归 /tmp 的房客:前缀匹配认的是边界,字母是不认的。眼下表里的住户只有一家,挂的前缀就是根,咱们一根走到底。可章程是照多家住户立的,多家来的时候它已经站在那儿了。

resolve 交还的东西里,没有住户的名字。您拿到的 ops、self、rest 三件,没有一件能告诉您今天当差的是 ramfs:脸是 FsOps,真身走的是 void*。头注里其实有一句原文,咱们抄给您看:`Consumers never learn a backend's name`,消费者永远学不到后端的名字。这里头藏着设计里最要紧的一手:名字不出去,换房客就惊动不了楼上的住户。今天住在表里的是 ramfs,哪天 ext2 来了,open 的那几行一个字都不用改。

咱们内核里,挂载表连同它的根住户只此一套,行话 singleton 说的就是全局只此一份:它的名号是 FileWorld,singleton 家族的新成员,排在 Pmm 与 Tick 的后头。它的 init() 办的三件事一件比一件实:new 一个 RamFs,mount 的前缀就是根,然后把随身的行李搬进树里。

行李的行话是 initrd,initial ramdisk 的缩写:实物是一份 tar 档,构建那一步打的包,链接期烤进了内核镜像,成了镜像里一段只读字节。行李这词上一章咱们也用过,2.4 数过的五件行李是任务随身的家当:一件一件住在 Task 里。本章往后的行李,只认镜像里烤着的 tar 档。咱们烤进去三件:readme.txt 一件,docs 一家也带了一件 note.txt。起机这会儿的活归 UstarReader:一户一户地走,见了目录就 make_dir,见了文件就 make_file,再把字节也 write 了进去,名字、类型、个头、内容全从档里来的。ustar 是 tar 家族里老资历的一种格式,一块定的是 512 字节,咱们不逐字段细读,您只消认下一件事:它是行李的打包法,解包的时点只有起机这一次,进了树,行李就再没人去碰了。

**咱们停一停:替期票排一场戏。** 期票上写的是 ext2 要来。它真来的那天,ramfs 为它改几行?挂载表又得添什么手续?您拿着 mount 与 resolve 的章程推一推,再往下对就是了。

咱们排一遍:ramfs 一行不用改。ext2 进门的路跟 ramfs 走的一模一样:写成类、过 concept、擦成脸,要办的手续就是再 mount 一回,前缀随它的便。想让它坐根位的话也不难,动的只是 FileWorld 里接线那几行,脸面与 ramfs 都不必惊动了。尺寸的旋钮也备好了,名字的上限如今是 32 字节,fs_config.hpp 的头注写着,ext2 来的那天要拧的就只有其中一枚。期票之所以敢立,靠的就是前面这些。

## fd 表:3 号起发,空号回收再使

脸立了,家安了,轮到您用。ring 3 想开一个文件的话,递的是路径,内核发还的是一枚小整数:行话叫文件描述符,fd 是它的简写。您别把号当成文件:拿着号来,柜台在一张表里按号把票根找了出来。找票根的表是 FileTable,咱们看票根长什么样,file_table.hpp 里的 FileSlot,报的是四件:

```cpp
struct FileSlot {
    const FsOps*  ops     = nullptr;  ///< The backend's erased face.
    void*         backend = nullptr;  ///< The backend instance.
    FsNode        node{};             ///< Which node of that backend.
    unsigned long offset = 0;         ///< Bytes read so far, or the directory entry ordinal.
};
```

三件是认路的:哪张脸、哪家后端、哪个节点。第四件 offset 是走的进度,最有戏的也归它:一枚数字,扛的是两副差事。开的是文件,它数的是字节:读到哪儿了,写到哪儿了,下次调用接着的位置在哪儿。开的是目录,它数的是孩子:现在数到了第几个,下一回 getdents 领的是哪个。差事不写在这枚数字的身上,写在 node 的 type 里,表存的只是数,后端按节点的身份解读。这一枚数字够咱们停一停细想,题在节末候着您。

头三号是有主的,而且是终身制:0 号键盘进、1 号控制台出、2 号控制台错。表里从来没有它们的席位:read 与 write 在柜台那侧直接特判,fd 0 的去处是键盘,fd 1 与 2 的去处是控制台,压根不进文件系统的门。2.3 记过 write 只认 1 号的窄处,fd 2 只有挨 -9 的份,这一版宽到了 2,当初应下的放宽手续,这就兑现了一笔。所以表发出去的头一枚号是 3,定在了 kFirstDescriptor 里。头注给了一句交代,`the shape every Unix program grew up expecting`,Unix 世界长大的程序,睁眼拿到的头一枚 fd 从来就是 3,咱们照这副形状长,客人搬进来也不用换习惯了。

发号的章程,咱们一句话念完:挑最低空闲、空号回收再使。close 还回来的号,下一回 open 头一个拿去补的就是它。表的封顶 8 席,记在 kFileTableMax 的名下,头注把预算摊开了念,`Three reserved standard streams plus five open files is a teaching kernel's honest budget`,三条保留流加五个开着的文件,教学内核的老实话预算。满了怎么办?alloc 回 kOutOfMemory,柜台翻成的是 errno 12,老老实实地拒,不偷偷挤掉任何人的号。

表的住处,2.4 交代过任务的家当,如今 task.hpp 里又添了一件:files,一张 FileTable 按值住在了 Task 身上,头注的原话是 `Lives inside the Task by value`。咱们在 2.3 立过一条老话:帧跟着任务走,CPU 是不跟的,fd 表走的是同一条理,一人一张的待遇,您开您的 3 号,隔壁任务开它的,谁也看不见谁的。任务退场的那天,表跟着一起析构了,close_all 把还开着的号一枚一枚关掉,release 每关一枚就松了一道引用,散场散得干干净净的。

柜台这次也翻了门面,咱们指认一回,课就不重开了。咱们在 2.3 立柜台时数过:词典 61 格,开张的是四号。这一版的词典长到了 218 格:号排到了 217,记数的名号是 kSyscallCount,开张的九号是 read 0、write 1、open 2、close 3、yield 24、exit 60、mkdir 83、unlink 87、getdents 217,新开张的五号全在文件系统当差。三条约定咱们一条没动:号还是随 Linux 报,话走的还是 rdi、rsi、rdx,错回的还是负 errno。

errno 的名册厚了。咱们手里还是 2.3 备的那三枚:-38 空号、-9 错门、-14 越界。这一版后端进来了,后端说自家的话,咱们把 KernelError 逐个认一遍:查无此人 kNotFound、名字已有人用 kAlreadyExists、路径中段站着个文件 kNotADirectory、拿目录当文件使 kIsADirectory、目录还有孩子 kDirectoryNotEmpty,五枚报齐了。柜台那侧 sys_common.cpp 里立了一座翻译台 ErrnoFrom,把它们挨个翻成客人的话:2、17、20、21、39,兜不住的一律 22。挂载表立章程时报过的 kInvalidArgument,到这儿也有了下落:它没排上专格,落的就是兜底的 22,而 22 在 Linux 里的本名正是 EINVAL,说的是非法参数,所以兜得不冤。号码还是照 Linux 抄的,syscall.hpp 的注释写明 Linux errno values,跟号码词典走的是同一个道理,将来 musl 的程序来敲门,报错就能对上号了。拒门 -9 也添了新差事。getdents 拿 0、1、2 来问目录的话,回的还是它:门不对,跟文件不文件的无关。

咱们把 open 的路整条走一遍,九号里最见工夫的就是它。头一步走的是 CopyUserPath,把客人递的路径搬进内核,越界的回 -14,空串或超长的回 -22,这是 2.3 验指针那道岗的翻版。第二步 resolve 找的是脸。第三步 lookup 拿余路找的是门。查无此人怎么办,看旗:带了 kOpenCreat,值定的是 0x40,跟 Linux 的 O_CREAT 同一个数,咱们就 make_file 现造一个,没带的话,回的就是一声错。末一步的活是 alloc 发号,retain 把节点看住了,3 号也就递了出去。您 cat 一声 nope,走到 lookup 就折了,kNotFound 翻成的是 2:实况节屏上见。

目录怎么读,独挑一号的是 getdents,咱们把核心请出来:

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

这是它跟 Linux 的分岔:一回一枚。人家一次调用打包的是一整块缓冲区,咱们一回发一件,头注自己交代了分期,号码与语义还是照旧的,打包的功课等 busybox 需要那天再补。read_dir 照序号领它的孩子,领到 false 就是到头了,回的是 0。领到了一枚,offset 进了一步,名字与类型写进客人递的缓冲区,type 1 说的是目录,0 说的是文件。壳的 ls 就是它养的:open 一回、getdents 数到 0、close 收工,三步就把一趟走完了。

**咱们停一停:给一枚数字派两回差。** 同一个目录 open 了两回,拿回的是两张票根。头一张数到了第 2 个孩子,第二张现在数到了第几?再深一层:同一张表里,3 号开的是文件,4 号开的是目录,一枚字段管的是两种单位,凭什么不打架?您推一推,再往下对就是了。

咱们对:两张票根、两个 offset,各数各的。第二张还是从 0 起数的,头一张数到了第 2 个孩子,跟它是不相干的:一人一张表、一个槽一个数。单位也是不打架的,offset 不解释自己的单位,node 的 type 才解释。3 号槽里的节点是文件,它的 offset 是字节,4 号槽里住的是目录,同一枚字段当的就是序号。表存的只是数,后端读数凭的是身份,职责切开了,数字也就不串门了。

## Code: Cinux 实况:起机、十单、随身的行李

咱们这回的实况锚是 r14_vfs_ramfs_initrd,文件系统立起来的那一版。切版的老话照旧:git switch --detach 切过去,行号认的就是这一版,看完 git switch - 回 main 就行了。镜像的老话也照旧:改完代码的话 boot_image 得显式重打。喂键的法门 2.3 交过底:monitor 接 stdio、字母原样、空格 sendkey spc、回车 sendkey ret,十单照此喂下去就是了。这一版的壳还有个新脾气,咱们一句话交代:单字命令它一声不吭地吞掉,所以咱们每一单都带参,收场交给的是 timeout。

```bash
git switch --detach r14_vfs_ramfs_initrd
cmake -B build && cmake --build build --target boot_image
timeout 180 qemu-system-x86_64 -accel tcg \
    -drive format=raw,file=build/boot/cinux.img \
    -display none -no-reboot \
    -serial file:build/console.log \
    -monitor stdio
```

TCG 起机要等的那一阵,console.log 您另开一窗 tail -f 盯着,长到 cinux> 露了头就能喂。咱们从 pmm 行抄起,抄的是五格:

```text
[kern] pmm: 32309 pages free (probe 113000 ok)
[kern] vmm: address space up, magic 114514
[kern] heap: new/delete live (probe 114514)
[kern] irq on, tick 100Hz
[kern] keyboard on, type to echo
```

老面孔是全在的,新的数有两处,pmm 报的是 32309,比 2.4 那屏的 32336 又小了。2.4 立过的老话还作数,内核长了个头,占地就多,余页也就少了。个头长在了哪儿,您那窗全量日志里写着一行,`kernel at 200000 size 976C0`,比 2.4 那屏的 8F090 多出一截,多出来的就是文件系统这一层:从挂载表到 tar 解包,全部落在了镜像里。vmm、heap、irq、keyboard 四行咱们在 2.3 认过名,irq 的来路是 1.6 追的,就不重叙了。再往下六位任务的三场拉练照旧流水,2.4 逐场读的,咱们直接翻到会话。

会话开张之前咱们还有一行报幕要停半拍,写在了 kernel.cpp 里:

```cpp
    Println("[kern] launching shell in ring 3");
    cinux::fs::FileWorld::self().init();
```

launching 之后的那行,就是世界起底的一行:FileWorld 单例起用,里头 new RamFs、mount 到了根、行李入了树,三件事一次就办完了。从这一拍起机器就有了名字。壳在 ring 3 里开了张,头一单咱们就问它要行李里的东西。

```text
cinux> cat readme.txt
welcome to cinux - this file was baked into the kernel image
cinux> echo fs lives here > note
cinux> ls /
note
docs/
readme.txt
```

单一:您敲的是 cat readme.txt,名字是裸的,壳补了道手续,make_path 见您没带斜杠,头一个字符替您补上,递给柜台的是 /readme.txt。内核认的只是绝对路径,这一版还没有当前目录的说法:认路全从根起,补斜杠的活归壳。open、read、close 一路走通了,那行 welcome 就是行李里 readme.txt 的本尊字节。单二:您敲了 echo fs lives here > note,壳认出了 > 号,拿 open 带 0x40 造出了 note,发的是 3 号,这架机器发出去的头一枚 3 号,把 fs lives here 连换行写了进去,close 还了号。单三:咱们看 ls /,壳 open 的是根目录,getdents 一枚一枚地领到 0。念出来的次序有讲究:新孩子进链头,note 是最新的,占了排头,readme.txt 是进门最早的,垫了底,docs 稳稳地待在中间。ls 是不排序的,树里怎么躺,屏上怎么念。

```text
cinux> cat note
fs lives here
cinux> cat nope
open failed: errno 2
cinux> mkdir docs
mkdir failed: errno 17
cinux> mkdir own
```

单四念的是 cat note,单二写进去的字,咱们从堆里原样读回来,写与读对了个正着。单五问的是 cat nope:lookup 查无此人,kNotFound 过翻译台变的是 2,壳印的就是 open failed: errno 2。单六试的是 mkdir docs,这名字已经有主了,kAlreadyExists 变的是 17。2.3 说的就是拒得各有名目,名目也厚了起来,说的就是这个意思:同名拒 17,查无拒的是 2,各回各的数,客人看了一眼,差在哪儿就知道了。单七敲的是 mkdir own,成了,屏上静悄悄的。静默是设计好的,不是出了故障,成功的 mkdir 没有一句废话。这会儿 own 已经进了链头,等下一单就见分晓了。

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

单八:own/ 坐的是排头。目录名尾巴的那道斜杠,是壳看 type 印的,type 1 报的是目录,补个斜杠让您一眼分得清文件与房子。单九来的是 rm note:unlink 拆的是名,又静默了一回。单十:note 没了,树里少了一门。十单走完了,咱们把住处点一遍:readme.txt 与 docs 一家住得稳,起机时从镜像搬进了树,关了机也不怕,行李的原件还在镜像里,下回起机再搬一回就是了。note 活的就是这一世,堆上的字节,机器一关就还给了内存。要它活得过重启的话,得等它搬进一块真的盘,那是第 5 部的戏。十单之外咱们还补发了一发 exit,单字的命令,也在被吞的名单里,屏上见到的只有命令自己的回显,接的是一行新提示符,再没有下文了。这一发是收尾的探针,顺手给单字被吞留了个实证。timeout 到了点,收了场。

## 对照:同一件事,别的机器怎么做

xv6 翻到的是第 10 章。书页 89 起的那段,咱们整段请出来,自下而上一层一层数,底下数的是三层。原话咱们单摆一行:

- “The xv6 file system implementation is organized in seven layers, shown in Figure 10.1. The disk layer reads and writes blocks on an virtio hard drive. The buffer cache layer caches disk blocks and synchronizes access to them, making sure that only one kernel process at a time can modify the data stored in any particular block. The logging layer allows higher layers to wrap updates to several blocks in a transaction, and ensures that the blocks are updated atomically in the face of crashes (i.e., all of them are updated or none).”

咱们从底下三层认起。同一段咱们翻过页,inode 那句在 89 页的末行起了头,写到 90 页才收的尾,顶上的四层接着排。原话咱们也单摆一行:

- “The inode layer provides individual files, each represented as an inode with a unique i-number and some blocks holding the file's data. The directory layer implements each directory as a special kind of inode whose content is a sequence of directory entries, each of which contains a file's name and i-number. The pathname layer provides hierarchical path names like /usr/rtm/xv6/fs.c, and resolves them with recursive lookup. The file descriptor layer abstracts many Unix resources (e.g., pipes, devices, files, etc.) using the file system interface, simplifying the lives of application programmers.”

咱们接着认。

七层数完了,回头该数自家了。Cinux 站着的只有三层:顶上一层 fd 表,中间的一层是挂载表带 resolve,底下的一层是 ramfs,唯一的住户。顶上四层咱们挨个对过来,pathname 对咱们的挂载表寻路,file descriptor 对咱们的 fd 表,连 0/1/2 的行话都同源。directory 那层咱们也有对应:人家说它是装着一串目录条目的特殊 inode,咱们的对应物就长在底下一层,ramfs 的名字树,目录当的是节点,孩子挂在了链上。没对上的是 inode:咱们的 FsNode 只是凭条,顶不了它的差,只好把它归进缺的那摞。这样缺的凑成四层:disk、buffer cache、logging、inode,咱们一层都没有。立于盘前的是名字,立于介质前的是形状。盘以下的那一摞,等第 5 部的《文件系统实现》开讲,块设备、崩溃一致性的功课,也都记在那一部的名下。这一章咱们盖的是上面三层,盘来之前它们已经能整套当差了。

人家书里的 inode 一词,咱们轻点一下它的一语两说:盘上一份,当的是数据结构,记着数据块的号,内存里的一份,当的是活动副本,原文里 `represented as an inode with a unique i-number` 说的就是 inode 这个词。咱们的 FsNode 不是 inode:也不装作它,其实是手里的一枚凭条,ino 一格是后端愿意报的号,opaque 里装的什么,只有铸它的后端认得。ramfs 报的 ino 是自家发的流水号,根领的是 1 号,后来者领的号挨个往上。同名不同物的话,咱们读人家书的时候,把这个词在心里译成节点凭条就对了。

fd 表两边的形状拿来对一对也有味,xv6 那头的表也是每进程一张:0、1、2 常开。咱们这边 0/1/2 固定意义、3 号起发、八席预算,按值住进了 Task,2.4 交代过的那间屋,如今又添了一口人。Linux 那头咱们报得出口的只有两样,都是自家注释里的交代:号码随它排,errno 也是随它抄的。它家 VFS 的全本怎么写,咱们一行手册没翻过,就不比了,留给将来的书。

自家的窄处,咱们挨件交底:名字最长 32 字节、路径最长 96、挂载点 8 席、每任务 fd 8 席,全是教学预算的安排,各有各的旋钮,名字、路径、挂载点三枚的住处都在 fs_config.hpp,fd 的席数一枚,住在了 file_table.hpp。offset 是不能拨的,lseek 是没有的,读法说的是读到哪儿算哪儿。目录条目发的还是一回一枚,打包的功课记在 busybox 名下。当前目录是没有的:认路全从根起。几笔都是记了名的债,到期各有各的债主。

## 这一章留下了什么

收官的这一章,咱们把家什点一遍:一张脸 FsOps 九针,concept 在编译期验的是资格,EraseFs 管的是机械擦成,后端的名字是不报的。挂载表立下的章程:前缀最长者赢,边界断的是斜杠、余路不带前导斜杠,空串说的就是根本人。fd 表的家底:0/1/2 是固定的三席,要发号的话从 3 号起,挑的是最低空闲,封顶的是八席,按值住进了 Task,一人一张的待遇。柜台词典长到了 218 格,开张的是九号,errno 名册翻成的是 Linux 号码。行李带的是一份:ustar 三件、起机入树,readme.txt 是关机不怕的,note 活的只有一世。

开部的那会儿,机器一口气就把话讲完了,提示符背后站着什么,咱们两眼一抹黑。后来它分清了主客,换了一副门牌,执行流下了楼(2.1)。疼了,它把名姓喊了出来,一张表管的是两类来路(2.2)。楼下有了常客,柜台立起了号、话、错三条约定(2.3)。任务多了,它们也就会生、会让、会退了,轮着上了场(2.4)。到了今天,字落了地,名字成了家。一部就收在这儿了,机器从裸机变成了有户口、有机关、有差事的系统,手里的盘还是一块都没有。

下一部咱们进《内存》。地址空间、分页与 TLB、缺页与按需分页,都排在那一部的名下,2.3 欠下的低半区试验、2.1 照过面没讲的堆,也一并记在了它那儿。字有了住处:可住处本身还借住在堆上,下一部开锣唱的,就是内存自己的正戏。

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

咱们留三道题给您:一道动手的,一道推票根的,一道读源码的。

**题一:咱们动手。** 实况十单您自己整场走一遍,第十单喂完了别急着收:再补一单 rm docs。敲下去之前把回执猜好了,敲完对底下的参考答案。

**题二:咱们推票根。** 同一个文件 open 了两回:open /note 拿到 3 号、不关、再 open /note 一回,第二回拿到几?接着经 3 号写 fs lives here 加换行,经 4 号写的是 ok 加换行,note 里躺着什么?您拿 offset 的差事推一推,再对一遍答案就是了。

**题三:咱们读源码。** 假设一位新后端挂在 /tmp 上的话:`resolve("/tmpx")` 会交给谁,余路是什么?您拿 vfs.cpp 那两道前缀检查推一推。再追一层:这一版的内核里,挂载表真正住着的是几家,谁?

### 参考答案

题一:前十单与实况节的屏一致,次序是都不差的。rm docs 这回有了回音:rm failed: errno 39。docs 里躺着的正是 note.txt,kDirectoryNotEmpty 过翻译台变的就是 39,带孩子的房子不让拆。您要真想把它拆了:rm docs/note.txt、再 rm docs,两单回的都是静默。

题二:您第二回拿到的是 4 号。还开着的是 3 号,所以最低空闲轮不到它。写完了之后,cat note 看到的是两行,头一行的字是 ok,第二行写的是 lives here。3 号那笔从 offset 0 写了 14 个字节,offset 走到了 14,4 号起笔的也是 0,ok 加换行占的是 3 个字节,把头三个字符顶掉了,o、k、换行换下了 f、s、空格,后头剩下的原样接上。两张票根配的是两个游标,谁也不等谁、各走各的,这就是没有 lseek 的世界,重开一回的话,等于重写一回的开头。

题三:交给挂在根上的 ramfs,余路给的是 tmpx。盖不住 /tmpx 的是 /tmp:第二道检查看见 /tmp 后头跟着字母 x,前缀的边界不成立,/tmp 出了局,盖得住 /tmpx 的只剩根。根的余路是去掉头一道斜杠剩下的整条。至于第二问的答案,这一版表里真正住着的只有一家,根上的 ramfs,挂 /tmp 是咱们给这题搭的戏台。边界检查其实不认字母,认的是斜杠。

## 延伸

- xv6 书第 10 章(书页 89 到 90):本章整段借的就是这两页。往后翻,盘面分区的图,boot、super、log、inodes、bit map、data,咱们一步没进,logging 那节讲崩溃一致性,也一步没进,它们归第 5 部当面学。您拿着正文里引的原句去核,借得准不准,您说了算。
- 正文《无盘起家:名字、内存里的盘与随身的行李》一卷:本章的九针、挂载表、fd 表与行李,那一卷一步一步搓出来,第一棵树怎么长、锁怎么排,全本都在那边。教科书层把它们收拢成一副读法,您要现场,那一卷的门开着。
