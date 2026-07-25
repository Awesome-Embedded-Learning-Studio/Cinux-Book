---
title: 03 · Framebuffer 与 PSF2 字体:画像素、画字
---

# Framebuffer 与 PSF2 字体:画像素、画字

### Framebuffer:一块能随机写的显存

映射好了,[framebuffer.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/framebuffer.cpp) 这层反而简单——它就是把这块显存当成一个 `uint32_t` 数组来用:

```cpp
void Framebuffer::init(const BootInfo& bi) {
    uint64_t fb_phys = bi.fb_addr;
    width_  = bi.fb_width;  height_ = bi.fb_height;
    pitch_  = bi.fb_pitch;  bpp_    = bi.fb_bpp;

    uint64_t fb_size = static_cast<uint64_t>(pitch_) * height_;
    arch::map_mmio(fb_phys, fb_size);                 // ← 上一步的映射

    addr_ = reinterpret_cast<volatile uint32_t*>(fb_phys);  // 物理地址当虚拟地址用
}
```

注意 `fb_size = pitch_ * height_`,用的是 `pitch` 不是 `width`——因为每行实际占 `pitch` 字节(含补齐),整块显存大小得按 `pitch` 算。映射的尺寸算少了,后面写像素就会越界到没映射的区域。

画一个像素,核心就一个下标公式:

```cpp
void Framebuffer::put_pixel(uint32_t x, uint32_t y, uint32_t argb) {
    if (x >= width_ || y >= height_) return;          // 越界直接丢弃
    addr_[y * (pitch_ / 4) + x] = argb;
}
```

`addr_` 是 `uint32_t*`,所以下标按「32 位单元」计;而 `pitch` 是**字节数**,得 `/4` 换算成「每行几个像素单元」。于是第 `y` 行第 `x` 个像素,就是这个公式。这个 `pitch/4` 是第二个新手坑:如果你下意识写成 `y * width_ + x`,一旦显卡的 `pitch > width*4`(很常见,为了对齐),画面就会整体歪斜、错位。

颜色 `argb` 是 `0x00RRGGBB`(高字节 0x00 在大多数 32bpp 模式下是忽略或作 alpha)。`fill_rect` 就是两层循环套 `addr_[...] = argb`,`clear` 是填满。`get_pixel` 反过来读,给测试回读用。

稍微值得一提的是滚动 `scroll_up`,因为后面的文本控制台靠它翻页:

```cpp
void Framebuffer::scroll_up(uint32_t lines, uint32_t line_height, uint32_t bg) {
    if (lines >= height_) { clear(bg); return; }
    auto* buf = reinterpret_cast<volatile uint8_t*>(addr_);
    const volatile uint8_t* src = buf + pitch_ * lines;   // 从第 lines 行起
    volatile uint8_t*       dst = buf;                    // 搬到最顶
    uint32_t move_bytes = (height_ - lines) * pitch_;
    for (uint32_t i = 0; i < move_bytes; i++) dst[i] = src[i];  // 字节级 memmove
    fill_rect(0, height_ - line_height, width_, line_height, bg);  // 清底部空带
}
```

它把整块显存当**字节**数组,把第 `lines` 行以后的内容整体往上搬 `lines` 行,再 `fill_rect` 把底部露出来的空带清成背景色。这里按字节搬而不是按像素搬,是因为「行」这个概念只在文本层有意义,对裸显存来说它就是一段连续字节,字节级搬运最直接。代价是 O(显存大小) 的拷贝——对一个控制台来说完全可接受。

### PSF2 字体:从 .py 生成到 .incbin 嵌进内核

能画点了,但点不等于字。要把 `'A'` 画出来,得有一张「字符 → 点阵」的表,也就是字体。Cinux 用的是 PSF2(PC Screen Font v2)格式,一个 8×16、256 个字形的小字体。

这个字体不是手写的,是用脚本生成的——[gen_psf_font.py](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/scripts/gen_psf_font.py)。它把经典的 IBM PC 8×16 CP437 点阵数据,按 PSF2 格式写进 `assets/font.psf`。PSF2 文件长这样:先一个 32 字节的 header,再紧跟每个字形的数据。

```python
PSF2_MAGIC       = 0x864AB572   # PSF2 魔数
PSF2_HEADER_SIZE = 32
PSF2_LENGTH      = 256          # 256 个字形
PSF2_CHARSIZE    = 16           # 每字形 16 字节(16 行 × 1 字节/行)
PSF2_HEIGHT      = 16
PSF2_WIDTH       = 8
```

每个字形 16 字节,正好对应 16 行,每行 1 字节、8 个位,哪位是 1 哪个像素就亮——宽度 8 刚好塞进一个字节。比如 `'!'`(0x21)的点阵是 `[0x18,0x18,0x18,...]`,第一行 `0x18 = 0001_1000`,中间两个像素亮,正是一个感叹号上半段的竖线。

生成出来的 `assets/font.psf` 是个二进制文件,得想办法把它带进内核镜像。这里用的是汇编的 `.incbin` 指令,在 [font_data.S](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/font_data.S) 里把它直接嵌进内核的 `.rodata` 段:

```asm
.section .rodata
.global font_psf_start
font_psf_start:
    .incbin "assets/font.psf"     ; 把整个二进制文件原样塞进来
.global font_psf_end
font_psf_end:
.global font_psf_size
font_psf_size:
    .long font_psf_end - font_psf_start   ; 4 字节小端长度
```

`.incbin` 是 GAS 的指令,作用类似 `.incbin "文件"`——把指定文件的内容一字不差地拼进当前段,并导出起始/结束符号。于是 C++ 侧就能拿到这段数据的指针:

```cpp
extern "C" {
extern const uint8_t  font_psf_start[];
extern const uint8_t  font_psf_end[];
extern const uint32_t font_psf_size[];
}
```

字体驱动 [font.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/video/font.cpp) 拿这个指针,先解析 header:

```cpp
static constexpr uint32_t PSF2_MAGIC = 0x864AB572;

void PSFFont::init() {
    const auto* hdr = reinterpret_cast<const PSF2Header*>(font_psf_start);
    if (hdr->magic != PSF2_MAGIC) return;     // 魔数不对就什么都不画(安全退化)
    num_glyphs_     = hdr->length;            // 256
    bytes_per_glyph_= hdr->charsize;          // 16
    width_          = hdr->width;             // 8
    height_         = hdr->height;            // 16
    glyphs_         = font_psf_start + hdr->header_size;  // header 之后就是字形数据
}
```

魔数校验是个小细节但很重要:`.incbin` 嵌进来的东西对不对,运行时没人替你把关。`hdr->magic != PSF2_MAGIC` 时直接 `return`,让 `glyphs_` 保持 `nullptr`,后面 `render_char` 见到 `nullptr` 就什么都不画——比起乱解引用崩掉,这种「安全退化」对一个显示驱动是合理选择。

最后是画字,逐行逐位地把字形转成像素:

```cpp
void PSFFont::render_char(Framebuffer& fb, uint8_t c, uint32_t x, uint32_t y,
                          uint32_t fg, uint32_t bg) {
    if (glyphs_ == nullptr) return;
    if (c >= num_glyphs_) c = 0;                       // 越界字形退化到 0 号
    const uint8_t* glyph = glyphs_ + c * bytes_per_glyph_;
    for (uint32_t row = 0; row < height_; row++) {
        uint8_t bits = glyph[row];                     // 这 1 字节就是这一行的 8 个像素
        for (uint32_t col = 0; col < width_; col++) {
            bool on = (bits >> (7 - col)) & 1;         // 最高位是最左边的像素
            fb.put_pixel(x + col, y + row, on ? fg : bg);
        }
    }
}
```

`(bits >> (7 - col)) & 1` 这一句里,`col=0` 取最高位(bit7),对应最左边的像素——这和 PSF 格式「高位在左」的约定一致。`on ? fg : bg` 同时画了前景和背景,所以每个字符都是一个完整的 8×16 色块,不会和旁边重叠的字符糊在一起。

这里有个隐含的边界条件:循环里 `bits` 是个 `uint8_t`、`glyph[row]` 每行只取 1 字节,所以这套写法**只对宽度 ≤ 8 的字体成立**。对 8×16 刚好精确;但如果哪天换了个 16 宽的字体,每行得取 2 字节,这个循环就得改。此刻我们不假装支持它——把边界划清楚,比留个隐患强。
