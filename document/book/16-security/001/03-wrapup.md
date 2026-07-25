---
title: 03 · 验证与诚实的边界
---

# 验证与诚实的边界

## 怎么验证一个「开了但不一定生效」的机制

这三个位开了之后,怎么知道真生效?最直白的办法是「故意触发一次攻击看它拦没拦」——比如往 NX 的栈页里塞 shellcode 然后跳过去,看是不是 #PF。但这在测试里又难造又脆。Cinux 走的是**机制回读**:直接读 EFER 和 CR4,看那几个位是不是按预期设上了。这在 `test_usermode.cpp` 的 `test_f9_nxe_smep_smap_enabled`:

```cpp
void test_f9_nxe_smep_smap_enabled() {
    // F9: EFER.NXE (bit 11) is x86_64 baseline -- always expected on.
    ...  // 读 EFER,断言 bit 11 = 1

    // SMEP/SMAP are CPUID-gated (CPUID.07H:EBX[7]/[20]). The test mirrors
    // enable_smep_smap(): CPU 报支持才断言 CR4 位设上。
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    ...  // CPUID 查 SMEP/SMAP 支持,支持才断言 CR4[20]/[21]
}
```

（`test_usermode.cpp:122`。）NXE 是 baseline,**无条件断言**——任何环境都该设上。SMEP/SMAP **跟随 CPUID**:CPU 报支持(`-cpu host`,真机暴露 SMEP/SMAP)才断言 CR4 位设上;CPU 不报支持(WSL2 的 `-cpu max`,CPUID.07H:EBX=0)就不断言、也不断错。测试和 `enable_smep_smap()` 用同一套 CPUID 判据,逻辑自洽。

这里有个写测试的坑值得记一笔:第一版断言写成 `TEST_ASSERT_TRUE(efer & 0x800)`。`efer & 0x800` 的结果是 `0x800`(不是 0/1),如果断言宏按 `== true`(==1)判,就**假阴性 FAIL**。改成 `(efer >> 11) & 1`(结果恒为 0/1)才稳。位测试一律用「移位再 `& 1`」取 0/1 形式,别直接拿掩码与的结果当布尔。

## 诚实的边界

**SMEP/SMAP 在 WSL2 开发机上验证不了生效。** 这是环境限制,不是代码问题。WSL2 的嵌套 KVM 用 `-cpu max` 时,不透传 CPUID.07H:EBX(整个 leaf 7 返回 0),所以 `enable_smep_smap()` 的 CPUID-gated 逻辑**正确地跳过**了 SMEP/SMAP——往不支持的位写会 #GP,跳过是对的。表现就是:开发机上 EFER.NXE 设上了(NX 真生效),CR4 的 SMEP/SMAP 没设(代码 CPUID-gated 跳过)。`stac`/`clac` 此时是 NOP,无害。换真机、完整(非嵌套)KVM、或 QEMU TCG,CPUID.07H 正常暴露,SMEP/SMAP 就设上、真生效。所以别在本机上指望看到「SMEP 拦了一次内核执行用户页」——看不到不是没做对,是环境没给条件。

**NX 是真能在本机验的。** EFER.NXE 是 x86_64 baseline,WSL2 透传,设上了就是真生效:用户栈/heap/非可执行文件页不可执行,真要执行会 instruction-fetch #PF。这是三个位里唯一在本机板上钉钉的那个。

**SMAP 开了之后,「访用户内存」的纪律更严。** 入口已经不挂全局 `stac`(P3 移除),所以凡是没显式走 accessor(`copy_from_user`/`copy_to_user`,内部 `stac` 开窗 + `_ASM_EXTABLE` 容错)的访用户,一律被 SMAP 当非法访用户 #PF 拦下——包括 `validate_user_ptr` 那种只查 canonical 地址就直接解引用的旧路径。SMAP 让这条边界变成「不开窗就碰不得」,比之前 PF 兜底默默通过更安全。

验证该看到什么,见配套 lab。下一章接着开 ASLR——给用户态布局加随机化,那是 F9 安全的另一条线。
