---
title: "08 · 一张块脸,一间值班室"
description: "块设备五方法一份 concept 把关,擦除转发的手艺上一卷开过一间,这里又开一间。值班队列:16 槽池地址不搬家,环队排号,抢到值班位的人替全队跑设备,其余人睡在自己请求的判决上。一把锁只护占格与排号的眨眼瞬间,不过设备命令,更不过睡眠。"
chapter: 15
order: 8
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - storage
  - block-device
---

# 一张块脸,一间值班室

盘认下了,可 `Ahci` 现在还是个孤零零的类,藏在驱动目录里。客人不该认识它——上一卷立门面的时候咱们交过底:后端姓甚名谁,客人从头到尾不知道。这一卷把盘也送进门面后头,门上挂的合同,叫 `BlockDevice`:

```cpp
template <typename Dev>
concept BlockDevice = requires(Dev& device, Lba first, BlockSpan span, std::uint8_t* sink,
                               const std::uint8_t* source) {
    { device.read_blocks(first, span, sink) } -> std::same_as<bool>;
    { device.write_blocks(first, span, source) } -> std::same_as<bool>;
    { device.block_count() } -> std::same_as<std::uint64_t>;
    { device.block_size() } -> std::same_as<unsigned long>;
    { device.flush() } -> std::same_as<bool>;
};
```

五个方法:读写、容量、块大小、催落实。参数里站着两个新词法类型,咱们停一步看看:`Lba` 是扇区地址,`BlockSpan` 是扇区个数,两个都是自带说明书的整数——地址当个数用、个数当地址用,类型系统当场拦下。盘的地界按扇区编号,搬多少按扇区计,两种量各走各的门。

咱们把合同翻译成客人拿到的门把手,用的还是擦除那套手艺——上一卷给文件系统擦出九个转发,这里给盘擦出五个,同一个模子:转发函数无捕获,落成普通的函数指针,设备的指针当第一个参数跟着走。`adopt` 一下,盘就坐进了 `BlockPort`:

```cpp
template <BlockDevice Dev>
void adopt(Dev& device) {
    static constexpr BlockOps kErased = EraseBlockDevice<Dev>();
    ops_                              = &kErased;
    self_                             = &device;
}
```

您捧着 `BlockPort`,不问后头坐的是 AHCI 还是将来的谁。没人入座的时候,五个门把手个个客气地回 false 或者 0——宿主测试的头一案,请的就是这么一位空座的主儿。

盘的门面有了,接下来是值班室。问题摆在这儿:一块盘,一道命令没跑完就听不见下一道,可想用盘的任务往后只会越来越多。两个事实顶在一块儿,中间就得有排队的章程。咱们给章程起的名叫值班制,家当三样:

```cpp
base::container::SlotTable<BlockRequest, kBlockQueueDepth>     pool_;
base::container::RingQueue<unsigned int, kBlockQueueDepth + 1> pending_;
bool                                                           duty_taken_ = false;
```

16 格的请求池,每格住一条请求:读还是写、从哪个扇区起、搬几格、字节放哪,末了还有一根 `Completion`。池用的还是老熟人槽表,可这回它添了一手新功夫——`insert_default`:格子默认构造出来占住位子,咱们回头再往里填内容。为什么多这一步?请求一入池,它的判决就有人指着睡觉了。要是插入的时候连搬带拷,地址一换,睡觉的人手里攥的就是旧票根。所以槽表承诺:占了的格,地址不搬家。环队排号用的是 base 里早立好的那副轮子,自己的脸、自己的耳朵那一卷放进去的。容量 16 加 1,多出一格,是为了满和空不打架。

章程跑起来是这样。想用盘的任务,进屋办三件事:占格、填单、排号,全在一把锁的护送下,眨眼就完。然后分岔:要是值班位空着,您就是值班员——锁放下,替全队干活。值班位有人,您把自己请求的判决一睡,睡到有人替您把数据搬完、判决落定,醒了拿着结果销格走人。值班员的那段路:

```cpp
void BlockQueue::drive() {
    for (;;) {
        unsigned int slot_id = 0;
        {
            const proc::SpinGuard kGuard(lock_);
            if (!pending_.pop(slot_id)) {
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
        // —— write 与 flush 两案同款 ——
        }
        request.done.finish(moved);
    }
}
```

值班员从环队里弹一个号,锁就还回去。设备命令在锁外头跑。跑完,判决写进请求的 `Completion`,睡觉的人在梦里有专人叫醒。一圈一圈,环队空了,值班员把值班位放回,自己也回分岔口重新排队。三个任务同时想读盘的画面,您现在能想象了:谁抢到值班位谁干活,其余的睡,判决一落,睡的人次第醒来,盘上从来没有两道命令叠着跑。

值班室里就一把锁,它的纪律只有两条,咱们把重音敲足:绝不过设备命令,绝不过睡眠。锁护的是占格、排号、弹号这几下眨眼的活。命令一跑几十毫秒,睡觉更是没底——锁要是跟着进了命令和睡眠,持锁入睡正是上一节禁令拦的事,一串任务陪着值班员一起僵。眨眼的活护得住,长活锁外面跑,这是值班制能立住的地基。

对了,咱们把 flush 也请进这间屋,拿号、排队、值班员代跑,插队免谈——“前面的写都落实了”这句承诺,才有分量。门面、值班室都齐了,该上实弹了。
