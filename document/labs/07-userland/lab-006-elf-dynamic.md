---
title: Lab 006 · ELF 动态链接验证
---

# Lab 006 · ELF 动态链接验证

> 对应 `document/book/07-userland/006/`。验证档 **A 档**:这一章交付的是动态链接的 musl hello 真能跑。可端到端 smoke 要先在宿主机编出 musl 动态工具链(`libc.so` + `hello-dyn` + ext2 装 interp),本机没编就跳过。内核侧的改动(PT_INTERP 识别、interp 加载、auxv、ET_DYN validate)靠 host 单测 + 内核测试罩,跟 004 静态 smoke 同样的分层。

## 目标

确认六件事:

1. **内核不自建 loader**:PT_INTERP → 读 interp 路径 → 加载 ldso → 改入口 + auxv,就这三件,重定位是 ldso 的事;
2. **`load_elf_image` 让主程序和 interp 共用 PT_LOAD 映射**:主 base=0,interp base=`USER_INTERP_BASE`;
3. **validate 收 ET_DYN**(interp 必需);
4. **musl 动态工具链**:`libc.so` 即 interp,`build-hello-dyn.sh` 产非 PIE 动态 hello,ext2 装 interp;
5. **ext2 4096 块绕过双重间接坑**:interp 822 KB 超 1024 块的单间接极限,改 4096 块;
6. **端到端动态 smoke**(若编了工具链):`/hello-dyn` 跑出 `Hello from musl`。

## 步骤

### 1. host 单测:ET_DYN validate

```bash
./build/test/test_fork_exec
```

应看到 `90 passed, 0 failed`,里头有 `test_valid_et_dyn`——interp 是 `ET_DYN`,validate 得收它(以前只收 `ET_EXEC`)。这一步顺带给 PIE 主程序铺路(主程序 PIE 也是 `ET_DYN`)。

### 2. load_elf_image:主 + interp 共用映射

```bash
sed -n '38,70p' kernel/proc/elf_load.hpp
```

应看到 `struct LoadedImage{entry, phdr_va, max_seg_end, has_load}` + `load_elf_image(space, inode, ehdr, phdrs, phnum, base, out)` + `load_interpreter(space, path, out_base, out_entry)`。注释会说:主程序 base=0(非 PIE 绝对 VA),interp base=`USER_INTERP_BASE`(`ET_DYN` base 相对)。这是把 004 那版 inline 在 execve 里的 PT_LOAD 映射抽出来,主和 interp 共用。

### 3. PT_INTERP 检测 + interp 加载

```bash
sed -n '256,282p' kernel/proc/execve.cpp
```

应看到扫程序头找 `PT_INTERP`,读到 `interp_path`(NUL 终止),置 `has_interp`。这是内核识别「这是个动态 ELF」的那一下——静态 ELF 没这个段,`has_interp` 保持 false 走原路。

### 4. USER_INTERP_BASE

```bash
sed -n '88,95p' kernel/arch/x86_64/memory_layout.hpp
```

应看到 `USER_INTERP_BASE = 0x10000000`(256 MB),注释说它卡在 heap(64 MB)和 mmap 区(4 GB)之间的空隙。ldso 是 `ET_DYN`,任意 base 都能跑(靠 `__ehdr_start` 自定位),取个固定值;ASLR 化留 follow-up。

### 5. ext2 4096 块:绕过双重间接坑

这是这一章最大的坑,值得看一眼怎么绕的:

```bash
sed -n '52,58p' scripts/create_ext2_disk.sh
```

应看到 `BLOCK_SIZE=4096` + 一段注释解释:内核 ext2 读只处理 direct + single-indirect,double-indirect 截断;1024 块下文件上限 ~268 KB,装不下 musl ldso(822 KB);4096 块把单间接上限推到 ~4 MB,interp 落进去。**真正的修——给 ext2 补 double/triple-indirect——留 ext2 间接块那卷**,这里用配置绕。

### 6. 动态 hello 的形态(readelf,需先编工具链)

要先编 musl 动态工具链(sysroot 里要有 `libc.so`):

```bash
tools/musl/build-musl.sh           # 产 build/musl-sysroot/lib/libc.so(= interp)
tools/musl/build-hello-dyn.sh      # 产 build/musl/hello-dyn
```

然后看动态 hello 的 ELF 形态:

```bash
readelf -hl build/musl/hello-dyn | head -25
```

应看到:`ET_EXEC`(非 PIE)+ `INTERP(/lib/ld-musl-x86_64.so.1)` + `DYNAMIC` 段 + 几个 `PT_LOAD`(R / R+E / R / RW)。`-no-pie` 保证是 `ET_EXEC`(主程序走绝对地址);若是 PIE 会变 `ET_DYN`,得走 PIE 重定位(follow-up)。`file` 也该报 `dynamically linked, interpreter /lib/ld-musl-x86_64.so.1`。

### 7. 端到端动态 smoke(需工具链 + ext2 装 interp)

`create_ext2_disk.sh` 会把 `hello-dyn` 和 interp(`libc.so` → `/lib/ld-musl-x86_64.so.1`)装进 ext2(两者同时存在才装)。然后开 smoke:

```bash
cmake -B build -DCINUX_MUSL_DYN_SMOKE=ON -DCINUX_MUSL_HELLO_SMOKE=OFF
cmake --build build --target run-kernel-test 2>&1 | grep -iE "interp|hello-dyn|Hello from musl" | head
```

应看到:`[EXECVE] loaded interp /lib/ld-musl-x86_64.so.1 base=0x10000000 entry=...` + 几行 `Hello from musl on CinuxOS!` + `hello-dyn 5/5 iters PASS`。这就是动态链接端到端跑通:内核加载 interp、喂对 auxv,musl ldso 重定位主程序,跳 `AT_ENTRY`,`write` 经 `libc.so` 输出。

没编工具链的话,smoke 跳过(`CINUX_MUSL_DYN_SMOKE` 默认 OFF),内核侧改动靠前面六步(尤其步骤 1 的 host 单测 + 步骤 2/3/4 的源码锚点 + 步骤 5 的 ext2 块)验。

## 验收清单

- [ ] `./build/test/test_fork_exec` 报 90 passed(含 `test_valid_et_dyn`)。
- [ ] `elf_load.hpp:64` `load_elf_image`(主 base=0 + interp base=`USER_INTERP_BASE` 共用);`:73` `load_interpreter`。
- [ ] `execve.cpp:256` 扫 `PT_INTERP` 读 `interp_path`;有 interp 改入口成 interp entry + auxv 喂 `AT_BASE`/`AT_ENTRY`/`AT_PHDR`。
- [ ] `memory_layout.hpp:94` `USER_INTERP_BASE = 0x10000000`(256 MB,heap/mmap 间空隙)。
- [ ] `create_ext2_disk.sh:58` `BLOCK_SIZE=4096`(绕 double-indirect;真修留 ext2 间接块卷)。
- [ ] (编了工具链)`readelf` 见 `ET_EXEC + INTERP + DYNAMIC + PT_LOAD`;`CINUX_MUSL_DYN_SMOKE=ON` 跑出 `Hello from musl` + `hello-dyn 5/5 PASS`。
- [ ] 知道内核**不自建 loader**:GOT/PLT/`DT_NEEDED`/符号解析/重定位全是 musl ldso 用户态的事。

## 别做这些

- **别**以为内核要解析 GOT/PLT/`DT_NEEDED`——那些全是 ldso 在用户态干的。内核只装载 interp + 喂 auxv,「重定位」一词都不沾。这是对齐 Linux 的分工。
- **别**指望本机直接跑动态 hello——`PT_INTERP` 是绝对路径 `/lib/ld-musl-x86_64.so.1`,host(glibc)没这文件。要在 QEMU 的 ext2 镜像里装 interp 才跑得起来。host 上只能 `readelf` 验二进制形态。
- **别**以为 ext2 双重间接「修好了」——只是用 4096 块把单间接上限从 268 KB 推到 4 MB,让 822 KB 的 interp 读得到。ext2 块映射的 double/triple-indirect 缺失是真 follow-up(后面 ext2 间接块卷),>4 MB 的文件现在还是会读截断。
- **别**以为主程序是 PIE——这一章主程序是非 PIE `ET_EXEC`(base=0 绝对地址)。interp 是 `ET_DYN`,但主程序 PIE 的重定位(`R_X86_64_RELATIVE`)留 follow-up。`test_valid_et_dyn` 只是给 validate 收 `ET_DYN` 铺路,不代表主程序已经 PIE。
- **别**把 `USER_INTERP_BASE` 当 ASLR——它是固定值。interp base 的随机化要配 PIE 一起做,这一章不做。
