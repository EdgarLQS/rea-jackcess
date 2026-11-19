# rea-jackcess

rea-jackcess 是 Jackcess 的 C++ 重写项目。首期目标是在原生 Linux x86_64 和 ARM64 上交付 C++17 核心库，完整对齐 Java Jackcess 5.2.0 对 Access MDB 和 ACCDB 文件的公开能力与可观察行为。

## 当前状态

已实现 Jet/ACE 版本头识别、只读页访问，以及 Jet4 表定义头、列定义和 UTF-16LE 列名读取。诊断 CLI 的 `inspect` 子命令会解析页 2 的系统目录表定义，并输出列名及基础列属性。目录记录、普通表发现、数据行和值读取，以及完整 P0 行为矩阵和上游同步 CI 仍待实施。

Jet4 读写是第一个内部里程碑，核心库在完整矩阵通过后发布。PGeo 导入和样本克隆使用独立的 GIS 验收关口，各自发布。

## 本地构建

需要 CMake 3.16 或以上以及支持 C++17 的编译器。

```sh
cmake -S . -B build
cmake --build build
(cd build && ctest --output-on-failure)
./build/rea-jackcess --version
```

`rea_jackcess::core` 是核心库的 CMake 链接目标。CLI 提供 `--help`、`--version` 和 `inspect <database-file>`。

## 包与代码分类

| 目录 | 职责 |
|---|---|
| `include/rea_jackcess/` | 对外 C++ API。当前包含文件句柄、格式信息、页读取和错误类型；后续按验收工作包增加数据库、表、游标、值、属性和构建器接口。 |
| `src/format/` | Jet/ACE 版本和格式布局；当前识别已列出的版本头，按格式分别补充页、记录及编码规则。 |
| `src/page/` | 有界随机页 I/O；后续加入页缓存、页分配和写入通道。 |
| `src/catalog/` | 系统目录、表定义、列 schema 和行定位；当前读取 Jet4 表定义、列头和 UTF-16LE 列名。 |
| `src/value/` | 行字段解码、类型化值、文本和长值；待实现。 |
| `src/metadata/` | 表与列属性、关系、逻辑和物理索引、排序键及索引游标；待实现。 |
| `src/write/` | 新库构建、表结构、页分配、行插入/更新/删除和索引维护；待实现。 |
| `tools/cli/` | 诊断和互操作对照工具；当前提供帮助、版本和 Jet4 `inspect` 输出。 |
| `tests/` | CTest 单元与格式回归入口；当前覆盖头识别、页 I/O 和 Jet4 表定义边界。 |
| `adapters/pgeo/`（规划） | 单独依赖核心库和 GDAL 的 PGeo 导入、样本克隆适配。 |

C++ 以目录划分内部模块：公开接口使用 `rea_jackcess` 命名空间，内部实现使用 `rea_jackcess::detail`。构建目标为 `rea_jackcess::core`（静态核心库）、`rea_jackcess_cli`（产物名 `rea-jackcess`）和 `rea_jackcess_tests`。

### 公开接口分类

以下是目标 API 的职责边界，接口会随对应行为矩阵和 Jackcess 5.2.0 证据逐步定型，未实现的类型不会提前作为可链接 API 发布。

| API 类别 | 目标职责 |
|---|---|
| `DatabaseFile` | 当前接口：以 RAII 方式打开文件、返回格式信息和原始页。 |
| `Database` / `DatabaseBuilder` | 打开已有文件、枚举表和库属性；创建受支持格式的新库。 |
| `Table` / `TableBuilder` | 提供 schema、表属性和按格式支持的结构变更。 |
| `RowCursor` / `Row` | 顺序或索引遍历记录，并提供字段访问。 |
| `Value` | 表示类型化字段、NULL、长值及必要的原始字节。 |
| 属性与索引 API | 读取库/表/列属性、关系和索引，并按已验收能力执行查询。 |
| `DatabaseError` | 以稳定错误码报告失败，并携带可获得的文件、页、表、行和列上下文。 |

依赖从高层流向低层：`write` 使用 `metadata`、`value`、`catalog`、`page` 和 `format`；`metadata` 使用 `catalog`、`value`、`page` 和 `format`；`value`、`catalog` 使用 `page` 和 `format`；`page` 使用 `format`。目标公开 API 组合内部模块；PGeo 适配器通过公开 API 使用核心库。当前诊断 CLI 为验证 Jet4 表定义直接调用内部解析器，待表 API 落地后再改为公开入口。PGeo 适配器不进入核心库依赖图，核心库不依赖 JVM、libmdb 或 GDAL。

## 从哪里开始

- [重写计划](plan/Jackcess-C++重写/README.md)列出范围、固定版本、JAR 哈希和外部资料位置。
- [实施工作包](plan/Jackcess-C++重写/01-实施工作包.md)列出开发顺序、依赖和上游同步流程。
- [验收与发布门槛](plan/Jackcess-C++重写/02-验收与发布门槛.md)列出格式矩阵、双向回读、故障测试和发布条件。
- [AI 协作指南](AGENTS.md)说明从哪里查证、如何实施和记录结果。
