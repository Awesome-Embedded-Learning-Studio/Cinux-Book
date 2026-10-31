---
title: "15 · 开张:一条命令,干净树到可用系统"
description: "上一章末尾撂下的话在这里兑现:构建图原本不认识作坊,烤盘得靠人记着去敲两个门,这一章作坊长进图里,两门并作一条命令。章法是当年判例回魂,第一遍笔者拍的板,构建的活全交给依赖图记,依赖记到文件级,两条命令开机。busybox 的下载,校验,指纹缓存放进脚本本尊,图用有名字的目标够着它,烤盘的依赖单上挂进 busybox 与 hello 两件,当班表与户口本的种子也入册,料一变盘自动重烤。开机的三扇门早立着,这一章给它们挂上盘件:run 走串口,run_debug 冻结复位挂 gdb,run_gui 把无显示的参数摘掉弹出真窗口,门后暂时无人。作坊脚本降为薄壳,只挑出口,README 交代双入口。开张四验:强刷缓存重编一炉,二跑指纹命中,薄壳 verify 判据逐条落绿,run 与 run_gui 干跑依赖图九处含盘件,一条命令从干净树到 QEMU 里可用系统,盘上只有 glibc 一家,TCP 完备那一卷撂的四件方向就此全兑现。图记修改时间,作坊记指纹,两本册不同步时会烤旧树,疑心起时直接重烤。"
chapter: 33
order: 15
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - ecosystem
  - cmake
  - build
  - busybox
---

# 开张:一条命令,干净树到可用系统

改了 busybox 的料,咱们得记着去敲作坊的门,让它重出一炉新的。新货出了炉,又得记着去敲构建图的门,这回轮的是把盘重烤。哪一回忘掉了,机器照样欢欢喜喜地开机,盘上躺着的却还是昨天的身子,而且它是一声不吭的。上一章末尾咱们撂过,构建图眼下还不知道作坊的存在,烤盘靠的是人脑记着两个门。这个负担咱们这一章卸掉。

排工这件事笔者的第一遍早就交过答卷。当年收这一段的时候,笔者拍的板是:构建里所有的活,交给 cmake 手里的依赖图去记,依赖记到的是文件级,哪个文件动过了,重跑的就是哪份活,人只管敲的也就是两条命令,一条是配置的命令,一条是开工的命令,机器从干净的树里自己立了起来。判例是立得早的,重走的这一遍绕了一圈,又回到它跟前来了:作坊有了,进料册有了,欠的就是让图认识作坊。

活分的是三件,咱们一件一件落。头一件是 busybox 的活,挂成图上一个有名字的目标。cmake 的具名目标,朴素地说,就是给一坨活起了个名字,往后敲的是名字,活就开了工。这回起的名字叫 busybox_glibc,记在仓库顶层的 CMakeLists.txt 里,变量 CINUX_BUSYBOX_GLIBC 指的产物就是 build 目录下 busybox-glibc/busybox 那具身子:

```cmake
add_custom_command(
    OUTPUT ${CINUX_BUSYBOX_GLIBC}
    COMMAND ${CMAKE_COMMAND} -E env
            "CINUX_ROOTFS_CACHE=${CMAKE_SOURCE_DIR}/.cache/rootfs"
            bash ${CMAKE_SOURCE_DIR}/scripts/build-busybox-glibc.sh ${CINUX_BUSYBOX_GLIBC}
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/build-busybox-glibc.sh
            ${CMAKE_SOURCE_DIR}/user/rootfs/busybox/busybox.config
    COMMENT "glibc busybox for the ecosystem volume")
add_custom_target(busybox_glibc DEPENDS ${CINUX_BUSYBOX_GLIBC})
```

您贴着 OUTPUT 那一行看:图要的就是那具身子。至于怎么造的事,图是不操心的:下载、校验、解包、指纹与重编还是命中,全在 build-busybox-glibc.sh 本尊的身上,上一章的那本进料册一页没动,只是搬进了图够得着的地方。图自己只认的是两样:脚本的本尊,加上配置文件 busybox.config 的本身,这两样谁动了,重跑的就是目标,脚本是翻开册子对页的,页是对得上的,一声 `cache hit` 收了工,对不上的话,重编一炉新的。glibc 的 hello 也跟进,用的是同一副挂法,目标名写的是 hello_glibc,咱们不重复念它。

第二件咱们让烤盘的目标认料。烤盘的命令还是老脚本 create_ext2_disk.sh,变的是它挂出来的依赖单:

```cmake
    DEPENDS user_hello user_shell user_mapper user_sigdemo user_sigcatch user_nxdemo user_ecotest
            user_efaulter user_smepdemo user_aslrdemo
            user_tlsdemo user_threaddemo user_ttydemo user_smpstress
            busybox_glibc hello_glibc
            ${CMAKE_SOURCE_DIR}/scripts/create_ext2_disk.sh
            ${CMAKE_SOURCE_DIR}/scripts/ext2_seed/fstab
            ${CMAKE_SOURCE_DIR}/scripts/ext2_seed/inittab
            ${CMAKE_SOURCE_DIR}/scripts/ext2_seed/passwd
            ${CMAKE_SOURCE_DIR}/scripts/ext2_seed/group
            ${CMAKE_SOURCE_DIR}/scripts/ext2_seed/smp-stress.sh
```

进了单子的新名字有两个,一个写的是 busybox_glibc,一个写的是 hello_glibc。更值得咱们多看一眼的,是末尾那几家的种子:当班表、户口本、fstab,都齐了。前几章它们上盘的时候,身份只是盘里的种子文件,图是不认的,咱们改它一个字,盘是不会重烤的,全凭的是自觉。如今进了依赖单,料与种子的动静都归图管,重烤这件事从人脑搬进了图里。

第三件是铺面的三扇门都得认盘。这几扇门咱们都不是头回走:run 走的是正门,串口走的是 stdio,机器在咱们终端里说话。run_debug 是追内核案子的老门。run_gui 在自己的脸,自己的耳朵那一卷弹过真窗口,咱们还在里头亲手打过字。可门后的依赖单从前只挂内核镜像一件,盘是不在单上的,盘烤了没烤,烤的是不是今天的新料,门是一概不问的。这一章把盘件挂进三扇门的单子,正门的单子如今长这样:

```cmake
add_custom_target(run
    COMMAND ${CINUX_QEMU_PROGRAM} ${CINUX_QEMU_DEVICE_ARGS}
            -drive format=raw,file=${CMAKE_CURRENT_BINARY_DIR}/cinux.img
            -serial stdio
    DEPENDS boot_image ext2_img
)
```

单子末尾添的 ext2_img,就是烤盘的目标,三扇门走的是同一份改法,内核镜像与盘件一起备齐了,门才开得了。另两扇咱们不另贴,单子是一模一样的,变的只有门口的动作:run_debug 加 -S -s,起机是冻结在复位的,gdb 的 stub 开在 1234 口,挂上了就能查。run_gui 把设备参数里那一对 -display none 摘掉了,QEMU 就给您弹一个真窗口,门后暂时是没人的,窗口里眼下还是字符的天下。

作坊那边的门脸还留着,只是身份换了。从前的 build.sh 自己排工,下载什么是它定,编什么也是它定,什么时候烤盘还是它定,一步是一步地吩咐下来的。如今排工归了图,它降成的是一层薄壳,咱们把整个骨架请出来看:

```sh
repo="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$repo"
action="${1:-run}"
case "$action" in build|run|verify) ;; *) echo "usage: $0 [build|run|verify]" >&2; exit 2;; esac
export CINUX_BUILD_DIR="${CINUX_BUILD_DIR:-${repo}/build}"
export CINUX_ROOTFS_JOBS="${CINUX_ROOTFS_JOBS:-4}"

cmake -S . -B "$CINUX_BUILD_DIR"
cmake --build "$CINUX_BUILD_DIR" --parallel "$CINUX_ROOTFS_JOBS" --target boot_image ext2_img

if [ "$action" = "verify" ]; then
    python3 scripts/test_station33_ecosystem.py --image "${CINUX_BUILD_DIR}/boot/cinux.img"
    exit 0
fi
if [ "$action" = "run" ]; then
    test -c /dev/kvm && test -w /dev/kvm || { echo 'KVM is required and must be writable' >&2; exit 1; }
    cmake --build "$CINUX_BUILD_DIR" --target run
fi
```

您顺着这几行读:配置是打头的,构建是跟上的,末了按挑好的出口把目标名递给 cmake,verify 起的是机器,点的是判据,run 干的是开机进 KVM,顺带把 /dev/kvm 的可写检查做在开机之前。脚本里再没有一行排工的知识,活的次序全在图上。README 也把这层关系写给了往后的人:整条链已经并进了 CMake 依赖图,三个目标都会把镜像和系统盘备齐了再开机,user/rootfs/busybox/build.sh 是同一入口的薄壳,只负责的是挑选出口。作坊是没有退役的,是排工的活换了东家,手艺还住在原来的屋里。

> 图与作坊从今往后各记各的册:图记文件的修改时间,作坊记料的指纹。两本册绝大多数的时候步调是一致的,可它们一旦不同步了,烤出来的盘可能就是旧树,症状还是安静的。咱们疑心起来的时候,跟两本册讲理是讲不通的,直接把盘重烤一炉新的,新货出了手,什么都清楚了。

开张是要验的,咱们照四样来。头一样是把作坊缓存里的产物故意刷掉,逼它重编了一炉,busybox 从源码从头出了一炉,一路是走通的。第二样咱们原样再敲一遍,进料册翻开了,册页是对得上的,一行 `cache hit` 报出来了,作坊收了工,料是一动没动的。第三样是薄壳的 verify 走到底,机器自己起来了,当家的坐一号座,十四位客人逐个交了退出码,glibc 的 hello 出问候,fstab、临时隔间、共家的储物间、门内的传话,判据一条一条地到齐,绿灯是最后才亮的。第四样咱们拿 run 与 run_gui 干跑一遍依赖图,图上排出来的活九处,排在里头的就有盘件。

于是 TCP 完备那一卷撂下的那句承诺,今天能照实念了:从干净的树到可用的系统,一条命令就走到了。命令咱们也认得,就是作坊门口的薄壳:

```sh
bash user/rootfs/busybox/build.sh
```

咱们敲下去,下载了,校验了,编译了,烤了盘,开了机,一路走到了提示符,盘上有的只是 glibc 一家。愿意走图的正门,咱们换两条命令,效果是一样的:

```sh
cmake -B build
cmake --build build --target run
```

那一卷撂的方向有四件,到今天能对单了。盘上世界换了血,/lib 里剩下的只有 glibc 一家。动态的程序,busybox 与 hello 两具 glibc 的身子都在盘上跑。一号进程的 init,当家的坐镇一号座,照着当班表开的台。一条命令从干净的树到可用的系统,是这一章补上的末一件。面板、长凳、实弹的家底,下一章验收里咱们摊开逐项点数。

下一卷咱们要把编译器全家请进这间作坊,机器编自己的程序,而盘还得再扩。位置无关的身子至今没有座:所谓 PIE 就是程序不把自家住址写死、装到哪儿都能跑的形态,把后半场交给解释器那一卷挂的留白,下一卷咱们一并还。run_gui 门后的人,是收官那一卷的事。
