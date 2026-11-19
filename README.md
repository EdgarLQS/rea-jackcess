# rea-jackcess

rea-jackcess 是 Jackcess 的 C++ 重写项目。首期目标是在原生 Linux x86_64 和 ARM64 上交付 C++17 核心库，完整对齐 Java Jackcess 5.2.0 对 Access MDB 和 ACCDB 文件的公开能力与可观察行为。

## 当前状态

已建立 C++17/CMake 构建框架，包含核心静态库、诊断 CLI 和 CTest 冒烟测试。下一步按 P0 冻结参考版本、样本和行为矩阵，验证关键难点并重估工量。MDB/ACCDB 读写、格式回归测试和上游同步 CI 待实施。

Jet4 读写是第一个内部里程碑，核心库在完整矩阵通过后发布。PGeo 导入和样本克隆使用独立的 GIS 验收关口，各自发布。

## 本地构建

需要 CMake 3.16 或以上以及支持 C++17 的编译器。

```sh
cmake -S . -B build
cmake --build build
(cd build && ctest --output-on-failure)
./build/rea-jackcess --version
```

`rea_jackcess::core` 是核心库的 CMake 链接目标。CLI 当前提供 `--help` 和 `--version`。

## 代码结构

| 目录 | 职责 |
|---|---|
| `include/rea_jackcess/` | 公开 C++ 接口 |
| `src/` | 核心库实现和内部头文件 |
| `tools/cli/` | 诊断 CLI，后续承载 canonical JSON 输出 |
| `tests/` | CTest 入口和后续回归用例 |
| `adapters/pgeo/`（规划） | 依赖核心库与 GDAL 的 GIS 适配 |

核心模块在相应工作包中创建。依赖由上层指向下层，CLI 和适配层调用公开接口。

| 规划目录 | 职责 |
|---|---|
| `src/page/` | 有界页 I/O、页缓存与页分配 |
| `src/format/` | Jet/ACE 文件头识别、页布局和格式差异 |
| `src/catalog/` | 系统目录、表定义、Usage Map 与行游标 |
| `src/value/` | 字段、文本和长值编解码 |
| `src/metadata/` | 属性、索引、排序键与索引游标 |
| `src/write/` | 建库、表结构与行变更，复用页、格式和索引逻辑 |

## 从哪里开始

- [重写计划](plan/Jackcess-C++重写/README.md)列出范围、固定版本、JAR 哈希和外部资料位置。
- [实施工作包](plan/Jackcess-C++重写/01-实施工作包.md)列出开发顺序、依赖和上游同步流程。
- [验收与发布门槛](plan/Jackcess-C++重写/02-验收与发布门槛.md)列出格式矩阵、双向回读、故障测试和发布条件。
- [AI 协作指南](AGENTS.md)说明从哪里查证、如何实施和记录结果。
