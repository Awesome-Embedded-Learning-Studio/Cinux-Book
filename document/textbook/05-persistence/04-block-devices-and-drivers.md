---
title: 04 · 块设备与驱动:两条端口一扇窗,一张证件一间值班室
---

# 04 · 块设备与驱动:两条端口一扇窗,一张证件一间值班室

第 5 部的第四章,咱们还的还是一摞旧话。5.2 讲 flush 时撂下的原话,“一块盘一次只听一个命令的排班,内里归 5.4”。5.3 收卷又添了半句,“看 queue 的内里,栅栏那一道,到时候当面讲”。更早的债主还有几位:1.6 讲 DMA 的时候留过一句“往后引擎真开工,一样得走铃线”。2.5 盘家底时咱们自认“手里一块盘都使唤不动:磁盘驱动整个不存在”,四层缺的那摞里 disk 摆在头一层。3.6 开缓存柜的时候只把队列当现成的家什使,把队列与引擎的正戏记在名下。4.4 讲条件等待时,把值班队列的家法两句、完成件的前科一桩各记了半页。4.6 收卷,又记下 MMIO 与 DMA 的两张欠条。今天咱们把这些话一门一门还清。

这一章的活,咱们数下来四样,次序照着当年重走的脚印。头一样,总线走线,开机之后一无所知的咱们,怎么把满总线的设备一台一台问出来、唤醒、摆进一张终身稳定的表。再一样,开窗与对讲,认下的卡怎么在内存里开一扇不缓存的窗,怎么按 AHCI 的黑话把一道读盘的命令发出去、把完成收回来。第三样,验明正身,盘交上来的词表怎么逐字对,凭什么信它是一块能用的盘。末一样,排队与判决,读块写块从文件系统落下来之后,谁排号、谁跑设备、谁宣布成没成,断电语义那一侧立起来的栅栏,也在这儿当面讲。门房、册子与消息中断的新路,还有几台真驱动的对照,咱们顺路各看一眼,不抢正戏的台。

量法还是三条。码打头,实况锚是 r34_compile_self 的克隆,本章新戏的码全按它数,命令与断言行号,给到文件名加冒号加行号。5.2 教过的内容,引的仍是 5.2 章里的行号,那一章按同一个克隆数,两套数字可以直接对。5.3 立起来的批次与三席 sync,行号引 5.3 章里的,那一章按 r35_gui_fin 的克隆数,与本章不同锅,咱们不混着舀。屏,两面旧屏进正文,全按当年转写请回,各自的年代随屏标注,台数咱们不当常数念。书,OSTEP 的第 36 章 I/O Devices,咱们翻书页 4、5、7、8、11 五处,快慢取舍那一句从页 5 跨到页 6 才收尾,页 6 余下的半页话转述不引原字,引文逐字核过,书里跨行断开的词拼回整词,字模里的连字符号照原样抄,您拿着原句去核,借得准不准,您说了算。

> **5.2 撂过话,“一块盘一次只听一个命令的排班,内里归 5.4”。您接着往下想:文件系统、缓存、日志,如今谁都能往盘上递活,活一多,谁排号?谁替谁跑设备?跑完的消息走哪条线、谁宣布成没成?再往深一层,开机的时候咱们连总线上坐着谁都不知道,名单是从哪儿来的?**
>
> 名单是问出来的。开机之后,CPU 拿两条端口挨家挨户地敲门,答话的是各台设备配置空间里的户口。名单进了内核,才有窗、有命令、有证件、有值班室。这一章四样活,就是这么一个次序,当年重走的脚印也是这个次序,咱们顺着走。

## 总线走线,一台一台问出来

进门头一个问题,问谁、拿什么问。开机的一瞬,CPU 眼里只有内存,总线对面坐着谁,一个字都没有。PCI 的答法给在两条端口上:0xCF8 收地址,0xCFC 收数据,往 0xCF8 写一个 32 位的地址字,再从 0xCFC 读一个 32 位的回字,一问一答就成一拍。书里把对设备说话的门路归成两条,显式 I/O 指令一条,咱们两条端口就是它的实例。另一条叫内存映射 I/O,书页 8 的原话:“The second method to interact with devices is known as memory-mapped I/O. With this approach, the hardware makes device registers available as if they were memory locations.”后一条的窗,咱们到下一节开。

地址字不是裸地址,咱们拿四个字段拼一张寻人启事,拼法在 MakeConfigAddress 的手里,pci_layout.cpp 的 :96。

三十一位是总开关,写下它端口才认这是配置访问。十六到二十三位装总线号,十一到十五位装槽位号,八到十位装功能号,二到七位装寄存器的序号,按 4 字节一格数,所以偏移得除以 4 再入位,错位了读到的就是隔壁的字。四枚字段常数在 pci_layout.cpp 的 :36 到 :45 躺平,咱们对着位段念一遍就行。

问话的门道,咱们拿两件说。一件是空位怎么认:没坐人的座位,0xCFC 读回来的是全 1,厂商号 0xFFFF,扫线的人看见全 1 就跳过。另一件是 0 号功能当家:一台卡可以在一份配置空间里住好几个功能,可 PCI 只担保 0 号功能一定在,所以扫一个槽位,头一件是拿 0 号功能探一探,pci.cpp 的 :190 起,探不到就整槽收工,探到了再挨个功能问过去。空位认得出、功能数得出,走线的骨架就成了。

问出来的名单落进一张固定 32 座的槽表,容量常数在 pci.hpp 的 :133,装满就报数收工。从此设备在内核里住进终身稳定的位置:表只在开机时填这一回,填完不改,拿出去的指针一辈子有效。要用设备,咱们按类点名,find_by_class 拿大类加子类去查,pci.cpp 的 :233,不拿槽位号查,槽位号跟着装配变,类别长在设备身上,换台机器也认得。pci.cpp 的 :241 还立着另一张脸,find_by_ids,拿厂商号加型号号认人,类别底下常常一户人家好几种硅,按类认就要绑错硅,它的正主在这一章靠后的地方,咱们到那儿再叙。当年走线的报数,咱们把旧屏请回来,正文《验明正身》那一卷的实况:

```text
[kern] pci 0:0:0 8086:1237 bridge 6:0
[kern] pci 0:1:0 8086:7000 bridge 6:1
[kern] pci 0:1:1 8086:7010 mass storage 1:1
[kern] pci 0:1:3 8086:7113 bridge 6:80
[kern] pci 0:2:0 1234:1111 display 3:0
[kern] pci 0:3:0 8086:100E network 2:0
[kern] pci 0:4:0 8086:2922 mass storage 1:6
[kern] pci walk found 7 devices
```

七行户口,咱们念末行:0 号总线、4 号槽、0 号功能,厂商号 8086、型号号 2922,大类小类念作 mass storage 1:6。正文《验明正身》的原话:“0:4:0 上的 8086:2922,就是模拟器给咱们硬盘配的控制器。”0:1:1 也写着 mass storage,子类却是 1:1,那是老 IDE 的控制器,咱们不跟它打交道。屏上数出来的台数,咱们只当当年的实况念:那一卷走线时七台,后来 USB 的主机控制器进门,第八台才凑齐,名单是活的,常数不是。

点名点中之后,还有一道唤醒的手续要办。咱们一口气开三样:内存解码,设备应答自己窗口的访问。总线主控,设备有权自己发起搬运,盘跟内存倒数据靠的就是它。末了把固件可能留着的关中断位摘掉。三样齐了,设备才算站到岗位上,码就三行,pci.cpp 的 :255,一行读旧值、一行置位带摘位、一行写回。

## 开一扇不缓存的窗

设备不能只靠两条端口说全套话,大批的寄存器要占一扇窗。PCI 的答法是六枚基址寄存器,卡往里面报自己想要的窗,咱们读出来再映射进内存。基址的拼法在 BarBase,pci_layout.cpp 的 :125,64 位的窗分高低两枚读。映射走 MapDeviceWindow,vmm.cpp 的 :184,窗开在高半区 ioremap 一带,按 2 MiB 大页落格,落格时每一格都带上禁缓存的位,vmm.cpp 的 :218 明写着 kCacheDisable。为什么禁缓存这么要紧?正文《验明正身》的原话:“此处不许缓存,读就真读。”寄存器是对讲机的听筒,您要听的是设备此刻说的话,CPU 拿旧抄本当回音递过来,咱们就对着空气聊起来了。4.6 记下的头一张欠条,MMIO 绕开缓存,到这儿兑清。

窗开好了,咱们对着 AHCI 的窗说话。头一拨是全局主机控制寄存器 GHC,开机要把控制器领回岗位,走三拍:置使能、复位、再置使能,ahci.cpp 的 :191。

中间那拍等复位位自己清零,等的家什是 wait_clear_observed,带一笔预算,复位给 100 拍,合 1 秒。正文《验明正身》的口径咱们请回:“等,不空转。空转的机器,连报数的力气都没有。”预算到点,面板报一声 stuck,预算的职责是诊断,不是放弃,规范担保会来的转变,报了号照样等。

AHCI 的客厅分三页:命令列表、回执表、数据页,咱们各开一页安家。门牌不靠人记,座位表在码里拿 static_assert 逐条锁着,ahci_layout.hpp 的 :94 到 :98:一座口的寄存器块整 0x80 字节,PxIS 坐 0x10,PxCMD 坐 0x18,PxTFD 坐 0x20,PxCI 坐 0x38。

一口寄存器怎么当一套对讲机使,书里给过一副标准像,书页 4 的原话:“three registers: a status register, which can be read to see the current status of the device; a command register, to tell the device to perform a certain task; and a data register to pass data to the device, or get data from the device. By reading and writing these registers, the operating system can control device behavior.”咱们窗里的 PxTFD 报状态,PxCMD 收命令,数据另有两页安家。同一页把问一句等一句的笨办法起了名:“we call this polling the device (basically, just asking it what is going on)”,CPU 亲手搬数的路数也有名有姓:“When the main CPU is involved with the data movement (as in this example protocol), we refer to it as programmed I/O (PIO).”名字咱们收下,活儿下两节见。

一道命令的家当分两层:命令头 32 字节,ahci_layout.hpp 的 :142 拿 static_assert 锁着身量。命令表 144 字节,:201 同样锁着。往盘递的话叫 FIS,寄存器到盘用的型号是 0x27,命令标志位在第七位,断言的文案自家写着:“the command flag is bit seven, not the scouting note's six”——bit 是七,不是早年探路笔记里的六,分歧作者自家在文案里挑明了,咱们引到这里为止,当年情节不展开。

咱们用到的操作码四种:0xEC 验明正身,0x25 读盘,0x35 写盘,0xEA 落实。48 位的扇区地址按字段插进 FIS 的空格,数据走数据页:命令表后头挂一串物理区描述子,一枚 16 字节,写明数据在哪、多长,搬运交给控制器与 DMA 引擎。DMA 的原理 1.6 教过,书页 7 的原话咱们对一遍:“A DMA engine is essentially a very speciﬁc device within a system that can orchestrate transfers between devices and main memory without much CPU intervention.”活计怎么交代,书里也是一句:“telling it where the data lives in memory, how much data to copy, and which device to send it to”。咱们码里那枚 16 字节的描述子,就是 where 与 how much 落的地。

4.6 的第二张欠条在这儿兑:DMA 的搬运要与缓存打招呼。本机 x86 的答案是硬件一致性兜底,缓存看得见 DMA 动过的内存,码里因此一行冲刷的手笔都没有。设备窗那一侧禁了缓存,数据腿两头都不靠 CPU 的抄本过活。至于硬件怎么“看见”,4.6 立过界,咱们没翻到那页书,不编,这儿也不越。

一切就位,咱们发命令。四拍挨着落,ahci.cpp 的 :410:

```cpp
    completion_.rearm();
    volatile HbaPort& port = regs_->ports[port_index_];
    port.interrupt_status  = kWatched.raw;
    asm volatile("" ::: "memory");
    port.command_issue = kSlotZero.raw;
```

头一拍把完成件重新置回未决,rearm 一声。第二拍清要盯的中断位,它也是写 1 清零。中间垫一道编译器栅栏,把清状态和发命令隔成前后两拍。末了往 PxCI 写上 0 号槽的位,控制器看见这根线,自己起身取信、搬数据。命令列表按槽编格,PxCI 一位对一个槽,咱们的命令全走 0 号槽,一次一道,正文《验明正身》的原话:“咱们这一卷的命令全走 0 号槽,一次一道,盘的脾气就是这样,一道没完,下一道不接。”码里也守着同一条线:完成件到期回收时回头查一眼 PxCI,ahci.cpp 的 :333,命令还压着没跑完,Check 当场喊停,不许糊弄过去。

## 验明正身,盘交上来的词表

控制器认下了,盘自己是块什么盘,还得它亲口说。问的话是操作码 0xEC,IDENTIFY DEVICE,盘答 256 个 16 位词,512 字节,落进一页中转,咱们管它叫弹跳页,它就是盘交上来的证件底稿。证件怎么念,咱们挑三样:容量在词 100 到 103,四词拼一个 64 位扇区数,拼词的活儿在 ahci_identify.cpp 的 :43。型号在词 27 到 46,高低字节要换装,换装完尾随的空格原样保留,:51 起。词 0 是盘的自报家门,坏没坏、是不是杂牌,头一枚词开腔。容量这个词咱们多念半句:《验明正身》那一卷的教学裸盘自报 2048 扇区,3.6 屏上念的 QEMU HARDDISK 8192 扇区是另一处当年的实况,年代不同,数字各自标各自的,咱们不把它们摞成一个数。

证件收了,门怎么守,咱们看码。六道准入,一道不省,IdentifyAdmits 的全身在 ahci_identify.cpp 的 :109:

```cpp
bool IdentifyAdmits(const std::array<uint16_t, 256>& words) {
    if ((words[0] & kWord0Cfa) != 0 || (words[0] & kWord0Incomplete) != 0) {
        return false;
    }
    if (!model_is_printable_and_nonblank(words)) {
        return false;
    }
    if ((words[83] & kWord83Invalid) != kWord83Valid || (words[83] & kWord83Lba48) == 0) {
        return false;
    }
    const uint64_t kSectors = SectorCeilingFromIdentify(words);
    if (kSectors < 1 || kSectors > kSectorCeilingTop) {
        return false;
    }
    return checksum_holds_when_marked(words);
}
```

咱们挨道念。头两道看词 0:CFA 卡,CompactFlash 那门方言,命令集有出入,咱们不伺候。残缺回执,材料都交不齐。一枚位不对就拒。第三道查型号串,印不出来的、全空的,拒。第四道查词 83 的式样位,式样对不上说明词表版本对不上,LBA48 的位没立,同样拒。第五道拿容量对天花板,扇区数出不了头也压不了底。末一道最俏:词 255 的低八位是校验和的记号,盘出厂把它标成 0xA5。记号在,咱们把 256 个词逐词再和一遍,和数的低八位得是零,证件才算全须全尾。记号不在,第六道压根不设,照放。

准入门凭什么这么守,正文《验明正身》有一句原话,咱们整句请回:“门是拿证件守的,不是拿门牌守的:上面六道查的全是盘自己交上来的词表,材料对不上,门牌再漂亮也不放行。”说的是 PxSIG 的闲话:江湖上不少教程认盘,头一件事看签名域读出 0x101,咱们没跟,签名域在结构体里躺着,代码一处都没引用它。

证件验完还有一场自检,3.6 开柜时咱们引过一笔:“盘后头的 AHCI 控制器在 scratch 区写一笔、读回来、对字,写读回路当天验当天用。”码在 ahci.cpp 的 :290 到 :313:写进去,读回来,对不上当场报,对上了面板落一行“ahci scratch write-readback matches”,:310。3.6 题三末一笔讲的就是这场自检,今天码见着了。盘报到的那行在 :279,型号加扇区数,3.6 的柜子就立在验过的回路上头。

## 一张块脸,一间值班室

盘认下了,可咱们不该让客人认识它。正文《验明正身》的原话:“后端姓甚名谁,客人从头到尾不知道。”门上挂的合同是一个 C++ concept,block_device_concept.hpp 的 :62:

```cpp
concept BlockDevice = requires(Dev& device, Lba first, BlockSpan span, std::uint8_t* sink,
                               const std::uint8_t* source) {
    { device.read_blocks(first, span, sink) } -> std::same_as<bool>;
    { device.write_blocks(first, span, source) } -> std::same_as<bool>;
    { device.block_count() } -> std::same_as<std::uint64_t>;
    { device.block_size() } -> std::same_as<unsigned long>;
    { device.flush() } -> std::same_as<bool>;
};
```

五样方法拼成一张脸,咱们叫它块脸:读块、写块、块数、块长、落实。Lba 是扇区号,BlockSpan 是几格,两个词在块脸上当参数,名字从这儿起当术语使。谁有块脸,谁就能被同一个客人使唤。擦除转发的老手艺您在 2.5 见过,EraseFs 拿 lambda 转调真身,把类型擦掉只留脸面,同一门手艺。

可块脸后头顶着两个事实。一头,盘一次只听一个命令。另一头,使唤盘的客人常常一股脑来好几个。两头一顶,就得有一间屋子管排班,咱们叫它值班室,屋子当差的章程,叫值班队列。家当三样,block_queue.hpp 的 :170:

```cpp
    base::container::SlotTable<BlockRequest, kBlockQueueDepth>     pool_;
    base::container::RingQueue<unsigned int, kBlockQueueDepth + 1> pending_;
    bool                                                           duty_taken_ = false;
```

头一样是 16 格的请求池,每格住一条请求,读还是写、从哪个扇区起、搬几格、字节放哪,末了还有一根完成件。池子是老熟人槽表,它在这儿添了一手 insert_default:格子默认构造出来占住位子,内容回头再填。为什么多这一步?正文《验明正身》的原话:“占了的格,地址不搬家。”请求一入池,它的判决就有人指着睡觉了,插入的时候连搬带拷,地址一换,睡觉的人手里攥的就是旧票根。槽深 16,常数在 block_config.hpp 的 :25。第二样是环队排号,容量 16 加 1,多出一格是为了满和空不打架。第三样是值班位一枚旗,谁握着它谁当班。4.4 替咱们把家法两句敲足了重音:“绝不过设备命令,绝不过睡眠。”前一句让睡在格子里的判决永远等得着地方,后一句给值班室划了界。

章程怎么走,咱们看排队一头,block_queue.cpp 的 :37 到 :51,顺着那把锁读:进门锁上,占格、填单、排号,出门还锁,前后眨眼的活儿,设备命令一寸都没碰,睡眠更没有,家法两句在每一行底下垫着。排不进队怎么办?槽满了 enqueue 答 16,调用人自己去想办法,值班室不欠谁的觉。

咱们再看当班的另一头,值班员,block_queue.cpp 的 :85:

```cpp
void BlockQueue::drive() {
    for (;;) {
        unsigned int slot_id = 0;
        {
            const proc::SpinGuard kGuard(lock_);
            if (!pending_.pop(slot_id)) {
                duty_taken_ = false;
                return;
            }
        }
        BlockRequest& request = *pool_.get(slot_id);
        bool          moved   = false;
        switch (request.kind) {
        case RequestKind::kRead:
            moved = device_.read_blocks(request.first, request.span,
                                        static_cast<uint8_t*>(request.data));
            break;
        case RequestKind::kWrite:
            moved = device_.write_blocks(request.first, request.span,
                                         static_cast<const uint8_t*>(request.data));
            break;
        case RequestKind::kFlush:
            moved = device_.flush();
            break;
        }
        request.done.finish(moved);
    }
}
```

值班员从环队里弹一个号,锁就还回去。设备命令在锁外头跑。跑完,判决写进请求的完成件,睡觉的人在梦里有专人叫醒。一圈一圈,环队空了,值班位放回,值班员自己也回分岔口重新排队。三个任务同时想读盘的画面,您现在能想象了,正文《验明正身》的原话:“判决一落,睡的人次第醒来,盘上从来没有两道命令叠着跑。”

switch 里第三支您别放过:flush 也请进了这间屋,拿号、排队、值班员代跑。正文原话:“插队免谈——“前面的写都落实了”这句承诺,才有分量。”5.3 收卷撂下的半句,咱们当面讲清:缓存那一层的 flush 把脏页压进队列,排在 flush 后头的写必须等它前头的写都跑完才轮得上。栅栏不用另发一道命令,拿号排队的次序本身就当了栅栏。5.2 讲 flush 时留的那句“一块盘一次只听一个命令的排班”,到这儿内外都对上了。

判决落进哪?完成件,sync.hpp 的 :340 起头注自家写明口径:“One request's completion: a one-shot state, no tokens.”一次请求一枚状态,判决只落一回,落过之后再有人多手多脚,它一个字不改。头注把话说明白,多出来的一次事件,不能让下一个等待的人提早走人,信号量多一枚令牌就放一个人的旧路数,咱们不走。它排在 4.4 那五家等待的名单里,课在那一章,咱们只认它当班上的差。

判决要有人送。中断的挂接是四层放行,一层都不能省,ahci.cpp 的 :248 起:头一层把处理函数注册到线上。第二层把线在芯片那头放开。第三层口的 PxIE 置上要盯的位,设备许诺这些事发生了它拉线。第四层 GHC 里的总闸放开。注册前顺手清两处状态,旧的中断残留不留到新差事里。线是合用的,正文原话:“室友的中断,咱们既不抢着应答,也不顺手吞掉。”1.6 撂下的“往后引擎真开工,一样得走铃线”,这四层就是它的兑现。

线响了,判词怎么写,咱们看码,ahci.cpp 的 :364:

```cpp
    volatile HbaPort& port = regs_->ports[port_index_];
    const AhciReg     kEvents{port.interrupt_status};
    if (!kEvents.has(kWatched)) {
        return;
    }
    const AhciReg kIssue{port.command_issue};
    const AhciReg kTask{port.task_file_data};
    const bool    kFailed = kEvents.has(kFatal);
    const bool kSucceeded = kEvents.has(kIsDeviceToHost | kIsPioSetup) && !kIssue.has(kSlotZero) &&
                            (kTask & kTfdNotClean).raw == 0;
    if (kFailed) {
        quarantined_.store(true, std::memory_order_release);
        port.interrupt_enable = 0;
        port.command          = (AhciReg{port.command} & ~kCmdStart).raw;
        print::Println("[kern] ahci quarantined: events %X tfd %X ci %X", kEvents.raw, kTask.raw,
                       kIssue.raw);
    }
    port.interrupt_status = (kEvents & kWatched).raw;
    regs_->interrupt_status =
        cinux::base::bit::MaskBit<uint32_t>(static_cast<unsigned char>(port_index_)).raw;
    [[maybe_unused]] const AhciReg kEcho{regs_->interrupt_status};
    if (kFailed || kSucceeded) {
        completion_.finish(!kFailed);
    }
```

咱们挨段念。头一段认领:PxIS 里盯的那几位一个没立,起立走人,一声不吭。进门头一笔拍两张快照,kIssue 抄 PxCI,记的是哪些命令槽还压着,kTask 抄 PxTFD,报的是盘忙不忙。判成三个条件:事件里有致命错,直接判败。或者设备到主机的回执立了、命令槽空了、任务文件干净,三样齐了判成。两头都对不上,谁也不判,判决留着等下一回。末三行是清与唤醒的次序,它为什么长那个样子,咱们马上讲,那是一桩案子。

kFatal 一立,头一样走的是隔离:quarantined 落 true,口的允许清零,引擎停转,面板报一行 quarantine,此后 issue 一进门就被拒,ahci.cpp 的 :390。这一手正文各卷都没有记载,咱们的凭据只有码,以码为证,咱们也不添当年的情节。

案子您在 4.4 见过前科,咱们把现场请回来。当年双核实弹,首绿跟复跑打了架,间歇性地停住。现场采回来:唯一的叶任务阻塞在完成件的等待上,调用链从完成件的等待,经盘驱动的读块、块设备的值班队列,一路念到文件系统的路径解析。设备那一头回读的寄存器一字不差,命令槽空了,状态清了。两头一对,正文《破案:被吞掉的完成通知》的判词:“中断确确实实来过,不然设备的状态不会自己清干净,而完成的通知没有发出去。”病根在旧次序:旧的服务例程头一步唤醒等待者,末了才清设备状态。等待者醒在另一颗核上,手脚飞快,复位完成件、布好新命令、发了出去,新命令的完成位亮起,而旧例程的尾巴才走到清状态,把新完成位当垃圾扫了。

修好的次序就是您刚读的那三行:取判定取的是进门头一笔的快照,成败在那一拍算清。清状态,清完回读一次,回读是给设备的承诺落了地。唤醒放最后,等待者醒来去布置新命令时,旧例程的手已经从设备上收回来,再没有人会碰新完成位。有人会问,加个轮询的兜底岂不省事,正文答得干脆:“兜底会把病盖住,复跑是不再停了,病根还是原来的病根。”咱们不加。清与发之间那道编译器栅栏,管的是编译器不许把两笔写换次序。两颗核之间真枪实弹的次序,4.6 请过 test_block.cpp 的 :39,那是真屏障的课,咱们回指引路,不重讲。4.6 说 test_smp 与块设备测试同一个形状,另一头说的就是它。

## 等的两种姿势

发命令的人怎么等,咱们看码,ahci.cpp 的 :418:

```cpp
bool Ahci::wait_for_completion() {
    if (proc::Scheduler::self().current() != nullptr) {
        return completion_.wait();
    }
    for (;;) {
        const unsigned long long kSnapshot = cinux::arch::SaveAndDisableIrq();
        if (completion_.finished()) {
            cinux::arch::RestoreIrq(kSnapshot);
            return completion_.success();
        }
        cinux::arch::EnableIrqAndHalt();
    }
}
```

一个函数,两条路。有任务在身,走 completion_.wait(),睡在完成件上,醒靠别人,这是任务的正路。没有任务的上下文,开机早期的检查、测试内核里的直呼,走下面一条:存下中断状态、关中断,查完成件,没到就开中断并停机,一觉睡到下一回中断,醒来回循环头重查。书页 5 把睡觉换重叠的好处说尽了,原话:“Instead of polling the device repeatedly, the OS can issue a request, put the calling process to sleep, and context switch to another task. When the device is ﬁnally ﬁnished with the operation, it will raise a hardware interrupt”——同一页还有半句总结:“Interrupts thus allow for overlap of computation and I/O, which is key for improved utilization.”轮询把 CPU 拴在设备上,中断让它腾出手来,咱们前头几章的缓存与日志,吃的都是重叠的饭。

停机那一拍有个讲究。sti 开中断,hlt 睡觉,两条指令必须挨着写:处理器执行完 sti 还憋着一拍不收中断,这一拍叫 STI shadow,于是“醒”与“睡”之间不存在缝,睡就是原子的一步。写成两笔,缝就开了:处理函数跑完、判决发完,hlt 才执行,睡觉的人白睡,要挨到下一回不相干的中断把自己拍醒,重查条件,才拿回落袋的判决。正文《验明正身》替咱们把两条禁令写在明处。头一条,持着自旋锁的时候别调它,锁在手里人睡了,一条锁拴住一串人。第二条,别自作聪明展开成“恢复中断,再睡一觉”两笔,缝就是这么来的。醒来必须重查条件,能把 hlt 拍醒的不止您要等的中断,时钟每 10 毫秒响一回,“醒来就当判决到手的人,拿到的常常是时钟的问候。”码的注释也用英文叮嘱了同一件事:不相干的中断会把人拍醒,等的条件得兜着圈重查。原子的课在 4.4,咱们只认它在盘侧的用法。

## 门房、册子与消息新路

正戏唱完,咱们顺路看一眼旁的院子。到上一章为止,注册的门是一家一个,null 自己注册,console 自己注册,盘那边一台控制器带一块盘自己管自己的事,内核说不出自己有几台设备。设备模型立起来,头一件是门房,DeviceModel 收三样,名字、册类、记名,check_seat 立着三道检查,名字的形状、该带的脸、有没有重名,名册里住的必须是机器上真有的设备,这是名册的诚实底线。册子分三本,照设备的营生分:字符册收字节流,块册收扇区,网络册收帧。父子挂树那些大家什没立,理由一条,今天没有一个消费者,正文原话:“现在立它就是给空气写代码。”

盘那边归块册,咱们要特别说清,正文《门房、册子与门铃》的原话:“只登记,行为上的事一概不碰。”登记的意义是让名册知道机器上有一台叫 sata0、主号八次号零的块设备,管它的是 AHCI 驱动,入册在 file_world.cpp 的 :77 到 :85,nvme0 有则入册,:86 到 :95,它是 NVMe 的盘,后文有一节专讲。正文《门房、册子与门铃》那一卷开张时的名册屏,咱们请回来:

```text
/ # busybox cat /proc/devices
[kern] syscall 40 unserved
character:
  null 1:3
  zero 1:5
  console 5:1
  ptmx 5:2
  usbkbd 10:0
block:
  sata0 8:0
network:
  lo
/ # 
```

咱们念屏:usbkbd 一行,是第八台设备进门之后添的新住户,[kern] 一行是当年 syscall 40 还没人应答的如实记录,sata0 主 8 次 0,盘在名册里的正座。/proc/devices 怎么长出来,5.7 开课。

配置空间第六十四字节往后是一沓活页,PCI 把每沓叫一项能力,头一枚指针在 0x34,单链表串到底,咱们顺着正文卷的叫法称它能力链。链表坏起来有一套,指针绕成环,走的人永远出不来,所以遍历带硬上限四十八步:配置空间二百五十六字节,头六十四是固定的家,一百九十二除以四,一格一步,正是四十八。在内核里读函数的真身是一个函数指针,宿主的长凳上给它假身,正文《门房、册子与门铃》的原话:“真假两个身吃的是同一个遍历、同一份解码,宿主长凳上测到的就是真代码本身。”能力的头两种是消息中断的窄宽两式,MSI 与 MSI-X。消息中断要的不是线的座,是向量的座,内核给消息划了八个座,常数在 irq_config.hpp 的 :25,当年起点定在 0x40,住户表的推理在正文卷里,咱们只指路。表项的武装是四步协议,msix.hpp 的头注自家写着:“mask first, so a half-written entry cannot fire, then the pair, then a read-back as the fence, then unmask only when the device kept what it was given”。屏蔽打头,写地址与数据,读回来当栅栏,对上了才解屏蔽,失败就保持屏蔽答失败,哑设备宁可让它哑着,也不许它拿陈旧地址把中断打到别人头上。

同一套消息武装,xHCI 是大户,咱们顺路看它一眼。认它还是按类点名,大类 0x0C 串行总线、小类 0x03、编程接口 0x30,xhci.cpp 的 :143 起。窗开了之后,寄存器分四个区,后三个区的起点一个都不猜,全部现读,把偏移写死,正文《门房、册子与门铃》的原话,“等于把自己的命交给一份过期的地图。”写六十四位寄存器,低半打底的事写在前,高半收尾的事写在后,“写序的通则一条也没有,每一台寄存器有自己的提交协议。”对讲的家什是三只环、一枚门铃:命令环咱们生产、控制器消费,事件环倒过来,传输环每端点一只,“生产与消费谁也不碰谁的笔,这是整套机制的骨架。”环上的单位是 TRB,传输请求块,十六字节三个字段,通体 volatile。环尾一枚 Link 绕回,一位 cycle 翻转,生产者与消费者各存一份状态,对上了是新货,对不上是上一圈的老货,没有锁的方案。门铃是窗里一排三十二位数组,写端点号摇铃,“没有一处轮询端口的旧式对讲。”volatile 在这几家的用法咱们并排看:xHCI 的 TRB 通体 volatile,环是与控制器共享的内存,编译器敢少读写一回,协议就走了样。AHCI 的寄存器块与命令头走 volatile 视图。e1000 的描述符是普通内存,volatile 只守在 MMIO 上,下一段咱们跟它正式见面。设备写内存、咱们拿什么盯着,三家三样,各按各的脾气立的姿势。

枚举的链子当年连着断过三回,破案的家什是模拟器的 trace,加开机固件的成功字节做对照,三案下来一个共性:“咱们自己写的东西,自己说了不算,看 trace 里控制器收到的是什么要走在前头。”分层取证的战法,咱们记在这儿,键盘认字是 5.5 的戏,这一章只送到桥边。还有一笔边界照挂:消息真送达在咱们的模拟器下不可靠,武装的各步接到了最后一格、没能真验一把送达,扛货的活归了排空,xhci.cpp 的 :363 那行“the drain carries the cargo”是码里自家的话,绝对坐标的平板(tablet)与热插拔,登记远期。

## 几台真驱动与一张对照表

块脸底下压着几台真身,咱们并排看。AHCI 的盘您已经认熟了。e1000 的卡,正文《异步基建与点亮网卡》那一卷立的,认卡用的是另一张脸:按名认卡,find_by_ids,8086 加 100E,e1000.cpp 的 :41,理由请那一卷《按名认卡》一篇的原话:“类别字节报的是网络,可网络的类别底下坐着不止一家,virtio-net 报的也是同一个类别,寄存器图却是完全不同的另一张。按类认卡的话,绑错了硅,写出去的每一笔都是乱写。”卡起来复位走完,头一件静音:中断屏蔽清除写全一,:59,轮询驱动的判例,中断线不指望,消息中断也不挂。排水的活交给一具泵:正文卷立的一根常驻线,一圈一圈摇收发的引擎,测试的手也伸得进来摇。收环发环各一只,环上的格是描述符,一枚一枚排开,描述符自带一枚 done 位,回执落没落,看它。哨兵两条纪律,都落在收尾寄存器 Rdt 上:起手写环长减一,:95。消费到第 k 格、尾进到 k 加一,写进 Rdt 的是 k,:166。poll_frame 头一件事读收头,收头住在 RDH 寄存器里,:145 起,那一读是 MMIO,CPU 陷出去一趟主循环转一圈,真硬件上该读也无害。发送摇 Tdt,:194。收包的等法,正文卷起过名号,迟滞的窗:单调钟上定一点二秒,窗到头认输,这一发算丢,机器不陪它挂死。等它的姿势,咱们对照着念才清楚,下一段表里见。

NVMe 的盘,正文卷未及记载,咱们以码为证,不添情节。nvme.hpp 的 :3 头注自家写着 “Serialized NVMe block device with checked completions”,全串行,一次一道,判完成的凭证查过才认。admin 与 io 两对队列,submit 把命令字压进尾格,写尾门铃,nvme.cpp 的 :108 到 :117。回执在完成队列上,等它用的就是轮询:phase 位翻面判新,:124。消费完把头门铃一摇,:139。错收拒判,超时隔离,online 落闸就是隔离,后续读写进门直接拒,:141 与 :159。init 自己按类点名,nvme.cpp 的 :204,BAR0 开窗 :212,namespace,NVMe 管它名下的卷叫这个,报到 :299,块脸三法 :312 读、:316 写、:328 落实。开机那一侧 init_sequence.cpp 的 :215 有才起、:217 必验 scratch,盘没在座上,一个字不提。

三台真身并排放,咱们拿当差的姿势对:

| 对照的哪一样 | AHCI 的盘 | e1000 的卡 | NVMe 的盘 |
| --- | --- | --- | --- |
| 认卡的脸 | 按类点名,大类 1 小类 6 | 按名认卡,8086:100E | 按类点名,大类 1 小类 8 |
| 等的姿势 | 线中断,完成件叫人 | 泵轮询,谁摇谁收 | 有界轮询,只等自家那枚回执 |
| 命令的字 | FIS,操作码四种 | 描述符写环 | 命令字压队列尾格 |
| 摇铃的手 | PxCI 写 0 号槽 | Tdt 写尾格 | 写尾门铃,完成队列的头门铃回摇 |
| 判完成的凭证 | PxIS 快照三条件 | 收头离了收尾,再看 done 位 | phase 位翻面 |
| 出错的去处 | 隔离,后续进门即拒 | 迟滞窗里等,一点二秒认输 | online 落闸,错收拒判 |

书里的老对照,咱们请最后两段。xv6 的 IDE 驱动四个函数,书页 11 的原话:“The ﬁrst is ide_rw(), which queues a request (if there are others pending), or issues it directly to the disk (via ide_start_request()); in either case, the routine waits for the request to complete and the calling process is put to sleep. … Finally, ide_intr() is invoked when an interrupt takes place; it reads data from the device (if the request is a read, not a write), wakes the process waiting for the I/O to complete, and (if there are more requests in the I/O queue), launches the next I/O via ide_start_request().”您把四样对到咱们头上:排队的,是值班队列。睡觉的,是完成件。中断应门的,是 on_interrupt。递下一条的,是值班员回到循环头。骨架两家同款,咱们的判决立在一次性的完成件上,多了一道防多手多脚的闩。快慢怎么取舍,书页 5 到 6 的原话:“Thus, if a device is fast, it may be best to poll; if it is slow, interrupts, which allow overlap, are best.”页 6 余下的半页还讲混合与活锁,铺天盖地的中断能把系统淹在应门上,咱们转述不引原字。三台真身在快慢的谱上各占一角:盘慢走中断,卡快走泵,NVMe 轮询带着超时的保险,等不来就落闸。排空扛货,是两边都不信时的老实路。

链子到这儿全通了,咱们把正文《认字》那一卷《写穿的缓存》一篇的原话请回:“ext2 找缓存要块,缓存找队列排号,队列牵着的,是上一卷认下的那块盘。”引文里的“上一卷”,就是《验明正身》那一卷。牵着的另一头,file_world.cpp 的 :59,队列绑上 AHCI——5.2 念过名字就走的那一句,内里今天见到了。5.1 收卷交底时把“块设备的队列与引擎”整笔记在 5.4 名下,5.2、5.3 又各指了一回,一个说那间值班室的内里,一个说 disk 那半边,指的都是今天这门。2.5 那摞四层缺的清单,disk 一笔今天收讫。3.6 记在名下的队列与引擎的正戏,今天唱完。4.6 的两张欠条,前文各兑了一张。第 5 部的地基到这儿齐了,再往上,是 5.5 的键盘与终端。

## 本章留下了什么

块设备与驱动这一门,咱们今天走了四程:走线,开窗对讲,验明正身,排队判决。新词入册:

| 术语 | 行话 | 一句话口径 |
| --- | --- | --- |
| 配置空间 | config space | 每台设备 256 字节的户口,按地址字里的总线、槽位、功能寻人 |
| 基址窗 | BAR | 卡开口要的内存窗,映射进高半区,页页禁缓存 |
| 弹跳页 | bounce page | 盘词表中转的一页,证件底稿落的地方 |
| 证件底稿 | IDENTIFY,0xEC | 盘答的 256 词词表,容量、型号与自报家门都在里面 |
| 块脸 | concept BlockDevice | 五样方法拼成的合同,谁有它谁就能被同一个客人使唤 |
| 值班队列 | block queue | 一间值班室管排班,占格排号眨眼完,命令睡眠锁外走 |
| 完成件 | Completion | 一次请求一枚状态,判决只落一回,多来的事件一个字不改 |
| 隔离 | quarantine | 致命错一立,停引擎关允许,后续命令进门即拒 |
| 能力链 | capability list | 配置空间第六十四字节往后的活页单链表,遍历上限四十八步 |
| 门铃 | doorbell | 共享内存环旁的一排格子,写端点号或队列尾的号就是摇铃 |

没讲的咱们如实交底:键盘认字与 USB 类驱动,5.5 开课。伪终端与 TTY,5.5 名下。网络栈的内里,5.7。缓存柜子的家什,5.2 与 3.6 已刊,本章只使不拆。DMA 的原理,1.6 已刊,硬件怎么“看见”搬运,4.6 立过界,咱们不越。中断的进门与分拣,2.2 的课。sti 与 hlt 挨着写的原子课在 4.4,本章只认盘侧用法。MSI 真送达的实验、tablet、热插拔,登记远期。NVMe 的正文叙事未刊,咱们以码为证,e1000 与门房册子的卷已刊,延伸里给了去处。

## 练一练

咱们留三道题,一道拼地址,一道守门,一道排队。

**题一,拼地址。** MakeConfigAddress 的位段表您已经有了:三十一位总开关,十六到二十三总线,十一到十五槽位,八到十功能,二到七按 4 字节一格的序号。您拿 (0,4,0,0x3C) 自己拼一遍配置地址字,十六进制写出来。再问一句:0 号功能探不到的槽位,咱们为什么整槽收工,不试着问问 1 号?

**题二,守门。** 一台盘的词表交上来:词 0 干净,型号串可印,词 255 低八位是 0xA5,可词 83 的 LBA48 位没立。六道准入哪一道拦下?PxSIG 读出漂亮的 0x101,救得了它吗?您拿“门是拿证件守的,不是拿门牌守的”答。

**题三,排队。** 三个任务同时读盘,您说值班位落在谁手里?另外两位睡在什么上、被谁叫醒?末了再问:flush 排在一串写后头拿号,谁等谁,等到什么才算轮得上?

### 参考答案

题一:咱们拼。总线 0、槽位 4、功能 0,偏移 0x3C 除以 4 是 15,序号入二到七位,15 左移两位是 0x3C。三十一位置 1,槽位 4 左移十一位是 0x2000,按位段拼起来 0x8000203C。0 号功能当家,是因为 PCI 只担保 0 号一定在座,1 号功能可以存在也完全可以没有,门都没开就往里问,问的是谁都不知道。

题二:咱们判。第四道拦下:LBA48 的位没立,词表的版本对不上,往后拼 48 位扇区地址的话就没有着落。PxSIG 救不了:签名域在结构体里躺着,代码一处都没引用,门牌再漂亮,材料对不上就不放行。

题三:咱们排。值班位被抢到它的任务握走,它弹号、跑设备、写判决,另外两位睡在各自请求的完成件上,被 finish 的专人叫醒。flush 拿号排队,排在它前头的写不跑完它就不动,排在它后头的写等着它,后来者看得见它的落实。

## 延伸

- OSTEP 第 36 章 I/O Devices,书页 4 到 11:三寄存器的标准像、轮询与中断的取舍、DMA 与驱动分层,xv6 的 IDE 四函数全本,本章请过的段落您再自读一遍。
- 正文《验明正身》那一卷:走线、开窗、对讲、六道门、值班室与两种等待的当差正本,咱们引的旧话,原文都在那一卷。
- 正文《门房、册子与门铃》那一卷:门房三样、三本册子、能力链与向量池、三只环与枚举链,顺路一眼的原文。
- 正文《异步基建与点亮网卡》那一卷:e1000 的按名认卡、哨兵纪律与一读踢醒主循环,泵与卡的当差全本。
- 正文《破案:被吞掉的完成通知》一篇:被吞判例的现场、病根与修法,4.4 已引,原文咱们请回了三句。
- 正文《认字》那一卷《写穿的缓存》一篇:ext2、缓存、队列到盘那条链子的原话,收卷请回的那句,出处在这儿。
