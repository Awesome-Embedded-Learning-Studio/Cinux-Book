---
title: "11 · 搬进搬出与四间临时隔间"
description: "当家的坐镇之后,家里添的头一件设施是搬进搬出。mount 一百六十五与 umount2 一百六十六两号位落地,挂载表还是假文件与真终端那一卷立的八座,根、/dev、/proc 三处常驻,空座住进新客。Vfs 补上卸载这一手:前缀出表,后端的寿命不归表管,后端从进表起就挂在一纸租约上,引用归零才走。tmpfs 的座池是四间临时隔间,每间记得自家门槛,卸载只拆自家的。开机自挂的三处根基不在池内,搬家的手碰不着。卸载两条死路与一条活路:普通卸载遇活引用答 EBUSY,MNT_DETACH 记名在前搬家在后,已打开的文件照样活着,卸完重挂照常。收卷前最后一轮验收的实弹新抓两案:umount2 把旗子所在的 RSI 当路径读,真路径在 RDI,号位锚定的测试考不出寄存器座次,真实 glibc 的 umount 一敲就 EFAULT,修后忙卸载、延迟卸载、活 FD 写入、重挂全通。重挂的负测又抓出 access 只 resolve 挂载点、从不真找文件的老假成功,补上实际 lookup,客人侧探针三查旧名字 ENOENT、空闲卸载与再次挂载。盘上种子一行 fstab,壳里 mount -a 照表把 /tmp 挂上,布告栏的 mounts 报出四行,隔间里写读一回,探针一行过关。搬进搬出开了张,家里还差两样说话的地方,共家的储物间与不出门的会客,都在后面两章。"
chapter: 33
order: 11
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - ecosystem
  - syscall
  - vfs
  - tmpfs
  - mount
---

# 搬进搬出与四间临时隔间

收卷前最后一轮验收的实弹场上,壳里敲下去一条 umount,回来的是 EFAULT。EFAULT 的意思是,门里把客人递来的地址当烂地址读了。路径明明好好写在命令里,怎么算都轮不到烂。咱们当时手里的牌面不差,host 的长凳一路绿到底,号位与 Linux 的表逐一对过,偏偏真 glibc 的 umount 头一回上门,就喊了这一声冤。

病灶在两参的座次。umount2 的客人递两样东西,路径坐 RDI,旗子坐 RSI,座次是 ABI 定死的。咱们把门读反了,拿 RSI 当路径去读。真 glibc 的 umount(path) 进门时走的其实是 umount2(path, 0),RSI 里装的是旗子零。把零当地址去读,读得出什么,当然是 EFAULT。门改过来之后是这几行:

```cpp
long long HandleUmount2(const SyscallFrame& frame) {
    if (CurrentTask()->euid != 0) {
        return -kEperm;
    }
    constexpr Word kDetach = 2;
    if ((frame.rsi & ~kDetach) != 0) {
        return -kEinval;
    }
    char            target[fs::kFsPathMax] = {};
    const long long kTarget                = FoldUserPath(frame.rdi, target, sizeof(target));
    if (kTarget < 0) {
        return kTarget;
    }
    const base::Result<void> kGone =
        fs::FileWorld::self().umount_tmpfs(target, frame.rsi == kDetach);
    if (!kGone.ok()) {
        if (kGone.error() == base::KernelError::kWouldBlock) {
            return -kEbusy;
        }
        return -ErrnoFrom(kGone.error());
    }
    return 0;
}
```

咱们一道一道过。头一道查身份,搬家不是人人动得了的,身份不是零,答 EPERM。第二道验旗,旗里只认 MNT_DETACH 一位,懒卸载靠它,混进别的位答 EINVAL。往下路径从 RDI 折进来,懒不懒由 RSI 判,底下答忙的那一声,翻译成 EBUSY 递出去。每一行各管一段,谁也不越界。

这一案能一路绿到收卷前的最后一轮,咱们把两头测试摆开看。号位那一头,测试把 kUmount2 跟系统的 SYS_umount2 用 static_assert 对表,考的是号对不对。长凳那一头,案子里直接调 Vfs 的类,压根不进系统调用的门。两头都点头,门里读错座次这件事,就没有一双眼睛看得见。这一课开门迎客那一卷上过一回:stat 在 lstat 的座上坐了六个卷,自家的测试对着自家的座次回回点头,busybox 头一个按真 ABI 敲门就破。同门课再上一回,考官还是同一位,真客人。

案翻过去,咱们看这一卷立的家什。mount 坐一百六十五,umount2 坐一百六十六,两号位落地。挂载表还是假文件与真终端那一卷立的八座,如今根、/dev、/proc 三处常驻,空座有的是,搬进来的新客尽管坐。卸载这一手 Vfs 原来没有,这一卷补上,管的事很窄:把带这个前缀的那一行从表里划掉,到此为止。后端的寿命从来不归表管,另有人管。

管的就是租约。后端从进表那天起就挂着一纸租约,谁在它身上 resolve 出一个视图、打开一个文件、把镜像装进一个世界,谁就攥一份引用,引用不归零,后端不走。租约不是咱们新造的,文件映射与装载早就在用同一份,resolve、open、ELF、VMA 一路贯穿,收权学会劈段那一章走通的路正是它。

tmpfs 这回进门。它是住在内存里的临时文件系统,东西只放在内存里,不往盘上写,挂上去时屋里是空的,卸下来连屋子带东西一起没了,咱们在文件世界里给它备了座池:一共四间临时隔间,每间记着自家门槛,挂上时前缀记在座上,卸载时逐间对门槛,只拆自家的。开机自挂的 /、/dev、/proc 三处不在池内,对它们喊卸载,答的是池里没有这一间。自家的根基,不交给搬家的手。

咱们再看 mount 的门,也窄。身份照查,target 得是个目录,不是目录答 ENOTDIR。文件系统型只认 tmpfs 一个词,别的型答 ENODEV,不装样子。第一参 source,门里读都不读,探针里递的 none,纯是礼数上的装饰。四间满座,第五位再敲门,答的那一声与挂载表满座同款,都是 ENOMEM。

卸载遇上活引用,咱们把两条死路摆在明处。硬答成功,客人手里还开着的文件当场悬空,是一条死路。老实回绝,答 EBUSY 把人顶回去,东西永远堆在屋里,是另一条。MNT_DETACH 从中间穿过去:旗子一递,名字当场从表里划掉,座位当场腾出,可已经打开的文件照样活着,写在活着的描述符上的字一个都没有少。等最后一位客人放手,引用归零,后端才真正搬走。记名在前,搬家在后。卸完重挂,照常。

隔间的这一辈子,咱们请客人侧的探针从头到尾过一遍:

```cpp
static void mount_lifetime(void) {
    const int kFile = open("/tmp/lifetime", O_CREAT | O_RDWR, 0600);
    require(kFile >= 0, "mount-open");
    require(umount("/tmp") == -1 && errno == EBUSY, "mount-busy");
    require(umount2("/tmp", MNT_DETACH) == 0, "mount-detach");
    require(write(kFile, "alive", 5) == 5, "mount-detached-open");
    close(kFile);
    require(mount("none", "/tmp", "tmpfs", 0, NULL) == 0, "mount-reuse-seat");
    require(access("/tmp/lifetime", F_OK) == -1 && errno == ENOENT, "mount-fresh-volume");
    require(umount("/tmp") == 0, "mount-idle-unmount");
    require(mount("none", "/tmp", "tmpfs", 0, NULL) == 0, "mount-remount");
}
```

咱们跟着判据走。open 攥住一个文件,普通 umount 答 EBUSY,验的是第一条死路的墙。MNT_DETACH 递进去,过了。再下一步最有意思:隔间的名字都从表里划掉了,客人手里还攥着描述符,write 照样写进五个字节,活引用活得踏实。close 放手,重挂一间新隔间,access 问旧名字,要的是 ENOENT,新屋里没有旧货。再往下,空闲时正常卸载,成。再挂,又成。require 的脾气是失败才喊 ECO_FAIL,这几步过关,一声不吭。

咱们在 access 那一步又翻出一桩旧案。负测的判据挂的是 ENOENT,可旧的 access 只把 /tmp 这一层挂载点 resolve 出来,从没有真去后端里找文件,挂载点在,它就答文件在。一个假成功,不知安静躺了多久,重挂的负测头一回逼着它表态。修法不玄:resolve 之后,把后半段路径真正递给后端找一遍,找得到才算在。

咱们把搬家这件事,最后落在盘上一张表。种子往 etc 目录里放了一行 fstab:

```text
tmpfs /tmp tmpfs defaults 0 0
```

壳里 mount -a,busybox 照表把 /tmp 挂上。您在验收的串口上接着看到的,是这一片:

```text
/ # mount -a; echo MOUNT_ALL_$?
MOUNT_ALL_0
/ # cat /proc/mounts; echo MOUNT_TABLE_$?
none / ext2 rw 0 0
none /dev devtmpfs rw 0 0
none /proc proc rw 0 0
none /tmp tmpfs rw 0 0
MOUNT_TABLE_0
/ # echo tmpfs >/tmp/probe; cat /tmp/probe; echo TMPFS_$?
tmpfs
TMPFS_0
/ # ecotest --guest
ECOSYSTEM_PASS shm unix poll relro
```

四行报数,咱们逐行认:头三行是开机自挂的根、/dev 与 /proc,第四行 tmpfs 是 mount -a 刚照表挂上的新客。点名十四位客人那一章在布告栏铺过几张纸,mounts 就在其中,如今正好拿来给搬家作证。/tmp 里写一行读一行,读回来的还是写进去的那几个字。末行落款是整套生态探针的过关签,卸载寿命不在落款里数名目,它们的过关方式,是不喊 ECO_FAIL。卸了再挂,/tmp 又是一间崭新的隔间。
