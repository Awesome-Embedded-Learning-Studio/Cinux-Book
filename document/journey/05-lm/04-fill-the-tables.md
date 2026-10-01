---
title: 04 · 把三张表填起来
description: "当年 rep stosl 的清零换成 512 次 for,三张表的地址从 layout 的法条派生;填完表,32 位世界把接力棒交给两个 extern &quot;C&quot; 的函数。"
chapter: 5
order: 4
platform: qemu
difficulty: beginner
cpp_standard: 23
tags:
  - qemu
  - beginner
  - paging
  - stage2
---

# 把三张表填起来

语法有了,地界也有了,咱们动笔填表。填表的代码住在 32 位的世界里,跟上一卷的 `pm.cpp` 是同一个院子,都是 `boot_pm` 这个 OBJECT 库的住户。整个 `boot/lm/page_tables.cpp` 不过四十来行的身量,咱们全量看:

```cpp
#include "cinux/page/page_entry.hpp"
#include "lm.hpp"

namespace {
using cinux::base::page::Entry;
using cinux::base::page::kLargePageSize;
using cinux::base::page::kWritable;
using cinux::base::page::MakeLargePageEntry;
using cinux::base::page::MakeTableEntry;

using cinux::boot::lm::kPdPhys;
using cinux::boot::lm::kPdptPhys;
using cinux::boot::lm::kPml4Phys;

constexpr unsigned int kEntriesPerPage = 512;

// NOLINTBEGIN(performance-no-int-to-ptr)
Entry* table_at(unsigned long physical) {
    return reinterpret_cast<Entry*>(physical);
}
// NOLINTEND(performance-no-int-to-ptr)
}  // namespace

extern "C" void BuildTemporaryPageTables() {
    Entry* const kPml4 = table_at(kPml4Phys);
    Entry* const kPdpt = table_at(kPdptPhys);
    Entry* const kPd   = table_at(kPdPhys);

    for (unsigned int i = 0; i < kEntriesPerPage; ++i) {
        kPml4[i] = Entry{};
        kPdpt[i] = Entry{};
        kPd[i]   = Entry{};
    }

    kPml4[0] = MakeTableEntry(kPdptPhys, kWritable);
    kPdpt[0] = MakeTableEntry(kPdPhys, kWritable);

    for (unsigned long i = 0; i < 4; ++i) {
        kPd[i] = MakeLargePageEntry(i * kLargePageSize, kWritable);
    }
}
```

咱们进门后的头一件事是清零,三张表咱们各来一遍 512 次。咱们为什么非清零不可?咱们从 MMU 的视角想:PG 亮起之后,表里的每一颗项,它都会当正经的项来读,咱们没填的 511 颗要是残留着上辈子的字节,那就是 511 条来历不明的路。而在当年第一遍里,这页清零是拿汇编写的:`rep stosl` 连发 1024 个 dword、三张表要来上三回,是汇编里最顺手的清障招式。这一遍咱们写成给 `Entry` 赋零值的循环。用 C++ 的循环替掉 `rep stosl`,图的是填表的每一步都落在 base 的语法里、有断言罩着,填错了的写法在编译期就有回应。boot 这边剩下的,只有搬运了。您看末尾的那个循环,它的循环变量是 `unsigned long`,乘的是 `uint64_t` 的 `kLargePageSize`,所以 8 字节的一致性从共享的头文件一路贯到了这一行,实地探测里那场 4 字节的暗亏,在这儿是天然免疫的。

`table_at` 里的那个 `reinterpret_cast`,您应该认出来了:它跟上一卷 GDTR 的 base 字段,走的是同一条家法。表的地址是 layout 立法的物理事实,而不是咱们算出来的指针,而整数常量借一个 cast 还给 MMU,再配一块 NOLINT 就把意图写明白了。而这个事实的家,咱们安在 `boot/lm/lm.hpp`:

```cpp
inline constexpr unsigned long kPml4Phys = kPageTables.base;
inline constexpr unsigned long kPdptPhys = kPml4Phys + 0x1000;
inline constexpr unsigned long kPdPhys = kPml4Phys + 0x2000;
```

三张表的地址,咱们都从 `kPageTables` 派生:根就是基地址,另外两张跟在根的后面、各隔一页。共用一个根的好处,到本卷末尾就兑现了——切换序列里拨给 CR3 的那个立即数,读的也是同一个常量,将来改地界时,要改的也只有一处。断言替咱们把两头都管住了:根必须踩在 4K 的对齐上,而尾巴加一页也不许探出地界。

函数名里带着的 `Temporary`,咱们把话说在名上:这座表只服务这一次的过桥,把执行流送过去了,它的活就算干完了。等内核将来在高处安了家、内存管理接管了页表,它自会立自己的正经表,这座桥到时候就退役了。

表填完了,谁来叫它?咱们看上一卷 `pm.cpp` 的尾部,在那行 `[pm] 32-bit world alive` 的下面,接着的就是这两句:

```cpp
    cinux::boot::serial::PutString("[pm] 32-bit world alive\n");
    BuildTemporaryPageTables();
    EnterLongMode();
```

两个声明放在文件顶上的 include 区,都挂了 `extern "C"`——跨世界被引用的符号要走 C 的链接约定,而这样的约定,咱们从 MBR 起立到了今天。而上一卷里,笔者把声明塞进过匿名 namespace,也吃过一回它的亏,这一回声明就直接站在了明处。您还注意到,原来那个 `hlt` 循环没有了,因为 `EnterLongMode` 标着的是 `[[noreturn]]`,它是一条没有回程的路,睡下去就没有人再醒了。所以那句 `[pm] 32-bit world alive`,就是 32 位世界说的最后一句话:它后面接的建表和切换,再添的输出一行也没有。
