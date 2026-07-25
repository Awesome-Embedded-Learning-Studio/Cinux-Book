---
title: 02 · ShmRegistry 表层:承 071 的模子,固定 16 槽 + index-as-handle
---

# ShmRegistry 表层:承 071 的模子,固定 16 槽 + index-as-handle

承 071 的「传输/应用不混」分层铁律,shm 也劈两层。先看纯逻辑层 `ShmRegistry`(`kernel/ipc/shm.{hpp,cpp}`)。

它是一张固定大小的内存表:`kShmRegistryMax = 16` 个槽(`shm.hpp:67`),每槽一个 `ShmSegment`(`shm.hpp:99-110`):

```cpp
struct ShmSegment {
    bool     used{false};
    int      key{0};                     // IPC_PRIVATE (0) or user-supplied key
    uint64_t phys_base{0};               // physical base of the page run
    uint64_t page_count{0};              // number of 4 KB pages mapped
    uint64_t size{0};                    // requested size in bytes (for IPC_STAT)
    uint16_t mode{0666};
    uint32_t nattach{0};                 // live shmat count
    uint32_t cpid{0};                    // creator tid
    uint32_t lpid{0};                    // last shmat / shmdt tid
    bool     marked_for_removal{false};  // IPC_RMID set; freed when nattach hits 0
};
```

([shm.hpp](../../../kernel/ipc/shm.hpp#L99-L110),有删节。)一把 `Spinlock` 串行化所有操作(`shm.hpp:206`)。registry 的全部 API 就这几件:`find_by_key`(按 key 查)、`add`(占个空槽)、`attach`(快照 + nattach++)、`detach`(nattach--,marked 且归零就返回 phys_base 给调用方 free)、`mark_removal`(IPC_RMID)、`stat`(IPC_STAT 填 shmid_ds)、`find_by_phys`(shmdt 用 phys 反查 shmid)。

关键证据在 `add` 的签名:

```cpp
cinux::lib::ErrorOr<int> add(int key, uint64_t size, uint64_t phys_base, uint64_t page_count,
                             uint16_t mode);
```

([shm.hpp](../../../kernel/ipc/shm.hpp#L143-L144)。)`phys_base` 是**入参**——调用方(syscall 层)已经 alloc 完物理页把基址传进来。registry 自己**从不碰 PMM**:不开页、不关页、不增减计数器。物理页生命周期全部下沉到 `kernel/syscall/sys_shm.cpp`。这就是分层:`ShmRegistry` 只管「这张表里有什么」,`sys_shm` 管「物理页是死是活」。

> 跟 071 是同一个模子。`FifoRegistry`(`fifo.hpp:96` 的 `class FifoRegistry` + `:99` 的 `instance()` 单例 + `:122` 的 `entries_` 固定表)是「名字→FIFO」的纯内存表;`ShmRegistry` 是「key→segment」的纯内存表。俩都是 `FIFO_REGISTRY_MAX=16` / `kShmRegistryMax=16` 的定长表(避开 `<map>`/`<string>`),都是 index-as-handle(shmid 就是表里第几个槽),都是一把 Spinlock 串行化。071 立了模子,这卷第三次复用。

这个分层的真好处:**`ShmRegistry` 全是表操作,零 kernel-only 依赖**(无 PMM、无 AddressSpace、无 kprintf——`shm.cpp:1-9` 头注释明说「no kprintf / no I/O, links into host unit tests like fifo.cpp」),所以这层若要 host 单测,设计上零摩擦。但(诚实标一句)目前 Book 没给它配 host 单测——`test/unit/` 无 `test_shm`、`test/CMakeLists.txt` 无对应行;这层纯逻辑当前只被 6 例 ring0 syscall 测顺带覆盖。对照看 `FifoRegistry`:`test/unit/test_fifo.cpp` 存在,并入了 ctest,有独立的 host 回归网。`ShmRegistry` 没有对称物——这是 **fifo↔shm 的对称缺口**,不是 bug。后续要补,设计上零摩擦(同 fifo.cpp 的链法)。

分层叙事本身是教学点。这卷的姊妹章 081 tmpfs 把「纯逻辑层(`tmpfs.cpp` 无 kprintf)vs boot I/O 层(`tmpfs_init.cpp`)」分 TU;071 把「传输(`Pipe`)不混应用(`FifoRegistry` 命名)」分清。shm 是同一套切法的第三次复用:纯逻辑表(`ShmRegistry`)管「key→segment 簿记」,syscall 层(`sys_shm.cpp`)管「物理页是死是活」。这是 Cinux 一以贯之的切法——**把可单测的纯逻辑跟 kernel-only 的副作用分开**,既让纯逻辑可独立验证,也让副作用集中在一条路径上好审查。

shmid=表索引(0..15)是教学简化。Linux 真 shmid 是 `inx + seq`(序列号编码),防一个 stale 句柄被快速复用后打到新段。Book 固定 16 槽 + index 即 handle,代价是 RMID 后槽位被复用时,stale shmid 会命中新段。`shm.hpp:24-26` 头注释把这个取舍明说了——「matches FifoRegistry's index-as-handle model」。代价也一并摆这儿:RMID 后槽位被复用,stale shmid 会命中新段,别只看省事那一面。
