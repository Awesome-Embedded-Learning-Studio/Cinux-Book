---
title: "09 · 实弹"
description: "机内第 8 台测试内核:开机阶梯自己爬,盘不在就直说停机。两案实弹:单任务一扇区写读比对加 flush。三任务各八轮、花样一轮一换,锤的就是值班制。mock 盘只喂宿主:64 格 RAM 盘,数 flush 的次数,证明命令真走了全程。"
chapter: 15
order: 9
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - storage
  - testing
---

# 实弹

机器上的测试台,咱们这一卷添了第 8 台:block。它跟别的台不太一样——别的台验的是内核自己身上的器官,block 要验的是一块真盘,所以它开机头一件事,是把阶梯里跟中断有关的那几级自己爬一遍:PIC 重排、时钟起跳、中断桩装好、0 号线放开,sti 开中断,然后 PCI 走线、AHCI 认盘,这一段一级不缺。产品内核阶梯里的快系统调用、键盘那两级,block 用不上,没爬。爬到头,盘要是不在——比如构建产物里没有那块 scratch 盘——它不装样子,一句话撂在串口上就停机:

```cpp
const bool kDriveUp = cinux::driver::Ahci::self().init();
if (!kDriveUp) {
    cinux::print::Println("[ktest] block stage needs the scratch disk, halting");
    arch::Halt();
}
```

needs the scratch disk——要那块实验盘。缺原料就停机,这句话就长在上面那段阶梯的末尾:盘没起来,机器不装没事,咱们让它直接 Halt。

台上两案,咱们一案一案看。头一案就是重头戏:三个任务一起上,锤的就是值班制:

```cpp
void worker_task() {
    for (unsigned round = 0; round < kRounds; ++round) {
        std::array<uint8_t, kSector> pattern{};
        std::array<uint8_t, kSector> readback{};
        const uint64_t               kLba = kBaseLba + round;
        for (unsigned long index = 0; index < pattern.size(); ++index) {
            pattern[index] = static_cast<uint8_t>(index + round);
        }
        const cinux::driver::Lba       kFirst{.value = kLba};
        const cinux::driver::BlockSpan kOne{.value = 1};
        if (!g_queue->write(kFirst, kOne, pattern.data()) ||
            !g_queue->read(kFirst, kOne, readback.data()) ||
            !cinux::base::EqualBytes(pattern.data(), readback.data(), pattern.size()) ||
            !g_queue->flush()) {
            ++g_failures;
            return;
        }
    }
}
```

三个任务,咱们让它们各跑八轮。不变式有两条,配合得正好:扇区号是 64 加轮次,轮与轮碰不上。花样是字节值加轮次,同一轮里,三个任务写出去的字节一模一样。您咂摸一下这个配合:同一轮挤进来的三个任务,谁抢在头里,扇区上落下去的花样都相同,并发天然无害。换了轮,扇区也换了,谁也踩不着谁的脚印。写、读、逐字节比对、flush,四步一套算一轮。逐字节比对用的是 `EqualBytes`,上一卷立树防前缀搅局的时候出生、当时就在用的同一把尺,这一卷换个场面再用。值班制要是含糊,比方说两道命令叠着跑、判决发错了人、销格销了别人的,读回来的字节就跟花样对不上。八轮二十四个来回里,总有一轮现形。机器上跑下来的结果:

```text
[ktest] block stage: ahci up

=== Cinux Kernel Test Runner ===
Running 2 case(s)...

[RUN] block: many tasks hammer the queue and each round trip holds
[PASS] block: many tasks hammer the queue and each round trip holds
[RUN] block: the queued drive answers single read and write
[PASS] block: the queued drive answers single read and write

=== Results: 2 passed, 0 failed ===
```

屏上压轴的单任务案,咱们拿来给命令通道做体检:一扇区写得进、读得回、逐字节对上,再 flush 一道。信填得对、引擎走得通、判决落得准,这一案验的就是这三样。

台子上还有一颗编译期的钉子,咱们单独亮出来:

```cpp
static_assert(cinux::driver::BlockDevice<cinux::driver::Ahci>,
              "the ahci drive satisfies the block face");
```

盘类过没过块脸的合同,您不用等机器跑,编译期就有答案。哪天有人动了 `Ahci` 的方法签名,五个方法缺了哪一个,这颗钉子当场把构建顶停。

机器上验的是真盘,宿主上,咱们验的是队列的骨架。宿主这一卷添了三份测试。pci 九案,验的是地址字的编法、BAR 的解法、类别的座位,不用机器,宿主里就盘得动。ahci 十四案更有看头——FIS 的填法、旗子的落位都在这儿,准入六道里五道备了拦下的案子,扇区天花板那一道只有放行和读数,没备拦下,咱们如实交底。block 五案配的陪练最省事:一块 mock 盘,64 格、每格 512 字节的 RAM 盘。接口长得跟真盘一模一样,肚子里就是一段数组:

```cpp
bool flush() {
    ++flushes_;
    return true;
}
```

咱们让它把自己接进同一个队列,还揣了一个小本子:flush 来一回记一回。host 五案里有三案都翻这个本子。flush 的次数对得上,才证明这道催落实的命令真的穿过整间值班室落到了盘上,不是半路被人吞了。mock 只在宿主世界里陪练,机器上从来不带它出门。

实弹打完,家底盘齐,咱们下一节收卷验收。
