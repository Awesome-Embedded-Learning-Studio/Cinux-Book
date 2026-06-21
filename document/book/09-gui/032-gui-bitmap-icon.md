---
title: 032 · 位图图标:给画布补一块带透明洞的贴纸
---

# 032 · 位图图标:给画布补一块带透明洞的贴纸

> 029 给了我们一块能画像素、矩形、直线、文字的双缓冲画布;031 把一个真能跑 shell 的终端窗口摆进了桌面。但桌面自己还是光秃秃的——你看着那块深靛色背景,会很自然地想:该有几个图标吧?点一下就开个程序。这一章我们把「画小图」这件事补齐:给 `Canvas` 加一个能吃任意彩色位图、还认透明色的 `draw_bitmap`;用 `consteval` 在编译期把「32 行字符画 + 一张调色板」翻成像素数组,做出第一批 32×32 图标;再定义一个带命中框的 `DesktopIcon` 结构。
>
> 但有一点要先把丑话说在前面:032 只是**原语层**。图标定义好了、`draw_bitmap` 写好了、命中框算好了——可这一章里**没有一个图标真的被摆上桌面**,屏幕上依旧空空荡荡。把它们排上去、接上点击、点了开窗口,是下一章 033 的事。这一章是在打地基。

## 这一章我们要点亮什么

一件具体的事:我们能在画布的任意位置画一张 32×32 的小图——一个带「>\_」提示符的终端图标、一个带 LCD 屏的计算器图标——而且图标的背景是**透明的**,不会拖一个黑框糊在桌面上。

这件事背后,点亮了三样东西,正好对应这一章的三块代码。

`draw_bitmap` 是 `Canvas` 第一个吃「现成像素数组」的接口。029 那套原语里,`draw_rect` 只会填纯色块,`draw_text` 只会画字,`blit` 倒是能拷一块画布过来,但那是「画布到画布」,而且没有透明度概念。画一张预先设计好的小图,得能直接喂一个 `uint32_t` 数组进去,还认得出「这个像素是透明的,别覆盖底色」。

编译期的图标工厂解决「1024 个像素没法手写」的问题。一张 32×32 图标是 1024 个 `uint32_t`,手写 1024 个十六进制颜色既没法看也没法维护。我们用一个 `consteval` 函数,把「32 行字符画 + 16 色调色板」在编译期翻成像素数组——零运行时开销,还能像画 ASCII 艺术一样编辑图标。

`DesktopIcon` 把「一张能画的图」升级成「一个有身份的东西」。一张图光能画还不够,它得知道自己在屏幕哪儿、被点了一下该干什么。`DesktopIcon` 把坐标、位图指针、标签、动作打包到一起,还自带一个半开矩形的命中测试。

## 为什么现在需要它

回看 031 的桌面:窗口管理器能合成窗口、终端窗口里 shell 跑得起来。但「启动一个程序」唯一的办法是代码里硬编码建窗口——用户没法**主动**点开什么。要做成一个像样的桌面,最少得有「图标 → 点击 → 开程序」这条链,而这条链的第一环就是「画图标」。

029 的 `Canvas` 恰恰力不从心。我们需要一个新的、能直接吃像素数组、还认「透明色」的接口,一套能在编译期生成的图标数据,以及一个「这是个可点的东西」的抽象。032 补的就是这三层。画法有了、图有了、「这是个可点的对象」的抽象有了——只差下一章把它们排上桌面、接上点击。

## 设计图

`draw_bitmap` 干的事很直白:拿一个 `w×h` 的像素数组,逐像素拷到画布上,遇到透明色(`0x00000000`)就跳过,越界就裁剪。

```text
  调用 draw_bitmap(x, y, w, h, pixels)
        │  pixels 是 w*h 个 uint32_t(0x00RRGGBB),row-major,从上到下
        ▼
  for row in 0..h:
      if (y+row >= height) break          ← 垂直越界,整行外,直接跳出
      for col in 0..w:
          if (x+col >= width) break        ← 水平越界,本行剩下的列必然更靠右
          color = pixels[row*w + col]
          if (color == 0x00000000) continue ← 透明哨兵:跳过,保留底色
          back_buf[(y+row)*pitch/4 + (x+col)] = color
```

图标数据的诞生则走一条更巧的路——**编译期字符画 → 像素数组**。

```text
  作者写 32 行 × 32 字符的「字符画」  +  一张 16 色调色板 palette[16]
        │  每个字符是一个十六进制 nibble(0..f),索引到调色板取色
        ▼
  consteval build_icon(palette, rows[32])
        │  对每个 (r,c): pixels[r*32+c] = palette[ hex_nibble(rows[r][c]) ]
        ▼
  std::array<uint32_t,1024>  ← 编译期算好,进 .rodata,运行时零开销
```

`DesktopIcon` 把「一张能画的图」升级成「一个可交互的对象」,自带半开矩形命中框。

```text
  DesktopIcon {
      x, y            屏幕坐标(左上角,int32_t 带符号)
      bitmap ─────────► 指向 consteval 算好的 1024 像素(或手工测试数组)
      label           "Shell" / "Calc"
      width, height   32 × 32
      action          IconAction::{None=0, OpenShell=1, OpenCalculator=2}
      contains(mx,my) 内联命中:矩形 [x, x+w) × [y, y+h)  ← 半开,右边下边 exclusive
  }
```

## 代码路线

### draw_bitmap:带透明度的位图拷贝

[canvas.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/canvas.cpp) 新增的 `draw_bitmap`,逻辑就是设计图里那套。注意它的返回类型是 `void`,越界和 null 都是静默 no-op:

```cpp
void Canvas::draw_bitmap(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         const uint32_t* pixels) {
    if (back_buf_ == nullptr || pixels == nullptr) return;   // 防御

    uint32_t pixels_per_row = pitch_ / 4;
    for (uint32_t row = 0; row < h; row++) {
        if (y + row >= height_) break;                        // 垂直裁剪
        for (uint32_t col = 0; col < w; col++) {
            if (x + col >= width_) break;                     // 水平裁剪
            uint32_t color = pixels[row * w + col];
            if (color == 0x00000000) continue;                // 透明:跳过
            back_buf_[(y + row) * pixels_per_row + (x + col)] = color;
        }
    }
}
```

几个设计点要讲清楚,因为它们都不是随便写的。

**透明色为什么选 `0x00000000`。** 这叫「色键透明」(color key,也叫 colorkeying)——约定一个特定像素值代表「这里什么都没有」,绘制时直接跳过,让底下的画面透上来。色键透明是最朴素的透明方案,真正的 alpha 混合要每像素一个透明度通道、做加权混合,开销大得多。选全零做色键有两个好处:判断便宜(一次整数比较),以及 `memset` 清零天然就是「透明背景」。这套做法在 2D 图形里很常见——pygame 的 `Surface.set_colorkey` 就是同一招:指定一个颜色值,所有匹配它的像素都不画。(注意别和视频领域的 chroma key 混为一谈——那是按色域范围抠像,机制不同。)代价我们放到「调试现场」讲——它和「纯黑」撞了。

**裁剪为什么用 `break` 而不是 `continue`。** 像素是按行主序、从左到右排的:一旦某一列超出右边界(`x+col >= width_`),后面同一行的列只会更靠右、必然也超界,继续判断纯属浪费,直接 `break` 跳到下一行。垂直方向同理,某行整行超出下边界,再往下也不会回来。画 32×32 图标时,边界附近能少跑不少无用迭代——是个小但实在的优化,也顺带把「越界」变成「裁剪」而不是「崩溃」。

**`pixels_per_row = pitch_ / 4`。** 这一行直接接上了 029 的内存布局:`pitch_` 是每扫描线的**字节数**,除以 4 才是每行多少个 `uint32_t` 像素。`draw_bitmap` 写的是 `back_buf_`(后备缓冲,堆上那块 ~3 MB),写完之后由 `Canvas::flip()` 按 `pitch` 逐行拷到硬件帧缓冲——这条「画在 back、再 flip 到 front」的双缓冲链 029 已经搭好,`draw_bitmap` 只是又一个往 back buffer 里写东西的原语。

**先 null 防御。** `back_buf_ == nullptr || pixels == nullptr` 直接返回。freestanding 内核里一个空指针解引用就是三字故障,防御性判断不嫌多——内核测试里专门有 `test_bitmap_null_pixels` 守这条边界。

### icon.hpp + icon_data.hpp:用 consteval 把字符画编译成像素

[icon.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/icon.hpp) 很薄,只 re-export 常量。它定义了图标的标准尺寸,然后把真正的像素数据 include 进来:

```cpp
namespace cinux::gui::icons {
constexpr uint32_t ICON_SIZE    = 32;
constexpr uint32_t ICON_PIXELS  = ICON_SIZE * ICON_SIZE;   // 1024
}  // namespace cinux::gui::icons
#include "kernel/gui/data/icon_data.hpp"
```

真正的重头戏在 [icon_data.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/data/icon_data.hpp)。作者面对的是这样的东西(终端图标,截取几行):

```cpp
inline constexpr uint32_t k_shell_palette[16] = {
    palette::BLACK,      // 0 - 透明
    palette::DARK_BLACK, // 1 - 终端主体
    palette::GREY_DARK,  // 2 - 标题栏
    palette::WHITE,      // 3 - 文字
    palette::GREEN,      // 4 - 光标绿
    0x00CC3333,          // 5 - 关闭点 红
    // ...
};

inline constexpr std::array<uint32_t, 1024> k_shell_icon =
    detail::build_icon(k_shell_palette, {
        "00222222222222222222222222222220",   // 标题栏行(边框 + 透明角)
        "02255672222222222222222222222220",   // 5/6/7 = 三色信号灯
        "02111111111111111111111111111110",   // 主体
        "01331111111111111111111111111110",   // ">"
        "01134111111111111111111111111110",   // ">_"
        // ...
    });
```

每一行是一个 32 字符的字符串,每个字符是一个十六进制 nibble(`0`-`f`),当作索引去调色板里取真正的颜色。`build_icon` 是 `consteval`,在**编译期**就把这 32×32=1024 个字符全部翻成像素,塞进 `std::array<uint32_t,1024>`:

```cpp
template <uint32_t Rows>
consteval std::array<uint32_t, 1024> build_icon(
        const uint32_t (&palette)[16],
        const char* const (&rows)[Rows]) {
    static_assert(Rows == 32, "Icon must have exactly 32 rows");
    std::array<uint32_t, 1024> pixels{};
    for (uint32_t r = 0; r < 32; r++) {
        for (uint32_t c = 0; c < 32; c++) {
            uint32_t nibble = hex_nibble(rows[r][c]);
            pixels[r * 32 + c] = palette_lookup(palette, nibble);
        }
    }
    return pixels;
}
```

拆开看那个 `pixels[r * 32 + c] = palette_lookup(palette, nibble)`,里面藏着两级映射,值得单独说一句。先是 `hex_nibble`:把一个 ASCII 字符翻成 0-15 的数字——`'0'-'9'` 映到 0-9、`'a'-'f'`/`'A'-'F'` 映到 10-15、其余一律返回 0(也就是当透明)。所以你写的每一个字符,先被压成一个 4 位的调色板下标 `nibble`。然后是 `palette_lookup`:拿这个下标去 16 项调色板里取真正的 `uint32_t` 颜色。两级映射的好处是**字符和颜色解耦**——同一张字符画,换个调色板就是另一套配色;调色板也只有 16 项,正好够一个 nibble 编址,不多不少。注意 `k_shell_palette` 并没有把 16 项填满(只用了 0-7),没用到的槽位编译期也不会报错,因为下标只要落在 `[0,16)` 内就合法——这是个小余地,以后想给图标加新颜色不用动字符画,只在调色板空槽里加一项即可。

为什么要 `consteval` 而不是普通函数?`consteval` 是 C++20 的「立即函数」(immediate function)——它强制编译期求值,根本不可能在运行时被调用。两个实在的好处:一是**零运行时开销**,图标在编译期就求值完毕,直接躺在 `.rodata` 里,运行时既不分配也不计算,`draw_bitmap` 拿到的就是一个现成的静态数组指针;二是**编译期保证**,哪个图标写错了(比如某行不是 32 个字符、或用了调色板没有的颜色),编译直接失败,而不是等运行时画出一个歪掉的图标才发现。`static_assert(Rows == 32)` 把「必须 32 行」也钉死在编译期。

这套机制和 `constexpr` 的区别也值得提一句:`constexpr` 函数是「**可以**在编译期求值」,但允许运行时也调用它;`consteval` 是「**必须**在编译期求值」,运行时根本调不动。这里我们就是要它铁定在编译期算完、产物进 `.rodata`,所以 `consteval` 比 `constexpr` 更贴切——它把意图钉死了,不会因为某次调用上下文不是常量表达式而偷偷退化成运行时计算。

至于「字符画 + 调色板」这套编码:它把一个本来反人类的 1024 数字数组,变成了**能用眼睛看出来画的是什么**的可编辑格式。你看 `"0225567..."` 那行,一眼就知道标题栏左边有三个信号灯点。这是 1024 像素的图标里最值得偷师的设计味道——用可读性换维护性。

这一章做出两个图标:`k_shell_icon`(黑底终端 + 「>\_」提示 + 三色信号灯)和 `k_calc_icon`(灰机身 + 绿 LCD 屏 + 按键网格 + 橙色等号键)。两个走各自的调色板,但机制完全一样。

### desktop_icon.hpp:图标的「身份」与命中框

光能画还不够。一个桌面图标得知道自己**在哪儿**、**点它该干嘛**。[desktop_icon.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/desktop_icon.hpp) 把这些打包进 `DesktopIcon`:

```cpp
enum class IconAction : uint8_t {
    None           = 0,
    OpenShell      = 1,
    OpenCalculator = 2,
};

struct DesktopIcon {
    int32_t          x;        // 屏幕左上角 X
    int32_t          y;        // 屏幕左上角 Y
    const uint32_t*  bitmap;   // 指向像素数据
    const char*      label;    // "Shell" / "Calc"
    uint32_t         width;    // 32
    uint32_t         height;   // 32
    IconAction       action;   // 点它要干嘛

    [[nodiscard]] bool contains(int32_t mx, int32_t my) const {
        return mx >= x && mx < static_cast<int32_t>(x + width)
            && my >= y && my < static_cast<int32_t>(y + height);
    }
};
```

两件事值得说。

**坐标用 `int32_t`(带符号)。** 虽然图标一般不会放负坐标,但用带符号类型和窗口的 `blit`(它也用带符号坐标处理「窗口拖到屏幕左上方」的部分越界)保持一致,省得命中测试时符号转换出错。内核测试专门有 `test_desktop_icon_contains_negative_position`,把图标放到 `(-10,-5)` 验命中边界,守的就是这个。

**`contains` 是半开区间** `[x, x+w) × [y, y+h)`——`mx < x+width`(严格小于),不是 `<=`。所以 32×32 图标放在 `(10,20)`,命中的是 `x∈[10,42)`、`y∈[20,52)`,最右一列 `x=42` 不算命中。内核测试里 `test_desktop_icon_contains_inside` 拿 `x=41`(即 `10+32-1`,最后一个像素)验证命中、`test_desktop_icon_contains_outside` 拿 `x=42` 验证「右边界 exclusive」,守的就是这条。半开区间是图形 hit-test 的惯例,好处是相邻两个图标不会在边界像素上「同时命中」——谁的区域归谁,边界永远清晰。

至于 `IconAction`,这一章它**只是个枚举**,`OpenShell`/`OpenCalculator` 还没有任何代码去消费它——真正「点了图标 → 派发动作 → 开窗口」是下一章的活。这里先把「该干什么」这个字段定义好,免得到时候再回头改结构体。

### window_manager.hpp:那个名不副实的光标常量

这一章顺手还改了 [window_manager.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/window_manager.hpp) 里两个光标常量,值得诚实记一笔:

```cpp
static constexpr uint32_t CURSOR_WHITE   = 0x00888888;   // 实际是灰色
static constexpr uint32_t CURSOR_BLACK   = 0x00FFFFFF;   // 实际是白色
```

名字叫 `WHITE`,值是灰(`0x00888888`);名字叫 `BLACK`,值是白(`0x00FFFFFF`)——名实完全对不上。看 diff 能确认这是 032 这一步改的(031 时它们还是名副其实的 `0x00FFFFFF` 和 `0x00000000`)。这大概率是调整鼠标配色时改了值、忘了改名字,然后就这么留下来了。它不影响光标正确画出来——光标位图用这两个常量照样能画出一个能看的鼠标——只是读代码的人会被名字骗到。这是个真实的小插曲,不是我们编出来的完整调试事故;下一章也没修它,它就这么挂着。记下来,免得你以后看光标代码时一头雾水。

## 调试现场

032 这个 tag **没有调试笔记**。按 Cinux 的规矩我们不硬造踩坑故事,但这一章有个现成的、源码可证的设计陷阱值得单独讲——就是前面埋下的「纯黑 = 透明」的冲突。它是那种**不爆、但会坑你**的隐患,这里放到「调试现场」的视角再看一遍。

**陷阱**:`draw_bitmap` 用 `0x00000000` 当透明色键。所以**任何想要纯黑像素的位图,都会得到一个洞**——那一像素被当透明跳过,底下是什么就显示什么(桌面色、别的窗口),而不是你想要的黑。

**怎么掉进去**:你照着某个图标的参考图(比如一个黑底终端),很自然地把主体区域填成 `0x00000000`。结果画出来,终端主体根本不是黑的,而是透出了桌面——图标像被咬了一块。你盯着 `draw_bitmap` 的代码看半天也看不出错,因为它确实「按约定」跳过了零像素。这种 bug 不会触发任何异常、不会打任何日志,只是画面上悄无声息地多了一个洞。

**怎么定位**:这种「静默跳过」的 bug 最坑,因为它没有任何报错线索。如果怀疑踩到了,最快的排查是把 `draw_bitmap` 里那一句 `if (color == 0x00000000) continue;` 临时注释掉重画——洞要是立刻被填上了(哪怕填的是纯黑而不是你要的近黑),就坐实了是透明色键把你的黑像素吞了。反过来,如果你看到图标本来该透明的地方(圆角外、异形边缘)冒出一块实色,那多半是把该写 `0`(透明)的地方写成了 `DARK_BLACK` 之类的实色。两种症状,同一个根因——色键和实色的边界没划清。

那怎么避免?就像 Cinux 自己做的——调色板里**禁用纯黑当实色**,凡是该黑的地方一律用近黑。看 [icon_data.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/data/icon_data.hpp) 里的调色板定义,这两行并排摆着,本身就是给后来者的警告:

```cpp
constexpr uint32_t BLACK       = 0x00000000;  // Transparent (skipped by draw_bitmap)
constexpr uint32_t DARK_BLACK  = 0x00101010;  // Near-black (opaque)
```

`BLACK` 是 `0x00000000`,注释直接写着 `Transparent`;可 `0x00000000` 不就是纯黑吗?所以 `DARK_BLACK = 0x00101010` 不是多此一举,是被透明色约定逼出来的妥协——一个肉眼几乎看不出区别、但 `draw_bitmap` 认它是「不透明实色」的近黑。终端图标主体那一大片 `1`,对应的就是 `DARK_BLACK`;只有真正想透过去(图标圆角外的边角)才用 `0`。连内核测试里那个程序化生成的 `build_test_icon`,主体填的也是 `DARK_BLACK`,只有四个角各 2×2 的小块填 `palette::BLACK`——同一个规矩。

换个角度看,色键透明不是免费的。它用一个**颜色**换一个**透明语义**:你把哪个颜色征用做色键,那个颜色就从可用调色板里消失。如果将来要画的东西真需要全光谱(比如一幅照片级图标需要纯黑),色键透明就不够用了,得上真 alpha 通道——每像素多一个透明度字节、绘制时做加权混合。但 032 的图标是扁平矢量风、颜色可控,色键透明是最划算的选择,前提是你记住「纯黑没了」。

这条也顺带解释了为什么 `0x00RRGGBB` 的高字节(`0x00`)在这套体系里**没**被当成 alpha 通道:这里不用真 alpha 混合,只用「全零 = 透明」这一个色键约定。高字节纯粹是 32 位对齐的填充。

### consteval 不进内核:测试为什么一拆为二

还有一个诚实的设计约束,它解释了这一章测试为什么分两层跑、为什么内核测试碰不到真图标。

`icon_data.hpp` 用了 `consteval`,这是 C++20 特性。可 Cinux 的**裸机内核构建没有 C++20**——freestanding 工具链对 C++20 的支持是个雷区,内核侧一贯只用到稳定的旧标准。结果就是:`k_shell_icon`、`k_calc_icon` 这两个 `consteval` 常量**根本进不了内核镜像**,它们只能在 host 测试里被编译。`test_bitmap_icon.cpp` 的文件头注释把这件事写得明明白白:

> `icon_data.hpp` uses consteval (C++20) which is not available in the freestanding kernel build, so icon_data constant tests and real icon rendering tests are covered in the host-side unit tests instead.

所以内核侧的 QEMU 测试怎么办?它**不碰**那两个 consteval 常量,改用两个手工构造的像素数组——`build_test_icon` 程序化生成「透明四角 + 边框 + 中心十字」、`build_test_icon2` 生成「LCD 屏 + 按键网格」——在运行时把 1024 个像素填出来,再喂给 `draw_bitmap`。这套手工数组既能验 `draw_bitmap` 的真实行为(它要碰 framebuffer,只能在机内测),又不依赖 C++20。真图标的像素值校验则留给 host 测试。

这是个被工具链现实逼出来的折中,不是设计缺陷。它把「验渲染逻辑」(机内,真实 framebuffer)和「验图标数据」(host,C++20 consteval 可用)拆到了两个地方,各测各的长处。

## 验证

按上一节说的,测试天然分两层。

**host 单元测试**——验 `draw_bitmap` 的纯逻辑(像素、透明、裁剪)和 `DesktopIcon::contains` 的边界,以及那两个 consteval 图标常量本身的像素值。host 测试文件自带一个 `MockCanvas`(把内核 `Canvas::draw_bitmap` 的逻辑原样镜像过来,跑在 `std::vector` 上),还自带一份 `DesktopIcon`/`IconAction` 的本地副本(不链内核、不 include `icon.hpp`——那个会拖进 consteval 数据),所以它能在没有 framebuffer、没有内核的环境下跑全:

```bash
cmake -B build && cmake --build build
ctest --test-dir build -R bitmap_icon --output-on-failure
```

`ctest` 名就是 `bitmap_icon`(注册在 `test/CMakeLists.txt` 里的 `add_test(NAME bitmap_icon ...)`),里面有 23 个 `TEST(...)` 用例,覆盖不透明渲染、透明跳过、棋盘格、四向裁剪、画布外 no-op、null 像素、零宽零高 no-op,以及 `contains` 的各种边界。

**QEMU 内核测试**——验 `draw_bitmap` 真实写 framebuffer 的行为,以及用手工 `build_test_icon` 数组画的 32×32 图标。`main_test.cpp` 注册了入口 `run_bitmap_icon_tests()`,机内跑:

```bash
cmake --build build --target run-kernel-test
```

预期串口出现 `Bitmap Icon Tests (032_gui_bitmap_icon)` 这个 section 标记,下面 17 个 `RUN_TEST` 全过,涵盖 `draw_bitmap` 的不透明渲染(2×2、原点、更大)、透明跳过(部分透明、全透明 no-op)、裁剪(右边界、下边界、画布外 no-op)、null 像素、两个手工 32×32 测试图标及并排带间隙渲染,还有 `DesktopIcon::contains`(内部 / 外部 / 负坐标 / 1×1 像素 / `IconAction` 枚举值)。GUI 关掉时(`-DCINUX_GUI=OFF`),`run_bitmap_icon_tests` 退化成一个 stub,只打一行 `[BITMAP_ICON] CLI mode -- GUI tests skipped.`——和别的 GUI 测试一样的退路。

**视觉效果**这一层 032 比较单薄。因为图标还没排上桌面(那是 033),单独 `make run` 看不到图标出现在屏幕上——桌面上依旧是空的。032 的视觉验证主要靠上面两层测试里的像素断言(「这个坐标应该是这个颜色」)。想亲眼看到图标画出来的样子,得等下一章把它们摆上桌面。

## 下一站

到 032,我们有了「画带透明度的小图」的能力,有了第一批编译期图标,也有了带命中框的 `DesktopIcon`。但这些东西还**散着**:`DesktopIcon` 没有被任何桌面收编,`IconAction` 没有被任何点击消费,用户在屏幕上看不到一个图标。`draw_bitmap` 甚至还没被任何 live GUI 代码调用过——这一章里它只有定义和测试在用。

下一步自然是把这些原语**焊成一个体验**:窗口管理器维护一组桌面图标、合成(composite)时把它们画出来、检测鼠标命中、按 `IconAction` 派发动作、打开对应的窗口(比如点终端图标开一个新的 shell 窗口)。那样,桌面才第一次有了「点图标开程序」的完整闭环。怎么把 032 这套原语焊成这个体验,是下一章的事。

---

**参考**

- 色键透明(color key,也叫 colorkeying):2D 图形的经典透明技法,指定一个像素值代表透明、绘制时跳过。`draw_bitmap` 用 `0x00000000` 作色键即此法。pygame `Surface.set_colorkey` 文档对「指定一个颜色值,所有匹配它的像素都不画」有清楚说明:<https://www.pygame.org/docs/ref/surface.html#pygame.Surface.set_colorkey>。
- `consteval`(C++20 立即函数):`build_icon` 强制编译期求值、零运行时开销、写错即编译失败的依据。cppreference `consteval specifier (since C++20)`:<https://en.cppreference.com/cpp/language/consteval>。
- `0x00RRGGBB` 整型打包 RGB:承接本仓库 029 章 `Canvas` 已确立的 32 位像素格式(高字节 `0x00` 为对齐填充,非 alpha)。pygame 用 `map_rgb`/`unmap_rgb` 把 RGB 打包成 Surface 相关的整数像素值,说明「RGB 打包进一个整数」是通用做法(具体位布局各系统不同,pygame 的并不固定为 `0x00RRGGBB`)。
