---
title: Lab 032 · 位图与图标:让画布画出会镂空的小图
---

# Lab 032 · 位图与图标:让画布画出会镂空的小图

> 对应主书 [032 · 位图与图标](../../book/09-gui/032-gui-bitmap-icon.md)。029 给了我们一块能画像素/矩形/直线/文字的双缓冲画布,但桌面图标是「不规则形状 + 透明背景的小彩色位图」,`draw_rect` 和 `draw_text` 都画不出来。032 补两件东西:一个能逐像素 blit、能跳过透明色的 `draw_bitmap`;以及把「字符画」编译成像素数组的图标数据 + 一个能被鼠标命中的 `DesktopIcon`。**注意边界**:本 lab 你不会把图标摆到桌面、也不会点图标开窗——那是 033。032 只是「颜料和模具」齐了,屏幕上还看不到图标。这里给签名、给约束、给验证手段,不贴完整答案代码。

## 实验目标

- 实现 `Canvas::draw_bitmap`:逐像素从源数组写到 back buffer,`0x00000000` 当透明跳过,越界按行/列裁剪,`nullptr` 防御。
- 理解「为什么 `0x00000000` 既是黑色又是透明色」,并说出调色板用 `0x00101010` 近似不透明纯黑的**原因**。
- 用 consteval(C++20)`build_icon` 把一张 32×32 字符画经 16 色调色板映射成 `std::array<uint32_t,1024>`;并解释为什么真图标常量只进 host 测试、QEMU 内核测试改用手工像素。
- 定义 `DesktopIcon` 与 `IconAction`,写出 `contains()` 的**半开矩形**命中,并解释「为什么右下是开区间」。

## 前置条件

- 已完成 029:理解 `Canvas` 的 back buffer、`pitch_/4` 的「一行多少个 uint32」、`0x00RRGGBB` 像素打包、`flip()` 逐行拷贝。
- 读懂主书第 032 章「为什么 029 不够」「那个没有纯黑的调色板」「DesktopIcon」三节。
- 构建:`cmake -B build && cmake --build build`(默认 `CINUX_GUI=ON`)。

## 任务分解

### 任务 1:实现 `draw_bitmap`,带透明跳过与边界裁剪

给 `Canvas` 加成员,签名是:

```text
void draw_bitmap(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 const uint32_t* pixels);
```

要求(自己想清楚每条的**为什么**):

- 目的地是 back buffer,写入索引沿用 029 公式 `back_buf_[row * (pitch_/4) + col]`——注意 `pixels_per_row = pitch_/4`,不是 `width_*4`,也不是 `pitch_`(那是字节)。把「每行字节数」当成「每行像素数」就错了。
- 逐像素拷贝。源像素等于 `0x00000000` 时**跳过、不写**,继续下一个像素(这就是「镂空贴纸」)。
- 越界裁剪:发现一个像素落在画布外(列或行),按方向直接 `break` 跳出当前行/列,而不是只跳过这一个像素再 `continue`。想清楚为什么裁剪要用 `break`:位图是矩形,一行画完就该进下一行源像素,你若 `continue` 掉一个越界像素、还在同一行继续读源像素,源/目的行号就对不上了。
- `back_buf_ == nullptr || pixels == nullptr` 直接返回,什么都不画。

实现位置见 [canvas.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/canvas.hpp) 声明、[canvas.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/drivers/canvas.cpp) 定义。029 的 `draw_pixel`/`blit` 是它的参照模板。

### 任务 2:解释那个「没有纯黑」的调色板

读 [icon_data.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/data/icon_data.hpp) 的调色板,你会看到两条相邻注释(大意):`BLACK = 0x00000000 —— Transparent (skipped by draw_bitmap)`、`DARK_BLACK = 0x00101010 —— Near-black (opaque)`。请口头回答:

- 想画一个**不透明纯黑**像素,为什么不能直接用 `0x00000000`?(任务 1 里它被当透明跳过,会留一个洞。)
- `DARK_BLACK` 用 `0x00101010` 在妥协什么?(在「肉眼接近黑」与「不被透明跳过」之间取折中,代价是它其实是个极暗的灰。)
- 这种「指定一个颜色当透明、画到它就跳过」的 2D 贴图技法叫什么?(color key / 色键透明,pygame 的 `set_colorkey` 同一思路。)

这一步不动代码,是为了让你记牢:**本工程像素格式里「黑」和「透明」撞了同一个值**,别拿 0 当黑色用。

### 任务 3:用 consteval 把字符画编译成像素

读 [icon_data.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/data/icon_data.hpp) 里的 `build_icon<Rows>(palette, rows)`(用 `hex_nibble` 把每个十六进制字符变成 0–15,再用 `palette_lookup` 查调色板那 16 项,拼成 `std::array<uint32_t,1024>`)。然后:

- 自己写一张**极简**的 32×32 字符画(一个能看懂的形状即可:实心方块、十字、字母),不要照抄仓库里 `k_shell_icon`/`k_calc_icon` 的全图。
- 配一张 16 色调色板,把字符画里用到的每个十六进制字符映射到一个 `0x00RRGGBB` 值。你的「黑」要用 `DARK_BLACK` 而不是 `0x00000000`(除非你本来就想要镂空)。
- 用 `consteval`/`constexpr` 把它变成一个 `std::array<uint32_t,1024>` 常量,确认它在编译期就能求值。

> 接口约束:`ICON_SIZE = 32`、`ICON_PIXELS = 1024`,见 [icon.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/icon.hpp)(`namespace cinux::gui::icons`)。调色板索引 0 在本工程惯例里就是透明色 `0x00000000`。

**关键约束**:裸机内核构建没有 C++20 consteval。你的真图标常量**只在 host 测试里能编译**;QEMU 内核测试([test_bitmap_icon.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/test/test_bitmap_icon.cpp))改用**手工构造**的像素数组(`build_test_icon`/`build_test_icon2`),不碰 consteval 数据。想清楚为什么 host 测试在 include `desktop_icon.hpp` 时特意标注了「no C++20 dependency」、而且它**不 include `icon.hpp`/`icon_data.hpp`**。

### 任务 4:定义 DesktopIcon 与半开矩形命中

参照 [desktop_icon.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/desktop_icon.hpp),实现:

- `enum class IconAction : uint8_t { None = 0, OpenShell = 1, OpenCalculator = 2 };`
- `struct DesktopIcon { int32_t x, y; const uint32_t* bitmap; const char* label; uint32_t width, height; IconAction action; };`
- `[[nodiscard]] inline bool contains(int32_t mx, int32_t my) const;`

命中判定的**正确写法是半开矩形**:

```text
mx >= x && mx < int(x + width) && my >= y && my < int(y + height)
```

自己回答:

- 为什么右边界和下边界是 `<`(开区间)而不是 `<=`?(用 `<=`,相邻两个图标共享的那一条边界像素会被**两个图标都**命中,鼠标停在缝上同时命中两个——开区间让每个图标独占 `[x, x+width)` 这一段,无缝重叠。)
- 给定 `x=10, width=32`,`mx=41` 命中、`mx=42` 不命中,和半开矩形对得上吗?(对得上:`41 < 42` 真,`42 < 42` 假。)
- `IconAction` 的值为什么从 0 开始连续递增?(0 当「无动作」哨兵,后续值方便当小整数直接比较或当数组下标。)

> 接口约束:`contains` 是 inline 成员,只读不改;`IconAction` 在 032 **只是定义**,「点了图标派发什么动作」的逻辑在 033,本 lab 不要写派发代码。

## 接口约束(汇总)

- `Canvas::draw_bitmap(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t* pixels)`:写 back buffer;`pixels_per_row = pitch_/4`;`0x00000000` 跳过;越界 `break` 裁剪;`nullptr` 直接返回。
- `namespace cinux::gui::icons`:`constexpr uint32_t ICON_SIZE = 32`、`ICON_PIXELS = 1024`;re-export [icon_data.hpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/gui/data/icon_data.hpp)。
- `build_icon`:`consteval`、`static_assert(Rows == 32)`、产出 `std::array<uint32_t,1024>`;**仅 host 编译**,不进裸机内核。
- `namespace cinux::gui`:`IconAction`、`DesktopIcon`、`contains`(半开矩形)。头文件只在 `CINUX_GUI` 定义时编译。

## 验证步骤

- **Host 单测**:ctest 名 `bitmap_icon`。

  ```bash
  ctest --test-dir build -R bitmap_icon --output-on-failure
  ```

  预期全绿。它压 `draw_bitmap` 的算法(不透明/透明跳过/裁剪/null 防御)和 `contains` 的逻辑(内/外/边界/负坐标/1×1),用文件自带的 MockCanvas 与本地 `DesktopIcon` 副本,不链内核、不 include `icon.hpp`。

- **QEMU 内核测试**:目标 `run-kernel-test`。

  ```bash
  cmake --build build --target run-kernel-test
  ```

  预期串口出现 `=== Bitmap Icon Tests (032_gui_bitmap_icon) ===` section 标记,其后逐个 `RUN_TEST` 通过(`test_bitmap_render_opaque`、`test_bitmap_transparent_skip`、`test_bitmap_clip_right`、`test_desktop_icon_contains_inside`、`test_icon_action_values` 等,共 17 个),最终 `TEST_SUMMARY` 全过。这里测的是手工像素 `build_test_icon`,不是 consteval 常量。

- **GUI 关闭回归**:`cmake -B build -DCINUX_GUI=OFF && cmake --build build && cmake --build build --target run-kernel-test`,串口应打 `[BITMAP_ICON] CLI mode -- GUI tests skipped.`,而非报错。

## 常见故障

- **画出来的图标有个「透明的洞」**:你把要当不透明黑色的像素写成了 `0x00000000`,被任务 1 的透明判定跳过了。改用 `0x00101010`(`DARK_BLACK`)。这是本 lab 最容易踩的坑,也是任务 2 要你记牢的点。
- **图标整行错位/画面倾斜**:`pixels_per_row` 算成了 `width_*4` 或 `pitch_`(字节)而不是 `pitch_/4`(像素数)。029 就强调过 pitch 是「每行字节数」,换算成「每行多少个 uint32」要除 4。
- **裁剪写成「逐像素 `continue`」导致整张图右移**:位图是矩形,一行画完就该进下一行源像素。越界后应 `break` 跳到下一行,不是把越界像素 `continue` 掉、还在同一行继续读源像素——那样源/目的行号就对不上了。
- **在内核源文件 include `icon_data.hpp` 后编译失败**:裸机内核构建不带 C++20 consteval。真图标常量只进 host 测试;QEMU 测试用手工像素。别把 consteval 数据拖进内核链。
- **`contains` 写成闭区间 `<=`,相邻图标边界双重命中**:右下边界必须是开区间 `<`。用 `x=10, width=32` 检验:`mx=42` 应判「不命中」。
- **QEMU 打印 `[BITMAP_ICON] CLI mode -- GUI tests skipped.`**:你用 `-DCINUX_GUI=OFF` 关了 GUI,这是预期行为不是 bug;要真跑 Bitmap Icon section 就别关 GUI。

## 通过标准

- 任务 1:`draw_bitmap` 实现正确,host `ctest -R bitmap_icon` 全绿,透明跳过与边界裁剪用例尤其要对。
- 任务 2:能说清「`0x00000000` 既是黑又是透明」的冲突,以及为什么调色板用 `0x00101010` 近似不透明黑。
- 任务 3:能用 consteval/constexpr 把一张**自己的**(非照抄)32×32 字符画编译成 `std::array<uint32_t,1024>`;能解释为什么真图标常量只进 host 测试、QEMU 改用手工像素。
- 任务 4:`DesktopIcon::contains` 是半开矩形,能解释右下开区间防止相邻图标边界双重命中;`IconAction` 值定义正确(0/1/2)。
- 两套验证都过:host `ctest -R bitmap_icon` 全绿;`run-kernel-test` 的 Bitmap Icon section 17 个 `RUN_TEST` 全过。
- 能口头回答:`draw_bitmap` 为什么用 `break` 而不是 `continue` 裁剪?consteval 为什么进不了裸机内核?半开矩形的右下边界为什么必须是 `<`?
