---
title: 02 · MMIO 范式
---

# MMIO 范式

## MMIO 范式:`mmio32_read/write` 的 volatile + byte-offset

先把底层那块搬出来。MMIO(Memory-Mapped I/O)说的是:把设备寄存器映射到一段物理地址区间(HPET 在 `0xFED00000`、LAPIC 在 `0xFEE00000`),CPU 用普通的 `mov` 读写它,不走 `in`/`out` 端口指令——后那是另一套 PIO 通道,见 [io.hpp](../../../kernel/arch/x86_64/io.hpp#L31-L45) 的 `io_inb`/`io_outb`,跟本章无关。MMIO 的好处是寻址空间大、能用普通内存指令批量操作;代价是访问纪律完全两样。

`mmio.hpp` 全文就十行,但每个字符都有理由:

```cpp
inline uint32_t mmio32_read(volatile uint32_t* base, uint32_t off) {
    return *reinterpret_cast<volatile uint32_t*>(
        reinterpret_cast<uintptr_t>(base) + off);
}

inline void mmio32_write(volatile uint32_t* base, uint32_t off, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(
        reinterpret_cast<uintptr_t>(base) + off) = value;
}
```

见 [mmio.hpp](../../../kernel/drivers/mmio.hpp#L18-L24)。两个设计点焊在签名里:

**(1) `volatile`**。编译器对普通 RAM 会做一堆优化——合并相邻写、消除「读了没用」的读、把「写后立刻读」优化成只读寄存器里的旧值。对 RAM 这些都是好事,对设备寄存器全是灾难。比如写 LAPIC 的 EOI 寄存器(中断结束信号)被编译器合并/消除掉,中断永远不结束,CPU 再也收不到下一个 IRQ。所以 MMIO 访问必须 `volatile`——每次 deref 真的去访问设备,不许缓存、不许合并。这个 `volatile uint32_t* base` 焊在签名上,谁也别想漏。

**(2) byte-offset,不是 element-index**。硬件文档(HPET 规范、Intel SDM LAPIC 章节)全用**字节偏移**记寄存器位置——HPET General Config 在 `0x010`、LAPIC EOI 在 `0x0B0`、LAPIC Timer 在 `0x320`。看 [local_apic.hpp](../../../kernel/drivers/apic/local_apic.hpp#L26-L36) 的常量:

```cpp
constexpr uint32_t kRegEoi          = 0x0B0;  ///< write 0 to end interrupt
constexpr uint32_t kRegLvtTimer     = 0x320;
constexpr uint32_t kRegTimerInit    = 0x380;
```

代码里 `read(kRegEoi)` 跟手册一字对齐。要是用元素下标 `base[0x30]`,C 指针算术对 `uint32_t*` 自动 `×4`,你每次都得心算「`0x0B0 / 4 = 0x2C`」——心算错一个寄存器就飞了。helper 内部 `reinterpret_cast<uintptr_t>(base) + off` 做的是字节加法,绕开了 `uint32_t*` 自动 `×4` 的隐式放大。LAPIC 的薄包装就这么一行,见 [local_apic.cpp](../../../kernel/drivers/apic/local_apic.cpp#L37-L43):

```cpp
uint32_t LocalAPIC::read(uint32_t off) const {
    return mmio32_read(base_, off);
}
void LocalAPIC::write(uint32_t off, uint32_t value) {
    mmio32_write(base_, off, value);
}
```

**为什么名字带 `32`,锁死访问宽度**。这是 QEMU(和部分真机)实机教训出来的——dev note 记了两个坑,本章把坑的**技术**讲清楚(批号那些不进教程):

- **坑之一:General Config 在 `0x010` 不在 `0x008`。** HPET 通用寄存器**间距 `0x10`**,不是密集布局。第一版按 `0x008` 写 ENABLE 位,死活不生效——读回恒 0。根因:`0x008` 落在 reserved 区,写全丢。改成 `0x010` 立刻通。诊断的权威是 QEMU 源码 `hw/timer/hpet.c` 的 `HPET_CFG` 宏,跟 [hpet.hpp](../../../kernel/drivers/hpet/hpet.hpp#L31-L34) 的常量一致:

  ```cpp
  constexpr uint32_t kHpetRegGeneralCaps = 0x000;  // [31:0]=vendor/rev, [63:32]=period(fs)
  constexpr uint32_t kHpetRegGeneralConfig =
      0x010;  // bit 0 = ENABLE_CNF (general regs are 0x10 apart)
  ```

  诊断时有个不对称的线索值得记:对 `0x000` 的**读**是对的(能拿到 `0x9896808086A201`,高 32 位 `0x00989680` 正是 100MHz 周期 fs 值),只有对 `0x008` 的**写**丢——这个「读对写错」正是定位偏移错的方向标。这个坑是 byte-offset 范式被实战逼出来的证据:偏移跟手册对齐不是审美,是「写错位置寄存器就不响应」的硬约束。

- **坑之二:一律 32-bit,别用 64-bit 写。** QEMU **丢 64-bit MMIO 写**——64-bit 写 Config 读回 0;但 32-bit 写 Main Counter(`0x0F0`)能写进 `0xDEADBEEF` 并读回。所以 HPET 读 64-bit 主计数器是「两次 32-bit 半读 + rollover 保护」,见 [hpet.cpp](../../../kernel/drivers/hpet/hpet.cpp#L49-L60):

  ```cpp
  uint64_t HPET::read_counter() const {
      uint32_t high1 = read32(kHpetRegMainCounter + 4);
      uint32_t low   = read32(kHpetRegMainCounter);
      uint32_t high2 = read32(kHpetRegMainCounter + 4);
      if (high1 != high2) {
          low = read32(kHpetRegMainCounter);  // 高半变了,低半重读
      }
      return (static_cast<uint64_t>(high2) << 32) | low;
  }
  ```

  高半两次读之间 low 可能 rollover,如果 high 变了就重读 low,标准无 race 算法。helper 名字带 `32` 就是把这个教训焊死。

最后是 `FLAG_PCD` 映射纪律——所有 MMIO 窗口映射都带 `FLAG_PCD`(Page Cache Disable),uncached。LAPIC 窗口在 [local_apic.cpp](../../../kernel/drivers/apic/local_apic.cpp#L23-L34) 映射时就带了:

```cpp
constexpr uint64_t kLapicMmioVirt = cinux::arch::KMEM_MMIO_BASE + 0x10000;
constexpr uint64_t kFlags =
    cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE | cinux::arch::FLAG_PCD;
if (!cinux::mm::g_vmm.map(kLapicMmioVirt, mmio_phys, kFlags)) {
    return false;
}
```

理由同一类:cache 命中会给设备「假数据」或丢写。013 讲过 `map_mmio` 的恒等映射,这里不重讲页表细节,只点纪律——**volatile 管编译器优化,uncached 管 CPU cache,两条加一起才把 MMIO 访问钉住**。

诚实边界:这套 helper 是「共享范式的提取」,实际只有 LocalAPIC 和 HPET 经它走。别的 MMIO 设备(e1000、virtio、xHCI)各自内联同款 `volatile` deref——**范式一致,未收编**。IOAPIC 甚至走的是反面教材,见 [io_apic.cpp](../../../kernel/drivers/apic/io_apic.cpp#L34-L42):

```cpp
uint32_t IOAPIC::read(uint32_t reg) {
    base_[kIoapicIORegSel / 4] = reg;   // ← 元素下标 base_[常量/4],不是 byte-offset
    return base_[kIoapicIOWin / 4];
}
```

IOAPIC 这里的 `base_[kIoapicIORegSel / 4]`、`base_[kIoapicIOWin / 4]` 是两个**写死的常量偏移**(`kIoapicIORegSel`、`kIoapicIOWin` 是固定 byte offset,运行时 `/4` 折成元素下标),跟 helper 的 byte-offset 范式相反。这不算 bug(IOAPIC 寄存器是「先写选择口再读数据口」的 indirect 寻址,跟直接 MMIO 不一样),但风格上没对齐——是「范式还没收编所有驱动」的诚实留白。

