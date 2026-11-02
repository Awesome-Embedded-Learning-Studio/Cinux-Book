---
title: 01 · 文件与目录:折好的路,认名字的树,还清的债单
---

# 01 · 文件与目录:折好的路,认名字的树,还清的债单

第 5 部开了门,咱们办的头一件事是对债单。2.5 把文件系统的脸立了起来,收卷时自家交了底:名字的上限是 32 字节,路径的上限是 96,挂载点备了 8 席,每任务的 fd 也备了 8 席,offset 拨不了、lseek 也没有,目录条目一回一枚地数,任务没有随身的当前目录。原文收尾一句咱们请回来:“几笔都是记了名的债,到期各有债主。”这些债的债主就是这一部,今天走的是头一站。这一章走名字世界的内里:路怎么折,名字怎么住进树里,号与户口怎么立,咱们顺手把债单挨笔对清。

这一章的活,咱们数下来三样。头一样是折路:客人递来一条可能带着点与点点的路,内核怎么把它折成一条从根起、没有回头箭头的绝对路。再一样是名字树与门面:2.5 从外面看过的那棵树,内里凭什么认名、认错了会出什么冤案,树前头的挂载门面按什么章程派活。末一样是座位与户口:fd 表从八席长到了什么形状,一个开着的文件在内核里立了哪本档。三样之外还有两件小的,getdents 的打包手艺与 stat 的一桩错位案今天压轴。

量法还是老的三条。码的事,实况锚是 r34_compile_self 的克隆,行号一处一处地按它数。屏的事:本章不跑新机,新起的屏一面也没有,挂载表的屏面按代码推演,咱们如实标注,压轴案里的那行断点赃物,是从已刊正文卷请回来的旧屏,标明了出处。书的事:xv6 还是第 10 章,新翻到的是书页 99,目录的那一层,咱们整段请。

> **2.5 收卷的那会儿,头三号是终身制的座位:0 号进的是键盘,1 号出的是控制台,2 号报的是控制台的错,表从来不发它们的号,柜台那侧走的是直接特判,发出去的头一枚号是 3。今天您睁眼拿到的头一枚真文件号,多半还是当年的 3。可特判没了,那枚写死的常量也下岗了。您想:形状没变,凭什么?**
>
> 凭座位是真座位了。起机的那会儿,壳的表被安进三枚真开着的座,座上坐着三个真开着的文件,发号的章程是挑最低空闲、从 0 找起,三座都有了主,空出来的头一号自然落在 3。当年柜台把 3 写死了,是预算紧时的直通车,如今没人写死它了,3 是算出来的。头注收尾的一句,`the shape every Unix program grew up with, arrived at honestly`,Unix 程序从小认的这副形状,这回是诚实地挣来的。整件事的码在座位与户口一节,咱们到那儿见分晓。

## 路名的念法与折法

一条路进了门,头一件要办的事是切,找名字的活排在后头。PathCursor 的家当只有一格,装的余路叫 rest,NextComponent 一勺一勺地舀:头上的斜杠吃干净,吃尽了说明路走完,回来的是 false,没尽咱们就找下一道斜杠切一刀,切下来的一段就是名字,rest 的位置往前挪。咱们把这把刀抄来,path.hpp 的 :58:

```cpp
[[nodiscard]] inline bool NextComponent(PathCursor& walk, std::string_view& component) {
    while (!walk.rest.empty() && walk.rest.front() == '/') {
        walk.rest.remove_prefix(1);
    }
    if (walk.rest.empty()) {
        return false;
    }
    const unsigned long kCut = walk.rest.find('/');
    component                = walk.rest.substr(0, kCut);
    walk.rest.remove_prefix(kCut > walk.rest.size() ? walk.rest.size() : kCut);
    return true;
}
```

您留意这副念法里没有点与点点的戏份。切出来的 . 就是名叫点的普通一段,特殊含义留给折的人。折的人是 Fold,path.cpp 里一个只活在折路函数里的结构,四格家当:出口的缓冲 out、管容量的 capacity、记已写字节的 used,还有摞了几层的 depth。缓冲其实就是栈,path.hpp 的头注点破了这层身份,`The buffer doubles as the component stack`:写进去的每一层名字,同时就是栈里的一帧,另开存储的事就免了。咱们把折的主循环抄来,path.cpp 的 :55:

```cpp
    bool walk(std::string_view view) {
        PathCursor       cursor{view};
        std::string_view part;
        while (NextComponent(cursor, part)) {
            if (part == ".") {
                continue;
            }
            if (part == "..") {
                pop();
                continue;
            }
            if (!push(part)) {
                return false;
            }
        }
        return true;
    }
```

pop 管的是弹层:从尾往回找,把最后一层名字连同它头上的斜杠一起收掉,depth 见了零就收手,根之上就不再弹了,弹层是有底的。walk 是折的主循环:点白吃一回,路是不动的,见了点点弹一层,正经的名字压一层,压层走的是 push,咱们补一道斜杠把名字接在尾巴上。您拿 a/b/../c 走一遍,b 压了进去又弹了出来,折完剩下的是 /a/c。咱们管这一趟活叫折,折路:进去的路带着回头的箭头,交出去的路一步不绕。这词打这儿起就当术语使了。

折路是要有个基座的。绝对路的头上一根斜杠,基座坐的就是根,相对路没有开头的斜杠,基座是任务的当前目录,行话的名字是 cwd,current working directory 的缩写,任务随身的一格家底,住在 task.hpp 的 :131,`char cwd[cinux::fs::kFsPathMax] = {'/'};`,起机给的是一根斜杠,谁生下来都落在了根上。您叫 chdir 搬家,内核把新路折好了、验明是目录,再把字节抄进 cwd 的格子里,getcwd 是反着要的,您给的缓冲装不下整条 cwd,回的就是 ERANGE,一句话的事,折扣是谈不上的。相对性收在进门的那一步:认路从来不必一路问“我现在在哪”,出发前把家问清楚了,路就定死了。尺度您也分清:客人递来的原始路,按 4096 的尺子收,fs_config.hpp 里的 kUserPathMax,头注写明是 Linux 的 PATH_MAX,折完的路按 256 验,折的这一步是归一化,归一化之前的路可以比之后长得多,所以宽收、折、按窄的验,三步走成的是一条走廊。open、chdir、stat、unlink 这些过系统调用的路,条条走的都是它,咱们把走廊抄来,sys_common.cpp 的 :88:

```cpp
long long FoldUserPath(unsigned long long source, char* out, unsigned long cap) {
    const base::Own<char[]> kRawPath(new char[fs::kUserPathMax]);
    if (!kRawPath.holds()) {
        return -kEnomem;
    }
    const long long kRaw = CopyUserPath(source, kRawPath.get(), fs::kUserPathMax);
    if (kRaw < 0) {
        return kRaw;
    }
    const char* const  kBase = kRawPath[0] == '/' ? "/" : CurrentTask()->cwd;
    const base::Folded kFold = base::FoldOntoPath(kBase, kRawPath.get(), out, cap);
    if (!kFold.ok) {
        return -kEnametoolong;
    }
    return static_cast<long long>(kFold.length);
}
```

96 到 256 的这一步,头注里记着的一段真事,咱们念给您听。当年的路径上限是 96 字节,尺子量的是折之前的原始路,后来内核里跑起真的编译器,它自报家门要打开的文件,名字里要翻四层的父目录,不折不拼开就过了百字节,好路在折路之前就被拒成了 EINVAL。尺子拧到了 256,头注把来路原样记了下来,`a teaching limit of 96 denied as EINVAL before normalization could shrink it`,96 在归一化来得及缩小之前就拒了路。名字上限同一场变动里从 32 拧到 256:根上从只活内存的假盘换成真盘之后,盘上的真名字本来就长,32 装不下真盘的世界。挂载点的 8 席没动,那笔债的债主还没到站。

**咱们停一停,您来折。** /a/b/../c 折完是什么?再折 a/../..,基座坐的是根,又是什么?第二问里 depth 弹到零之后再弹,会发生什么?您推一推,题的答案在节末。

## 一棵只认名字的树

2.5 是从外面看这棵树的:resolve 认门,进门的活归它。今天咱们进内里。这棵树其实不认盘、不认块,认的只有名字,骨架是 dir_tree.hpp 里的一个模板 DirNode,五格的家当:名字、类型、门牌号 ino、头孩子指针、弟妹指针。谁想当树里的节点,就把自己的结构从它派生出来,ramfs 的 RamNode 就这么长的,模板使的那一手是 CRTP,白话版的讲法只有一句:父亲只管串链子的活,孩子长什么样父亲不关心,孩子自己报就是了。

认人凭的是名字,咱们把三道关卡抄来,dir_tree.hpp 的 :62:

```cpp
    [[nodiscard]] bool named(std::string_view wanted) const {
        return wanted.size() < kFsNameMax && base::EqualBytes(name, wanted.data(), wanted.size()) &&
               name[wanted.size()] == '\0';
    }
```

头一道比的是长,名字的数组定长,要找的名字必须更短,第二道把字节挨个地比,第三道比的是一枚收尾的空字节。三道是一道也缺不得的,单比字节会出冤案:您找 note,树里住了个 note.txtx,note 的四个字节在人家头四个字节里全对得上,少了第三道,note 就把别人认作了自家人。这一案是正文《第一棵树》一篇里真发过的,冤是有头的,债是有主的,第三道就是那笔债的清法。住进来的手续是 adopt,走的是头插,拢共两行:新孩子的弟妹指针指向早前的头孩子,头孩子换成新来的。头插是一条道走到黑的,读出来就是建的倒序,谁最后进的链谁就当排头,想读正序的客人,排的活自己领,树是不替次序操心的。搬走靠的是 disown,方向是反着的:从链上把一个孩子摘下来,条件不齐是摘不动的,回的是空。unlink 落在它的身上:摘链,若发现是还带着孩子的目录,adopt 挂了回去,再回的是 kDirectoryNotEmpty,带孩子的房子不让拆,旧话是这么说的,码还是从前的那句。

树对外的公开走法是 WalkPath,咱们整件抄来,dir_tree.hpp 的 :157:

```cpp
template <typename Node>
[[nodiscard]] base::Result<Node*> WalkPath(Node& root, std::string_view path) {
    Node*            current = &root;
    base::PathCursor walk{path};
    std::string_view component;
    while (base::NextComponent(walk, component)) {
        if (current->type != InodeType::kDirectory) {
            return base::KernelError::kNotADirectory;
        }
        current = current->find(component);
        if (current == nullptr) {
            return base::KernelError::kNotFound;
        }
    }
    return current;
}
```

您看它用的正是头一节那把刀,切的活不分家。两样错的次序有讲究:每进一段之前,咱们验一下脚下是不是目录,验出来的不是目录,回的就是 kNotADirectory,认名的活排在后头,认不到回的是 kNotFound。您拿一条三段的路走,第二段是文件、第三段是空的,错在第三段进门之前就报定了,不会认了半截路再回头。xv6 的目录层咱们请书证,书页请的还是 99:

- `A directory is implemented internally much like a file. Its inode has type T_DIR and its data is a sequence of directory entries. Each entry is a struct dirent (4165), which contains a name and an inode number. The name is at most DIRSIZ (14) characters; if shorter, it is terminated by a NULL (0) byte. Directory entries with inode number zero are free.`

书里说的样子是:名字至多 14 字节,短了拿空字节填,条目空下了,门牌号记的是零。咱们的名字 256 字节,条目住的是链,不排在字节流的队伍里,真盘那头的目录条目是另一副打包法,咱们到 5.2 见。

## 门面与挂载表

2.5 立脸的那会儿,FsOps 名下排的是九枚指针。今天咱们再数,数出的是十五。新添的是六枚:stat 问的是身家,readable 问的是可读,ioctl 接终端的话,adopt_open 管的是开门时换立节点,append 与 truncate 管写的新姿势。进门的门槛 concept FileSystem 从九法长到十三法,过不了编译这一关,后端进不了家门。擦法 EraseFs 把十五枚一枚不落地生出来:后端有的姿势转手递过去,没有的,擦法替它回的是一句不支持。Vfs 自己也长了肉:自家一把锁,挂载表的每一条添了 type_name 一格报家门,卸载也签了字。2.5 问框里请过 vfs.hpp 头注的期票,`ramfs today, ext2 one station later, behind one face`,今天根上坐的正是 ext2,票兑了,脸也长了,长的是针数,待客的章程没换,脸还是原来的同一张。

挂载表还是原来的 8 席,走法 2.5 把章程立齐了:前缀要真前缀,/dev 不能认 /dev2 的路,边界要断的地方在斜杠上,盖得住它的前缀里,赢的是最长的那个。今天咱们把裁判那几行抄来,vfs.cpp 的 :43:

```cpp
        if (!path.starts_with(kPrefix)) {
            return;
        }
        if (kPrefix.size() > 1 && path.size() > kPrefix.size() && path[kPrefix.size()] != '/') {
            return;
        }
        if (best == nullptr || slot.prefix_len > best->prefix_len) {
            best = &slot;
        }
```

您递 /dev/console,根席与 /dev 席的前缀都咬得住,长的那席赢,余路 console 交给的是它。交回来的 FsView 从三件长成四件:脸、真身、余路,加一张 lifetime 的 lease。第四件在这一章里报的只是名字,它的正戏在挂真根与卸载,到了 5.2 再演。

座次实况一句话:起机时根上挂的是真盘,盘上的文件系统行话 ext2,内里是 5.2 的一整章,/dev 挂的是 devtmpfs,设备节点安身的家,内里的正戏归 5.7,/proc 挂的是 procfs,挂载表里报的家门写作 proc,内里走的也是 5.7,各占一席之地,tmpfs 也进了家门,一块 ramfs 转世的临时地,可挂可卸的门道归 5.2,咱们一句带过。挂载表有一面公开的镜子,procfs 里的 /proc/mounts,家什是 Vfs::mounts_text 的手笔:落了锁才动笔,挨席挨个地走,每席交出的是一行,喂的三样参数是 none、前缀、类型名,再配上固定的尾巴,格式串写的是 `"%s %s %s rw 0 0\n"`。咱们不跑新机,镜子里的样子按码推演,标注照实落了笔,席序按挂的早晚:

```text
none / ext2 rw 0 0
none /dev devtmpfs rw 0 0
none /proc proc rw 0 0
```

头一列 none 是老实话:今天的码给每一席都写 none,设备名的这一列还没接线,真盘怎么挂上根,到了 5.2 见。第三列是 type_name 报的家门。客人世界里谁要问起挂载的事,您看到的就是 mounts_text 照出来的样子。

## 座位、户口与那桩悬案

fd 表的旧样子,是 2.5 那年的八席。今天咱们看封顶,file_table.hpp 的 :33:

```cpp
/** @brief Linux's default nr_open ceiling; storage grows only when used. */
inline constexpr unsigned long kFileTableMax = 1024UL * 1024;
```

一百零四万八千五百七十六席的家底,是 1024 乘 1024 的得数,头注认的是 Linux 默认的 nr_open 上限,格子跟着用到的号往上长,一百万席的天花板就压在那儿,房租是一分不预付的。发号的章程没变,走的还是挑最低空闲:alloc 一进门问的就是 `vacant_locked(0, …)`,用的是从 0 号起的找法。alloc 的注里还写着“发号从 3 起”,那是句过期的话,咱们以代码为准。表本身也换了住法:堆上的一张,任务生下来就带了一张,fork 出的孩子另立新表,继承的开档一张一张挪进去,线程组里共用表的一伙人指着同一张、同一副游标。这些认亲的章程全套在 5.6,这里咱们只把舞台搭好。

找是从 0 找起的,可您睁眼头一枚真文件号还是 3,问框里押着的那桩悬案,答案的码在这儿。起机的那会儿,开机头一个立起来的任务 init,替壳安顿好了表,file_world.cpp 的 :194:

```cpp
    kStream->status_flags = kFsReadWriteStatus;
    for (unsigned long seat = 0; seat < 3; ++seat) {
        const base::Result<unsigned long> kSeated = table.adopt_at(kStream, seat);
        if (!kSeated.ok()) {
            return kSeated.error();
        }
    }
```

/dev/console 被打开了,立了一份开档,读写位拧到了 kFsReadWriteStatus,0、1、2 三座 adopt_at 的是同一份:三座三张票,合的是一份档。表头从此没有了空位,壳的头一个 open 落在 3。2.5 的老话咱们原样记得:头三号终身制,表是从来不发的,柜台办的是特判。那是当年的真话,如今换成了另一副:特判撤了,常量下岗了,0/1/2 成了三枚真座位,不是什么保留号,座上坐的是真开档,键盘进、控制台出的活计,搬到了 /dev/console 门后,开讲的正戏在 5.5。当年 2.3 只认 1 号的窄处,2.5 宽到了 2 号,到了今天,特判整个撤了。

开档落地那一刻,内核立的档就是 OpenFile。POSIX 是管 Unix 接口口径的那套标准,它给这类东西起的正名,叫的是 open file description,中文的说法是开档描述,咱们顺着叫它户口:fd 是座位号,户口是坐在座上的人,您要换座、拼座、退座,动的都是号,户口是不动的。咱们把它抄来,open_file.hpp 的 :45:

```cpp
    OpenFile(const FsOps* face, void* self, FsNode handle)
        : ops(face), backend(self), node(handle) {}

    const FsOps*             ops     = nullptr;  ///< The backend's erased face.
    void*                    backend = nullptr;  ///< The backend instance.
    FsNode                   node{};             ///< Which node of that backend.
    unsigned long            offset = 0;  ///< Bytes read so far, or the directory entry ordinal.
    /// The open's own status bits (the O_RDWR/O_APPEND/O_NONBLOCK half
    /// of the open flags): what F_GETFL answers and F_SETFL re-arms.
    unsigned long            status_flags = 0;
    base::Shared<MountLease> lifetime{nullptr};  ///< Keeps a detached runtime backend alive.

    /// Drops the node reference this open has carried since birth.
    ~OpenFile() {
        if (ops != nullptr) {
            ops->release(backend, node);
        }
    }
```

前四格 2.5 那时就熟了:脸、真身、节点、游标,游标说的就是 2.5 的那枚 offset,开文件数字节、开目录数孩子的那枚。两格是新的:status_flags 是开档自己的状态位,O_APPEND、O_NONBLOCK 的这半边,F_GETFL 问的就是它,那是 fcntl 那门系统调用里管问旗的问法,这一章就不展开了,lifetime 是一张续命的 lease,挂着的临时后端被拆台时替户口撑着,这一章记的只是名字。再往下的两桩章程,您记好:析构函数只干一件事,把出生时领的节点票还回去,拷贝与移动全删了,户口是不许抄的,共享的路只有一条,走的都是票。这样的票,是 base::Shared 的引用计数,咱们叫它持有票:表里每存的一席,换来的是票数加一,close 退掉的一座,跟着的是票数减一,fork 继承的当口,添上的又是一票。户口的本体自始至终就一份,游标长在户口的身上,fork 传下去的父子拼同一副游标。两家各自 open 的是同一个文件,立起来的是两份户口两副游标,谁也不等谁的,这还是 2.5 的老话。表满了也不含糊,满席当场拒的是 kTooManyOpenFiles,alloc 的注原话,`a full table takes no reference`,一张票也是不占的。

票怎么救命的,ramfs 的这头看得最清楚,咱们把节点抄来,ramfs.cpp 的 :48:

```cpp
struct RamNode : DirNode<RamNode> {
    std::unique_ptr<unsigned char[]> data;
    unsigned long                    capacity = 0;
    unsigned long                    size     = 0;
    base::RefCount<>                 references;

    void release_ref() {
        if (references.release()) {
            delete this;
        }
    }
};
```

章程两条:孩子在父目录的链上,占的是孩子边的一张票,有开档引用了它,开档边又多了一张。make_file 落了地,票数生而为一,占的就是孩子边的票:名分落进父目录的链上,链上的收养不另加票。open 认到了它,户口边添的是一张,合起来的就是两张。unlink 摘了链,孩子边的一张当场还了,此刻若还有开着的读档,票数是没有归零的,节点死不了,您继续读,读的是没了名字的字节,最后一枚 close 触发的是户口析构,release 还掉的是最后一张,票数归了零,`delete this`。2.5 十单里 cat nope 回的 errno 2,那是名字边的死,字节边活到了最后一票,内里的来历今天补齐了。真盘那头同样的章怎么唱,到了 5.2 见。

fd 表的名下还有四把椅子,本章给您摆出来:adopt_at 那把 0/1/2 刚坐过,另三把留给了 5.6,您挨把记名。`adopt`,挪来坐的是别人家的户口,dup 认领用的就是它,`adopt_at`,连座位号一起指定的是它,0/1/2 入座坐的就是它,`adopt_from`,带的是下限与继承开关的挪法,`duplicate_at`,办的是换座不换人的形。fork 的整表认亲不走椅子,走的是一把锁下的整表快照,手续是 clone()(fork.cpp 的 :128),四把椅子伺候的都是单座交接。重定向、管道、fork 与 exec 的全套交接,到了 5.6 一气演完。

## 目录的念法与文件的名录

2.5 的窄处,说的是目录条目一回一枚,打包的功课记在 busybox 名下。今天的答卷咱们照着 Linux 的册子写,行话的名目是 dirent64,咱们管它叫打包记录:一趟 getdents,把孩子的名录挨个装进您给的缓冲,装到了装不下为止。字段偏移写成了常量,咱们抄来,syscall.hpp 的 :296:

```cpp
/// Where the inode number sits in one packed record.
inline constexpr unsigned long kDirent64InoAt  = 0;
/// Where the resume cookie sits in one packed record.
inline constexpr unsigned long kDirent64OffAt  = 8;
/// Where the record's own length sits in one packed record.
inline constexpr unsigned long kDirent64LenAt  = 16;
/// Where the type word sits in one packed record.
inline constexpr unsigned long kDirent64TypeAt = 18;
/// Where the NUL-terminated name starts in one packed record.
inline constexpr unsigned long kDirent64NameAt = 19;
```

您挨格看下来,一条记录的五样:门牌号在 0,续读的凭据在 8,记录自身的长度在 16,类型词的位置在 18,名字从 19 起、补的是一枚空字节。为什么不用一个结构体硬造?名字长短不齐,记录得按名字的实际长度收边,头部定长、尾巴按需的这一副,在 C 里靠的是柔性数组成员,C++ 的结构体给不了,所以裸字节、按偏移落笔,整条再凑成 8 的倍数。续读的凭据行话叫 cookie,直译过来成了饼干,咱们就这么叫。饼干存的是游标加一:读档的游标数到第几个孩子,饼干报的是几加一,下一趟的 getdents 接着数下去。目录读到一半合上了再重开,游标归了零从头数,客人把饼干记在自己的心里,重开之后拨 lseek 靠的还是它,才接得上停下的地方。类型词的三种:文件 8、目录 4、字符设备 2,跟 Linux 的 DT_REG、DT_DIR、DT_CHR 对齐,ls 连挨个 stat 问类型的活都省了。门牌号的一格,真盘填的是真号,内存假盘交白卷:节点身上其实带着号,ramfs 发节点的时候一号一号地发,只是 read_dir 的那一步没往条目里誊。

主循环咱们整件抄来,sys_getdents.cpp 的 :84:

```cpp
        const unsigned long kNeed = record_bytes(entry);
        if (filled + kNeed > frame.rdx) {
            break;
        }
        unsigned char record[kRecordBytes];
        put_record(record, entry, kDir->offset + 1);
        if (!uaccess::CopyToUser(frame.rsi + filled, record, kNeed)) {
            return -kEfault;
        }
        filled += kNeed;
        ++kDir->offset;
    }
    if (filled == 0 && frame.rdx < kDirent64NameAt) {
        return -kEinval;
    }
    return static_cast<long long>(filled);
```

算的是一条要多长,缓冲装不下了就 break,硬塞的一条也没有,装得下的就落笔、拷给客人、游标加一。收尾的门槛:一条都没装下、缓冲连 19 字节的头都搁不下,回的就是 EINVAL,否则回装下的字节数。您留意零的读法:缓冲够大却一条没装,回的就是零,零在这门接口的念法里是“到头了”,所以接名录的缓冲,至少要装得下目录里最长的一条,这是客人自己的责任。

stat 的错位案,压轴的就是它。正文《打包的记录与说谎的 mode》记过全案,咱们请回来讲。症状匪夷所思:您裸敲 busybox,把一串常用命令打包在一身的工具,不带斜杠的写法,壳沿一张叫 PATH 的目录清单挨个找命令,稳定地回 Permission denied,报的 errno 13,绝对路径倒是全通,内核的 errno 表里报不出 13 的来路,断点架在 execve 的 handler 上,断了个空,叫 execve 的事压根没发生过。硬件断点抓的现行,ASH_TEST_EXEC 是那枚断点自报的名,旧屏按原样抄了下来:

```text
ASH_TEST_EXEC mode=0x128790 IX=0 size=288230513590665216
```

定案:stat 的返回结构错位。咱们当年教的是五个 64 位字段、40 字节的教学形状,musl 按 Linux x86_64 的 144 字节读:它的 mode 在偏移 24、32 位宽,咱们的 mode 写在偏移 8、64 位宽。错位了之后,musl 读到的 mode 其实是文件大小 1214352 的低 32 位,得的就是 0x128790:碰巧长得像普通文件,可执行位偏偏与不上,三组执行位的掩码 0x49 按位与它得零,旧屏 IX 一栏亮的就是这场与出来的零。壳找命令的章程,是对 PATH 的候选挨个 stat,查的正是类型与执行位,查不过当场自判 EACCES,13 的本名,fork 与 exec 的事根本没开始,带斜杠的命令跳过候选检查,所以绝对路径全通。上屏那行里 size 也是错位读出的天文数字,一行报错里藏着的是两处赃物。

修法的三层,咱们挨层看。头一层管的是结构:照 Linux 重排成 144 字节。第二层管的是位次:字段位次一枚一枚写进 static_assert,syscall.hpp 的 :344 起排成一排,`sizeof(SyscallStat) == 144`,`mode 在 +24`,`size 在 +48`,位次从此动一动就编不过,把守的活交给了编译器。第三层是这一案留下的教训。写出去给外人按固定格式解的字节,行话的名目是 ABI,咱们叫它线上格式,立成术语:格式的裁量权不在写的一侧,在线上早已印好的那本册子。绿了多少年都不算数,musl 不会来问咱们要不要 40 字节。内核内里自己递结构体的场合,倒是不必看谁的脸色,FsStat 的家当数下来六格,头注的原话,`Half of the classical fourteen on purpose`,经典的十四字段,有意只收了半份,rdev 的一格眼下恒报零,等设备进门那天才名副其实,到了系统调用的出口,HandleStat 再按 144 字节的样式誊一遍。mode 的一枚字里装着三样话,类型、权限、修饰:目录的 040755、普通文件的 0100644、字符设备的 020666,fs_config.hpp 里的三枚常量。ramfs 的 mtime 老实填零,头注明明白白说了,教学假盘装不了钟,真盘的时辰,到了 5.2 见。

## 实况与回望:不跑新机的一章

债单摊了开来,2.5 收卷记下的七笔,咱们挨笔念。名字欠的 32,还成了 256。路欠的 96,还成了 256,为什么,头注里记了案。挂载点的 8 席照旧,债主是还没到站的。fd 欠的八席,还成了 1024 乘 1024 的动态长。lseek 的一笔,HandleLseek 立在了 syscall 侧,SEEK_CUR 拿游标当了锚,“读到哪儿算哪儿”的旧债销了。目录一回一枚的欠条,还成了打包。当前目录没有的旧话,还成了任务随身的一格 cwd。七笔念了下来,清掉的是六笔,悬下的是一笔,悬的不赖着,哪天席真满了,咱们哪天再扩。

还有 2.5 十单收尾应下的那句戏:note 就这一世,您要它活得过重启,得等它搬进一块真的盘。今天根上挂的就是真盘,note 这就搬得了,一世的字据作废了,咱们替它高兴。当年随身的行李 initrd 也退了役:根从盘上起,也不用从镜像里解包了,ustar 那户人家下了岗,头注还挂在了墙上,是没来得及换的旧招牌,咱们如实记一笔。tar 与盘的家世、真盘怎么挂上根,都归到了 5.2 那一头。

## 对照:xv6 的目录,与 Linux 的四笔来往

xv6 的那一头,目录当的就是文件:类型 T_DIR,数据是一串定长的条目,read 直读的是字节流,DIRSIZ 定的是 14,截长补短的章法,书页 99 那段书证说的就是它。咱们这头条目住的是链,读走 getdents 的打包,名字给到的是 256。两副形状的背后各有一句实话:它教“目录也是文件”,取的是最简,咱们教“客人手里的接口长什么样”,落笔照的是册子。取简与取真的两条路,都是教学的选择,您两头都见过,就不容易把哪一头当成天经地义。

Linux 与咱们的四笔来往,全写在自家的注释里,咱们一笔一行地数:PATH_MAX 拦的 4096,是客人递路的原始上限,nr_open 立的 1024 乘 1024,是 fd 的天花板,dirent64 的那本,是 getdents 的打包册子,stat 的 144 字节,是 x86_64 的册子,位次交给了 static_assert 把着。四笔都认下来了,咱们跟它用同一本册子。它家 VFS 的全本,咱们还是一行手册没翻过,这回不比了,留给将来的书。

## 本章留下了什么

名字世界的内里,咱们走了一遍:路在走廊里折好了,名字认人的活归树,门面按最长的前缀派活,座位上坐的是户口,户口兜里的票管着节点的生死,名录照册子打好了包。2.5 的债单七笔,对清了六笔,余下一笔挂了名。新词也入了册:

| 术语 | 行话 | 一句话口径 |
| --- | --- | --- |
| 折路 | fold | 把带着点与点点的路归一成从根起、无回头箭头的绝对路 |
| 弹层 | pop | 折路栈退一层,收掉最后一层名字与它的斜杠,根之上不再弹 |
| 当前目录 | cwd | 任务随身的一格家底,相对路出发前问一次的家 |
| 户口 | open file description | open 落地立的那本档,fd 是座位号,它是坐在座上的人 |
| 持有票 | 引用计数 | 谁引用谁持票,票尽人散,节点的生死由票数说了算 |
| 打包记录 | dirent64 | getdents 一条目录条目的线上形状,19 字节头加名字 |
| 线上格式 | ABI | 写给外人按固定格式解的字节,裁量权在印好的册子那侧 |

没讲的咱们如实交底:lease 只报了名字,挂真根与卸载的门道、tmpfs 的可挂可卸、ext2 条目的内里、盘上的时钟,是 5.2 的地界,断电之后盘上的字还作不作数,是 5.3 的地界,块设备的队列与引擎,是 5.4 的地界,0/1/2 三座后头键盘与控制台的来路,是 5.5 的地界,四把椅子与 fork、exec、重定向的全套交接,是 5.6 的地界,devtmpfs 与 procfs 的内里、门面当模式用,是 5.7 的地界。

## 练一练

咱们留三道题:一道折路、一道算字节、一道回望。

**题一考的是折路。** /a/b/../c 折完是什么?再折 a/../..,基座坐的是根,又是什么?第二问里 depth 弹到零之后再弹,会发生什么?您推一推,再对底下的参考答案。

**题二算的是字节。** 目录里最长的名字 250 字节,一条打包记录几字节?您拿 256 当缓冲递进去,这一趟 getdents 回几?客人下一步该干什么?

**题三问的是回望。** stat 错位案的里头,为什么带斜杠的绝对路径一条条都通行,裸命令一条条都吃了闭门羹,您把两头对上说说?

### 参考答案

题一:头一条您该折出 /a/c,第二道的点点连弹两层,depth 见了零后再弹是空操作,折完剩的就是 /,根之上就不再弹了,弹层是有底的。

题二:19 加 250 加 1 拼起来得的是 270,凑成 8 的倍数是 272,256 是装不下它的,`break`,已装下的照交,一条没装回的就是 0,零念作的是“到头了”,您该放大缓冲再来一趟。

题三:壳只对 PATH 里的候选做 stat 前置检查,带斜杠的命令跳过检查直接 exec,假 mode 骗的是“查类型与执行位”的那一问,您不问,它就骗不着您。

## 延伸

- xv6 书第 10 章,书页 99 的目录层整节,您拿它与名字树一节对读,看同一件事最简与最真的两种取法。
- man 2 open,2 是手册分给系统调用的节号,open file description 的词条在里头,POSIX 给户口的正名,您一段读完。
- man 2 getdents 与 man 2 stat,Linux 那本册子的原页,本章跟着写的两份格式,您要核对,出处都在这儿。
