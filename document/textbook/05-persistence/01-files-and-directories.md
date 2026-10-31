---
title: 01 · 文件与目录:折好的路,认名字的树,还清的债单
---

# 01 · 文件与目录:折好的路,认名字的树,还清的债单

第 5 部开门,咱们办的头一件事是对债单。2.5 把文件系统的脸立了起来,收卷时自家交了底:名字最长 32 字节,路径最长 96,挂载点 8 席,每任务 fd 8 席,offset 不能拨、没有 lseek,目录条目一回一枚,当前目录没有。原文收尾一句咱们请回来:“几笔都是记了名的债,到期各有债主。”债主就是这一部,今天是第一站。这一章走名字世界的内里:路怎么折,名字怎么住进树里,号与户口怎么立,顺手把债单挨笔对清。

这一章的活,咱们数下来三样。头一样,折路,客人递来一条可能带着点与点点的路,内核怎么把它折成一条从根起、没有回头箭头的绝对路。再一样,名字树与门面,2.5 从外面看过的那棵树,内里凭什么认名、认错了会出什么冤案,树前头的挂载门面按什么章程派活。末一样,座位与户口,fd 表从八席长成了什么形状,一个开着的文件在内核里立了哪本档。三样之外,getdents 的打包手艺与 stat 的一桩错位案今天压轴。

量法还是三条。码,实况锚是 r34_compile_self 的克隆,行号一处一处按它数。屏,不跑新机,本章没有一面新起的屏:挂载表的屏面按代码推演,咱们如实标注,压轴案里那行断点赃物,是从已刊正文卷请回来的旧屏,标明出处。书,xv6 还是第 10 章,新翻到书页 99,目录那一层,咱们整段请。

> **2.5 收卷那会儿,头三号是终身制:0 号键盘进,1 号控制台出,2 号控制台错,表从来不发它们,柜台那侧直接特判,发出去的头一枚号是 3。今天您睁眼拿到的头一枚真文件号,多半还是 3。可特判没了,那枚写死的常量也下岗了。您想:形状没变,凭什么?**
>
> 凭座位是真座位了。起机那会儿,壳的表被安进三枚真开着的座,座上坐着三个真开着的文件,发号的章程是挑最低空闲、从 0 找起,三座有主,头一个空号自然落在 3。当年柜台写死 3,是预算紧时的直通车,如今没人写死它,3 是算出来的。头注收尾一句,`the shape every Unix program grew up with, arrived at honestly`,Unix 程序从小认的这副形状,这回是诚实地长出来的。整件事的码在座位与户口一节,咱们到那儿见分晓。

## 路名的念法与折法

一条路进门,第一件事不是找,是切。PathCursor 只有一格家当,余路 rest,NextComponent 一勺一勺舀:头上的斜杠吃干净,吃尽了说明路走完,回 false,没尽就找下一道斜杠切一刀,切下来的一段就是名字,rest 往前挪。咱们把这把刀抄来,path.hpp 的 :58:

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

您留意这副念法里没有点与点点的戏份。切出来的 . 就是名叫点的普通一段,特殊含义留给折的人。折的人是 Fold,path.cpp 里一个只活在折路函数里的结构,四格家当,出口缓冲 out,容量 capacity,已写字节 used,摞了几层 depth。缓冲就是栈,path.hpp 的头注点破了这层身份,`The buffer doubles as the component stack`:写进去的每一层名字,同时就是栈里的一帧,不另开存储。咱们把折的主循环抄来,path.cpp 的 :55:

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

pop 是弹层:从尾往回找,把最后一层名字连同它头上的斜杠一起收掉,depth 见零就回,根之上不再弹,弹层有底。walk 是折的主循环:点白吃一回,路不变,点点弹一层,正经名字压一层,压层是 push,补一道斜杠把名字接在尾上。您拿 a/b/../c 走一遍,b 压进去又弹出来,折完是 /a/c。咱们管这一趟活叫折,折路:进去的路带着回头的箭头,交出去的路一步不绕。这词从这儿起当术语使。

折要有个基座。绝对路斜杠打头,基座就是根,相对路不打头,基座是任务的当前目录,行话 cwd,current working directory 的缩写,任务随身的一格家底,住在 task.hpp 的 :131,`char cwd[cinux::fs::kFsPathMax] = {'/'};`,起机给一根斜杠,谁生下来都落在根上。chdir 想搬家,内核把新路折好、验明是目录,再把字节抄进 cwd 里,getcwd 反着要,您给的缓冲装不下整条 cwd,回 ERANGE,一句话的事,不打折扣。相对性收在门口:认路从来不必一路问“我现在在哪”,出发前问一次家,路就定死了。尺度您也分清:客人递来的原始路,按 4096 收,fs_config.hpp 里 kUserPathMax,头注写明是 Linux 的 PATH_MAX,折完的路按 256 验,折是归一化,归一化之前的路可以比之后长得多,所以宽收、折、按窄的验,三步一条走廊。open、chdir、stat、unlink,每条路过系统调用都走它,咱们把走廊抄来,sys_common.cpp 的 :88:

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

96 到 256 这一步,头注里记着一段真事,咱们念给您听。当年路径上限 96 字节,尺子量的是折之前的原始路,后来内核里跑起真的编译器,它自报家门要打开的文件,名字里翻四层父目录,不折不拼开就过百字节,好路在折路之前就被拒成了 EINVAL。尺子拧到 256,头注把来路原样记下,`a teaching limit of 96 denied as EINVAL before normalization could shrink it`,96 在归一化来得及缩小之前就拒了路。名字上限同一场变动里从 32 拧到 256:根上从只活内存的假盘换成真盘之后,盘上的真名字本来就长,32 装不下真盘的世界。挂载点 8 席没动,那笔债的债主还没到站。

**停一停,您来折。** /a/b/../c 折完是什么?再折 a/../..,基座是根,又是什么?第二问里 depth 弹到零之后再弹,会发生什么?您推一推,题在节末。

## 一棵只认名字的树

2.5 从外面看这棵树:resolve 认门,进门的活归它。今天咱们进内里。这棵树不认盘、不认块,只认名字,骨架是 dir_tree.hpp 里的一个模板,DirNode,五格家当,名字、类型、门牌号 ino、头孩子指针、弟妹指针。谁想当树里的节点,就把自己的结构从它长出来,ramfs 的 RamNode 就这么长的,模板那一手 CRTP,白话一句,父亲只管串链子,孩子长什么样父亲不关心,孩子自己报。

凭名字认人,咱们把三道关卡抄来,dir_tree.hpp 的 :62:

```cpp
    [[nodiscard]] bool named(std::string_view wanted) const {
        return wanted.size() < kFsNameMax && base::EqualBytes(name, wanted.data(), wanted.size()) &&
               name[wanted.size()] == '\0';
    }
```

头一道比长,名字数组定长,要找的名字必须更短,第二道字节挨个比,第三道比一枚收尾的空字节。三道缺一不可,单比字节会出冤案:您找 note,树里住着 note.txtx,note 的四个字节在人家头四个字节里全对得上,少了第三道,note 就把别人认作自己。这一案正文《第一棵树》一篇真发过,冤有头债有主,第三道就是那笔债的清法。住进来 adopt,头插,两行:新孩子的弟妹指针指向早前的头孩子,头孩子换成新来的。头插一条道走到黑,读出来就是建的倒序,谁最后进链谁排头,想要正序,客人自己排,树不替次序操心。搬走 disown 反着来,从链上摘一个孩子,条件不齐摘不动,回空。unlink 落到它身上:摘链,若发现是还带着孩子的目录,adopt 挂回去再回 kDirectoryNotEmpty,带孩子的房子不让拆,一句旧话,码还是那句码。

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

您看它用的正是头一节那把刀,切的活不分家。两样错的次序有讲究:每进一段之前,验一下脚下是不是目录,不是回 kNotADirectory,再认名,认不到回 kNotFound。您拿一条三段的路走,第二段是文件、第三段是空,错在第三段进门之前就报定,不会认半截路再回头。xv6 的目录层咱们请书证,书页 99:“A directory is implemented internally much like a file. Its inode has type T_DIR and its data is a sequence of directory entries. Each entry is a struct dirent (4165), which contains a name and an inode number. The name is at most DIRSIZ (14) characters; if shorter, it is terminated by a NULL (0) byte. Directory entries with inode number zero are free.”名字至多 14 字节,短了拿空字节填,空下的条目,门牌号记零。咱们的名字 256 字节,条目住在链上,不排在字节流里,真盘那头的目录条目是另一副打包法,5.2 见。

## 门面与挂载表

2.5 立脸那会儿,FsOps 九枚指针。今天咱们再数,十五。新添的六枚,stat 问身家,readable 问可读,ioctl 接终端的话,adopt_open 管开门时换立节点,append 与 truncate 管写的新姿势。进门的门槛 concept FileSystem 从九法长到十三法,过不了编译这一关,后端进不了家门。擦法 EraseFs 把十五枚一枚不落地生出来:后端有的姿势转手递过去,没有的,擦法替它回一句不支持。Vfs 自己也长了肉:自家一把锁,挂载表的每一条添了 type_name 一格报家门,卸载也签了字。2.5 问框里请过 vfs.hpp 头注的期票,`ramfs today, ext2 one station later, behind one face`,今天根上坐的正是 ext2,票兑了,脸也长了,长的是针数,待客的章程没换,还是同一张。

挂载表还是 8 席,走法 2.5 把章程立齐了:前缀要真前缀,/dev 不能认 /dev2 的路,边界要断在斜杠上,盖得住它的前缀里,最长者赢。今天咱们把裁判那几行抄来,vfs.cpp 的 :43:

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

您递 /dev/console,根席与 /dev 席的前缀都咬得住,长的那席赢,余路 console 交给它。交回来的 FsView 从三件长成四件:脸、真身、余路,加一张 lifetime 的 lease。第四件这一章只报名字,它的正戏在挂真根与卸载,5.2 再演。

座次实况一句话:起机时根上挂的是真盘,盘上的文件系统行话 ext2,内里 5.2 一整章,/dev 挂 devtmpfs,设备节点安身的家,内里 5.7,/proc 挂 procfs,挂载表里报的家门写作 proc,内里也归 5.7,各占一席,tmpfs 也进了家门,一块 ramfs 转世的临时地,可挂可卸,门道归 5.2,咱们一句带过。挂载表有一面公开的镜子,procfs 里的 /proc/mounts,家什是 Vfs::mounts_text:锁上门,挨席走,每席一行,三样参数 none、前缀、类型名,配上固定尾巴,格式串原样是 `"%s %s %s rw 0 0\n"`。咱们不跑新机,镜子里的样子按码推演,如实标注,席序按挂的早晚:

```text
none / ext2 rw 0 0
none /dev devtmpfs rw 0 0
none /proc proc rw 0 0
```

头一列 none 是老实话:今天的码给每一席都写 none,设备名这一列还没有接线,真盘怎么挂上根,5.2 见。第三列是 type_name 报的家门。客人世界里谁要问挂载,您看到的就是 mounts_text 照出来的样子。

## 座位、户口与那桩悬案

fd 表,2.5 那年八席。今天咱们看封顶,file_table.hpp 的 :33:

```cpp
/** @brief Linux's default nr_open ceiling; storage grows only when used. */
inline constexpr unsigned long kFileTableMax = 1024UL * 1024;
```

一百零四万八千五百七十六席,1024 乘 1024,头注认的是 Linux 默认的 nr_open 上限,格子跟着用到的号长,一百万席是天花板,房租不预付。发号的章程没变,挑最低空闲:alloc 一进门就问 vacant_locked(0, …),从 0 找起。alloc 的注里还写着“发号从 3 起”,那是句过期的话,咱们以代码为准。表本身也换了住法:堆上的一张,任务生下来带一张,fork 出的孩子另立新表,继承的开档一张一张挪进去,线程组里共用表的一伙人指着同一张、同一副游标。这些认亲的章程全套在 5.6,这里咱们只把舞台搭好。

从 0 找起,可您睁眼头一枚真文件号还是 3,问框里那桩悬案,码在这儿。起机时,开机头一个立起来的任务 init,替壳安顿表,file_world.cpp 的 :194:

```cpp
    kStream->status_flags = kFsReadWriteStatus;
    for (unsigned long seat = 0; seat < 3; ++seat) {
        const base::Result<unsigned long> kSeated = table.adopt_at(kStream, seat);
        if (!kSeated.ok()) {
            return kSeated.error();
        }
    }
```

/dev/console 被打开,立了一份开档,读写位拧到 kFsReadWriteStatus,0、1、2 三座 adopt_at 同一份:三座三张票,一份档。表头从此没有空位,壳的头一个 open 落在 3。2.5 的老话咱们原样记得:头三号终身制,表从来不发,柜台特判。那是当年的真话,如今长成了另一副:特判撤了,常量下岗了,0/1/2 是三枚真座位,不是保留号,座上是真开档,键盘进、控制台出的活计,搬到了 /dev/console 门后,正戏 5.5 开讲。当年 2.3 只认 1 号的窄处,2.5 宽到 2 号,到今天,特判整个撤了。

开档落地那一刻,内核立的档就是 OpenFile。POSIX,管 Unix 接口口径的那套标准,给这类东西起了正名,open file description,开档描述,咱们顺着叫它户口:fd 是座位号,户口是坐在座上的人,换座、拼座、退座,动的都是号,户口不动。咱们把它抄来,open_file.hpp 的 :45:

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

前四格 2.5 就熟:脸、真身、节点、游标,就是 2.5 那枚 offset,开文件数字节、开目录数孩子的那枚。两格是新的:status_flags 是开档自己的状态位,O_APPEND、O_NONBLOCK 这半边,F_GETFL 问的就是它,那是 fcntl 那门系统调用里管问旗的问法,这章不展开,lifetime 是一张续命的 lease,挂着的临时后端被拆台时替户口撑着,这一章只记名字。再往下两桩章程,您记好:析构函数只干一件事,把出生时领的节点票还回去,拷贝与移动全删,户口不许抄,要共享,走票。票,是 base::Shared 的引用计数,咱们叫它持有票:表里每存一席,票数加一,close 退一座,票数减一,fork 继承,再添票。户口本体自始至终一份,游标长在户口上,fork 传下去的父子拼同一副游标。两家各自 open 同一个文件,是两份户口两副游标,谁也不等谁,2.5 的老话。表满了也不含糊,满席当场拒 kTooManyOpenFiles,alloc 的注原话,`a full table takes no reference`,一张票都不占。

票怎么救命的,ramfs 这头看得最清楚,咱们把节点抄来,ramfs.cpp 的 :48:

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

章程两条:孩子在父目录的链上,占孩子边一张票,有户口引用它,开档边再一张。make_file 落地,票数生而为一,占的就是孩子边的票:名分落进父目录的链上,链上的收养不另加票。open 认到它,户口边再添一张,两张。unlink 摘链,孩子边的一张当场还,此刻若还有开着的读档,票数没归零,节点不死,您继续读,读的是没了名字的字节,最后一枚 close 触发户口析构,release 还掉最后一张,归零,delete this。2.5 十单里 cat nope 回 errno 2,那是名字边的死,字节边活到最后一票,内里的来历今天补齐了。真盘那头同样的章怎么唱,5.2 见。

fd 表还有四把椅子,本章摆出来:adopt_at 那把 0/1/2 刚坐过,另三把留给 5.6,您挨把记名。adopt,别人家的户口挪来坐,fork 继承用的就是它,adopt_at,连座位号一起指定,0/1/2 入座坐的就是它,adopt_from,带下限与继承开关的挪法,duplicate_at,换座不换人的形。重定向、管道、fork 与 exec 的全套交接,5.6 一气演完。

## 目录的念法与文件的名录

2.5 的窄处,目录条目一回一枚,打包的功课记在 busybox 名下。今天的答卷咱们照着 Linux 的册子写,行话 dirent64,叫它打包记录:一趟 getdents,把孩子的名录挨个装进您给的缓冲,装到装不下为止。字段偏移写成常量,咱们抄来,syscall.hpp 的 :296:

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

您挨格看下来,一条记录五样:门牌号在 0,续读的凭据在 8,记录自身的长度在 16,类型词在 18,名字从 19 起、补一枚空字节。为什么不用一个结构体硬造?名字长短不齐,记录得按名字的实际长度收边,头部定长、尾巴按需,这副形状在 C 里靠柔性数组成员,C++ 的结构体给不了,所以裸字节、按偏移落笔,整条再凑成 8 的倍数。续读的凭据行话叫 cookie,直译过来是饼干,咱们就这么叫。饼干存的是游标加一:读档的游标数到第几个孩子,饼干报几加一,下一趟 getdents 接着数下去。目录读到一半合上重开,游标归零从头数,客人把饼干记牢,重开后拿它拨 lseek,才接得上停下的地方。类型词三种,文件 8、目录 4、字符设备 2,跟 Linux 的 DT_REG、DT_DIR、DT_CHR 对齐,ls 不必再挨个 stat 问类型。门牌号一格,真盘填真号,内存假盘交白卷:节点身上其实带着号,ramfs 发节点时一号一号发,只是 read_dir 没往条目里誊。

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

算一条要多长,缓冲装不下就 break,一条都不硬塞,装得下就落笔、拷给客人、游标加一。收尾的门槛:一条都没装下、缓冲连 19 字节的头都搁不下,回 EINVAL,否则回装下的字节数。您留意零的读法:缓冲够大却一条没装,回零,零在这门接口的念法里是“到头了”,所以接名录的缓冲,至少要装得下目录里最长的一条,这是客人自己的责任。

stat 的错位案,压轴。正文《打包的记录与说谎的 mode》记过全案,咱们请回来讲。症状匪夷所思:您裸敲 busybox,把一串常用命令打包在一身的工具,不带斜杠,壳沿一张叫 PATH 的目录清单挨个找命令,稳定回 Permission denied,errno 13,绝对路径全通,内核的 errno 表里报不出 13 的来路,断点架在 execve 的 handler 上,断了个空,execve 压根没被叫过。硬件断点抓的现行,ASH_TEST_EXEC 是那枚断点自报的名,旧屏原样抄:

```text
ASH_TEST_EXEC mode=0x128790 IX=0 size=288230513590665216
```

定案:stat 的返回结构错位。咱们当年教的是五个 64 位字段、40 字节的教学形状,musl 按 Linux x86_64 的 144 字节读:它的 mode 在偏移 24、32 位宽,咱们的 mode 写在偏移 8、64 位宽。错位之后,musl 读到的 mode 其实是文件大小 1214352 的低 32 位,0x128790:碰巧长得像普通文件,可执行位偏偏与不上,三组执行位的掩码 0x49 按位与它得零,旧屏 IX 一栏亮的就是这场与出来的零。壳找命令的章程,是对 PATH 的候选挨个 stat,查类型查执行位,查不过当场自判 EACCES,13 的本名,fork 与 exec 根本没开始,带斜杠的命令跳过候选检查,所以绝对路径全通。上屏那行里 size 也是错位读出的天文数字,一行报错,两处赃物。

修法三层,咱们挨层看。头一层,结构照 Linux 重排成 144 字节。第二层,字段位次一枚一枚写进 static_assert,syscall.hpp 的 :344 起排成一排,`sizeof(SyscallStat) == 144`,`mode 在 +24`,`size 在 +48`,位次从此动一动就编不过,钉子交给编译器把着。第三层是这一案留下的教训。写出去给外人按固定格式解的字节,行话 ABI,咱们叫它线上格式,立成术语:格式的裁量权不在写的一侧,在线上早已印好的那本册子。绿了多少年都不算数,musl 不会来问咱们要不要 40 字节。内核内里自己递结构体倒不必看谁脸色,FsStat 数下来六格,头注原话,`Half of the classical fourteen on purpose`,经典十四字段,有意只收半份,rdev 一格眼下恒报零,等设备进门那天才名副其实,到了系统调用的出口,HandleStat 再按 144 字节誊一遍。mode 一枚字里三样话,类型、权限、修饰:目录 040755、普通文件 0100644、字符设备 020666,fs_config.hpp 里三枚常量。ramfs 的 mtime 老实填零,头注明说,教学假盘不装钟,真盘的时辰,5.2 见。

## 实况与回望:不跑新机的一章

债单摊开,2.5 收卷记下的七笔,咱们挨笔念。名字 32,还成 256。路 96,还成 256,为什么,头注记在案。挂载点 8 席,原样,债主还没到站。fd 八席,还成 1024 乘 1024 的动态长。lseek,HandleLseek 立在 syscall 侧,SEEK_CUR 拿游标当锚,“读到哪儿算哪儿”的旧债销了。目录一回一枚,还成打包。当前目录没有,还成任务随身一格 cwd。七笔,六清一悬,悬的不赖着,哪天席真满了,哪天再扩。

还有 2.5 十单收尾应下的那句戏:note 就这一世,要它活得过重启,得等它搬进一块真的盘。今天根上挂的就是真盘,note 这就搬得,一世的字据作废,咱们替它高兴。当年随身的行李 initrd 也退了役:根从盘上起,不再从镜像里解包,ustar 那户人家下了岗,头注还挂在墙上,是没来得及换的旧招牌,咱们如实记一笔。tar 与盘的家世、真盘怎么挂上根,都归 5.2。

## 对照:xv6 的目录,与 Linux 的四笔来往

xv6 那头,目录就是文件:类型 T_DIR,数据是一串定长条目,read 直读字节流,DIRSIZ 14,截长补短,书页 99 那段书证说的就是它。咱们这头条目住在链上,读走 getdents 的打包,名字 256。两副形状背后各有一句实话:它教“目录也是文件”,取最简,咱们教“客人手里的接口长什么样”,照册子写。取简与取真,都是教学的选择,您两头都见过,就不容易把哪一头当成天经地义。

Linux 与咱们的四笔来往,全写在自家注释里,咱们一笔一行:PATH_MAX 4096,客人递路的原始上限,nr_open 1024 乘 1024,fd 的天花板,dirent64,getdents 的打包册子,stat 144 字节,x86_64 的册子,static_assert 把着位次。四笔都认,咱们跟它用同一本册子。它家 VFS 的全本,咱们还是一行手册没翻过,不比,留给将来的书。

## 本章留下了什么

名字世界的内里,咱们走了一遍:路在走廊里折好,名字在树上认人,门面按最长前缀派活,座位上坐着户口,户口兜里的票管着节点的生死,名录按册子打包。2.5 的债单七笔,对清了六笔,余下一笔挂了名。新词入册:

| 术语 | 行话 | 一句话口径 |
| --- | --- | --- |
| 折路 | fold | 把带着点与点点的路归一成从根起、无回头箭头的绝对路 |
| 弹层 | pop | 折路栈退一层,收掉最后一层名字与它的斜杠,根之上不再弹 |
| 当前目录 | cwd | 任务随身的一格家底,相对路出发前问一次的家 |
| 户口 | open file description | open 落地立的那本档,fd 是座位号,它是坐在座上的人 |
| 持有票 | 引用计数 | 谁引用谁持票,票尽人散,节点的生死由票数说了算 |
| 打包记录 | dirent64 | getdents 一条目录条目的线上形状,19 字节头加名字 |
| 线上格式 | ABI | 写给外人按固定格式解的字节,裁量权在印好的册子那侧 |

没讲的咱们如实交底:lease 只报了名字,挂真根与卸载的门道、tmpfs 的可挂可卸、ext2 条目的内里、盘上的时钟,在 5.2,断电之后盘上的字还作不作数,5.3,块设备的队列与引擎,5.4,0/1/2 三座后头键盘与控制台的来路,5.5,四把椅子与 fork、exec、重定向的全套交接,5.6,devtmpfs 与 procfs 的内里、门面当模式用,5.7。

## 练一练

咱们留三道题,一道折路,一道算字节,一道回望。

**题一,折路。** /a/b/../c 折完是什么?再折 a/../..,基座是根,又是什么?第二问里 depth 弹到零之后再弹,会发生什么?您推一推,再对底下的参考答案。

**题二,算字节。** 目录里最长的名字 250 字节,一条打包记录几字节?您拿 256 当缓冲递进去,这一趟 getdents 回几?客人下一步该干什么?

**题三,回望。** stat 错位案里,为什么带斜杠的绝对路径一条条都通行,裸命令一条条都吃闭门羹,您把两头对上说说?

### 参考答案

题一:头一条您该折出 /a/c,第二条点点连弹两层,depth 见零后再弹是空操作,折完就是 /,根之上不再弹,弹层有底。

题二:19 加 250 加 1 得 270,凑成 8 的倍数是 272,256 装不下它,break,已装下的照交,一条没装就回 0,零念作“到头了”,您该放大缓冲再来一趟。

题三:壳只对 PATH 里的候选做 stat 前置检查,带斜杠的命令跳过检查直接 exec,假 mode 骗的是“查类型与执行位”这一问,您不问,它就骗不着。

## 延伸

- xv6 书第 10 章,书页 99 的目录层整节,您拿它与名字树一节对读,看同一件事最简与最真的两种取法。
- man 2 open,2 是手册分给系统调用的节号,open file description 的词条在里头,POSIX 给户口的正名,您一段读完。
- man 2 getdents 与 man 2 stat,Linux 那本册子的原页,本章跟着写的两份格式,您要核对,出处都在这儿。
