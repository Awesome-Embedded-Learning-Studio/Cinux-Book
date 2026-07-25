---
title: 09 · 主线八:host PAL——脱开 QEMU 上 TSAN 抓 race
---

# 主线八:host PAL——脱开 QEMU 上 TSAN 抓 race

## 主线八:host PAL——让 ext2 在 host 上脱开 QEMU,上 TSAN 抓 race

主线一开头咱们说过:QEMU forensics 漏掉的那一类 race,真正能秒抓的是 **TSAN**——它直接报「这块内存在线程 A 读、线程 B 写」。可上 TSAN 的前提是 ext2 能在 **host 上脱开 QEMU 直接跑**。搬家到 `libs/ext2/` 是表,让这一层成为可能才是底。这一节就把这条动机回环讲完:host PAL 怎么 mock 掉内核依赖、host 单测怎么验真逻辑、host 并发压测怎么让 TSAN 把 `block_buf_` race 喊出来。

### 为什么 ext2 能在 host 上跑——PAL 切了哪几刀

ext2 的源码(`libs/ext2/ext2_*.cpp`)在内核里编一份(`-mcmodel=kernel`),现在又**在 host 上编第二份**——直接用 host 的 codegen,跟 `net_tcp` 那条 host 测试同一个路子。可 ext2 调了几个只有内核才有的符号,host 上没有:

- `cinux::lib::kprintf` / `kvprintf` / `kpanic`:内核版走 serial + x86 inline asm(`outl` / `cli` / `hlt`),host 上没串口;
- `cinux::mm::kmalloc` / `kfree`:内核版路由到 PMM/buddy slab,host 上没有那套页管理。

host PAL 的活就是给这几个符号提供**libc 后端**,让 ext2 链得过、跑得动。`test/unit/ext2_host_pal.cpp` 是这块 PAL 的全部:

```cpp
namespace cinux::lib {

void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);  // host: route to stdout
    va_end(args);
}

void kvprintf(const char* fmt, va_list args) {
    vprintf(fmt, args);
}

[[noreturn]] void kpanic(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    abort();
}

// Sink registration is a kernel multi-backend concept (serial + framebuffer +
// QEMU debugconsole). The host PAL has a single implicit stdout sink (the
// vprintf above), so these are no-ops.
void kprintf_register_sink(OutputSink /*fn*/, void* /*ctx*/) {}
void kprintf_set_sink_enabled(OutputSink /*fn*/, void* /*ctx*/, bool /*enabled*/) {}
void kprintf_enable_all_sinks() {}
void kprintf_init() {}

}  // namespace cinux::lib
```

([ext2_host_pal.cpp:29-58](../../../test/unit/ext2_host_pal.cpp#L29))`kprintf` 在 host 上就是 `vprintf` 往 stdout,kpanic 直接 `abort`。sink 注册那一套是内核的多后端(serial + framebuffer + debugconsole)概念,host 只有一个隐式 stdout sink,所以全是 no-op。

`kmalloc` / `kfree` 同理走 libc,但有一个**对齐 + 清零契约**要守:

```cpp
void* kmalloc(size_t size, size_t align) {
    if (size == 0) return nullptr;
    if (align < sizeof(void*)) align = sizeof(void*);
    // aligned_alloc requires size to be a multiple of alignment.
    size_t rounded = (size + align - 1) & ~(align - 1);
    void* p = aligned_alloc(align, rounded);
    // Match the kernel slab contract: returned memory is zeroed (no stale-data
    // leak), which ext2 scratch buffers (KmBuf) and the allocator paths assume.
    if (p != nullptr) {
        memset(p, 0, rounded);
    }
    return p;
}

void kfree(void* ptr) {
    free(ptr);
}
```

([ext2_host_pal.cpp:60-80](../../../test/unit/ext2_host_pal.cpp#L60))。注意那个 `memset(p, 0, rounded)`——内核 slab 返回的内存是清零的(无 stale-data 泄漏),ext2 的 `KmBuf` scratch buffer 和分配路径都依赖这点。host PAL 必须守这条契约,不然 host 上跑出来的行为跟内核里不一致,测出来的就没意义。

剩下两个符号来自别处、但同一个 target 里:`cinux::proc::Spinlock` 由 `host_spinlock.cpp` 提供(用 libc 的 `pthread_mutex` 实现 `Spinlock::guard`),`inode_ref` / `inode_unref` 由 `kernel/fs/file.cpp` 直接链进来(这俩是纯逻辑、host-safe,跟 devfs 测试一个先例)。这三个 PAL + 一个 ext2 库 = host 上能跑真 ext2 的全部材料。

### host 单测:真逻辑往返读写 + 硬链接

PAL 备齐,先验 ext2 在 host 上跑得对——`test/unit/test_ext2_host.cpp` 挂一张预构的 64 KiB ext2 镜像(`test/data/ext2_test.img`,由 `mke2fs -d` 预填 `/etc/motd` + `/hello.txt`)进一个 `RAMBlockDevice`,然后走**完整 VFS 路径**:

```cpp
Ext2 ext2(&dev);
ASSERT_OK(ext2.mount());

// --- readdir "/" : expect etc/ and hello.txt (index 0/1 are "." / "..") ---
auto root_r = ext2.lookup("/");
...
for (uint64_t i = 2; i < 64; ++i) {
    char nm[256];
    auto r = root->ops->readdir(root, i, nm, 255);
    ASSERT_TRUE(r.ok());
    if (*r == 0) break;  // end of directory
    if (std::strcmp(nm, "etc") == 0) found_etc = true;
    if (std::strcmp(nm, "hello.txt") == 0) found_hello = true;
}
ASSERT_TRUE(found_etc);
ASSERT_TRUE(found_hello);

// --- multi-level lookup + read /etc/motd ---
auto motd_r = ext2.lookup("/etc/motd");
...
    char buf[64];
    auto rd = motd->ops->read(motd, 0, buf, sizeof(buf) - 1);
    ASSERT_TRUE(rd.ok());
    ASSERT_GT(*rd, 0);
    buf[*rd] = '\0';
    ASSERT_TRUE(std::strstr(buf, "hello ext2 motd") != nullptr);
```

([test_ext2_host.cpp:58-88](../../../test/unit/test_ext2_host.cpp#L58))。注意它走的不是某个 mock 出来的接口,而是**真 ext2 的真 `Ext2::lookup` / 真 `InodeOps::read`**——`RAMBlockDevice` 只是替 NVMe/AHCI 把盘字节握在内存里,I/O 路径上别的逻辑全真的。这条测下来绿,说明 ext2 在 host 上跑得对:挂载、多层 lookup、readdir、read 都对得上镜像内容。

写路径也压——create + write + read-back + unlink 一条往返:

```cpp
// --- create /newfile + write + read-back (exercises block allocator) ---
auto newf_r = root->ops->create(root, "newfile", 7);
...
const char  payload[] = "hello-write";
const auto  plen = static_cast<int64_t>(sizeof(payload) - 1);
auto        wr = newf->ops->write(newf, 0, payload, static_cast<uint64_t>(plen));
ASSERT_TRUE(wr.ok());
ASSERT_EQ(*wr, plen);

char  rb[32];
auto  rb_r = newf->ops->read(newf, 0, rb, sizeof(rb) - 1);
ASSERT_TRUE(rb_r.ok());
ASSERT_EQ(*rb_r, plen);
rb[*rb_r] = '\0';
ASSERT_TRUE(std::strstr(rb, "hello-write") != nullptr);
```

([test_ext2_host.cpp:90-107](../../../test/unit/test_ext2_host.cpp#L90))。这一段最关键的是它**走了块分配器**——`create` 要分一个新 inode、`write` 要分一个新数据块,正是 `block_alloc_lock_` 要保护的那条 RMW 路径。在 host 上能跑通,说明新 ext2 的写+分配逻辑跟读路径一样,脱开 QEMU 也是对的。它跑在 ASAN 下,UAF / OOB / leak 都在毫秒级冒出来——这是 QEMU forensics 想要但抓不到的确定性。

> 这一节顺带说一个**先验链接**的测试:`test_ext2_host_link.cpp` 不验语义,只验「host PAL 备齐了」——构造一个 `Ext2` over `RAMBlockDevice`、调 `mount()`(零填充盘 → superblock magic 校验失败 → 返回 not ok)、析构。它链得过来、不崩,PAL 覆盖就完整了。这是 host 测试基建的「make-or-break」门槛,真语义留给上面那条 host 测。详见 [test_ext2_host_link.cpp:37-47](../../../test/unit/test_ext2_host_link.cpp#L37)。

### host 并发压测:TSAN 秒抓 `block_buf_` race

PAL 备齐、单线程真逻辑验对,接下来才是这条搬家绳真正的 payoff——**让 TSAN 把 `block_buf_` 这一类 race 喊出来**。`test/unit/test_ext2_concurrent.cpp` 起两个 stressor,每个都 N 线程压**同一个** `Ext2` + `RAMBlockDevice`:

**Stressor 1:并行 `alloc_block` / `free_block`,压 `block_alloc_lock_`**:

```cpp
// N threads each perform 100 alloc/free cycles on the SAME Ext2 instance. All
// bitmap / superblock / BGDT updates run under block_alloc_lock_; a missing
// guard shows up as a TSan data-race report on the bitmap bytes or free-count
// fields.
TEST("ext2_concurrent: parallel alloc_block / free_block (TSAN)") {
    ...
    constexpr int kThreads = 4;
    constexpr int kIters   = 100;
    std::thread   threads[kThreads];
    for (int t = 0; t < kThreads; ++t) {
        threads[t] = std::thread([&ext2]() {
            for (int i = 0; i < kIters; ++i) {
                uint32_t b = ext2.alloc_block();
                if (b != 0) {
                    ext2.free_block(b);
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
}
```

([test_ext2_concurrent.cpp:51-83](../../../test/unit/test_ext2_concurrent.cpp#L51))。4 线程 × 100 次循环,每次都进 bitmap RMW。如果 `block_alloc_lock_` 漏了某处,TSAN 在几毫秒内就会报「bitmap 字节被线程 A 写、线程 B 读」——这就是主线六那把锁要拦的东西。

**Stressor 2:并行 `lookup("/etc/motd")`,压 `block_buf_` + `inode_cache_lock_`**:

```cpp
// Regression for the shared block_buf_ race found by the original M6 TSan run:
// lookup_in_dir() now owns one KmBuf per call and reuses it across directory
// blocks, so concurrent path resolution never shares scratch storage.
TEST("ext2_concurrent: parallel lookup (TSAN)") {
    ...
    constexpr int    kThreads = 4;
    constexpr int    kIters   = 200;
    std::atomic<int> failures{0};
    std::thread      threads[kThreads];
    for (int t = 0; t < kThreads; ++t) {
        threads[t] = std::thread([&ext2, &failures]() {
            for (int i = 0; i < kIters; ++i) {
                auto result = ext2.lookup("/etc/motd");
                if (!result.ok() || result.value() == nullptr) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                inode_unref(result.value());
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    ASSERT_EQ(failures.load(std::memory_order_relaxed), 0);
}
```

([test_ext2_concurrent.cpp:88-125](../../../test/unit/test_ext2_concurrent.cpp#L88))。这条是**直接回归 `block_buf_` race 的**——4 线程 × 200 次并发 `lookup`,每次 lookup 进 `lookup_in_dir` 都要 `read_block` 目录块。注释里写得很直白:「`lookup_in_dir()` now owns one KmBuf per call and reuses it across directory blocks, so concurrent path resolution never shares scratch storage」——也就是说,如果哪天有人把 `KmBuf` 改回共享 `block_buf_`,这条测在 TSAN 下会立刻报「`block_buf_` 在线程 A 读、线程 B 写」,正是主线一描述的那种 wild 块号 race 的根。这一条是 `block_buf_` 治理的**确定性回归证据**,不是 `run-kernel-test` 跑全绿那种概率证据。

构建开关也讲清:`-DCINUX_HOST_TSAN=ON` 给所有 host 测试加 `-fsanitize=thread`,跟 ASAN 互斥(见 `test/CMakeLists.txt:38-41`);`test_ext2_concurrent` 显式链 `-pthread`(`test/CMakeLists.txt:327`)。镜像路径由 CMake 注入:`EXT2_TEST_IMG_PATH="${CMAKE_SOURCE_DIR}/test/data/ext2_test.img"`([test/CMakeLists.txt:325-326](../../../test/CMakeLists.txt#L325))。

> **TSAN 和 lockdep 的分工**。内核里有 lockdep,但它只做 lock-order(锁序)检查,看不见「两个线程摸同一个字段、却没加锁」这种 race。TSAN 正好补这一块——它不需要你声明「这块内存归哪把锁管」,它直接看内存访问。所以 host 上跑 TSAN,是把内核 lockdep 看不见的那一类 race,在 host 上用确定性工具捞出来。这正是这一节存在的意义。
