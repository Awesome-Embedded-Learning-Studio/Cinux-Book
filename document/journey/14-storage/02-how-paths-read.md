---
title: "02 · 路名怎么念,名字多长算长"
description: "名字世界的地基是一对纯函数:PathCursor 逐组件发路名,多重斜杠照收,全斜杠发空;SplitLeaf 在末一道斜杠处分成父路与叶。名字与路径的长度上限收进一份三常数的小配置,全楼层共用一张尺寸表。路径以视图旅行,一个字节都不抄。"
chapter: 14
order: 2
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - filesystem
---

# 路名怎么念,名字多长算长

立名字的世界,咱们从最底下的零件动起:一条路名,怎么念。`/docs/note.txt` 这样的路,人眼一看就分成了三段,可机器拿到手的是一串字节,分段这件事得有人干。干这件事的代码,咱们放在 base 里,做成一对纯函数,谁的仓库都不沾——它念的是字节,不问字节背后住的是内存还是盘。

头一件是逐段发放的路标,名字叫 `PathCursor`。它朴素得让您想笑:一个结构,就一个成员,装着还没念完的路:

```cpp
struct PathCursor {
    std::string_view rest = {};
};

[[nodiscard]] inline bool NextComponent(PathCursor& walk, std::string_view& component) {
    while (!walk.rest.empty() && walk.rest.front() == '/') {
        walk.rest.remove_prefix(1);
    }
    if (walk.rest.empty()) {
        return false;
    }
    const unsigned long kCut = walk.rest.find('/');
    component                = walk.rest.substr(0, kCut);
    walk.rest.remove_prefix(kCut > walk.rest.size() ? walk.rest.size() : kCut);
    return true;
}
```

咱们拿一条真的路走一遍。`a//b`:头一回进函数,开头不是斜杠,查找函数报出第一道斜杠在 1 号位,段名 `a` 交出去,游标挪到 `/b`。第二回进门,开头的斜杠被头一道循环剥掉,剩下的 `b` 整个成为段名,再往后就没有斜杠了,游标一步挪到头。第三回,路空了,回一声假——念完了。您看,中间那道多出来的斜杠,就这么无声无息地吞掉了。路名的这一头要是全剩斜杠,`a/` 也好、光杆的 `///` 也好,一段名都发不出来,函数如实回假。人写路径手一抖多敲一道斜杠是常事,念路的人不跟手抖计较。

第二件是分叶子:`SplitLeaf` 在最后一道斜杠处把路分成两半,前半是父路,后半是叶。`docs/note.txt` 分出父路 `docs` 与叶 `note.txt`,删名字的调用要的就是这么一对。`readme.txt` 里没有斜杠,父路空着,叶子就是它自己。`docs/` 的叶子是空串。删名字的那类调用全指着它:要删谁,得找到它的父目录,再报出名字。叶子是空的,说明来路没报清楚,后头您会看到,这样的请求当场退回。

两件家伙合起来,还有一个脾气值得您多看一眼:路径以视图旅行。`string_view` 拿的是别人手里的字节,从头到尾一个字节都不抄。念到哪,视图跟到哪,树爬完,一份拷贝都没发生过。这个脾气咱们是从头立的——将来的路径动辄几十个字节,每层楼抄一遍,堆就白忙了。

名字多长算长,得全楼层坐同一张表。咱们立的小配置,拢共三行:

```cpp
inline constexpr unsigned long kFsNameMax = 32;
inline constexpr unsigned long kFsPathMax = 96;
inline constexpr unsigned long kMountMax  = 8;
```

名字的上限是 32,NUL 算在里头,真正能用的名字是三十一个字节。路的上限 96,挂载点的座位数 8,数目您记个脸熟就好,后面各归各家。上限收在公共配置里,理由很实际:名字的尺寸是跨后端的合同,后端的节点名、递给客人的目录条目,坐的都得是同一张尺寸表,哪一家自说自话,跨过去的那一刻就对不上数了。往后真有哪一位后端嫌三十一个字节短,抬的就是这儿,全楼层跟着变。

路名会念了,长短有了法度。名字光会念还不够,得有个地方把它们挂起来——下一章,咱们种头一棵树。
