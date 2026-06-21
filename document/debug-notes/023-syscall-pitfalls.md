---
title: 调试档案 023 · 用户态 movaps 炸 #GP、boot.S 字节序列被改、以及 sys_exit 走了 halt
tag: 023_syscall
---

# 调试档案 023 · 用户态 movaps 炸 #GP、boot.S 字节序列被改、以及 sys_exit 走了 halt

> 从 `document/notes/023/fpu_sse_debug_notes.md`、`design_notes.md` 提炼并补全「定位 / 防复发」,配套主书 [023 · 让用户态会说话:SYSCALL/SYSRET 系统调用](../book/07-userland/023-syscall.md)。023 把 Ring 3 从「只会 `cli` 撞 `#GP`」升级成「用户 `syscall` 请求内核、内核 `sysretq` 送回」,顺带为了让用户态 C++ 能用 SSE,把 FPU/SSE 支持也打通了。这条路上一共有三个坑最典型:一个是「用户程序第一条 SSE 指令就 `#GP`」,先误判是 FPU 没开、开了还炸,最后才挖到是栈没满足 SysV ABI 对齐;一个是「boot.S 加了几行 FPU 初始化,大内核测试一个都不跑」,根因藏在小内核验真内核用的那 3 个魔法字节里;还有一个「看着像 bug、其实是设计」——`sys_exit` 没切到下一个进程,而是 `cli;hlt` 死循环。最后这条是跨里程碑解耦留下的隐形契约,当下不炸、024 启了调度器才显出意义。

## 案例一:用户态 `movaps` 第一条就 `#GP`——根因有两层,栈对齐才是真凶

- **症状**:换上用 C++ 写的 `hello.cpp` 用户程序,一跳进 Ring 3 立刻 `#GP`,QEMU 寄存器现场长这样:

  ```
  RIP   = 0x0000000000400019
  RSP   = 0x00000007FFFFEFD8
  movaps XMMWORD PTR [rsp], xmm0   ← #GP
  ```

  崩在用户程序入口附近的 `movaps`,程序连一句 `sys_write` 都没来得及发。

- **根因**:这个坑是**两层叠加**,第一层是障眼法,直接修第一层还会接着炸,必须挖到第二层才真修好。

  第一层:GCC 把 `const char msg[] = "..."` 的初始化优化成了 SSE 的 `movdqa` / `movaps`。`movaps` 要求目标地址 16 字节对齐,但此刻 RSP 没对齐。反汇编确认崩溃点确实是那条 `movaps`:

  ```asm
  400000: sub    rsp, 0x28
  ...
  400019: movaps XMMWORD PTR [rsp], xmm0   ← 崩溃点
  ```

  看到这一步,第一反应通常是「FPU 没开」,于是去 `boot.S` 里把 CR0/CR4 的 OSFXSR / OSXMMEXCPT 打开、清掉 EM/TS。但开了之后 `movaps` **仍然 `#GP`**——说明第一层只是障眼法,真正的根因在第二层。

  第二层才是真凶:**栈没满足 x86_64 SysV ABI 的对齐契约**。ABI 要求函数入口处 `RSP ≡ 8 mod 16`(模拟 `call` 指令压入 8 字节返回地址后的状态)。我们设的 `USER_STACK_TOP = 0x7FFFFF000`,它本身是页对齐的、也就是 `0 mod 16`。编译器按 ABI 假设入口 RSP 已经 `8 mod 16`,生成 `sub rsp, 0x28`(0x28 = 40,是 16 的整数倍)后期望结果仍对齐;可起点是 `0 mod 16`,减完变成 `0x7FFFFEFD8`,末位是 `8`,即 `8 mod 16`——**该对齐的地方反而不对齐**,`movaps` 当然炸。算一下:`0x7FFFFF000 - 0x28 = 0x7FFFFEFD8`,末位 `8 ≠ 0`,对齐失败。

- **定位**:关键信号是崩溃指令那条 `movaps` + 崩溃点 RSP 末位 `8`。只要看到「SSE 对齐指令炸 `#GP`」且 RSP 末位不是 `0`,几乎可以直接锁定栈对齐。判断顺序上有个教训:**先别急着改 FPU**。FPU 启用后还在炸,本身就是「根因不在 FPU」的铁证——这时候该怀疑的是调用约定,而不是硬件支持。

- **修复**:跳进 Ring 3 之前把 RSP 减 8,让它满足 `8 mod 16`,并用编译期断言把这条契约钉死:

  ```cpp
  constexpr uint64_t USER_ABI_RSP_OFFSET = 8;
  static_assert((USER_STACK_TOP - USER_ABI_RSP_OFFSET) % 16 == 8,
                "User entry RSP must satisfy x86_64 ABI alignment");
  ```

  调用处把对齐后的栈顶传进去:

  ```cpp
  jump_to_usermode(USER_ENTRY_BASE, USER_STACK_TOP - USER_ABI_RSP_OFFSET, 0);
  ```

  为什么是减 8 而不是减别的:用户入口相当于「没有 `call` 的函数入口」,缺的正是 `call` 本该压进去的那 8 字节返回地址,补一个等量的偏移就对上 ABI 的假设了。

- **防复发**:**ABI 对齐是编译期就该锁死的契约,不该靠运行时侥幸**。任何「内核手工跳进一个函数入口」的路径——无论跳的是 Ring 3 用户态、还是内核线程起点——只要这个入口会被编译器按 ABI 假设生成代码,就必须保证它落地的 RSP 满足 `8 mod 16`。`static_assert` 把这条从「人脑记住」升级成「编译器记住」,以后谁动 `USER_STACK_TOP` 都会在编译期就被拦下来。

## 案例二:boot.S 加了 FPU 初始化,大内核测试一个都不跑

- **症状**:在 `boot.S` 里加了 CR0/CR4 的 FPU 初始化之后,`make run-kernel-test` 一上来就吐:

  ```
  === Loaded ELF is not a real kernel, exiting ===
  ```

  紧接着整个 169 项大内核测试套件**全部被跳过**,一个用例都没跑成。改动看起来人畜无害——只是多了几行 `mov %cr4,%rax` 之类的 MSR/CR 操作,跟「内核是不是真内核」有什么关系?

- **根因**:这颗雷埋在小内核(mini kernel)验真内核的环节里。小内核加载大内核后,靠**检查大内核入口的前 3 个字节**判断它是不是「真内核」:

  ```cpp
  // kernel/mini/test/main_test.cpp
  auto* code = reinterpret_cast<const uint8_t*>(big_kernel_entry);
  bool  is_real_kernel =
      (code[0] == 0xFA) &&                       // cli
      (code[1] == 0x48) &&                       // REX.W
      (code[2] == 0xC7 || code[2] == 0xBC);      // mov rsp, imm
  ```

  它认的字节模式是 `cli`(0xFA)紧跟一条 `mov rsp, imm`(0x48 0xBC 或 0x48 0xC7),也就是「关中断 + 立刻把栈指好」。`boot.S` 原本前两条正是这个顺序,字节 `FA 48 BC ✓`,验证通过。

  加 FPU 初始化时,顺手把 `mov %cr4,%rax` 塞到了第二句——于是字节序列变成 `cli` 紧跟 `mov %cr4,%rax`,即 `FA 0F 20 ✗`。小内核一看第三个字节对不上 0xC7 / 0xBC,当场判否,直接 `outl` 退出,后面的测试自然全跳过。FPU 代码本身一行没错,坏在它**站错了位置**。

- **定位**:症状里那句 `Loaded ELF is not a real kernel, exiting` 是直接线索——一旦看到它,就该去查 `boot.S` 的前两条指令有没有被改。把 `build/.../big_kernel_test` 扔进反汇编(`objdump -d` 或直接 `xxd` 看入口前几个字节),对照 `kernel/mini/test/main_test.cpp` 里那段 3 字节校验,就能看到实际字节是 `FA 0F 20` 而不是 `FA 48 BC/C7`。

- **修复**:把 FPU 初始化挪到栈设置之后,保住「`cli` + `mov rsp, imm`」作为头两条指令:

  ```asm
  _start:
      cli
      movq  $__kernel_stack_top, %rsp   # FA 48 BC — 小内核验真点,必须保持在前两条
      xorq  %rbp, %rbp
      # FPU init follows...
      movq  %cr4, %rax
      orq   $((1 << 9) | (1 << 10)), %rax
      ...
  ```

- **防复发**:**`boot.S` 头两条指令的字节模式是和小内核之间的硬契约,属于「不能动的魔法序言」**。后续任何「我要在内核入口最早处插一段初始化」的诱惑——FPU、SSE、PAT、SMEP/SMAP——都得排在 `cli; mov rsp, imm` 之后。最稳的做法是在 `boot.S` 那两行旁边留一句注释,写明「前两条指令的字节序列被 mini kernel 校验,改动前先看 `kernel/mini/test/main_test.cpp`」,让后来人撞进来就知道这条红线。

## 案例三:`sys_exit` 走了 `cli;hlt`,没切到下一个进程——不是 bug,是设计

- **症状**:hello 程序跑完 `sys_write` 调 `sys_exit(0)`,串口收尾不是「切到下一个任务」,而是:

  ```
  [SYSCALL] sys_exit: no scheduler, halting.
  ```

  然后整机就 `cli;hlt` 挂住了。看着像 `sys_exit` 没实现完——毕竟 milestone 020 就有调度器了,exit 理应收尾后 `yield` 给下一个任务。

- **根因**:这其实是预期行为,根因在「谁初始化了什么」。`sys_exit` 的实现里有个**防御性双分支**:

  ```cpp
  if (cinux::proc::Scheduler::is_initialized()) {
      cinux::proc::Scheduler::yield();
  } else {
      cinux::lib::kprintf("[SYSCALL] sys_exit: no scheduler, halting.\n");
      while (1) {
          __asm__ volatile("cli; hlt");
      }
  }
  ```

  本 tag 的 `kernel_main` 在调 `launch_first_user()` **之前并没有启动调度器**——020 写了调度器,但 023 这条用户态测试路径没把它接进启动序。于是 `is_initialized()` 返回 false,`sys_exit` 走的是 halt 分支。对 023 的目标(验证 SYSCALL/SYSRET 这条机制本身能跑通)来说,单任务测试足够,程序跑完一句、干净停住,就算达标。

  `yield` 那个分支是为下一个里程碑留的:024 要让 shell 作为常驻进程,届时会在 `launch_first_user()` 之前启动调度器,`sys_exit` 就会走 yield、真正把 CPU 让出去。但**那条路径在 023 一行也跑不到**,本章不实现、也不该提前实现。

- **定位**:这不是排查出来的 bug,是从设计笔记里读出来的「为什么这么写」。判断依据是启动序:[main.cpp](https://github.com/Awesome-Embedded-Learning-Studio/Cinux-Book/blob/main/kernel/main.cpp) 的 Step 19 是 `syscall_init` + 注册三个 handler,Step 20 是 `launch_first_user()`,中间**没有** `Scheduler::init()`。串口那句 `no scheduler, halting` 本身就是分支选择的直接证据,不是异常。

- **修复**:本 tag 不修——这正是设计意图。要让它走 yield,得等 024 把调度器接进启动序。

- **防复发**:**这是「跨里程碑解耦」留下的隐形契约,当下不炸、上 024 才显出价值**。教训有两条。其一,syscall 模块应当能在「无调度器」的环境下也干净收场——`is_initialized()` 这道判断把 `sys_exit` 与调度器之间的强耦合解开了,任何一个里程碑单独验证 syscall 机制时都不会被调度器拖住。其二,**别被「020 不是已经有调度器了吗」这种记忆骗了**——「代码存在」和「启动序里调没调」是两回事;`Scheduler` 的实现早就在那儿,但本 tag 的启动路径没启用它,`is_initialized()` 就是 false。排查「某段代码该执行却没执行」时,先确认它依赖的前置模块有没有被 `init`,别默认「早就有」就等于「已经开」。

---

### 一句话总结

023 的三个坑,一个是**栈没满足 ABI 对齐**(用户入口 RSP 不是 `8 mod 16`,`movaps` 当场 `#GP`),修在「跳转前 RSP-8 + `static_assert`」,顺手记下「FPU 没开只是障眼法」;一个是**boot.S 头两条指令的魔法字节被改**(FPU 初始化插在了 `cli;mov rsp,imm` 中间,小内核验真失败、测试全跳过),修在「FPU 初始化挪到栈设置之后」;还有一个**看着像 bug 的设计**——`sys_exit` 走 `cli;halt` 而非 `yield`,因为本 tag 启动序没接调度器,是跨里程碑解耦的隐形契约,等 024 把调度器接进启动序才会走 yield 路径。前两个是「ABI 契约」和「加载器硬契约」的必修课,最后一个是「解耦要付的代价」——当下沉默、上线才发声的那种。
