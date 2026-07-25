---
title: 04 · 调试现场、验证与下一站
---

# 调试现场、验证与下一站

## 调试现场

这一章没有专门的 notes 文件(`document/notes/013` 是空的),但代码里藏着三个真实的、写错就一定翻车的隐患,值得当成「调试现场」拎出来——它们都是你第一次写 framebuffer 几乎一定会撞上的。

**第一个,也是最关键的:帧缓冲通常在高地址,1GB 大页是「刚需」而不是「优化」。** QEMU 的 Bochs VBE 线性帧缓冲,物理地址常常落在几 GB 开外(远超 1GB)。这意味着 `map_mmio` 里第一段(<1GB 的 2MB 页)根本覆盖不到它,真正起作用的是第二段——1GB 大页。而 1GB 大页要 `has_1gb_pages()` 返回 true。如果你的代码漏了这一段、或者跑在一个不支持 PDPE1GB 的 CPU 上,显存就映射不进来,`Framebuffer::init` 之后第一个 `put_pixel` 立刻 page fault。所以记住:`map_mmio` 的两段不是「锦上添花」,而是覆盖不同物理地址区段的**两条腿**,少了高地址那条腿,屏幕就永远是黑的。

怎么确认你栽在这一步?最直接的办法是在 `Framebuffer::init` 调 `map_mmio` 之前,先把 `fb_addr` 用 kprintf 打出来(`fb_addr` 本身在 BootInfo 里,不依赖映射就能读)。如果它是个像 `0xe0000000` 这样的大数,你就知道一定得走 1GB 页那条路。然后确认 `map_mmio` 第二段的 `if (end > PAGE_1GB_SIZE && has_1gb_pages())` 真的进了、`pdpt[n]` 真的写进去了。`pdpt[n] == 0` 这个判断也别漏——它保证不覆盖已有映射,但反过来,如果你期望覆盖却没覆盖,就得查这个条件。

**第二个,硬编码的页表地址是这整套方案最脆的地方。** `PD_VIRT_ADDR = 0xFFFFFFFF80003000`、`PDPT_VIRT_ADDR = 0xFFFFFFFF80002000` 这两个常数,是「bootloader 把页表放在了这里」这个约定的硬编码。它对的时侯一切都好;一旦 bootloader 那边动了页表的布局(比如换了链接地址、加了新表),这两个地址就指向了别处,你往里写表项等于在破坏随机内存,症状是各种莫名其妙的花屏、崩溃、甚至 triple fault,而且没有任何报错告诉你「页表地址错了」。这种 bug 极难定位,因为代码看起来完全没毛病。我们这里的对策是**明知它是临时的**——等 015、016 做了正经的页表管理器,这种摸黑改表的做法会被替掉。在那之前,如果你动了 boot 的页表布局,第一件事就是回来核这两个地址。

**第三个,`pitch/4` 和「宽度 ≤ 8」这两个隐含假设。** `addr_[y * (pitch_/4) + x]`,少除了那个 4、或者误用 `width_`,画面就会整体歪斜错位(不是黑屏,是「能亮但全错」,反而更容易让人怀疑别的逻辑)。字体那边,如果哪天换了宽字体忘了改 `render_char` 的逐字节取位,字符就会只画出左半截。这两个都不是会崩的错,而是「安静地错」,排查时容易绕远路。把它们当成已知边界记在心里,撞上时能第一时间想到。

## 验证

这一章的验证分两层:能在 host 上跑的纯算术单测,和必须在 QEMU 里跑的真硬件测。

纯算术部分,host 单测把驱动里的关键公式**镜像**了一份出来测(注意:不是直接测内核代码,而是把同一个公式抄到测试里测,因为内核代码本身在 host 上跑不起来)。比如 [test_framebuffer.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_framebuffer.cpp) 测的就是那个下标公式和越界判断:

```cpp
static uint32_t pixel_index(uint32_t x, uint32_t y, uint32_t pitch) {
    return y * (pitch / 4) + x;     // 镜像 Framebuffer::put_pixel 的下标
}
TEST("framebuffer: pixel index at (100, 50)") {
    // pitch=4096(即 1024*4), offset = 50*(4096/4)+100 = 50*1024+100
    ASSERT_EQ(pixel_index(100, 50, 4096), 51200u + 100u);
}
```

同理 [test_font.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_font.cpp) 测 PSF2 header 解析和字形偏移,[test_console.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_console.cpp) 测光标/换行逻辑(那部分属于 013b,但算术单测放在同一批)。这些用 `-O2` 编、由 `CINUX_HOST_TEST` 宏门控,直接:

```bash
ctest --test-dir build -R 'font|framebuffer|console' --output-on-failure
```

但算术对,不代表真屏幕能亮——显存映射、VBE 模式这些只有 QEMU 里才验得了真。所以还有一组**机内测** [test_video.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_video.cpp),它在 QEMU 里跑,对着 bootloader 切出来的真 VBE framebuffer(1024×768×32)测:

```cpp
void test_fb_init_from_bootinfo() {
    auto* bi = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    g_fb.init(*bi);
    TEST_ASSERT_EQ(g_fb.width(), 1024u);
    TEST_ASSERT_EQ(g_fb.height(), 768u);
    TEST_ASSERT_GE(g_fb.pitch(), g_fb.width() * 4u);   // pitch >= width*4
    TEST_ASSERT_EQ(g_fb.pitch() % 4, 0u);              // 且 4 字节对齐
}
void test_fb_put_pixel_readback() {
    g_fb.clear(0);
    g_fb.put_pixel(100, 100, 0x00FF0000);
    TEST_ASSERT_EQ(g_fb.get_pixel(100, 100), 0x00FF0000u);  // 写进去读得回来
    TEST_ASSERT_EQ(g_fb.get_pixel(99, 100), 0u);            // 邻像素仍是黑
}
```

`get_pixel` 能把 `put_pixel` 写进去的值原样读回来,就证明那段显存确实被映射进来了、下标公式也对——这其实一并验证了 `map_mmio` 真的生效。跑它用带测试钩子的内核:

```bash
cmake --build build --target run-big-kernel-test
```

这组测试绿的,「显存能访问、像素能写能读、字体能画」就算点亮了。至于把这些像素组织成会换行、会滚动的文本终端,并让 kprintf 也往屏幕上吐——那是 013b 的活。

## 下一站

到这一步,内核已经能在屏幕上画像素、画字了。但你会注意到一个落差的:我们画字的能力(`PSFFont::render_char`)和内核唯一的诊断通道(kprintf)之间,还隔着一层——kprintf 此刻依旧只走串口,我们刚搭好的屏幕画字能力,还没有人调用它。

换句话说,这一章把「舞台」搭好了(像素、字体),但还没把「演员」(kprintf)请上来。下一站 [007](../007/) 就做这件事:我们会在 framebuffer + 字体之上盖一层文本控制台 `Console`,管好光标、换行、滚动,然后回头兑现 012 那个一直悬着的承诺——把 kprintf 的格式化引擎接上屏幕这第二个输出后端,让每一句诊断同时出现在串口和屏幕上。012 当初抽出来的那个回调式架构,到那时才真正显示出它的价值。

---

### 参考

- OSDev — [VBE (VESA BIOS Extension)](https://wiki.osdev.org/VESA_Video_Standards):`INT 0x10` `AX=0x4F01` 取模式信息、`AX=0x4F02` 设模式、模式号 bit 14(`0x4000`)启用线性帧缓冲(linear framebuffer)。本章 bootloader 设模式的机制以此为准(注意 Cinux 是重定向到 0x144,非首创 VBE 调用)。
- OSDev — [PC Screen Font (PSF)](https://wiki.osdev.org/PC_Screen_Font):PSF2 魔数 `0x864AB572`、header 字段(magic/version/header_size/flags/length/charsize/height/width)、字形布局。
- Intel SDM Vol.3(System Programming,分页):4 级页表、2MB 大页(PDE.PS = bit 7)、1GB 大页(PDPE1GB,探测位 `CPUID.80000001h:EDX[26]`)、`INVLPG` 与 `MOV CR3` 的 TLB 刷新语义。本地 PDF `document/reference/intel/SDM-Vol3A-*.pdf`,可用 `pdf-reader` 搜 "PDPE1GB"/"PS (Page Size)" 复核位号。
- GCC 在线文档 — [`.incbin` 指令](https://sourceware.org/binutils/docs/as/Incbin.html):把二进制文件原样嵌入当前汇编段,导出起止符号。
- 本 tag 源码:[framebuffer.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/framebuffer.hpp) / [framebuffer.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/framebuffer.cpp)、[paging.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/paging.hpp) / [paging.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/arch/x86_64/paging.cpp)、[font.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/font.hpp) / [font.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/font.cpp) / [font_data.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/font_data.S)、[gen_psf_font.py](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/gen_psf_font.py)、[serial.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/common/serial.S)(VBE 模式重定向)、[boot_info.h](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/boot/boot_info.h);测试 [test_framebuffer.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_framebuffer.cpp) / [test_font.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/test/unit/test_font.cpp) / [test_video.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_video.cpp)。
