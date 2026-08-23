我已经获取了所有必要的信息。这是完整的 markdown 文档：

---

# Logger

> 轻量日志框架：级别过滤 + 格式化 + Sink 分发

## 头文件

`#include <cinux/logger.hpp>`

## 依赖

`detail/vformat`（内部格式化引擎，提供 `vformat_to_buf` 用于 printf 风格的格式化输出）

## 概述

`Logger` 是 `cinux::lib` 中的轻量级日志框架，采用单例模式设计，不使用任何堆内存分配。它将日志处理流程分解为三个阶段：**级别过滤**、**格式化**和 **Sink 分发**。日志消息首先与当前阈值级别比较，低于阈值的消息会被静默丢弃并计入 `dropped_count_` 计数器；通过过滤的消息被格式化为 `[LEVEL] message` 形式的字符串；最终该字符串被分发到所有已注册的 Sink 回调函数。

该组件适用于嵌入式或裸机环境中的日志记录需求。由于格式化引擎 `detail::vformat` 不依赖标准库的 `printf`，Logger 可以在没有完整 C 运行时支持的场景下工作。内部使用固定大小的 256 字节栈缓冲区进行格式化，最多支持 8 个 Sink 同时注册。线程安全由调用方负责，适合在单线程或已受外部锁保护的上下文中使用。

## API 参考

### 枚举 `LogLevel`

```cpp
enum class LogLevel : uint8_t {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERROR = 3,
};
```

日志严重级别枚举，底层类型为 `uint8_t`。数值越大表示越严重。`Logger` 使用此枚举进行消息过滤和级别标记。

| 枚举值 | 数值 | 说明 |
|--------|------|------|
| `LogLevel::DEBUG` | 0 | 调试信息，最详细 |
| `LogLevel::INFO`  | 1 | 一般信息 |
| `LogLevel::WARN`  | 2 | 警告信息 |
| `LogLevel::ERROR` | 3 | 错误信息，最严重 |

---

### 函数 `log_level_string`

```cpp
constexpr const char* log_level_string(LogLevel level);
```

将 `LogLevel` 枚举值转换为对应的短字符串标签。

**参数**

| 参数 | 类型 | 说明 |
|------|------|------|
| `level` | `LogLevel` | 要转换的日志级别 |

**返回值**

`const char*` —— 对应的字符串常量：`"DEBUG"`、`"INFO"`、`"WARN"`、`"ERROR"`；未知级别返回 `"UNKNOWN"`。

**示例**

```cpp
using namespace cinux::lib;

const char* tag = log_level_string(LogLevel::WARN);
// tag == "WARN"
```

---

### 类型别名 `LogSink`

```cpp
using LogSink = void (*)(LogLevel level, const char* message, void* ctx);
```

Sink 回调函数指针类型。每条通过级别过滤的日志消息都会以该函数指针的形式分发到已注册的 Sink。

**参数说明**

| 参数 | 类型 | 说明 |
|------|------|------|
| `level` | `LogLevel` | 当前消息的日志级别 |
| `message` | `const char*` | 已格式化的完整消息字符串（包含 `[LEVEL]` 前缀） |
| `ctx` | `void*` | 注册时传入的用户上下文指针 |

---

### 类 `Logger`

```cpp
class Logger;
```

轻量级日志器，采用单例模式，支持级别过滤和多 Sink 分发。不使用堆分配，线程安全由调用方保证。

#### `Logger::instance`

```cpp
static Logger& instance();
```

获取 Logger 单例引用。使用函数局部静态变量实现，首次调用时构造。

**返回值**

`Logger&` —— 全局唯一实例的引用。

**示例**

```cpp
auto& log = cinux::lib::Logger::instance();
```

---

#### `Logger::kMaxSinks`

```cpp
static constexpr int kMaxSinks = 8;
```

可注册的最大 Sink 数量常量。

---

#### `Logger::set_level`

```cpp
void set_level(LogLevel level);
```

设置日志阈值级别。低于该级别的消息将被静默丢弃。

**参数**

| 参数 | 类型 | 说明 |
|------|------|------|
| `level` | `LogLevel` | 新的最低日志级别 |

**示例**

```cpp
auto& log = cinux::lib::Logger::instance();
log.set_level(cinux::lib::LogLevel::WARN);
// 此后 debug() 和 info() 的消息将被丢弃
```

---

#### `Logger::level`

```cpp
LogLevel level() const;
```

获取当前日志阈值级别。

**返回值**

`LogLevel` —— 当前的最低日志级别。

---

#### `Logger::register_sink`

```cpp
bool register_sink(LogSink sink, void* ctx = nullptr);
```

注册一个 Sink 回调。新注册的 Sink 会追加到 Sink 列表末尾。当已注册 Sink 数量达到 `kMaxSinks`（8 个）或传入的 `sink` 为 `nullptr` 时，注册失败。

**参数**

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `sink` | `LogSink` | — | Sink 回调函数指针 |
| `ctx` | `void*` | `nullptr` | 传递给回调的用户上下文指针 |

**返回值**

`true` —— 注册成功；`false` —— 注册失败（达到上限或 `sink` 为空）。

**示例**

```cpp
using namespace cinux::lib;

// 自定义 Sink：将日志写入 UART
void uart_sink(LogLevel level, const char* msg, void* ctx) {
    // 将 msg 发送到硬件串口 ...
}

auto& log = Logger::instance();
log.register_sink(uart_sink);

// 带 context 的 Sink
void file_sink(LogLevel level, const char* msg, void* ctx) {
    auto* fp = static_cast<FILE*>(ctx);
    fputs(msg, fp);
    fputc('\n', fp);
}

FILE* log_file = fopen("app.log", "a");
log.register_sink(file_sink, log_file);
```

---

#### `Logger::clear_sinks`

```cpp
void clear_sinks();
```

移除所有已注册的 Sink。调用后，即使消息通过了级别过滤，也不会有任何输出。

**示例**

```cpp
auto& log = cinux::lib::Logger::instance();
log.clear_sinks();
```

---

#### `Logger::log`

```cpp
void log(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
```

以指定级别输出一条日志消息。格式字符串遵循 printf 风格，编译器会进行格式字符串类型检查。

**参数**

| 参数 | 类型 | 说明 |
|------|------|------|
| `level` | `LogLevel` | 本条消息的日志级别 |
| `fmt` | `const char*` | printf 风格格式字符串 |
| `...` | — | 对应格式占位符的可变参数 |

**支持的格式说明符**

由底层 `detail::vformat` 引擎提供：`%%`、`%c`、`%s`、`%d`、`%u`、`%x`、`%X`、`%p`。支持长度修饰符 `%ld`、`%lu`、`%lx`、`%lX`、`%lld`、`%llu`、`%llx`、`%llX`。支持宽度 `%Nd`、`%0Nd`、`%-Nd`、`%-Ns`。

**示例**

```cpp
auto& log = cinux::lib::Logger::instance();
log.log(cinux::lib::LogLevel::INFO, "Server started on port %d", 8080);
```

---

#### `Logger::debug`

```cpp
void debug(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
```

以 `LogLevel::DEBUG` 级别输出日志。等效于 `log(LogLevel::DEBUG, fmt, ...)`。

**参数**

| 参数 | 类型 | 说明 |
|------|------|------|
| `fmt` | `const char*` | printf 风格格式字符串 |
| `...` | — | 可变参数 |

**示例**

```cpp
log.debug("x=%d, y=%d", x, y);
```

---

#### `Logger::info`

```cpp
void info(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
```

以 `LogLevel::INFO` 级别输出日志。等效于 `log(LogLevel::INFO, fmt, ...)`。

**示例**

```cpp
log.info("Connected to %s", host);
```

---

#### `Logger::warn`

```cpp
void warn(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
```

以 `LogLevel::WARN` 级别输出日志。等效于 `log(LogLevel::WARN, fmt, ...)`。

**示例**

```cpp
log.warn("Low memory: %u bytes remaining", free_bytes);
```

---

#### `Logger::error`

```cpp
void error(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
```

以 `LogLevel::ERROR` 级别输出日志。等效于 `log(LogLevel::ERROR, fmt, ...)`。

**示例**

```cpp
log.error("Failed to open %s: error %d", path, errno);
```

---

#### `Logger::dropped_count`

```cpp
size_t dropped_count() const;
```

返回因级别过滤被丢弃的消息总数。

**返回值**

`size_t` —— 自上次调用 `reset_dropped_count()` 或 Logger 创建以来被丢弃的消息数量。

---

#### `Logger::reset_dropped_count`

```cpp
void reset_dropped_count();
```

将丢弃计数器归零。

---

## 完整使用示例

```cpp
#include <cinux/logger.hpp>
#include <cstdio>

using namespace cinux::lib;

// 将日志输出到 stdout 的 Sink
void console_sink(LogLevel level, const char* msg, void*) {
    std::puts(msg);
}

int main() {
    auto& log = Logger::instance();

    // 设置最低级别为 DEBUG
    log.set_level(LogLevel::DEBUG);

    // 注册 Sink
    log.register_sink(console_sink);

    // 输出日志
    log.debug("Initializing system...");
    log.info("Module %s loaded, size=%u bytes", "network", 4096);
    log.warn("Configuration file not found, using defaults");
    log.error("Connection refused to %s:%d", "192.168.1.1", 8080);

    // 调整级别，过滤低级别消息
    log.set_level(LogLevel::WARN);
    log.info("This message is dropped");   // 不会到达 Sink

    // 检查丢弃统计
    printf("Dropped: %zu\n", log.dropped_count());
    log.reset_dropped_count();

    // 清理
    log.clear_sinks();
    return 0;
}
```

预期输出（前四行来自前四次调用）：

```
[DEBUG] Initializing system...
[INFO] Module network loaded, size=4096 bytes
[WARN] Configuration file not found, using defaults
[ERROR] Connection refused to 192.168.1.1:8080
Dropped: 1
```

## 注意事项

- **单例模式**：`Logger` 不可拷贝、不可赋值。通过 `Logger::instance()` 获取全局唯一实例。
- **线程安全**：Logger 本身不提供线程安全保证。如果在多线程环境中使用，调用方需自行加锁保护。
- **缓冲区大小**：内部格式化缓冲区为 256 字节（栈分配）。超出部分会被截断，不会造成缓冲区溢出。
- **Sink 容量**：最多注册 8 个 Sink（`kMaxSinks`），超出后 `register_sink` 返回 `false`。
- **格式化支持**：内部使用 `detail::vformat` 而非标准库 `printf`，支持的格式说明符有限（见上文列表）。不支持浮点数格式化（`%f`、`%g`）。
- **消息前缀**：每条格式化后的消息自动附加 `[LEVEL] ` 前缀，例如 `[INFO] hello world`。
- **丢弃计数**：被级别过滤丢弃的消息不会到达任何 Sink，但会递增 `dropped_count()`。可通过 `reset_dropped_count()` 归零。
- **`clear_sinks` 后行为**：调用 `clear_sinks()` 后，所有消息仍会经过级别过滤（被丢弃的消息仍然增加 `dropped_count_`），但因无 Sink 注册，不会有任何输出。

## 另见

- `cinux/detail/vformat.hpp` —— 内部格式化引擎，提供 `vformat_impl` 和 `vformat_to_buf`
- `src/logger.cpp` —— Logger 实现
- `test/test_logger.cpp` —— Logger 单元测试（基于 Catch2）
