# rea-jackcess

rea-jackcess 是 Jackcess 的 C++ 重写项目。首期目标是在原生 Linux x86_64 和 ARM64 上交付 C++17 核心库，完整对齐 Java Jackcess 5.2.0 对 Access MDB 和 ACCDB 文件的公开能力与可观察行为。

## 当前状态

已实现 Jet/ACE 版本头识别、只读页访问，以及 Jet4 表定义、Usage Map、数据页行目录、普通本地表 schema 发现和逐行读取。字段值解码覆盖布尔值、整数、浮点、日期、Money/Numeric、文本、GUID、Binary、Memo/OLE 长值；Memo/OLE 保留原始负载字节，无法表示的短日期 serial 输出为 `$dateOutOfRange`。`DatabaseError` 可携带文件、页、表、行和列信息。CLI 的 `inspect` 输出内部诊断摘要，`canonical` 输出 Jet4 canonical JSON v1，包括文件元数据、表/列定义、类型化行值及行集哈希。

P1 尚未验收：列属性（包括可空性）、逻辑/物理索引和 Jet4 写入仍待实现；公开 `Database`、`Table`、`RowCursor`、`Row` 和 `Value` 已提供只读 Jet4 读取入口。复杂列或无法识别的字段类型会保留为不支持值并记录在 `errors` 中。完整 P0 行为矩阵和上游同步 CI 也待实施。只读本地样本 `test_data_only_read/test_mdb.mdb`（SHA-256 `0bc0acb9c732de1e17c43c2d0a6019f7b5a62ccf83c5818a1bc42db7dbc460c6`）的系统目录 51 行与 MDBTools 1.0.1 导出在 5 个字段上匹配；另与 Jackcess 5.2.0 对照的 43 张共同用户表无 schema 或行值差异。样本不属于仓库测试资产。

Jet4 读写是第一个内部里程碑，核心库在完整矩阵通过后发布。PGeo 导入和样本克隆使用独立的 GIS 验收关口，各自发布。

## 本地构建

需要 CMake 3.16 或以上以及支持 C++17 的编译器。

```sh
cmake -S . -B build
cmake --build build
(cd build && ctest --output-on-failure)
./build/rea-jackcess --version
```

`rea_jackcess::core` 是核心库的 CMake 链接目标。CLI 提供 `--help`、`--version`、`inspect <database-file>` 和 `canonical <database-file>`。当前 `canonical` 对 Jet4 输出已支持的 schema 与行值；属性图和索引定义仍未读取。

## 包与代码分类

| 目录 | 职责 |
|---|---|
| `include/rea_jackcess/` | 对外 C++ 接口。当前有 RAII 文件句柄，以及 Jet4 只读 `Database`、`Table`、`RowCursor`、`Row` 和 `Value`；属性接口与构建器待后续工作包。 |
| `src/format/` | Jet/ACE 版本和格式布局；当前识别已列出的版本头，按格式分别补充页、记录及编码规则。 |
| `src/page/` | 有界随机页 I/O 和 Jet4 数据页行目录；后续加入页缓存、页分配和写入通道。 |
| `src/catalog/` | 系统目录、表定义、列 schema、Usage Map 和通用行读取；当前发现普通本地表并装载 Jet4 表 schema/记录。 |
| `src/value/` | 字段解码、类型化值和长值；当前提供 Jet4 基础标量、日期、十进制、文本、GUID、二进制及 Memo/OLE 长值解码。 |
| `src/util/` | 与数据库解析相关的通用内部工具；当前提供 SHA-256 文件摘要。 |
| `src/metadata/` | 表与列属性、关系、逻辑和物理索引、排序键及索引游标；待实现。 |
| `src/write/` | 新库构建、表结构、页分配、行插入/更新/删除和索引维护；待实现。 |
| `tools/cli/` | 诊断和互操作对照工具；当前提供帮助、版本、Jet4 `inspect` 摘要和含行值的 `canonical` JSON v1。 |
| `tests/` | CTest 单元与格式回归入口；当前覆盖头识别、页 I/O、表定义、Usage Map、目录行、溢出行、普通表 schema、公开表/游标/行接口、文本和 Jet4 值解码、SHA-256 和代码页解码。 |
| `adapters/pgeo/`（规划） | 单独依赖核心库和 GDAL 的 PGeo 导入、样本克隆适配。 |

C++ 以目录划分内部模块：公开接口使用 `rea_jackcess` 命名空间，内部实现使用 `rea_jackcess::detail`。构建目标为 `rea_jackcess::core`（静态核心库）、`rea_jackcess_cli`（产物名 `rea-jackcess`）、`rea_jackcess_tests` 和 `rea_jackcess_catalog_tests`。

### 公开接口分类

以下是目标 API 的职责边界，接口会随对应行为矩阵和 Jackcess 5.2.0 证据逐步定型，未实现的类型不会提前作为可链接 API 发布。

| API 类别 | 目标职责 |
|---|---|
| `DatabaseFile` | 当前接口：以 RAII 方式打开文件、返回格式信息和原始页。 |
| `Database` / `DatabaseBuilder` | `Database::open` 当前打开 Jet4 只读库、枚举本地表并返回解析诊断；建库与库属性 API 待实现。 |
| `Table` / `TableBuilder` | 当前读取表/列 schema，并创建逐行游标；表属性与结构变更待实现。 |
| `RowCursor` / `Row` | 当前逐行读取；索引遍历与字段名便捷访问待实现。 |
| `Value` | 当前表示已实现类型、NULL、超范围日期、Memo/OLE 负载及不支持类型。 |
| 属性与索引 API | 读取库/表/列属性、关系和索引，并按已验收能力执行查询。 |
| `DatabaseError` | 以稳定错误码报告失败，并携带可获得的文件、页、表、行和列上下文。 |

依赖从高层流向低层：`write` 使用 `metadata`、`value`、`catalog`、`page` 和 `format`；`metadata` 使用 `catalog`、`value`、`page` 和 `format`；`catalog` 使用 `value`、`page` 和 `format`；`value` 使用 `page` 和 `format`；`page` 使用 `format`。公开只读接口组合目录、行和值读取；canonical CLI 仍直接调用内部解析器，以保留阶段和列级诊断。PGeo 适配器通过公开接口使用核心库，不进入核心库依赖图；核心库不依赖 JVM、libmdb 或 GDAL。

## 从哪里开始

- [重写计划](plan/Jackcess-C++重写/README.md)列出范围、固定版本、JAR 哈希和外部资料位置。
- [实施工作包](plan/Jackcess-C++重写/01-实施工作包.md)列出开发顺序、依赖和上游同步流程。
- [验收与发布门槛](plan/Jackcess-C++重写/02-验收与发布门槛.md)列出格式矩阵、双向回读、故障测试和发布条件。
- [AI 协作指南](AGENTS.md)说明从哪里查证、如何实施和记录结果。
