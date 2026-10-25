---
title: "03 · 第一棵树"
description: "目录装名字,文件装字节,这一章只管装名字的那一半。DirNode 以 CRTP 长在每个后端的节点上:名字、类型、inode 号、兄弟链全归树,货归派生。孩子头插,最新的排最前,ls 的输出次序从这里来。named 三重比对,disown 摘边不销毁,WalkPath 拿着 PathCursor 逐组件下爬。"
chapter: 14
order: 3
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - kernel
  - filesystem
---

# 第一棵树

树管什么?目录装名字,文件装字节。这一章咱们只管装名字的那一半,名字挂在哪个结构上,怎么找、怎么添、怎么摘。装字节的那一半,归下一章的后端。

结构的形状,咱们用模板接住了后端的节点:

```cpp
template <typename Node>
struct DirNode {
    char          name[kFsNameMax] = {};
    InodeType     type             = InodeType::kFile;
    unsigned long ino              = 0;
    Node*         first_child      = nullptr;
    Node*         next_sibling     = nullptr;
};
```

`DirNode` 拿的是派生类型的名字,链上挂的指针也全是派生型的。这个写法的名字叫 CRTP,全称是 Curiously Recurring Template Pattern——好奇的递归模板模式,说的就是派生类把自己当作模板参数递回基类的一手。咱们直接说它的用处:树的这一半只管结构,节点的名字、类型、inode 号(索引节点号,UNIX 给节点发的身份证号)跟孩子链,全搁在这几行里了。货那一半呢,派生的类型自己往身上添。ramfs 往节点上挂的是堆里的字节缓冲,将来的后端挂它自己的东西,两边爬树用的路子是同一份,谁也不必重写一遍找孩子、摘孩子的代码。

孩子怎么排?一个目录的孩子,住在一条兄弟链上,`first_child` 指着头,每个孩子拿 `next_sibling` 指下一个。新孩子进门的那一下值得您多看一眼:

```cpp
void adopt(Node& child) {
    child.next_sibling = first_child;
    first_child        = &child;
}
```

头插法,两行:新孩子的下一手牵住老的头,头换成人。后果是链上永远是最新的孩子排在最前头。这个次序眼下看着只是个写法,到验收那一章它就是屏幕上的现象了——您 ls 一遍,刚创建的名字永远排在清单的头一行,咱们到时候再对着屏幕认一遍。

找孩子靠的是名字。一个名字怎么算相等,咱们立的是三重比对,立的地方就在树这一半的成员函数 `named` 里,`find` 找孩子的每一回都得打它跟前过:

```cpp
[[nodiscard]] bool named(std::string_view wanted) const {
    return wanted.size() < kFsNameMax && base::EqualBytes(name, wanted.data(), wanted.size()) &&
           name[wanted.size()] == '\0';
}
```

咱们把三个条件从左往右数。头一重管的是长度,看它超没超上限——它排在头一个是有讲究的:后两重要拿 `wanted.size()` 当下标去读 `name` 的数组,来路是客人路径的视图,要是比三十二格的数组还长,这一读就越界了,把长度的比对放在头里,栏杆也就立在了头里。第二重管的是内容,要求字节一个对一个地相等,拿 `wanted` 的长度做基准。第三重是最要紧的一重,验的是 `name[wanted.size()]` 这个字节收没收在 NUL 上。为什么光有第二重不够?您拿 `note.txt` 去问一个存着 `note.txtx` 的节点,逐字节的比对只比前八个字节,全对——可人家的名字长一截,就算不得同一个名字了。第三重在比过的下一个字节上验明,存着的名字确实在这么长的地方收了尾,前缀的搅局,就是这么挡住的。

摘孩子的 `disown`,咱们立的是摘边不销毁:把孩子从链上解下来,节点原样交回调用者手里,杀不杀、什么时候杀,树不插手。链头和链中间两种位置,解法不同:链头直接挪头指针,链中间呢,得让前一个孩子的手跨过被摘的那个。函数里那根 `previous` 指针,走的就是这么一趟。

最后是爬树本身。`WalkPath` 拿着上一章的 `PathCursor`,咱们跟着它一个组件一个组件往下走:脚下的节点得是目录,不是就回 `kNotADirectory`——半道上踩着一个文件,后面的组件没了落脚处。找名字,找不到回 `kNotFound`。走到了头,脚下的节点就是答案。空的路名不发组件,一步不走,答案就是根本身。

树是骨架,骨架上还没长肉:名字挂上去了,文件的内容住哪?下一章,咱们把字节种进堆里——头一个真正的后端,ramfs。
