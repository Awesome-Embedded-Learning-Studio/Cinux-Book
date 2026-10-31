---
title: "09 · 一笔改判:sendfile 从明说到真办"
description: "点名那一章立的三种答法里,sendfile 起初归在明说不办那一档:门里有人应,应的是不办,用户库自己绕路。改判的证据不在咱们这边,在生态那一卷攒下的串口日志里,从头到尾再翻一遍,BusyBox 的脚印中真有它,客人真敲过,回退号位就不是终点。真办的章法照 exec 参数那一章的同款,拿 Linux 当正本:fs/read_write.c 的 do_sendfile 与 sendfile(2) 手册页。语义一件件立:数据在内核页缓冲按页搬运,不宣称零拷贝,单趟上界按 MAX_RW_COUNT 页对齐派生,EOF 答零,短写与进度后的错误交回已写字数,输入只推进实际写出的字节。显式 offset 更新用户递来的那个字,输入游标原地不动,NULL offset 推共享打开描述的游标,dup 的别名看得见,输出是普通文件推自己的游标,是管道守流语义,追加输出当场拒,错误码从 EBADF 到 EPIPE 八个配齐。配套三样:信号量添不睡的试等,管子添非阻塞的试推,读者写者寿命按打开描述计,最后一个读端关上,写这头答 EPIPE 并发 SIGPIPE,备了几卷的那对词在管子这头上岗。探针回手揪出两笔旧债:管子的 stat 把 FIFO 写成普通文件,空管子被当文件读,通用错误翻译漏了 BrokenPipe 一直错答 EINVAL,两笔都修。长凳十面,机内探针一行交卷,门上的拉锯打完,该请 C++ 的家当上桌。"
chapter: 34
order: 9
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - syscall
  - sendfile
  - pipe
---

# 一笔改判:sendfile 从明说到真办

点名那一章给单子立过三种答法,sendfile 起初归在明说不办那一档:门里有人应,应的是不办,用户库接到 ENOSYS,自己绕路。这一档本来稳当,坏在咱们多翻了一遍日志。生态那一卷攒下的串口日志,咱们从头到尾再过一遍,BusyBox 的脚印里有它:客人真敲过 sendfile。明说不办,说的是咱们暂不伺候,客人真用上了,回退号位就不是终点。单子上那一笔就地改判:门里真办。

sendfile 要办的事,一句话说得清:一个读着的文件,一个写着的描述符,中间的字节由内核在门里直送,不经过用户态的缓冲。真办的章法也是现成的,exec 参数那一章立过的老路:拿 Linux 当正本。这一回正本两处,内核源码 fs/read_write.c 里的 do_sendfile,加上 sendfile(2) 手册页,两边对着,咱们照着立语义。

零拷贝这块招牌,咱们不挂。数据在内核的页缓冲里搬运,一页从输入读出来,写进输出,再取下一页,搬运是真的,只是全程不出内核。单趟的上界人家也备好了,名叫 MAX_RW_COUNT,Linux 拿 INT_MAX 按页对齐往低取算出来的,咱们按同一个式子派生,一次想搬很多的客人,超出的部分留给下一趟。读到文件尾,门答零。写不下那么多,实际写了几字节就交几字节。搬到一半出错的也一样,已写的字数照交,错误那一半,客人下一趟叫门时还会再见。

offset 这个参数一身兼两种语义,咱们贴着代码看。客人递一个指针进来,内核从指针指的位置读,搬完把指针上的字改成新位置,输入自己的游标原地不动。递 NULL,内核才去推输入的游标,推的正是共享打开描述上的那一枚,dup 出来的别名与正主同看一个数,谁的名下搬了字,两边都读得到。输出那头另有一套:是普通文件,推它自己的游标,是管道,守流的章法,字节排队进去,没有游标可推。这些约定,咱们在代码里拢成了几行:

```cpp
    transfer.position += kPut.value();
    transfer.remaining -= kPut.value();
    if (transfer.seekable_output) {
        transfer.output.offset += kPut.value();
    }
    if (transfer.shared_input) {
        transfer.input.offset = transfer.position;
    }
    return kPut.value();
```

您细看 position 那一行,它加的是 kPut.value():写出去多少,读的位置才前进多少。短写的时候多读出来的字节等于没读,输入只认实际写出的部分,进度不虚报。

每一页的搬运,咱们跟着小函数走一圈,读一段,写一段,两头都看着:

```cpp
    const auto kRead = read_chunk(transfer.input, transfer.position, page);
    if (!kRead.ok()) {
        return kRead.error();
    }
    if (kRead.value() == 0) {
        return 0UL;
    }
    if (kRead.value() > page.size()) {
        return base::KernelError::kIOError;
    }
    const auto kPut = write_chunk(transfer.output, {page.data(), kRead.value()});
    if (!kPut.ok()) {
        if (kPut.error() == base::KernelError::kBrokenPipe) {
            proc::SendSignal(*CurrentTask(), kSigPipe);
        }
        return kPut.error();
    }
```

读回来零字节,咱们当它到尾:门答零,不硬凑一个错。写那头交回 BrokenPipe,说的是对面的读端死绝了,内核给写手补发一记 SIGPIPE,再把错误原样递出去。这对词有来历。跑别人的程序那一卷收卷时念过一句,“写往空管道的那对事件,词是备着的,挂点是没有接的”,生态那一卷验收又点了一遍名。管道这头,词与挂点在 sendfile 的路上都接上了。

拒答的面,咱们照正本配齐。输出以追加模式开着,当场答 EINVAL,Linux 明写追加不许。输入不是普通文件,递了显式指针答 ESPIPE,这词两位老师傅上工那一章给无游标的流立过,管子打从跑别人的程序那一卷立起来就是流,没有游标,正好归队,递 NULL 则答 EINVAL。坏描述符 EBADF,坏用户指针 EFAULT,两头顶到上界 EOVERFLOW,非阻塞遇上满管 EAGAIN,信号打断 EINTR,读端死绝 EPIPE。一扇门答得出八个错,才算把人家的脾气学全。

非阻塞的 sendfile 碰上满管子,不能睡等着。为这一口,咱们给信号量添了个不睡的试等 try_wait,问一声有没有空位,没有就回身,给管子添了一记非阻塞的试推 try_push,推得进几字节算几字节,推不动交回 WouldBlock,旗上带着非阻塞,门就答 EAGAIN。管道读者写者的寿命,这一遍按打开描述计:读端的描述在管子名下记一名读者,写端记一名写者,dup 出来的别名共享同一份,别名关门,名额不散,正主也关了,名额才散。末位读者一散,写这头再推,管子答 BrokenPipe,门翻成 EPIPE。

探针一上机,回手就给咱们揪出两笔旧债。头一笔在管子的身份上:探针拿管子的读端当输入递进去,等的是 ESPIPE,门却认真读起了管子。根子在 stat,管子自报身份时把 mode 写成了普通文件的 S_IFREG。FIFO,stat 户口里专给管子挂的那一类名分,没挂上,空管子就这么被当成了普通文件去读。修后,管子当着咱们的面自报家门:

```cpp
/// The stat a pipe answers: a fifo, both ends one identity.
base::Result<void> pipe_stat(void* /*self*/, FsNode node, FsStat& probed) {
    probed       = FsStat{};
    probed.ino   = node.ino;
    probed.mode  = 0010600;
    probed.links = 1;
    return {};
}
```

0010600,您读头四位,0010 正是 FIFO 的身份位,后头 0600 是读写权。stat 的身份号,跑别人的程序那一卷给管道立的是一份,假文件与真终端那一卷给假文件立的又是一份,两句老话记岔了一处,当面改回。第二笔出在翻译上,内核里这一错叫 BrokenPipe,翻给客人要过 ErrnoFrom 的翻译表,表里漏了它,一路落进默认的 EINVAL,咱们一直答错。errno 的词典这一年添过好几回,UDP 与 socket 那一卷一口气添过十二个新词,这一回一个新词都不缺,咱们补的是一行翻译:

```cpp
    case base::KernelError::kBrokenPipe:
        return kEpipe;
```

两笔旧债都还了,案卷咱们当场合上。

长凳上立一案,案名自带考点:`syscall face: sendfile keeps offsets and completed bytes across short writes and faults`。显式偏移这一头,咱们把数字摆出来:输入自己的游标停在 9,指针从 4 起,搬 37 个字节,指针上的字读回 41,输入的游标还在 9,一动没动。换成 NULL 再搬 7000,游标落到 7009。dup 的别名、跨页、短写、EOF、负偏移、溢出、坏描述符、追加拒绝、EAGAIN,逐面都过。机内的实弹是 sendfileprobe,核的比长凳上更细:写出的文件逐字节核对,别名共享的游标,坏的用户指针,非阻塞的满管与排空后的恢复,关掉读端后的 EPIPE。串口给咱们交回一行:

```text
SENDFILE_PROBE_PASS offsets shared eof nonblock pipe errors
```

长凳与机内的实弹都过了,门上这场拉锯打完,咱们该请 C++ 的家当上桌了。
