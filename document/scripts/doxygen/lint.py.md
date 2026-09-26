---
title: lint.py
description: "文档编写日期: 2026-03-20，- 递归扫描指定目录（默认为项目根目录）"
---

# lint.py

> 文档编写日期: 2026-03-20

## 使用办法 (Usage)

### 基本语法
```bash
python3 scripts/doxygen/lint.py
```

### 指定目录检查
```bash
python3 scripts/doxygen/lint.py /path/to/directory
```

### 工作模式
- 递归扫描指定目录（默认为项目根目录）
- 检查C++头文件中的Doxygen注释完整性
- 生成违规报告到 `FAILED_DOXYGEN.md`
- 返回非零退出码表示发现违规

## Scripts详解

### 脚本用途
严格检查C++头文件中的Doxygen注释是否符合规范，用于代码质量验证和CI/CD集成。

### 检查内容

#### 1. 文件级别检查
- **文件头部**: 必须包含 `@file` 标记的文件级注释
- **行长度**: 每行不超过100个字符

#### 2. 类型注释检查
- 检查 `enum`、`struct`、`class`、`union` 类型
- 要求有Doxygen注释块（`/** */` 或 `///`）
- 自动跳过私有类型和内部类型

#### 3. 函数注释检查
- 必须包含 `@brief` 标记
- 非void函数必须有 `@return` 标记
- `@param` 标记必须包含方向说明 `[in]`、`[out]` 或 `[in,out]`
- 自动跳过私有函数和成员变量

#### 4. 语言风格检查
- 禁止使用第一人称代词（I、we、our、my）
- 禁止使用将来时态（will）

### 排除目录
默认排除以下目录：
- `out` - 构建输出目录
- `.vscode` - 编辑器配置
- `cmake` - CMake配置目录
- `example` - 示例代码
- `scripts` - 脚本目录
- `.git` - Git仓库
- `test` - 测试代码
- `third_party` - 第三方库

### 输出

#### 成功输出
```text
All Doxygen checks passed
```
- 返回码: `0`
- 删除已存在的 `FAILED_DOXYGEN.md` 文件

#### 失败输出
```text
FAILED: N violations
See: /path/to/FAILED_DOXYGEN.md
```
- 返回码: `1`
- 生成 `FAILED_DOXYGEN.md` 详细报告，包含：
  - 违规总数
  - 每个违规的文件路径、符号名称和具体问题

### 配置项

| 配置项 | 值 | 说明 |
|--------|-----|------|
| `SCAN_EXTENSIONS` | (`.h`, `.hpp`) | 扫描的文件扩展名 |
| `MAX_LINE_LENGTH` | `100` | 最大行长度限制 |
| `CORE_REQUIRED_TAGS` | `(@brief)` | 必需的Doxygen标签 |
| `FAILED_OUTPUT_FILE` | `FAILED_DOXYGEN.md` | 输出报告文件路径 |

### 依赖项
- Python 3.9+
- 标准库: `re`, `sys`, `pathlib`, `dataclasses`, `typing`

### 集成方式

#### Git Pre-commit Hook
```bash
# .git/hooks/pre-commit
python3 scripts/doxygen/lint.py || exit 1
```

#### CMake集成
```cmake
add_custom_target(doxygen_lint
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/doxygen/lint.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking Doxygen compliance..."
)
```

### 注意事项
- 仅检查 `.h` 和 `.hpp` 头文件
- 不检查 `.hxx` 文件（可通过配置修改）
- 自动识别并跳过私有成员（class中的private:部分）
- 正确处理模板函数、构造函数、析构函数
- 能区分成员变量和函数声明
- 跳过代码块（`@code ... @endcode`）中的内容
- 对于复杂类型（如函数指针、std::function）有特殊处理逻辑

### 技术实现细节

#### 项目根目录查找
脚本通过查找以下标记来确定项目根目录：
1. `.git` 目录
2. `CMakeLists.txt` 文件
3. `.project-root` 文件

#### 函数检测正则
使用复杂的正则表达式匹配函数签名，支持：
- 模板函数 `template <...>`
- 各种存储类说明符 `static`、`inline`、`constexpr` 等
- C++20关键字 `consteval`、`constinit`
- 函数修饰符 `const`、`noexcept`、`override`、`final`

#### 访问权限判断
通过向后搜索最近的访问说明符（`public:`、`protected:`、`private:`）来判断成员的访问权限，同时考虑：
- class默认私有访问
- struct默认公共访问
- 嵌套类型的继承关系
