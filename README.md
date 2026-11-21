# rea-jackcess

`rea-jackcess` 正在用 C++17 重写 Java [Jackcess](https://github.com/spannm/jackcess)，目标是原生 Linux x86_64 和 ARM64 核心库，按固定基线 Jackcess 5.2.0 对齐 Access MDB/ACCDB 的公开能力和可观察行为。Jet4 读写是首个内部里程碑；PGeo 导入和已有样本克隆是依赖核心库的独立交付与验收，不属于核心库的格式解析职责。

## 当前状态

目前已实现 Jet/ACE 版本头识别、只读页访问，以及 Jet4 表定义、Usage Map、数据页行目录、普通本地表 schema 发现和逐行读取。基础字段解码覆盖布尔值、整数、浮点、日期、Money/Numeric、文本、GUID、Binary、Memo/OLE 长值；Memo/OLE 保留原始负载字节，无法表示的短日期 serial 输出为 `$dateOutOfRange`。`DatabaseError` 可携带文件、页、表、行和列信息。公开 API 提供只读或显式读写 `Database::open`、空 V2000/V2003 数据库创建，以及通过 `TableBuilder` 创建 Jet4 V2000/V2003 BOOLEAN、BYTE、SHORT、LONG、SINGLE、DOUBLE、MONEY、GUID、TEXT、BINARY、MEMO、OLE 列空表；MONEY 行值使用 `DecimalValue` 的十进制文本，按四位小数精确缩放到有符号 64 位整数，超精度或超范围会在写盘前拒绝。TEXT/BINARY 默认容量分别为 255 个 UTF-16 字符和 255 字节。Memo 通过 `MemoValue` 写入，非空 `raw_bytes` 原样保存，否则 `text` 编码为 Jet4 UTF-16LE；OLE 使用 `BlobValue`。表/列名限 ASCII 字母、数字、空格和下划线，创建后返回的 `Database` 句柄仍只读。读写句柄的 `Table::add_row/add_rows` 可向无索引本地 Jet4 V2000/V2003 表追加 BOOLEAN、BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY、GUID、TEXT/BINARY 和 Memo/OLE 长值；长值写入 LVAL 单页或链页。`Table::update_row` 支持等长原位更新、行增长后写入 overflow 数据行和行缩短时压实目标页，长值更新会分配新 LVAL 页；行长变化时保留逻辑 RowId。`Table::delete_row` 可删除无索引本地 Jet4 V2000/V2003 表中的行并保持其他行位置。写入门禁读取 `MSysDb.AccessVersion`，只接受 V2000 标记 `08.50` 与 V2003 标记 `09.50`；追加和更新均会检查目标行是否能放入单个数据页。`Table::indexes()` 返回逻辑索引定义，`Table::rows_by_index()` 可按指定 Jet4 索引顺序扫描记录。CLI 提供 `create`、`inspect` 和 `canonical`；`canonical` 输出 Jet4 canonical JSON v1，包括文件元数据、表/列定义、逻辑/物理索引定义、已支持类型的行值及行集哈希。

GUID 写入使用 `GuidValue{"00112233-4455-6677-8899-aabbccddeeff"}` 一类规范 8-4-4-4-12 十六进制文本，外侧大括号可省略；读取结果统一为小写且带大括号。BOOLEAN 列使用 `bool` 行值：TRUE/FALSE 存于行尾 NULL mask 位，列定义 `size` 为 1 但定长数据区不占字节；Jet4 无法区分 NULL 与 FALSE，写入 NULL 后读回为 FALSE。

当前 P1/P2 仍在开发中，不能视为阶段或 Jackcess 兼容性验收完成。索引读取已能从 B-tree 定位最左叶页并沿叶页链返回行；键值语义解码、按键查找/范围游标、用户索引创建与维护尚未实现。列 `Required` 属性已用于推导公开 `ColumnInfo::nullable`；`Database::create(path, format, tables)` 可在 V2000 新库中创建一个或多个 BOOLEAN、BYTE、SHORT、LONG、SINGLE、DOUBLE、MONEY、GUID、TEXT、BINARY、MEMO、OLE 列空表，并更新 `MSysObjects` 的目录行、`Id`/`ParentIdName` 索引和全局 Usage Map，已通过 C++ 重开和固定 Jackcess 5.2.0 独立打开检查。C++ 行写入需显式以 `read_write` 打开，目前限无索引本地 Jet4 V2000/V2003 表的 BOOLEAN/BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/GUID/TEXT/BINARY/MEMO/OLE；MONEY 接受可精确缩放为四位小数的 `DecimalValue`，并检查 Jet Currency 的有符号 64 位边界；写入门禁按 `MSysDb.AccessVersion` 识别 `08.50`（V2000）与 `09.50`（V2003），其他或未知标记拒绝；批量追加和更新先检查每一行能否放进单个数据页。Memo/OLE 数据写入外部 LVAL 页，长值超过单页容量时使用链式页；Memo 可从 UTF-8 文本编码或直接提供原始字节，OLE 使用原始二进制载荷。C++ 回归覆盖跨页批量追加、MONEY 精度、指数形式和正负边界及更新、其他数值类型边界、GUID 字节序和更新、BOOLEAN 位值与 TRUE→FALSE 更新、跨 NULL mask 字节、仅 BOOLEAN 列的表、BOOLEAN 与 Memo/OLE 混排、UTF-8/UTF-16LE、空值与 NULL 区分、无效类型/编码/长度及超页批次预检、等长原位更新、变长行增长及再次迁移到新 overflow 行、overflow 数据行等长更新、inline/overflow 行缩短压实、RowId/行数稳定、Memo/OLE 多页链追加与更新、Unicode、空值、NULL、V2003 建表与行写入、未知 AccessVersion 拒绝、行删除和重开读取。固定 Jackcess 5.2.0 已逐行回读 500 行跨页 LONG 批次和 NULL、MONEY 精度/边界/更新与 20,000 字符 Memo 及 8,192 字节 OLE 长值；Java 写入 MONEY 后 C++ canonical 也已逐行核对精确金额，BOOLEAN 亦完成双向校验：Jackcess 回读 C++ 文件的位值和尾部 LONG 位置，Java 写入 TRUE/FALSE/NULL 后由 C++ canonical 核对读回为 FALSE。其他类型由 Jackcess 核对相应字段值、GUID 字节序、TEXT/BINARY 类型、Unicode、空值与 NULL，并分别重开读取变长行增长和缩短结果。逻辑 RowId 稳定由 CTest 检查，行数及读回字段由 CTest 与 Jackcess 独立检查；也确认删除中间行后剩余值及顺序。`read_write` 句柄持有数据库文件的 OS advisory 排他锁，避免合作的进程同时获取写句柄；替换已存在且正被写入的数据库也会被拒绝，同一句柄内的操作另行串行化。全局页图和表 Usage Map 目前只支持内联范围；写入要求表拥有页/空闲页 Usage Map 共用声明页且 Memo/OLE 列含可解析的列级 Usage Map。长值更新和删除尚不回收旧 LVAL 页。该写入路径目前不创建 ACE 权限行，也不维护用户索引；其他字段类型、索引表行迁移、页图/Usage Map 扩展、索引页分裂及事务恢复仍待实施。外部非合作写入者、部分 I/O 失败后的回滚或自动修复尚未处理；失败后当前句柄拒绝后续写入。`src/page/` 的内部 Jet4 原始页写入器支持既有文件的整页覆盖、末尾追加和显式刷新。尚未冻结的 P0 行为矩阵及上游同步 CI 也仍待实施。复杂列或无法识别的字段类型会保留为不支持值并记录在 `errors` 中。

已有的本机只读样本 `test_data_only_read/test_mdb.mdb`（SHA-256 `0bc0acb9c732de1e17c43c2d0a6019f7b5a62ccf83c5818a1bc42db7dbc460c6`）曾用于有限对照：系统目录 51 行与 MDBTools 1.0.1 导出在 5 个字段上匹配；Jackcess 5.2.0 canonical 对照的 43 张共同用户表无 schema 或行值差异；138 个用户逻辑索引的逐索引 `page:row` 遍历序列与 Jackcess 完全一致。该样本上的 BOOLEAN 列（`GDB_GeomColumns.HasZ/HasM`、`MSysACEs.FInheritable`）现作为可选 CTest `real_sample_boolean` 的读取基准：样本存在时注册并校验固定哈希，缺失时该测试不进入套件。该样本不属于仓库测试资产，这些结果也不替代本仓库回归或产品验收。

Jet4 读写是第一个内部里程碑，核心库在完整矩阵通过后发布。PGeo 导入和样本克隆使用独立的 GIS 验收关口，各自发布。

## 本地构建

需要 CMake 3.16 或以上以及支持 C++17 的编译器。

```sh
cmake -S . -B build
cmake --build build
(cd build && ctest --output-on-failure)
./build/rea-jackcess --version
```

`rea_jackcess::core` 是核心库的 CMake 链接目标。CLI 提供 `--help`、`--version`、`create <database-file> <v2000|v2003>`、`inspect <database-file>` 和 `canonical <database-file>`。当前 `canonical` 对 Jet4 输出已支持的 schema、`Required` 推导的列可空性、逻辑/物理索引定义和行值；CLI 尚未提供索引遍历命令。

## 包与代码分类

| 目录 | 职责 |
|---|---|
| `include/rea_jackcess/` | 对外 C++ 接口。当前有 RAII 文件句柄、数据库读写打开/空 V2000/V2003 创建 `Database`、Jet4 V2000/V2003 的 `TableBuilder`/`ColumnBuilder`、`Table`、`RowCursor`、`Row` 和 `Value`；`Table` 支持 BOOLEAN/BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/GUID/TEXT/BINARY/MEMO/OLE 行追加、等长原位更新、增长行 overflow 更新、缩短行压实和无索引本地表行删除。MONEY 使用四位小数精度的 `DecimalValue`。 |
| `src/api/` | 公开类的实现门面，组合内部解析和写入模块；当前提供数据库读写打开、空 Jet4 数据库创建、带初始 schema 的 Jet4 V2000/V2003 创建、定长数值、BOOLEAN、GUID、TEXT/BINARY 及 Memo/OLE 行追加和更新、增长行 overflow 更新、缩短行压实和无索引本地行删除。 |
| `src/format/` | Jet/ACE 版本和格式布局；当前识别已列出的版本头，按格式分别补充页、记录及编码规则。 |
| `src/page/` | 有界随机页 I/O、Jet4 数据页行目录、内部原始页写入器和 advisory 写锁；写入器仅支持既有 Jet4 文件的整页覆盖、末尾追加和刷新，页分配与格式语义写入待实现。 |
| `src/catalog/` | 系统目录、表定义、列 schema、`LvProp` 中的列 `Required` 属性、逻辑/物理索引定义、Usage Map 和通用行读取；当前发现普通本地表并装载 Jet4 表 schema/记录。 |
| `src/value/` | 字段解码、类型化值和长值；当前提供 Jet4 基础标量、日期、十进制、文本、GUID、二进制及 Memo/OLE 长值解码。 |
| `src/metadata/` | 库/表/列属性、关系、约束和逻辑索引语义；待实现。当前 Jet4 属性块和索引定义解析留在 `catalog`。 |
| `src/index/` | Jet4 B-tree 页遍历；当前支持沿索引叶页顺序读取行位置。键值语义解码、查找/范围游标、索引维护待实现。 |
| `src/expression/` | 默认值、校验、计算列和查询表达式；待实现。 |
| `src/query/` | 行过滤、组合游标和公开查询流程；待实现。 |
| `src/write/` | 数据库创建和后续表/行写入；当前从随核心库嵌入的 V2000/V2003 官方模板创建空库，支持 Jet4 V2000/V2003 BOOLEAN/BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/GUID/TEXT/BINARY/MEMO/OLE 列空表 schema，维护系统目录和两个系统索引，并在同目录暂存、C++ 回读校验后发布；行写入门禁按 `MSysDb.AccessVersion` 接受 V2000 `08.50` 与 V2003 `09.50`，可向无索引本地表追加这些类型，MONEY 按四位小数缩放到有符号 64 位整数，Memo/OLE 使用列级 Usage Map 和 LVAL 单页/链页；支持等长原位更新、将增长行写入 overflow 数据行、压实缩短行和标记删除。长值旧页不回收；其他字段类型、通用/扩展页分配、V2003 schema、ACL、索引维护和事务恢复待实现。 |
| `src/util/` | 与具体格式无关的内部帮助代码；当前提供 SHA-256 文件摘要和供读写模块共用的小端整数编解码。 |
| `tools/cli/` | 诊断和互操作对照工具；当前提供帮助、版本、Jet4 `inspect` 摘要和含行值的 `canonical` JSON v1。 |
| `tools/validation/` | 手动互操作探针；当前提供索引行序列对照、空库回读、固定及变长列空表检查，以及 Jackcess 5.2.0 对跨页批量写入、MONEY 精度/边界/更新、定长数值、GUID、BOOLEAN、TEXT/BINARY、Memo/OLE 多页追加和更新、等长更新、overflow 行增长与行缩短结果及行删除的回读；另有 Java 写 MONEY 与 BOOLEAN 后由 C++ canonical 读取的反向验证。 |
| `tests/` | CTest 单元与格式回归入口；当前覆盖头识别、页 I/O、Jet4 空库和支持列类型的空表创建、模板替换保护、目录/值读取、列属性/索引定义、公开只读与读写接口、MONEY 精度/范围/更新、整数/浮点边界、GUID、BOOLEAN、TEXT/BINARY 及 Memo/OLE 行追加和更新、长值多页链、等长更新、overflow 行增长/更新及行缩短压实和无索引行删除。本机存在 `test_data_only_read/test_mdb.mdb` 时另注册 `real_sample_boolean`，用真实 PGeo 样本核对 BOOLEAN 列读取。 |
| `adapters/pgeo/`（规划） | 单独依赖核心库和 GDAL 的 PGeo 导入、样本克隆适配。 |

C++ 以目录划分内部模块：公开接口使用 `rea_jackcess` 命名空间，内部实现使用 `rea_jackcess::detail`。现有 `api`、`format`、`page`、`catalog`、`value`、`index`、`write`、`util` 由 `src/CMakeLists.txt` 编译为内部对象包，并合并进单一 `rea_jackcess::core` 静态库。构建目标还包括 `rea_jackcess_cli`（产物名 `rea-jackcess`）、页写入/数据库创建/目录回归目标。完整包边界和依赖方向见[源码架构](src/ARCHITECTURE.md)。

### 公开接口分类

以下是目标 API 的职责边界，接口会随对应行为矩阵和 Jackcess 5.2.0 证据逐步定型，未实现的类型不会提前作为可链接 API 发布。

| API 类别 | 目标职责 |
|---|---|
| `DatabaseFile` | 当前接口：以 RAII 方式打开文件、返回格式信息和原始页。 |
| `Database` / `DatabaseBuilder` | `Database::open` 打开 Jet4 只读库或显式读写库；`Database::create` 创建 V2000/V2003 空库，或按 builder 创建 Jet4 V2000/V2003 BOOLEAN/BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/GUID/TEXT/BINARY/MEMO/OLE 列空表，并返回只读句柄。库属性 API 待实现。 |
| `Table` / `TableBuilder` | 当前读取表/列 schema 和逻辑索引定义；builder 可声明初始表名及 BOOLEAN/BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/GUID/TEXT/BINARY/MEMO/OLE 列；显式读写库可对无索引本地 Jet4 V2000 表追加这些值，MONEY 以 `DecimalValue` 精确存储四位小数，Memo/OLE 使用长值页链，并可更新行和删除行。旧长值页回收、其他字段类型、表属性、用户索引维护和结构变更待实现。 |
| `RowCursor` / `Row` / `RowId` | 当前支持顺序读取、Jet4 索引叶链顺序读取和物理页/行标识；字段名便捷访问待实现。 |
| `Value` | 当前表示已实现类型、NULL、超范围日期、Memo/OLE 负载及不支持类型。 |
| 属性与索引 API | 当前读取列 `Required` 属性与索引定义，并支持按索引顺序扫描；其余属性、关系、按键查找和范围查询待实现。 |
| `DatabaseError` | 以稳定错误码报告失败，并携带可获得的文件、页、表、行和列上下文。 |

依赖从高层流向低层：`api` 组合 `query`、`metadata`、`index`、`catalog`、`write` 和 `value`；`query` 使用 `expression`、`index`、`metadata`、`catalog` 和 `value`；`write` 后续使用 `metadata`、`index`、`catalog`、`value`、`page` 和 `format`；`index` 使用 `metadata`、`catalog`、`value`、`page` 和 `format`；`metadata` 使用 `catalog`、`value` 和 `format`；`catalog` 使用 `value`、`page` 和 `format`；`value` 使用 `page` 和 `format`；`page` 使用 `format`。详细职责见[源码架构](src/ARCHITECTURE.md)。canonical CLI 仍直接调用内部解析器，以保留阶段和列级诊断。PGeo 适配器通过公开接口使用核心库，不进入核心库依赖图；核心库不依赖 JVM、libmdb 或 GDAL。

## 从哪里开始

- [重写计划](plan/Jackcess-C++重写/README.md)列出范围、固定版本、JAR 哈希和外部资料位置。
- [实施工作包](plan/Jackcess-C++重写/01-实施工作包.md)列出开发顺序、依赖和上游同步流程。
- [验收与发布门槛](plan/Jackcess-C++重写/02-验收与发布门槛.md)列出格式矩阵、双向回读、故障测试和发布条件。
- [Jackcess 5.2.0 能力矩阵](plan/Jackcess-C++重写/03-Jackcess-5.2.0能力矩阵.md)盘点公开 API、上游测试、当前实现状态和 P0 证据缺口；当前是未冻结的初版清单。
- [AI 协作指南](AGENTS.md)说明从哪里查证、如何实施和记录结果。
