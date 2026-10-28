# mini-primes

一个独立的 C++17 小项目，列出指定上限以内的素数。`main.cpp` 负责命令行和输出，`primes.cpp` 实现素数判断；CMake 编译两个源文件并链接为可执行程序。只依赖标准库，也能在 Linux 宿主运行。

在 Cinux 的串口或 GUI 终端中，将示例复制到自己的 HOME：

```sh
cp -r ~/Cinux/user/examples/mini-primes ~/mini-primes
cd ~/mini-primes
cmake -B build -S .
cmake --build build -j
./build/mini-primes 30
```

输出：

```text
Primes up to 30:
2 3 5 7 11 13 17 19 23 29
Found 10 primes.
```

不传参数时上限为 100，支持 2..10000；非法参数返回退出码 2。

修改输出文字，再增量编译：

```sh
python3 -c "from pathlib import Path; p = Path('main.cpp'); p.write_text(p.read_text().replace('Primes up to', 'Hello from Cinux! Primes up to'))"
cmake --build build -j
./build/mini-primes 30
```

首行变为 `Hello from Cinux! Primes up to 30:`。只有 `main.cpp` 重新编译，然后重新链接；再次执行构建命令则没有待编译内容。后续也可直接用编辑器修改源码。

保存工作后，在 Cinux 终端执行 `poweroff` 正常关机，等待 QEMU 自行退出；这会将源码、构建目录和文件系统元数据同步到盘上。工作过程中可用 `sync` 主动同步。直接关闭虚拟机窗口相当于断电，可能丢失尚在缓存中的内容。

开发环境默认用 Ninja。若要演示 Make，可在另一目录配置：

```sh
cmake -B build-make -S . -G "Unix Makefiles"
cmake --build build-make -j
./build-make/mini-primes 30
```

示例有自己的 CMake 入口，不加入 Cinux 内核的默认构建图。源码随 Buildroot 的开发 overlay 一起打包；宿主更新示例后，已有开发盘需单独同步文件，日常启动仍会保留盘内工作。
