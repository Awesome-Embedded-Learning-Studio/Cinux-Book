---
title: 07 · 调试现场 + 保留模式 vs 即时模式
---

# 调试现场 + 保留模式 vs 即时模式

## 调试现场

这一章没有现成的踩坑笔记(本 tag 的 notes 是空的),但有三个从源码和注释里就能看出来的、真实可复现的坑,提前点破——它们都长着同一张脸:"保留模式下,谁让出来的那块没人标脏"。

### 敲退格键字符被吃掉:BS 和 DEL 混了

**症状。** shell 里按左箭头(或者某些 shell 配置下的 Backspace),光标扫过的字符一个个消失了,光标停在哪儿、那儿就空了。

**根因。** ANSI 的 BS(`\b` 0x08)语义只移光标、不擦字符;真正删字是 DEL(0x7f)。可早期版本的 TerminalWidget 把 BS 当退格擦字用——`put_char_` 见到 `\b` 就 `--cur_col_` **并擦掉那一格的 cell`**。shell 行编辑发左箭头是 `\x1b[D`(光标左移,纯移不删),可有些 shell 把 Backspace 映射成 `\b`——一旦 BS 被实现成"移 + 擦",每收到一个 `\b` 就吃一个字。

**解法**。源码现在的实现严格分开:BS 只 `--cur_col_`,DEL 才 `--cur_col_` + 清 cell。注释里写得很清楚:`ANSI BS only moves the cursor; ash line-edit shifts the cursor with \b, so erasing here wiped every glyph it passed`。源码见 [`terminal.cpp`](../../../libs/gui/core/widget/terminal.cpp#L286-L304)。这个判据要带走:**控制字符的语义不能靠猜,得查 ANSI/VT100 规范——BS 移、DEL 擦,是两件事**。

### 光标移动留拖影:dirty 没覆盖旧光标行

**症状。** 终端能用,可每次光标换行(比如敲回车),原来光标位置那块白色方块不消失,屏幕上拖出一串光标残影。

**根因。** `put_char_` 写 cell 时只标了**新行**脏(`dirty_rows_[cur_row_]=true`)。光标从 row 5 跑到 row 6,row 5 那块光标方块不属于"被写过的行",`collect_dirty` 不会把它收进脏区——合成器不重画 row 5,白色方块就留在屏幕上。光标移动越频繁(每敲一个字光标都在动),残影越明显。

**解法**。`collect_dirty` 显式把**当前光标行 + 上一帧光标行**都加进脏区:

```cpp
const uint32_t cursor_rows[2] = {cur_row_, prev_cursor_row_};
for (uint32_t i = 0u; i < 2u; ++i) {
    const uint32_t r = cursor_rows[i];
    if (r < rows_) {
        const int32_t ry0 = y0 + static_cast<int32_t>(r * kGlyphH);
        sink.add(Rect{x0, ry0, x1, ry0 + static_cast<int32_t>(kGlyphH)});
    }
}
```

`clear_dirty` 里把 `prev_cursor_row_` 更新成这一帧的 `cur_row_`,下一帧 `collect` 就能找到"光标刚离开的那一行"。源码见 [`terminal.cpp`](../../../libs/gui/core/widget/terminal.cpp#L366-L379)。这个判据也通用:**任何"位置会移动的可视元素"都要把旧位置 + 新位置都标脏,否则旧位置必然留残影**——光标如此、拖动窗口如此(`move_to_` 标 old + new)、鼠标指针也如此(`process_pointer` 里 `invalidate` old footprint + new footprint)。

### 关窗留残影:remove_window 漏标 stale footprint

**症状。** 点窗口右上角 "x" 关掉,窗口本身消失了,可它原来盖着的那块矩形——本该露出来的桌面背景或下层窗口——画面上还留着关掉那个窗口的像素(标题栏、内容、边角),像一块"幽灵"贴在那儿,直到下次别的东西触发那一块重画才被覆盖掉。

**根因。** `remove_window` 把 Window 从 `windows_[]` 摘出去之后,WM 自己的 `dirty_rect_` 没把这块矩形加进来。`collect_dirty` 只收 `dirty_self_` 标了的矩形——窗口被摘掉时没人标脏,合成器下一帧不知道"这块要重画",屏幕上那块像素就 stale 了。源码注释把这事说得很直白:这是 core 的一个真 bug,host 之前的" workaround"是每帧都全屏 dirty flush(等于把脏区优化废掉);正解是在摘窗口那一刻把它的 rect 标进脏区。

**解法**。摘窗口前**先 capture 它的 footprint**(`const Rect stale = w->rect()`),摘完立刻 `invalidate(stale)`,跟 `add_window` 的 `invalidate()` 镜像:

```cpp
void WindowManager::remove_window(Window* w) {
    const int32_t idx = index_of_(w);
    if (idx < 0) { return; }
    // Capture the footprint BEFORE unlinking: once the window leaves the list
    // its rect is stale, and core must repaint that area (background / windows
    // below) or the closed window's pixels stay on screen.
    const Rect stale = w->rect();
    /* ... 从 windows_[] 摘掉、--count_、清 press_target_ ... */
    invalidate(stale);                       // 露出来的那块要重画
    if (on_remove_cb_ != nullptr) {
        on_remove_cb_(on_remove_ctx_, w);    // host: 拆 per-window 状态(fd 等)
    }
}
```

源码见 [`window_manager.cpp`](../../../libs/gui/core/widget/window_manager.cpp#L39-L62)。这跟 Window 的 `move_to_` 标 old footprint 是同一类问题:**保留模式下,"一个会消失/会移动的东西让出来的那块"必须有人显式标脏**——即时模式全屏重画自动解决、保留模式必须显式。这判据在本章里已经是第三次出现了(光标拖影、窗口移动、窗口关闭),值得记死。

## 保留模式 vs 即时模式:为什么换

讲了这么多,值得回头问一句:029 的 `Canvas` 即时模式(DrawRect 当场写像素)有什么不好,非得换保留模式(PaintList 收集指令再批量落屏)?三个理由,正好对应这一章点亮的三个能力。

**第一,脏区重绘。** 即时模式画一次就写一次像素,想"只重画变化的部分"得自己记哪些像素变了。保留模式天然有这个:控件改状态只标脏(`invalidate(Rect)`),`collect_dirty` 在帧边界把脏区收集成一组矩形,合成器只在这些矩形内重画。shell 输出一行字,只上传 704×16 那一小块,而不是整屏 720×440——上传量砍 90% 以上,WSLg 这种 streaming upload 慢的环境才能用。

**第二,批量合成 + 裁剪。** 即时模式下每个 `draw_rect` 立刻写像素,没有"全局视角"。保留模式一帧的指令全在 PaintList 里,合成器可以裁剪(每条 cmd 跟 clip 求交)、可以重排(将来按纹理分批)、可以跳过(clip 外的 cmd O(1) 跳)。窗口层级裁剪也是这么来的——`flatten` 的 clip 栈保证控件画不出祖先矩形,即时模式要做到这点得每个 `draw_*` 自己算偏移 + 裁剪。

**第三,跨进程共享。** 这是 087 把 GUI host 搬到用户态的地基。PaintList 是纯数据(`PaintCmd` 是 POD,无指针除了 `text` 借用的字符串),可以序列化进共享内存或 socket、丢给另一个进程的合成器落屏。087 那条 `/dev/fb0` mmap 路径——ring3 进程 mmap 显存、直接画像素——本质上就是 host 进程持有一个 staging Surface、由它自己的合成器执行 PaintList。即时模式下控件直接写物理显存,这是内核态才能干的活;保留模式把"产出指令"和"落屏"分开,前者任意进程能做、后者才需要显存访问权。这就是为什么 087 能让 GUI host 跑在用户态——它跑的就是这一章这套 Widget 树 + PaintList,只是 host 层从 SDL 换成了 `/dev/event0` + `/dev/fb0`。

至于 shell 字节通道:这一章用的是 POSIX `forkpty`(host 进程和 shell 都在 ring3、同一个 Linux 主机)。真要搬到 Cinux 内核里跑,这条通道就得换成内核的 PTY 设备(`/dev/ptmx` + `/dev/pts/N`,066 立的)或者 AF_UNIX socket(083 立的)——字节从 GUI host 进程的 fd 出去、经内核 PTY/socket、到 shell 进程的 fd 0/1 进来。Widget 树和 PaintList 一行不用改,改的只是 host 层那条字节管道。

