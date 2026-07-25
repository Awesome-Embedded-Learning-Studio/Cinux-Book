---
title: 065 · ELF 动态链接:内核只装 interp,重定位交给 ldso
---

# 065 · ELF 动态链接:内核只装 interp,重定位交给 ldso

> 上一章卷(059)让内核跑起了 musl 编译的静态程序——`libc.a` 整个链进可执行文件,扔到内核上就跑。那一章末尾留了句诚实话:「这一步是静态链接的,动态链接是后面的事」。这一章兑现。动态链接的程序不把 libc 链死在可执行文件里,而是带一个 `PT_INTERP`(指向动态链接器,ldso),运行时由 ldso 把程序和共享库(`libc.so`)拼到一起、做重定位。可这一章真正要讲的不是「ldso 怎么重定位」——那全是用户态的事,**内核压根不掺和**。内核在动态链接里只做三件小事:认出 `PT_INTERP`、把 ldso 装载进地址空间、喂对几张辅助向量(auxv)。剩下的(GOT/PLT/`DT_NEEDED`/符号解析/重定位)全交给 musl 的 ldso 在用户态干。这是对齐 Linux 的分工:**内核不自建 loader**。
>
> A 档:punchline 是动态链接的 musl `hello`(`hello-dyn`)真跑起来——`fork` + `execve("/hello-dyn")` → 内核加载 interp → musl ldso 重定位主程序 → 跳到 `AT_ENTRY` → `write` 经 `libc.so` 输出 `Hello from musl on CinuxOS!`。一条诚实的边界先说在前头:这个端到端 smoke 要先在宿主机上编出 musl 动态工具链(`build-musl.sh` 出 `libc.so`、`build-hello-dyn.sh` 出动态 hello、把 interp 装进 ext2 镜像),本机没编的话 smoke 跳过;内核侧的改动(PT_INTERP 识别、interp 加载、auxv)靠测试验,跟 059 那个静态 smoke 同样的分层。

## 这章咱们要点亮什么

1. **静态 vs 动态的差别在哪**:静态把 `libc.a` 链进可执行文件;动态只留一个 `PT_INTERP` 指向 ldso,运行时由 ldso 把 `libc.so` 拼进来 + 重定位。
2. **内核在动态链接里只做三件事**:认 `PT_INTERP` 读 interp 路径、把 ldso 当 `ET_DYN` 映到一个 base、把入口改成 interp 入口 + 喂对 auxv(`AT_BASE`/`AT_ENTRY`/`AT_PHDR`)。**重定位/GOT/PLT 全不是内核的事**——对齐 Linux,内核不自建 loader。
3. **抽出 `load_elf_image` 让主程序和 interp 共用一条 PT_LOAD 映射路径**:主程序 base=0(非 PIE 绝对地址),interp base=`USER_INTERP_BASE`(`ET_DYN` 是 base 相对寻址)。
4. **一个藏得很深的 ext2 坑**:interp 有 822 KB,超过了 ext2 单间接块的极限,读不到——这章用改块大小的办法绕过,真正的修(双重间接)留到后面 ext2 那卷。

## 静态和动态,差的那一下

先说清静态和动态到底差在哪。静态链接的程序(059 那个 hello)把 musl 的 `libc.a` 整个链进可执行文件——`printf` 的代码、`write` 的 syscall 封装,全在可执行文件里,文件大但自包含。动态链接的程序不链 `libc.a`,而是带一张「我要用 `libc.so`」的清单(`DT_NEEDED`)+ 一个 `PT_INTERP`(指向 ldso,比如 `/lib/ld-musl-x86_64.so.1`)。运行时:内核先把程序加载进来,看到 `PT_INTERP` 就再去加载那个 ldso,然后把控制权交给 ldso;ldso 在用户态读程序的动态表,把 `libc.so` 也映射进来,修好所有的重定位项(GOT/PLT),最后跳到程序的真正入口。

关键认识:**这一长串「读动态表、映射 libc.so、修重定位」全是用户态的事**,ldso 自己就是个普通的用户程序(只不过它专门干这个)。内核在动态链接里要做的,只有装载 + 喂对 auxv,跟「重定位」一词都不沾。这是 Linux 的分工,也是这一章的分工——咱们不写 loader,只把 ldso 装载到位、把它要的信息(auxv)喂对。

## 内核做的三件事

动态 ELF 跟静态 ELF 在内核眼里差在哪?就差一个 `PT_INTERP` 程序头。静态 ELF 没有 `PT_INTERP`;动态 ELF 有一个 `PT_INTERP`,里面是一段字符串(interp 路径,如 `/lib/ld-musl-x86_64.so.1`)。内核识别它、跟着加载,就这三件事。

**第一件:抽出 `load_elf_image`。** 059 那版的 `execve`,把 PT_LOAD 段的映射(alloc 页、清零、从 inode 读、map、记 VMA)inline 写在函数里。现在主程序要映射、interp 也要映射(它也是个 ELF,只是 `ET_DYN`),逻辑一模一样,只差一个 base。于是把这段映射抽成一个函数 `load_elf_image(space, inode, ehdr, phdrs, phnum, base, out)`(`elf_load.hpp:64`),返回一个 `LoadedImage{entry, phdr_va, max_seg_end, has_load}`(`elf_load.hpp:38`)。主程序调它 `base=0`(非 PIE,`p_vaddr` 就是绝对地址);interp 调它 `base=USER_INTERP_BASE`(`ET_DYN` 是 base 相对寻址,要加上 base)。

**第二件:扫 PT_INTERP 读 interp 路径。** 主程序映完,扫一遍程序头找 `PT_INTERP`(`execve.cpp:278`):

```cpp
char interp_path[256];
bool has_interp = false;
for (uint16_t i = 0; i < phnum; i++) {
    if (phdrs[i].p_type != elf::PT_INTERP) continue;
    uint64_t plen = phdrs[i].p_filesz;
    // 长度健全性,然后从 inode 的 p_offset 处读 plen 字节到 interp_path
    auto pread = inode->ops->read(inode, phdrs[i].p_offset, interp_path, plen);
    interp_path[plen] = '\0';   // PT_INTERP 通常自带 NUL,强制一下
    has_interp = true;
    break;
}
```

`PT_INTERP` 段的内容就是那个路径字符串,读出来。静态 ELF 没这个段,`has_interp` 保持 false,走原路。

**第三件:加载 interp + 改入口 + 喂 auxv。** 有 interp 的话,调 `load_interpreter(space, interp_path, &interp_base, &interp_entry)`(`elf_load.hpp:82`)——它 resolve + lookup interp 的 inode、读它的 ELF 头(validate 收 `ET_DYN`)、`load_elf_image` at `USER_INTERP_BASE`、`interp_entry = USER_INTERP_BASE + e_entry`(`ET_DYN` 的 entry 是 base 相对)。然后把**入口从主程序的 entry 改成 interp 的 entry**——因为要先跑 ldso,让它把主程序重定位好再跳过去。auxv 里喂三张关键的:主程序的 `AT_PHDR`(ldso 靠它定位主程序)、主程序的 `AT_ENTRY`(ldso 重定位完 `CRTJUMP` 跳过去)、interp 的 `AT_BASE`(ldso 靠 `__ehdr_start` 自定位,但 base 还是要喂)。

> 为什么 `USER_INTERP_BASE` 选 `0x10000000`(256 MB)?它得落在没人占的地方:heap 上限(64 MB)以下不行、mmap 区(4 GB 起)以上也不行,就卡在中间这片空隙,跟两边零碰撞。ldso 是 `ET_DYN`,理论上任意 base 都能跑(它靠自身的 `__ehdr_start` 定位自己),所以 base 取个固定值就行;真要 ASLR 化(每次随机 base),留 follow-up。

## musl 动态工具链:把 interp 造出来

内核侧铺好了,得有个真动态 ELF + interp 来端到端验。musl 的工具链现成:`build-musl.sh` 编出的 sysroot 里**本来就带 `libc.so`**——musl 把 ldso 和共享 libc 做成了同一个文件(`libc.so` 既是动态链接器又是共享库,二合一)。所以 interp 就是 `/lib/ld-musl-x86_64.so.1`,内容是那个 `libc.so`。

动态 hello 靠新写的 `build-hello-dyn.sh`:照静态 `build-hello.sh` 的手动 `-nostdlib` 链(musl-gcc wrapper 在 GCC≥14 坏,手动链),但去 `-static`、加 `-Wl,-dynamic-linker,/lib/ld-musl-x86_64.so.1`、加 `-no-pie`(保证产物是 `ET_EXEC` 非 PIE,主程序走绝对地址;若是 PIE 会变 `ET_DYN`,得走 PIE 重定位,那是 follow-up)。`readelf -hl` 验出来的形态该是:`ET_EXEC` + `INTERP(/lib/ld-musl-x86_64.so.1)` + `DYNAMIC` + 几个 `PT_LOAD`。

最后把 hello-dyn 和 interp 装进 ext2 镜像:`create_ext2_disk.sh` 在镜像里 `mkdir lib` + 把 `libc.so` 写到 `/lib/ld-musl-x86_64.so.1`(精确路径,跟 `PT_INTERP` 指的一致),hello-dyn 写到 `/hello-dyn`。

## 一个藏得很深的坑:ext2 的双重间接

这里踩了个这一章最大的坑,值得细讲。一切就绪,QEMU 里 `fork` + `execve("/hello-dyn")`,结果:

```
[EXECVE] loaded interp /lib/ld-musl-x86_64.so.1 base=0x10000000 entry=...
[ELF] segment read failed at offset 274432
```

interp 加载到一半,读某个段失败,offset 是 274432。这个数不是随便的——274432 ÷ 1024 = 268,正好是 ext2(1024 字节块)下 direct(12 块)+ single-indirect(256 块)的总和的**下一块**,也就是**双重间接(double-indirect)的起点**。

意思是:`libc.so` 有 822 KB,超过了 ext2 单间接能寻址的极限(1024 块 × (12+256) ≈ 274 KB)。而 CinuxOS 的 ext2 驱动,块映射只处理 direct + single-indirect,double-indirect 那个分支直接 `break` 截断了——文件超过 274 KB 的部分读不到。静态 hello 才几 KB,从没触发过这个上限;interp 一来 822 KB,正好撞上。

> 解法选了**最小、不动 kernel** 的:把 ext2 镜像的块大小从 1024 改成 **4096**(`create_ext2_disk.sh`)。4096 字节块下,single-indirect 能寻址 (12+1024) × 4096 ≈ 4 MB,822 KB 的 interp 落进单间接范围,读得到。ext2 驱动本来就动态读 `block_size`,自适应,不用改。镜像从 4 MB 变 8 MB。**真正的修——给 ext2 块映射补 double/triple-indirect——登记成 ext2 的 follow-up**(后面 ext2 那卷会专门做间接块)。这是个「用配置绕过、把根因留给专门里程碑」的典型处理:当下要的是动态链接跑通,不是顺手重构 ext2。

## 端到端:动态 hello 真跑起来

工具链就绪、ext2 块大小修好,跑端到端 smoke:QEMU 里 smoke worker `fork` → child `launch_user_program("/hello-dyn")` → `execve` 认出 PT_INTERP → 加载 interp → musl ldso 重定位主程序 → 跳 `AT_ENTRY` → `write` 经 `libc.so` 输出。串口会打:

```
[EXECVE] loaded interp /lib/ld-musl-x86_64.so.1 base=0x10000000 entry=0x10072530
Hello from musl on CinuxOS!
[F10-M2] smoke: hello-dyn 5/5 iters PASS -> PASS
```

五次迭代(每次都重新加载 interp + 重定位,比静态重,所以少于静态的 20 次),父 `waitpid` 收到 exit 0。**CinuxOS 能跑 musl 动态用户程序**——内核加载 PT_INTERP 指定的 interp、喂对 auxv,符号解析/GOT/PLT/`DT_NEEDED` 全由 musl ldso 在用户态完成。

## 验证

三层验证,跟 059 的静态 smoke 同样的分层。

**第一层:host 单测,ELF validate。** `test_fork_exec` 里加了 `test_valid_et_dyn`:interp 是 `ET_DYN`,validate 得收它(以前只收 `ET_EXEC`)。这一层顺带给 PIE 主程序铺了路(主程序要是 PIE 也是 `ET_DYN`)。host `test_fork_exec` 90 passed。

**第二层:内核测试,静态路径不回归。** 动态加载是在静态基础上加的分支(有 PT_INTERP 走动态、没有走静态原路)。`run-kernel-test-all` 两腿各 **977 passed / 0 failed**(+1 = `test_valid_et_dyn`,静态路径零回归)。AP1 机制回读 PASS。

**第三层:端到端动态 smoke。** `CINUX_MUSL_DYN_SMOKE=ON` 时,ring-3 跑 `/hello-dyn`,看那 5× `Hello from musl` + `hello-dyn 5/5 PASS`。这层要本机先编 musl 动态工具链,没编就跳过(默认 OFF,CI 安全)。

## 这章没做的

- **不自建 dynamic loader**:GOT/PLT/`DT_NEEDED`/符号解析/重定位,全交 musl ldso。内核只装载 + 喂 auxv。
- **PIE 主程序 + ELF base ASLR**:这一章主程序还是非 PIE `ET_EXEC`(base=0 绝对地址)。interp 的 `ET_DYN` 接受已铺好路(`test_valid_et_dyn`),但 PIE 主程序的重定位(`R_X86_64_RELATIVE`)留 follow-up。
- **interp base ASLR**:`USER_INTERP_BASE` 现在固定,配 PIE 时一起做。
- **ext2 double/triple-indirect**:用 4096 块绕过了 822 KB interp 的读取问题,但 ext2 块映射的双重间接缺失是真 follow-up(后面 ext2 间接块那卷)。>4 MB 的文件现在还是会读截断。
- **glibc 动态二进制**:`PT_INTERP=/lib64/ld-linux-x86-64.so.2` 内核侧天然支持(同样的 PT_INTERP 机制),按需验。

## 小结

- 动态链接的程序带一个 `PT_INTERP` 指向 ldso,运行时由 ldso 把 `libc.so` 拼进来 + 重定位。内核不自建 loader——对齐 Linux 的分工。
- 内核只做三件事:扫 `PT_INTERP` 读 interp 路径(`execve.cpp:278`)、把 ldso 当 `ET_DYN` 映到 `USER_INTERP_BASE`(`load_interpreter`)、改入口成 interp 入口 + 喂 auxv(`AT_BASE`/`AT_ENTRY`/`AT_PHDR`)。重定位/GOT/PLT 全是 ldso 用户态的事。
- 抽出 `load_elf_image`(`elf_load.hpp:64`)让主程序(base=0)和 interp(base=`USER_INTERP_BASE`)共用一条 PT_LOAD 映射路径。
- musl 的 `libc.so` 就是 interp(ldso + 共享 libc 二合一);`build-hello-dyn.sh` 产非 PIE 动态 hello;ext2 装 interp 到 `PT_INTERP` 指定的精确路径。
- 最大的坑是 ext2 双重间接缺失:interp 822 KB 超过单间接极限(274 KB)读不到,用 4096 块(单间接到 4 MB)绕过;真正的修留 ext2 间接块那卷。
