---
title: 03 · 实现:resolve_user_path、规范化、Task::cwd、stat
---

# 实现:resolve_user_path、规范化、Task::cwd、stat

### 先把散落的路径逻辑收拢:resolve_user_path

先看这一章最直接的一笔「去重」。006 的 `sys_creat` 开头长这样(简化):

```cpp
// 校验用户指针(canonical address)
if (path_virt == 0) return -1;
uint64_t bit47 = (path_virt >> 47) & 1;
uint64_t upper = path_virt >> 48;
if (bit47 == 0 && upper != 0) return -1;
if (bit47 == 1 && upper != 0xFFFF) return -1;
// ...然后 split_pathname 拆父目录和叶子...
```

这段「canonical 校验」在五个 syscall 里一字不差地重复。007 把它抽成 `path_util.hpp` 里一个 inline 函数:

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

为什么 x86_64 要做这个校验?64 位虚拟地址里,只有低 48 位是有效的,第 47 位决定「符号扩展」:第 47 位为 0 时,高 16 位(48..63)必须全 0;为 1 时必须全 1。符合这个规则的叫「规范地址(canonical address)」,不符合的访问直接触发 #GP。用户态传进来的指针内核不能信,得先确认它是规范地址,否则一旦解引用就崩。这个道理 006 讲过,007 把它从五份拷贝变成一个函数。

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

注意中间那行:`cwd` 取自「当前进程」的 `Task::cwd`,如果拿不到 current 就退回根目录 "/"。这一行是 cwd 支持的核心——相对路径到底是相对谁,答案就是「当前进程的工作目录」。006 的各 syscall 改成调这个函数后,既去掉了重复,又自动获得了 cwd 支持。

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

工作目录是「每个进程一份」的状态,所以它得挂在进程结构上。007 给 `Task` 加了一个字段:

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

007 的解决办法是个实用主义的小补丁:在 `launch_first_user` 跳进用户态之前,手动造一个 `Task`,设好 cwd,并把它登记为 current:

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

这是个**临时**手段。`shell_task` 是个 `static` 单一 Task,不是正经的进程创建。它的意义是「让 cwd 相关的 syscall 在调度器完整运作之前也能用」。等到后面真正实现 fork/exec、进程创建走完整流程时,每个进程自然会有自己的 Task 和 cwd,这个补丁就会被取代。但在 007,它够用。

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

这里要诚实交代几个「填不了的」:`st_dev` 和 `st_rdev` 是 0,因为 Cinux 这会儿没有「设备号」的概念(它只有一个 AHCI 盘、一个 ext2,不需要用主从设备号区分);三个时间戳直接取磁盘 inode 里的,而 006 已经说过 Cinux 没有实时时钟,这些时间戳全是 0。所以你 `stat` 一个文件,看到的时间是 1970 年初——不是 bug,是还没接 RTC。

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
