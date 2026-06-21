---
title: 028c · 工作目录与文件信息:让文件系统「认得」相对路径
---

# 028c · 工作目录与文件信息:让文件系统「认得」相对路径

> 028b 让 ext2 能写了,你可以在 shell 里 `echo hi > /hello.txt`。但用着用着你会发现两个别扭:第一,每条命令都得从根写起——`/hello.txt`、`/etc/motd`,没有「我现在在哪个目录」的概念,也就没有 `cd`、`pwd`,更用不了 `cat hello.txt` 这种相对路径;第二,你没法看一个文件「有多大、是什么类型、inode 号是多少」——`ls -l` 想显示的那些东西,内核根本拿不出来。这一章就补这两块:给进程一个**工作目录(cwd)**,给文件系统一套**查文件元信息(stat)**的能力。顺手把 028b 里那个每个系统调用各写一份的路径拆分逻辑,收拢成一个公共的路径解析模块。
>
> 这章没有 028b 那种跌宕的崩溃调试(它没留下 notes),但有两个同样值得讲的设计取舍:路径规范化怎么保证 `cd /..` 不越过根、以及为什么第一个用户进程得临时「造」一个 Task 才能让新 syscall 跑起来。

## 这一章我们要点亮什么

在 028b 的写能力之上,补三件让文件系统「好用」的事。

第一,**路径解析模块化**。新建 `kernel/fs/path.{hpp,cpp}`,提供 `path_canonicalize`(把 `.`、`..`、重复斜杠折叠成规范绝对路径)和 `path_resolve`(把相对路径按工作目录拼成绝对路径)。再建 `kernel/syscall/path_util.{hpp,cpp}`,提供 `resolve_user_path`:把 028b 各 syscall 里内联的「用户指针 canonical 校验」收进来,并叠上这一章新加的「按 cwd 解析 + 规范化」——一个函数换掉五段重复校验,还让所有路径 syscall 自动支持了相对路径。(拆父目录/叶子名的 `split_pathname` 028c 仍让它内联在各 syscall,没动它;它消费的正是 `resolve_user_path` 的输出。)

第二,**per-process 工作目录**。`Task` 结构加一个 `cwd[256]` 字段,记录当前进程的工作目录;新增 `sys_chdir`(改 cwd)、`sys_getcwd`(读 cwd);为了让这套机制在第一个用户进程上就能用,还得给 `Scheduler` 加一个 `set_current`,并在 `launch_first_user` 里临时造一个 Task。

第三,**stat**。定义 `struct stat`(沿用 Linux x86_64 布局),给 `InodeOps` 加第 7 个虚方法 `stat`,让 ext2 把磁盘 inode 的字段翻译成 `stat`;新增 `sys_stat`(按路径查)、`sys_fstat`(按 fd 查)。

验收点:shell 里能 `cd /etc`、`pwd` 显示 `/etc`、`stat /hello.txt` 打出文件大小和类型,相对路径的命令能正确解析。

## 为什么现在需要它

为什么紧跟 028b。028b 把「写」补齐后,文件系统在功能上已经完整——能建、能写、能读、能删。但「完整」不等于「好用」。一个每次都得敲绝对路径、查不到文件信息的文件系统,只是个能跑的内核接口,不是给人用的系统。028c 把它从「能用」推向「好用」。

还有一笔技术的账,和前几章一脉相承:**去重**。028b 那会儿,`sys_creat`、`sys_mkdir`、`sys_rmdir`、`sys_unlink`、`sys_open` 五个系统调用,每个开头都内联了一份一模一样的用户指针 canonical 校验(那段 `bit47`/`upper` 判断)。五份重复,改一处要改五遍。028c 把这段校验收进 `validate_user_ptr`,再包上「按 cwd 解析 + 规范化」做成 `resolve_user_path`:一个函数换掉五段重复校验,还让所有路径 syscall 自动获得了相对路径支持。这是个典型的「加新功能的同时还清技术债」——我们要加 cwd,正好把指针校验公共化。需要说明的是,`resolve_user_path` 收的只是「校验 + 解析 + 规范化」;找最后一个 `/` 拆父目录和叶子名的 `split_pathname`,仍内联在每个 syscall 里(它要的输入恰好是 `resolve_user_path` 产出的规范绝对路径),这章没把它公共化。

而且 stat 不只是给用户看的信息。它把原本藏在 ext2 磁盘 inode 里、只有 ext2 驱动自己知道的元数据(模式、链接数、大小、块数),通过一个统一的结构暴露给 VFS 层和用户态。从此上层不用直接碰 ext2 的内部结构,问一句 `stat` 就行。这是抽象边界往前推的一步。

## 设计图

一次相对路径的解析,从用户态字符串到内核绝对路径:

```text
用户敲: cat hello.txt         (当前在 /etc)
  │
  ▼  path = "hello.txt" (相对)
sys_xxx(path_virt)
  ▼
resolve_user_path(path_virt, out[])            [path_util.cpp]
  │  ① validate_user_ptr(path_virt)           ← canonical 地址校验
  │  ② cwd = Scheduler::current()->cwd        ← 取本进程 cwd ("/etc")
  │  ③ path_resolve(cwd, "hello.txt", out)
  │       相对 → 拼成 "/etc/hello.txt"
  │       path_canonicalize → "/etc/hello.txt"
  ▼
out = "/etc/hello.txt"  (规范绝对路径)
  ▼
vfs_resolve(out) → fs + rel_path              [交给 027 的挂载层]
  ▼
fs->lookup(rel_path)                          [交给 028 的 ext2]
```

`path_canonicalize` 的核心是一个「栈式」的处理:逐个读路径分量,普通分量压进结果缓冲,遇到 `.` 跳过,遇到 `..` 就把最后一个分量弹掉:

```text
输入 /a/b/../c/./d//
分量序列:  a   b   ..   c   .   d
处理:      a   b   (弹 b) c   (跳) d
结果:      /a/c/d
```

## 代码路线

### 先把散落的路径逻辑收拢:resolve_user_path

先看这一章最直接的一笔「去重」。028b 的 `sys_creat` 开头长这样(简化):

```cpp
// 校验用户指针(canonical address)
if (path_virt == 0) return -1;
uint64_t bit47 = (path_virt >> 47) & 1;
uint64_t upper = path_virt >> 48;
if (bit47 == 0 && upper != 0) return -1;
if (bit47 == 1 && upper != 0xFFFF) return -1;
// ...然后 split_pathname 拆父目录和叶子...
```

这段「canonical 校验」在五个 syscall 里一字不差地重复。028c 把它抽成 `path_util.hpp` 里一个 inline 函数:

```cpp
inline bool validate_user_ptr(uint64_t ptr) {
    if (ptr == 0) return false;
    uint64_t bit47 = (ptr >> 47) & 1;
    uint64_t upper = ptr >> 48;
    if (bit47 == 0 && upper != 0) return false;
    if (bit47 == 1 && upper != 0xFFFF) return false;
    return true;
}
```

为什么 x86_64 要做这个校验?64 位虚拟地址里,只有低 48 位是有效的,第 47 位决定「符号扩展」:第 47 位为 0 时,高 16 位(48..63)必须全 0;为 1 时必须全 1。符合这个规则的叫「规范地址(canonical address)」,不符合的访问直接触发 #GP。用户态传进来的指针内核不能信,得先确认它是规范地址,否则一旦解引用就崩。这个道理 028b 讲过,028c 把它从五份拷贝变成一个函数。

在这之上,`resolve_user_path` 把「校验 + 按 cwd 解析 + 规范化」三步打包:

```cpp
bool resolve_user_path(uint64_t path_virt, char* out) {
    if (!validate_user_ptr(path_virt)) return false;
    auto* path = reinterpret_cast<const char*>(path_virt);
    if (path[0] == '\0') return false;

    cinux::proc::Task* current = cinux::proc::Scheduler::current();
    const char* cwd = (current != nullptr) ? current->cwd : "/";

    return cinux::fs::path_resolve(cwd, path, out);
}
```

注意中间那行:`cwd` 取自「当前进程」的 `Task::cwd`,如果拿不到 current 就退回根目录 "/"。这一行是 cwd 支持的核心——相对路径到底是相对谁,答案就是「当前进程的工作目录」。028b 的各 syscall 改成调这个函数后,既去掉了重复,又自动获得了 cwd 支持。

### 路径规范化:折叠 . .. // 的栈式算法

`path_resolve` 负责把相对路径拼上 cwd,但真正干活的是 `path_canonicalize`——它把任意乱糟糟的路径(`//a/./b/../c`)折叠成干净的规范形式(`/a/c`)。这是个经典的字符串处理算法,值得看清。

```cpp
void path_canonicalize(char* buf) {
    char out[PATH_MAX];
    uint32_t out_pos = 0;
    out[out_pos++] = '/';          // 结果恒为绝对路径

    // 逐个分量处理
    while (i < len) {
        // 提取一个分量(到下一个 '/' 为止)
        // ...

        if (comp_len == 1 && buf[comp_start] == '.') continue;          // "." 跳过

        if (comp_len == 2 && buf[comp_start] == '.' && ...) {           // ".." 回退
            if (out_pos > 1) {
                --out_pos;
                while (out_pos > 0 && out[out_pos - 1] != '/') --out_pos;  // 弹到上一个 '/'
                if (out_pos > 1) --out_pos;                              // 去掉那个 '/'
            }
            continue;
        }

        // 普通分量:追加 '/' + 分量名
        // ...
    }
    if (out_pos == 0) out[out_pos++] = '/';
    out[out_pos] = '\0';
    memcpy(buf, out, out_pos + 1);
}
```

整个算法用一个输出缓冲 `out[]` 当「栈」:遇到普通分量就往上拼(中间补 `/`),遇到 `.` 直接忽略,遇到 `..` 就把栈顶最后一个分量抹掉。重复的斜杠(`//`)因为「提取分量」时跳过了连续斜杠,自然被折叠。最后保证至少有一个 `/`(根)。

这个 `out[]` 缓冲有 `PATH_MAX`(4096)那么大,但注意它只是规范化的**临时**缓冲。最终结果拷回 `buf`——而 `buf` 在 `sys_chdir` 里会是另一个 256 字节的 `resolved[]`,最终又要拷进 `Task::cwd[256]`。所以路径解析的全程能处理 4096 长度的路径,但**工作目录本身最多存 256 字节**。这是个容量上的小不对称,正常用够了,但 `cd` 一个超长路径会被截断。

一个关键细节:`..` 的回退有 `if (out_pos > 1)` 保护。我们专门讲它——见「设计现场」第一节。

还要说清楚一件事:`path_canonicalize` **只做字符串处理,不检查路径是否存在**。它会把 `/不存在的目录/../foo` 规范成 `/foo`,但它不知道 `/foo` 到底有没有。存在性是后面 `vfs_resolve` + `lookup` 的事。这一步只管把字符串弄干净。

### 工作目录挂在进程上:Task::cwd 与 set_current

工作目录是「每个进程一份」的状态,所以它得挂在进程结构上。028c 给 `Task` 加了一个字段:

```cpp
struct Task {
    // ...原有字段...
    alignas(16) uint8_t fpu_state[512];

    /** Per-process current working directory (absolute path, NUL-terminated). */
    char cwd[256];
};
```

`cwd[256]`,存绝对路径。进程创建时(`TaskBuilder::build()`)初始化成 "/":

```cpp
// Step 7.5: Initialise cwd to "/"
task->cwd[0] = '/';
task->cwd[1] = '\0';
```

有了字段,`sys_chdir` 就是「解析路径、确认是目录、写进 cwd」:

```cpp
int64_t sys_chdir(uint64_t path_virt, ...) {
    char resolved[PATH_MAX];
    if (!resolve_user_path(path_virt, resolved)) return -1;

    // vfs_resolve + lookup
    cinux::fs::Inode* inode = fs->lookup(rel_path);
    if (inode == nullptr) return -1;

    if (inode->type != InodeType::Directory) return -1;   // chdir 必须是目录

    cinux::proc::Task* current = Scheduler::current();
    // 把 resolved 拷进 current->cwd
    uint32_t i = 0;
    while (resolved[i] != '\0' && i < sizeof(current->cwd) - 1) {
        current->cwd[i] = resolved[i]; ++i;
    }
    current->cwd[i] = '\0';
    return 0;
}
```

两件事值得说。一是 `chdir` 会**真的去 lookup 一次**,确认目标是目录——你不能 `cd` 到一个文件或不存在的路径。二是它**不规范化后再存**吗?其实 `resolve_user_path` 里已经 `path_canonicalize` 过了,所以存进 `cwd` 的已经是规范绝对路径,下次 `getcwd` 拿出来就是干净的。

`sys_getcwd` 反过来,把 `current->cwd` 拷给用户:

```cpp
int64_t sys_getcwd(uint64_t buf_virt, uint64_t size, ...) {
    // 校验 buf_virt(规范化地址)、size != 0
    cinux::proc::Task* current = Scheduler::current();
    uint32_t cwd_len = strlen(current->cwd) + 1;   // 含 NUL
    if (cwd_len > size) return -1;                  // 缓冲不够
    memcpy(reinterpret_cast<char*>(buf_virt), current->cwd, cwd_len);
    return static_cast<int64_t>(cwd_len);           // 返回长度(含 NUL)
}
```

返回值是「含 NUL 的长度」。这和 Linux `getcwd`(返回缓冲指针、缓冲不够设 ERANGE)的细节不完全一样——我们简单点,不够就返回 -1。以代码为准。

这两个 syscall 都靠 `Scheduler::current()` 拿「当前进程」。这就引出一个问题:第一个用户进程跑起来的时候,`current()` 返回的是什么?

### 第一个用户进程的小补丁:为什么 launch_first_user 要造一个 Task

`Scheduler::current()` 返回的是调度器记录的「当前正在跑的进程」。但 Cinux 启动第一个用户进程(shell)的时候,调度器其实还没真正开始调度——shell 是被 `launch_first_user` 直接 `jump_to_usermode`「手动」丢进用户态的,没有走完整的「创建 Task → 入队 → 调度」流程。

问题来了:`sys_chdir`/`sys_getcwd`/`resolve_user_path` 全都依赖 `Scheduler::current()->cwd`。如果 shell 一进去就敲 `pwd`,而 `current()` 返回 `nullptr`,这些 syscall 就没法工作(代码里对 nullptr 的处理是「退回 "/" 或返回 -1」)。

028c 的解决办法是个实用主义的小补丁:在 `launch_first_user` 跳进用户态之前,手动造一个 `Task`,设好 cwd,并把它登记为 current:

```cpp
// Create a minimal Task so chdir/getcwd can read/write a per-process cwd
static cinux::proc::Task shell_task{};
shell_task.cwd[0] = '/';
shell_task.cwd[1] = '\0';
shell_task.state = cinux::proc::TaskState::Running;
cinux::proc::Scheduler::set_current(&shell_task);

jump_to_usermode(USER_ENTRY_BASE, USER_STACK_TOP - USER_ABI_RSP_OFFSET, 0);
```

为此 `Scheduler` 还加了一个 `set_current`,同时更新 `current_` 和 per-CPU 的 `g_per_cpu.current`:

```cpp
void Scheduler::set_current(Task* task) {
    current_ = task;
    g_per_cpu.current = task;
}
```

这是个**临时**手段。`shell_task` 是个 `static` 单一 Task,不是正经的进程创建。它的意义是「让 cwd 相关的 syscall 在调度器完整运作之前也能用」。等到后面真正实现 fork/exec、进程创建走完整流程时,每个进程自然会有自己的 Task 和 cwd,这个补丁就会被取代。但在 028c,它够用。

顺带一提,`launch_first_user` 这版还动了另一处:用户代码的映射从「单页」改成了「多页循环」。原因是 shell 这章加了不少命令(`cd`/`pwd`/`stat` 等),二进制变大,一页(4KB)装不下了。所以现在按 `code_pages = ceil(user_size / PAGE_SIZE)` 分配多页、逐页映射和拷贝。这和 cwd 没直接关系,但它是「shell 长大了」的必然后果,顺带交代。

### 查文件信息:struct stat 与 InodeOps::stat

第二块能力是「查文件信息」。先定义结构,沿用 Linux x86_64 的 `struct stat` 布局:

```cpp
struct stat {
    uint64_t st_dev;      // 设备 ID
    uint64_t st_ino;      // inode 号
    uint32_t st_mode;     // 类型 + 权限
    uint32_t st_nlink;    // 硬链接数
    uint32_t st_uid;      // 属主
    uint32_t st_gid;      // 属组
    uint64_t st_rdev;     // 特殊文件的设备 ID
    int64_t  st_size;     // 字节数
    uint64_t st_blksize;  // 建议 I/O 块大小
    uint64_t st_blocks;   // 占用的 512 字节块数
    uint64_t st_atime;    // 访问时间
    uint64_t st_mtime;    // 修改时间
    uint64_t st_ctime;    // 状态改变时间
};
```

沿用 Linux 布局是有好处的:将来真要跑为 Linux 写的简单程序,结构体能对得上。但「沿用布局」不等于「字段都有意义」——下面会看到,有些字段 Cinux 填不了。

要把磁盘 inode 翻译成 `stat`,给 `InodeOps` 加第 7 个虚方法 `stat`,并给 `Inode` 结构补上几个字段(mode/uid/gid/nlink/atime/ctime/mtime/blocks),让 VFS 层的 inode 自己就带着这些元数据(`populate_vfs_inode` 在 lookup 时顺带填好)。ext2 的实现很直白——就是字段对字段地拷:

```cpp
int64_t Ext2FileOps::stat(const Inode* inode, struct stat* st) {
    auto* cached = static_cast<const Ext2CachedInode*>(inode->fs_private);
    const Ext2Inode& disk = cached->disk_inode;

    st->st_dev     = 0;                    // 无设备号概念
    st->st_ino     = inode->ino;
    st->st_mode    = disk.i_mode;
    st->st_nlink   = disk.i_links_count;
    st->st_uid     = disk.i_uid;
    st->st_gid     = disk.i_gid;
    st->st_rdev    = 0;                    // 无特殊设备
    st->st_size    = disk.i_size;
    st->st_blksize = ext2_.block_size();
    st->st_blocks  = disk.i_blocks;
    st->st_atime   = disk.i_atime;         // 来自磁盘(无 RTC,全 0)
    st->st_mtime   = disk.i_mtime;
    st->st_ctime   = disk.i_ctime;
    return 0;
}
```

这里要诚实交代几个「填不了的」:`st_dev` 和 `st_rdev` 是 0,因为 Cinux 这会儿没有「设备号」的概念(它只有一个 AHCI 盘、一个 ext2,不需要用主从设备号区分);三个时间戳直接取磁盘 inode 里的,而 028b 已经说过 Cinux 没有实时时钟,这些时间戳全是 0。所以你 `stat` 一个文件,看到的时间是 1970 年初——不是 bug,是还没接 RTC。

`Ext2DirOps::stat`(目录版本的)和上面这个**逐字相同**。文件和目录的 stat 在 ext2 里没有区别(都从同一个 `disk_inode` 拷字段),所以两份代码一模一样。这是个重复,不是精心设计——后面真要整洁,可以把它提到一个公共 helper,或者干脆让基类提供默认实现。留个尾巴,lab 里可以动手。

syscall 侧,`sys_stat` 把这条链路接通:

```cpp
int64_t sys_stat(uint64_t path_virt, uint64_t st_virt, ...) {
    if (!validate_user_ptr(st_virt)) return -1;

    char resolved[PATH_MAX];
    resolve_user_path(path_virt, resolved);        // cwd-aware 解析

    FileSystem* fs = vfs_resolve(resolved, &rel_path);
    Inode* inode = fs->lookup(rel_path);

    cinux::fs::stat kst;
    inode->ops->stat(inode, &kst);                 // 后端填
    memcpy(reinterpret_cast<stat*>(st_virt), &kst, sizeof(stat));  // 拷给用户
    return 0;
}
```

同一个文件里还有 `sys_fstat`,差别只在「怎么拿到 inode」:`sys_stat` 靠路径(lookup),`sys_fstat` 靠文件描述符——从全局 fd 表 `g_global_fd_table.get(fd)` 取出 `File`,再拿它的 `inode`,剩下的 `stat` + 拷贝完全一样。

四个新系统调用的号码我们继续复用 Linux:`stat=4`、`fstat=5`、`chdir=12`、`getcwd=79`。加上 shell 的 `cd`(`sys_chdir`)、`pwd`(`sys_getcwd`)、`stat`(`sys_stat`)命令,这一章的用户态接口就齐了。

## 设计现场

028c 没有崩溃调试的 note,但有两个写在代码里的真实隐患,值得拿出来讲——它们都是「要是漏了这一步,功能就错或者炸」的点。

### cd /.. 不能越过根:canonicalize 的根目录保护

`path_canonicalize` 处理 `..` 的那几行,有个不起眼但关键的保护:

```cpp
if (comp_len == 2 && ... '..' ...) {
    if (out_pos > 1) {            // ← 只有不在根时才回退
        --out_pos;
        while (out_pos > 0 && out[out_pos - 1] != '/') --out_pos;
        if (out_pos > 1) --out_pos;
    }
    continue;
}
```

那个 `if (out_pos > 1)` 是给根目录兜底的。`out[]` 一开始就写了 `out[0] = '/'`,`out_pos = 1` 表示「现在只有根」。如果输入是 `/..`(或者 `/a/b/../../..` 这种一路 `..` 想越过根的),处理 `..` 时 `out_pos` 已经是 1(在根),这个 `if` 不成立,直接跳过——结果还是 `/`。

POSIX 对此有明确规定:路径解析时,根目录的 `..` 仍是根,你不能「.. 出根」。如果漏了这个保护,`out_pos` 会被减到 0 甚至下溢,`out[]` 缓冲就会被越界写——轻则路径乱掉,重则写穿栈。这是个典型的「边界条件漏一个就崩」的隐患,代码里用一行 `if` 守住了。

### resolve_user_path 依赖 current,所以必须先 set_current

第二个隐患是上面已经埋下的伏笔。`resolve_user_path` 里有这么一句:

```cpp
cinux::proc::Task* current = cinux::proc::Scheduler::current();
const char* cwd = (current != nullptr) ? current->cwd : "/";
```

它对 `current == nullptr` 有兜底(退回 "/"),所以不会直接崩。但想想:如果第一个用户进程跑起来时没有 `set_current`,`current()` 一直返回 `nullptr`,那**所有**相对路径都会被当成相对于 "/" 解析——`cd etc` 会被当成 `cd /etc`(碰巧可能对),但更微妙的是 `sys_chdir` 后面还要 `Scheduler::current()` 来写 cwd,那个调用 `sys_chdir` 里是 `if (current == nullptr) return -1;`,直接失败——也就是说,没有 `set_current`,`chdir` 永远成功不了,`pwd` 永远显示 "/"。

这就是为什么 `launch_first_user` 必须在跳进用户态前 `set_current(&shell_task)`:不是为了让 shell「能跑」(没有它 shell 也能跑,只是 syscall 行为错),而是为了让 cwd 这套机制有「当前进程」可依附。新功能依赖一个前置条件,补丁就是来满足这个前置条件的。这种「功能正确性依赖一个看似无关的初始化」的依赖关系,比一个直接崩溃更难发现——因为症状是「功能静默地不工作」而不是「炸了」。

## 验证

028c 的测试叫 `test_cwd_stat`,分 host 单测和 QEMU kernel 测试。

**host 单测**(纯函数,不碰硬件):

```bash
cmake --build build
ctest --test-dir build -R cwd_stat --output-on-failure
```

它重点测两块纯逻辑:`path_canonicalize`(喂一批 `/a/b/../c`、`/a/./b//c`、`/..`、`/a/b/../../..` 之类的输入,断言规范化结果,尤其验根目录保护)和 `path_resolve`(相对/绝对路径拼接),以及 `stat` 的字段映射。这些是字符串和结构体操作,host 上就能跑全。

**QEMU kernel 测试**(端到端):

```bash
cmake --build build --target run-kernel-test
```

(和 028b 一样,它会先 `regenerate-ext2-image` 重建干净的 ext2 盘。)

**预期现象**。在 shell 里:

```text
$ pwd                  # sys_getcwd → 当前 cwd
/
$ cd etc               # sys_chdir("/etc") → 验证是目录 → 写 cwd
$ pwd
/etc
$ stat motd            # 相对路径解析成 /etc/motd(它确实存在),打出 size、type、inode 号
$ cd /                 # 回根
$ stat /hello.txt      # 绝对路径也能 stat
```

几个能验证的点:`cd` 进非目录会被拒(`sys_chdir` 的类型检查);相对路径能正确解析(依赖 cwd);`stat` 打出的 size 和 debugfs 看到的一致(详见 lab);`pwd` 反映 `cd` 后的状态。

## 下一站

028c 让文件系统有了工作目录和文件信息查询,还顺手把路径处理收拢成了公共模块。到此,单个进程视角下的文件系统已经很完整:能读写、能建删、能 cd、能 stat。但你可能已经察觉到一个隐患——这一路下来,我们所有的写操作(`echo >`、`touch`、`mkdir`)都是「直接写盘」,中途要是断了电,文件系统就可能处于半写完的不一致状态(位图改了、inode 没写回,或者反过来)。而且多个操作之间没有任何同步保护。下一章(028d)会引入同步原语(spinlock)和一系列内存管理上的安全加固,给这些操作加上「并发安全」和一定的「一致性」保障。怎么加锁、加在哪些临界区,那是 028d 的事——我们这一章的文件系统,已经能认得相对路径、能告诉你一个文件长什么样了。

---

**参考**

- POSIX 路径解析(4.11 Pathname Resolution):`.` 指当前目录、`..` 指父目录、根目录的 `..` 仍是根——`path_canonicalize` 的根目录保护由此而来。参见 Open Group Base Specifications。
- Linux man-pages:`chdir(2)`(目标须为目录)、`getcwd(2)`(返回工作目录)、`stat(2)`/`fstat(2)`(`struct stat` 字段含义);syscall 号 4/5/12/79 复用 Linux x86_64:<https://man7.org/linux/man-pages/>。
- Linux `struct stat`(x86_64)字段布局,`stat.hpp` 注释声明"follows the Linux x86_64 convention":<https://man7.org/linux/man-pages/man2/stat.2.html>。
- x86_64 规范地址(canonical address):有效虚拟地址仅低 48 位,第 47 位决定高 16 位的符号扩展——`validate_user_ptr` 的依据。参见 AMD64 APM / Intel SDM 卷 1。
