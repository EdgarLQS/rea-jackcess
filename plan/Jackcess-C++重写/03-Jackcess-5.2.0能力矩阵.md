# Jackcess 5.2.0 能力矩阵

## 状态与基线

本清单按 Jackcess `jackcess-5.2.0` 标签、源码提交 `6159c680933625ffa952334b5e0e6b24f37bfdfe` 和发布 JAR SHA-256 `9451d6f336cfaa82f5af178d5f6668b933cda4f27e261f29a1faa83630f95775` 盘点公开能力。它是 P0 初版能力目录，**尚未冻结，也不表示表中行为已经全部查证或实现**。格式与操作的初始边界见[验收门槛](02-验收与发布门槛.md#2-格式矩阵)。

状态只说明当前证据：

| 状态 | 含义 |
|---|---|
| 部分实现 | 有 C++ 代码和本地测试，但仍缺格式、行为或错误用例。 |
| 样本试验 | 与一个或有限样本对照；不能据此放行该格式或能力。 |
| 未实现 | 当前核心没有该能力的实现。 |
| 待核实 | 需继续从固定版本源码、测试和样本确定具体边界。 |

P0 放行前仍需补齐每项的适用格式、读/建/改/拒绝结果、具体预期、测试输入和产物、实际结果与版本信息。Jackcess 测试类名是证据入口，测试本身和格式条件须逐条审阅。

## 公开能力清单

| 能力域 | Jackcess 5.2.0 公开 API | 需要对齐的行为 | 上游测试入口 | 当前 C++ 状态与缺口 |
|---|---|---|---|---|
| 打开、创建与生命周期 | `DatabaseBuilder`、`Database`、`Database.FileFormat` | 既有库打开/只读模式；Jet/ACE 版本选择和新建；刷新、关闭、自动同步；密码、文件通道、字符集、时区、日期类型及打开选项 | `BuilderTest`、`DatabaseTest`、`DatabaseReadWriteTest`、`LocalDateTimeTest` | **部分实现**：Jet4 只读/显式读写打开；ACE V2007/V2010 固定样本只读，V2019 扩展日期固定样本只读；所有 ACE 写打开拒绝，V2016 与 V2019 其他 schema/行行为未验证；读写句柄排他 advisory 锁；官方模板可创建 V2000/V2003 空库；页写批次先落盘 `<库>.reajournal` 写盘日志（fsync 后）再改库，进程中断后读写打开会回放并删除日志，只读打开拒绝并提示需读写恢复；配置信息、刷新/关闭语义与跨格式行为未实现。 |
| 格式识别、编码和 codec | `Database.FileFormat`、`DataType`、`CodecProvider`、`CodecHandler` | Jet3、Generic Jet4、V2000/V2003、各 ACE 版本、MSISAM、有编码页的识别和逐操作拒绝；codec 调用契约、部分页编码和错误传播 | `JetFormatTest.getFormatNull/getFormat/readOnlyFormat/fileFormat1/fileFormat2/jet4SqlTypes/sqlTypes`；`CodecHandlerTest.codecHandlerSimple/codecHandlerNotSimple` | **部分实现**：头部识别 Jet/ACE 引擎版本、码页和 MSISAM；`FileFormat`（`DatabaseFile::file_format()` 与 `Database::file_format()`）已按 Jackcess 风格区分 generic Jet4、V2000、V2003 与 V2007+，CTest `database_file` 覆盖。核心无 codec hook，原始 `DatabaseFile` 页读取不解码。 |
| 数据库目录与库级元数据 | `Database`、`TableMetaData`、`PropertyMap`、`Relationship`、`query.Query` | 枚举系统/用户/隐藏/链接表、库属性、关系、已保存查询、linked database 与 resolver；对象名称、类型和错误语义 | `DatabaseTest`、`PropertiesTest`、`PropertyMapsTest`、`RelationshipTest`、`LinkedTableTest`、`QueryTest` | **部分实现**：Jet4 系统目录与普通本地表发现；固定 ACE V2007/V2010 样本可枚举表并发现 `Table1`；库/表属性块读写（含长值页上的大块）、关系读取与创建、保存查询读取与 SQL 重建、链接表目录行与解析式打开、Jackcess 风格格式分类。隐藏对象与完整对象分类、查询执行未实现。 |
| 表、列和结构构建 | `Table`、`TableDefinition`、`Column`、`TableBuilder`、`ColumnBuilder` | schema/属性/创建时间；列类型、大小、精度、可空性、默认值、自动编号、计算列、超链接、多值和版本列；建表、加列和名称转义/拒绝 | `TableTest`、`ColumnBuilderTest`、`BuilderTest`、`TableUpdaterTest`、`CalcFieldTest` | **部分实现**：Jet4 普通本地表 schema、列 `Required` 推导可空性、逻辑/物理索引定义读取；固定 ACE V2007/V2010 样本可读 `Table1` schema，raw type `0x12` 映射为 `DataType::complex`，`ColumnInfo` 暴露 `complex_type_id`、复杂值外键标记及 calculated 标记；固定 V2010 `calcFieldV2010` 样本按 `ResultType` 暴露逻辑类型并读取已存结果，V2019 日期计算列也仅读取已存结果（均不求值）；`ColumnInfo::complex_column_reference` 已映射 `MSysComplexColumns` 到类型表/flat table 并报告外键列名；复杂类型按固定 5.2.0 的表名优先级和 schema 形状分类；建表可声明单列升序/降序非唯一/唯一索引并写入索引定义与空根叶页（TEXT 索引键支持 General Legacy（1033/0）、General（1033/1）、General 97（1033/-1）及固定 5.2.0 将 LCID 1049/1055/1058 version 0 回退到 General Legacy 编码的行为；固定版 Jackcess 的这些 locale 描述符也委托 General Legacy；其他 LCID 默认将索引标为只读；MEMO 索引支持已实现的文本排序编码、255 个 UTF-16 单元和 510 字节完整键截断），索引页分裂与行维护已实现（唯一索引重复键拒绝）；Jet4 V2000/V2003 空库可创建 BOOLEAN/BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/SHORT_DATE_TIME/GUID/TEXT/BINARY/MEMO/OLE 列空表（定义超过一页时建表阶段即按续页链写出），`Table::add_index` 可给既有表加索引；既有定义的修改同样按续页链重组（`definition_size`、名字区与续页头同步改写，新增续页登记进全局页图），CTest `database_create`（27 个长名列）与 `multipage_definition_v2000/v2003` 覆盖；其他字段属性和列级结构修改未实现。仅 Jet4 V2000/V2003 无索引本地表可追加这些类型并读写 MONEY、BOOLEAN 与 Memo/OLE 值；MONEY 使用十进制 `DecimalValue` 精确存四位小数并校验有符号 64 位范围；BOOLEAN 列定义 `size` 为 1 但定长区不占字节，值存于行尾 NULL mask 位。带索引表可插入/更新/删除并维护单列升序/降序 BOOLEAN/BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/GUID/BINARY 键。V2003 表写入按 `MSysDb.AccessVersion` 接受 `09.50`，与 V2000 同路径。
| 行值和 RowId | `Row`、`RowId`、`DataType` | 类型化值和 NULL；各类型便捷取值；字段名匹配；行 ID 相等/排序；复杂值与字节载荷的资源生命周期 | `TableTest`、`BigIntTest`、`ExtendedDateTest`、`LongValueTest`、`ComplexColumnTest` | **部分实现**：Jet4 基础标量、日期、GUID、文本、Binary、Memo/OLE 原始字节；固定 ACE V2007/V2010 样本中的普通 `Table1` 行可遍历；V2019 固定样本 `Table1.DateExt` 可投影读取 EXT_DATE_TIME，包括 NULL、纳秒精度和早期年份；同表 `DateNormalCalc` 的 calculated schema 与已存日期值可读取，不执行公式求值；V2010 固定 `calcFieldV2010` 样本覆盖 11 个计算列的 `ResultType` 与 Boolean/TEXT/MEMO/LONG/MONEY/FLOAT/NUMERIC 已存值；多值、附件、版本历史和 unsupported 复杂值已支持类型化只读读取；SHORT_DATE_TIME 可建列、插入、更新并按 Access 序列值写入，日期文本取 Java `LocalDateTime.toString()` 形式（零秒与零小数省略、小数按三位分组），与固定 JAR 的导出文本逐字节一致；`Row::id()` 暴露页/行位置。EXT_DATE_TIME 目前只读，固定 Jar 对 Jet4 建表即拒绝该类型。BOOLEAN 读取返回 `bool`，但 Jet4 把值存在 NULL mask 位，NULL 与 FALSE 不可区分，读回只能是 `false`。Java 各便捷转换、完整复杂类型和边界错误仍待补齐。 |
| 表扫描、游标和过滤 | `Cursor`、`CursorBuilder`、`util.RowFilter`、`util.TableIterableBuilder` | 前后移动、定位 RowId、保存/恢复位置、列投影、查找/匹配、更新/删除当前行、过滤与单次/重复迭代语义 | `CursorTest`、`CursorBuilderTest`、`RowFilterTest`、`TableTest` | **部分实现**：`RowCursor` 支持前进 `next()`、后退 `previous()`、`first()`/`last()`、按 `RowId` 定位 `move_to()`、`position()` 取保存点、`current()` 重读当前行；`find_first_row()`/`find_next_row()`/`current_row_matches()` 支持按列名模式查找，`set_column_matcher()` 可注入比较器。默认遵循 Jackcess `SimpleColumnMatcher`，支持 BOOLEAN、BYTE、SHORT、LONG、MONEY/NUMERIC、FLOAT/DOUBLE、SHORT_DATE_TIME、TEXT/MEMO/GUID 的类型转换；浮点字符串遵循 Java `Double.valueOf` 语法（十六进制、`f/F/d/D` 后缀、Infinity、NaN），非法文本传播转换异常；唯一索引游标中的文本列按 OpenJDK 21.0.12 字符映射执行 Java `String.equalsIgnoreCase` 比较；BINARY 列支持 `BlobValue` 字节相等和 String 到 Java 对象序列化字节的匹配，其他对象序列化及复杂值转换仍未实现。`Table::rows(columns, filter)` 与 `rows_by_index(name, columns, filter)` 提供列投影与行过滤，导航与定位都按过滤后的行集合进行；CTest `cursor_v2000/v2003` 覆盖导航、过滤、行模式和匹配器。更新/删除当前行、`RowFilter` 的等价表达式对象与重复迭代（`TableIterableBuilder`）语义未实现。 |
| 索引定义、查询和维护 | `Index`、`IndexBuilder`、`IndexCursor`、`CursorBuilder` | 唯一/必需/忽略 NULL、复合键和升降序；键编码、排序规则、精确/最近查找、范围端点；页分裂、删除、重开后的可达性 | `IndexTest`、`BigIndexTest`、`IndexLevelTest`、`IndexCodesTest`、`CursorBuilderTest` | **部分实现**：Jet4 索引定义及叶链顺序扫描；一个 MDB 样本中 138 个用户逻辑索引的完整 RowId 序列与 Jackcess 5.2.0 一致。单列升序/降序可编码键支持从根下降的精确查找、起始扫描、`[start, stop)` 区间扫描及同区间反向扫描，真实样本索引与分裂后多层树的语义均与固定 Jackcess 一致。建表可声明单列升序/降序非唯一/唯一索引并写入逻辑/物理定义、索引名、索引使用图行与空根叶页；唯一索引在插入或键变更更新出现重复的非 NULL 键时拒绝且不写盘；插入、键变更更新与删除时按 Jet4 升序/降序键编码维护 BOOLEAN/BYTE/SHORT/LONG/MONEY/SINGLE/DOUBLE/GUID/BINARY 键（MONEY 按四位小数缩放为 64 位整数，GUID 用 8 字节分段长度前缀的二进制条目），并按"去重键"语义更新条目计数，固定 Jackcess 可读回升序条目且不残留已删除条目；插入写满叶页会分裂（根叶页升级为节点页，节点满则向上递归分裂并提升根层；22k GUID 键已验证三层树），删除把叶页清空时会摘除该叶页、删除父条目并把页面释放回索引使用图与全局页图，叶页低于半页且与同一父节点下的兄弟页合并后能容纳时合并并释放源页（CTest `index_merge_v2000/v2003` 做大面积删除回归，固定 Jar 复核叶链与查找）；BINARY 在 V2000/V2003 上通过双向对照，涵盖 NULL、空字节串、分段边界及增删改。General Legacy、General 和 General 97 TEXT 键支持建表、维护和查找；固定 5.2.0 对 LCID 1049/1055/1058 version 0 委托 General Legacy 的行为已匹配；MEMO 索引支持沿用可用文本排序编码创建、回填、维护和查找。复合索引已有建表/回填、唯一性维护、增删改、完整键查找、范围与反向范围及前导列前缀扫描回归（[database_create_test.cpp](../../tests/database_create_test.cpp)，CTest `database_create`，覆盖 V2000/V2003）；其他 LCID 在固定 Jackcess 中默认标为只读，本实现拒绝其索引写入；Jackcess 可选 `writeBrokenIndex` 策略尚未映射；固定 Jackcess 回读验证了降序单列键顺序；样本结果不等于验收。 |
| 属性、约束和关系 | `PropertyMap`、`Relationship`、`RelationshipBuilder`、列/表属性 API | 库/表/列属性读写；默认值/校验；主外键约束、引用完整性、级联更新/删除/置空、关系方向和连接类型 | `PropertiesTest`、`PropertyMapsTest`、`RelationshipTest`、`FKEnforcerTest`、`PropertyExpressionTest` | **部分实现**：读取列 `Required` 并推导可空性；Jet4 SINGLE/DOUBLE 列默认值支持有限十进制和指数数字字面量，MONEY 支持四位精度和 64 位范围内可精确编码的十进制/指数数字字面量；SHORT_DATE_TIME 默认值支持显式年份的 US 日期字面量 `#M/D/YYYY#`；显式行值优先，算术表达式、溢出、非有限数及超精度 MONEY 值拒绝（CTest `database_create`，V2000/V2003）；`Database::relationships()` 读取本地表间关系、按序列映射、引用完整性与级联标志；写入时按文件已有的本地外键索引拒绝孤儿子行和未声明级联时仍有依赖行的主端改键或删除（支持复合键与 TEXT/BINARY/GUID 键，拒绝码 `constraint_violation`），并按关系的级联标志执行级联更新、级联删除和删除置空（复用常规写入路径，可继续向孙表级联）；级联与关系创建的写盘批次共享内存回滚记录，失败时恢复到操作前，回滚自身失败则当前句柄停写。`RelationshipBuilder` + `Database::create_relationship()` 可新建关系（写两端逻辑索引描述与 `MSysRelationships` 行，缺子端外键索引时按当前行建索引并校验既有子行，要求父端唯一索引，支持跨页定义，级联环按 `std::invalid_argument` 拒绝）；缺表/列、非法 ASCII 标识符、类型不匹配、缺父端唯一索引、重复引用列，以及未启用引用完整性时设置级联均按 `std::invalid_argument` 报错。非 ASCII 关系名仍属能力限制；无效 `RelationshipJoinType` 按非法参数拒绝；级联删除和删除置空标志可同时保存；无引用完整性的关系不受十列索引上限限制，启用引用完整性时最多十列。非级联自引用已支持 V2000/V2003 创建、双索引描述、约束检查和重开。列规则支持整数比较、NULL 与整数 `BETWEEN`，以及 SINGLE/DOUBLE 有限数值比较和 `BETWEEN`；MONEY 列规则支持精确十进制/指数比较和 `BETWEEN`，表级规则支持精确 MONEY 十进制/指数比较及 BYTE/SHORT/LONG、SINGLE/DOUBLE、MONEY 数值 `BETWEEN`；列级和表级数值规则均由固定 5.2.0 JAR probes 对照；其他校验表达式和完整表达式引擎未实现 |
| 查询定义与表达式 | `query.*`、`expr.Expression`、`EvalConfig`、`EvalContext` | 各类保存查询的读取/SQL 表达；表达式解析与计算、内置函数、区域/数字/日期配置、错误传播 | `QueryTest`、`ExpressionatorTest`、`DefaultFunctionsTest`、`NumberFormatterTest`、`FormatUtilTest`、`CalcFieldTest` | **部分实现**：读取保存查询的类型、对象号、flags 与目录行，`QueryInfo::to_sql()` 重建 SELECT、UPDATE/APPEND/DELETE、MAKE TABLE、CROSSTAB、UNION、DDL 与 passthrough 的 SQL，与固定 5.2.0 `Query.toSQLString()` 逐字节一致（CTest `saved_query_v2000/v2003`）。查询执行与表达式引擎（解析、内置函数、区域/数字/日期配置）未实现，CLI `canonical` 仍是诊断导出。 |
| 复杂列、OLE 与长值 | `complex.*`、`util.OleBlob` | 附件、多值、历史记录、OLE 封装/流式读取；跨页长值读写、所有权和关闭行为 | `ComplexColumnTest`、`AttachmentColumnInfoImplTest`、`MultiValueColumnPropertyMapTest`、`LongValueTest`、`OleUtilTest`、`OleBlobTest` | **部分实现**：固定 ACE V2007/V2010 样本的普通表 schema/行可读，ComplexID 关联、类型分类和 flat-table 原始行读取可用；多值、附件、版本历史和 unsupported 均有只读类型化值；附件保留 wrapper 并解出文件载荷，复杂列写入与完整 OLE 流接口未实现；Memo/OLE 长值及 LVAL 页读写在 Jet4 范围有独立实现和回归。固定 Jackcess 5.2.0 已回读 20,000 字符 Memo 和 8,192 字节 OLE 更新 fixture，以及 Unicode、空值和 NULL。更新或删除后已回收不再被存活行引用的 LVAL 页，并同步列级拥有/空闲 Usage Map 与全局空闲位；V2000/V2003 CTest 覆盖独占页释放和共享页保留。页内孤儿行空间、Usage Map 扩展、附件/多值/历史记录写入、OLE wrapper 解码和完整流接口仍未实现。
| 错误处理和扩展点 | `JackcessException`、`ConstraintViolationException`、`ErrorHandler`、`ColumnValidator`、`ColumnMatcher`、`LinkResolver` | 公开错误类型/上下文、可恢复错误、校验器/匹配器替换、链接解析、调用顺序和失败传播 | `ErrorHandlerTest`、`ColumnValidatorTest`、`CustomLinkResolverTest`、`LinkResolverTest` | **部分实现**：`DatabaseError` 和诊断可带文件/页/表/行/列上下文；`Database::set_column_validator_factory()` 可为本地 Jet4 列提供值替换或拒绝逻辑，插入/更新时在编码和落盘前执行；整批插入的拒绝不改文件。`open_linked_table()` 接受调用方路径 resolver，按源数据库状态缓存目标，支持多跳解析、循环拒绝和返回 `Table` 的 RAII 生命周期；`CustomLinkedSourceResolver` 可按外部源缓存用户自有状态，并在首次请求表时调用 `CustomLinkedSource` 惰性加载快照；空源表示不是自定义源，文件链接随后走路径 resolver；已识别源缺表时返回空表结果且不回退。源对象随返回表句柄保活。ODBC 链接可交由自定义 resolver 提供快照；合成 Jet4 ODBC 目录行回归覆盖 V2000/V2003 元数据传递、文件 resolver 不回退、快照读取和缓存。表快照分别物化到临时 Jet4 V2000 数据库，受现有表创建/写入类型与 ASCII 名称约束，尚无同源多表共享临时库、内存数据库/自定义临时目录选项及完整错误分类。`ErrorHandler` 可在 Database/Table/RowCursor 级处理列解码异常，替换或重抛并获得列原始字节和行位置；`RowCursor` 支持行模式、自定义比较器和 Jackcess 默认比较器（常见标量类型转换、唯一索引游标的文本列按 OpenJDK 21.0.12 执行 Java `String.equalsIgnoreCase` 比较）；BINARY 列支持 `BlobValue` 字节相等和 String 到 Java 对象序列化字节的匹配，其他对象序列化、复杂值转换及完整 Jackcess 错误分类仍未实现；CTest `database_create` 覆盖 V2000/V2003 错误替换、resolver 多跳/缓存/生命周期与重抛，`cursor_v2000/v2003` 覆盖 matcher。 |
| 导入导出、通道和帮助工具 | `ImportUtil`、`ExportUtil`、filters、`ReadOnlyFileChannel`、`MemFileChannel`、格式化与 OLE helpers | CSV/对象导入导出、过滤器、文件通道、OLE 构造与读写、辅助格式和异常行为 | `ImportTest`、`ExportTest`、`ImportExportUtilTest`、`FileChannelTest`、`MemFileChannelTest`、`OleBlobBuilderTest`、`ColumnFormatterTest` | **部分实现**：`<rea_jackcess/csv.hpp>` 的 `export_table_csv`/`import_table_csv` 与 CLI `export`/`import` 支持 CSV 导出导入，覆盖 BOOLEAN/BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/GUID/SHORT_DATE_TIME/TEXT/BINARY/MEMO/OLE；导出文本与固定 5.2.0 `ExportUtil` 逐字节一致（默认全列输出，CTest `csv_round_trip`），导入支持表头名称映射、引号/换行/双引号转义、空字段写 NULL（[csv_test.cpp](../../tests/csv_test.cpp)）。C++ 导出 API 可按名称选择列并指定输出顺序，也可按投影行谓词跳过行；导入 API 可在字段映射和类型转换前按原始字段谓词跳过行（同文件回归）。SHORT_DATE_TIME 的导入导出与固定基线文本一致，由 `date_time_v2000/v2003` 覆盖（[date_time_test.cpp](../../tests/date_time_test.cpp)）。复杂列、列/行值改写过滤器、`ResultSet` 导入、文件通道与 OLE helpers 未实现。 |

## P0 抽查记录：建库、建表与行修改

以下测试方法已从固定提交 `6159c680933625ffa952334b5e0e6b24f37bfdfe` 的源码逐项查看。这里只记录一个与当前 C++ 实现直接相关的切片，不代表整张矩阵已完成审阅或冻结。源码路径均相对于 Jackcess 仓库根目录 `src/test/java/io/github/spannm/jackcess/`。

| 能力 | Jackcess 5.2.0 测试方法 | 从测试确认的预期 | 当前 C++ 对照与差距 |
|---|---|---|---|
| 新库创建 | `DatabaseTest.create`（179–185，`@FileFormatSource`）；`BuilderTest.databaseBuilderCreateStatic`（204–210） | 新建库初始没有用户表；静态 create 调用 V2000 重载并检查返回格式属于 `FileFormat` 枚举。前者按 `FileFormatSource` 参数化，具体格式由该 source provider 决定 | C++ 能用固定模板创建空 V2000/V2003，并通过独立 Java 检查；目前没有逐个 Jackcess `FileFormatSource` 格式对照，不能外推到其他格式 |
| 表定义校验 | `DatabaseTest.invalidTableDefs`（88–116，`@FileFormatSource`） | 拒绝无列表、大小写不敏感的重名列/表、不合法列长度和过长列名；有效单列定义可创建 | C++ 建表限制为 ASCII 名称及 BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/GUID/TEXT/BINARY/MEMO/OLE；拒绝名称超出子集，但 Jackcess 的标识符转义和完整长度规则尚未对齐 |
| 构建器语义 | `BuilderTest.tableBuilderEscaping`（36–51）、`tableBuilderCollections`（53–72）、`databaseBuilderProperties`（178–202）；`ColumnBuilderTest` 的 `maxPrecisionAndScale`、`maxLength`、`flags`、`properties`、`validate`、`validateCalculated`（31–204） | 覆盖保留字/标识符转义、批量列和索引添加、数据库属性、列长度/精度/scale、压缩 Unicode、超链接、AutoNumber、列属性及格式相关定义校验 | 当前 C++ builder 只有表名与列名/类型，未提供索引、数据库/列属性、精度、scale、AutoNumber、计算列或标识符转义；这些是完整 API 对齐缺口 |
| 行追加和批量追加 | `impl/DatabaseReadWriteTest.writeAndRead`（49–90）、`writeAndReadInMem`（57–63）、`writeAndReadInBatch`（92–117，均为 `@FileFormatSource` 参数化） | 写入单行循环与 1,000 行批次后逐字段回读；样例含 NULL，并比较各列值 | C++ 仅支持 Jet4 V2000 无索引本地表和受限类型；本地批次/Jackcess 对照记录见[工作包 P2](01-实施工作包.md)，不能覆盖测试 source 中的其他格式或类型 |
| 行更新 | `impl/DatabaseReadWriteTest.updateRow`（119–220，`@FileFormatSource`） | 支持 `KEEP_VALUE`、AutoNumber 语义、变长文本更新、当前行更新、Row 对象更新后回读；测试包含更长的新文本。[本机参考探针](04-P0基线核验记录.md#2026-11-20已有-v2000-库变长行与索引探针)确认 V2000 变长行更新会写 overflow 数据行并保留逻辑表头 RowId | C++ 的 `Table::update_row` 支持 Jet4 V2000/V2003 本地表原位更新、行增长 overflow 迁移、缩短压实并保留逻辑 RowId；Memo/OLE 长值更新可复用列级空闲页，更新/删除后回收无存活引用的页并同步列级拥有/空闲与全局 Usage Map。V2000/V2003 合成回归覆盖；长值回收后的 Usage Map 尚未由固定 Jackcess 独立检查，完整格式/API 矩阵仍待验收。
| 行删除 | `DatabaseTest.deleteCurrentRow`（187–250）、`deleteRow`（252–282，均为 `@FileFormatSource` 参数化） | 覆盖按游标删除、按 Row 删除、行数和剩余顺序、删除后再插入，以及重复删除后数据不变。本机参考探针还确认一次索引 TEXT 键变更、索引表删除和重开后的顺序 | C++ 的 `Table::delete_row` 支持 Jet4 V2000/V2003 本地表并维护受支持的索引条目；删除后回收不再被存活行引用的 LVAL 页并更新 Usage Map。V2000/V2003 合成回归覆盖 LVAL 回收；尚无游标删除接口，其他格式行为仍待验收。 |

测试行号对应固定 Jackcess 5.2.0 源码；基线 JAR、模板及本机样本哈希见[本机基线核验记录](04-P0基线核验记录.md)。本表抽查揭示的范围缺口仍需按格式、公开 overload、异常语义和可复现输入继续展开，并进入 P0 冻结审查。

## P0 抽查记录：索引分裂与游标顺序

| 能力 | Jackcess 5.2.0 测试方法 | 从测试确认的预期 | 当前 C++ 对照与差距 |
|---|---|---|---|
| 大索引追加、排序和删除 | `BigIndexTest.bigIndex`（53–180，`@TestDbSource(BIG_INDEX)`） | 覆盖 2,000 个伪随机长键、批次与单行追加、NULL 键、索引结构校验、升序游标、中间删除、保留首两项、再次追加及清空索引 | 固定版本单样本参考结果与命令见[基线记录](04-P0基线核验记录.md#2026-11-20已有-v2000-库索引分裂与重开顺序)；C++ 目前能扫描现有叶链，但未创建/维护索引，也未实现按索引游标定位、删除和页分裂 |
| 多层索引页 | `impl/IndexLevelTest.writeDataPage_multiLevelIndex_writesLevel`（42–78，`@TestDbSource(BIG_INDEX)`） | 插入 1,000 个长键后验证根页是 node 且层级至少为 1；扫描全部页，要求 leaf 层级为 0、node 层级大于 0 | 单个 V2000 参考副本重开后根层级为 2，观察到 90 个 leaf 与 8 个 node；其余格式和 C++ B-tree 写入仍未验证 |

上述测试的 `@TestDbSource` 实际覆盖 V2000、V2003、V2007、V2010；本机探针只验证了 V2000，不得据此推定其他格式行为。

## P0 抽查记录：ACE 复杂列

| 能力 | Jackcess 5.2.0 测试方法 | 从测试与本机样本确认的行为 | 当前 C++ 对照与差距 |
|---|---|---|---|
| 复杂列关系发现 | `ComplexColumnTest.complexInfo_flatTable_usesFlaggedForeignKeyColumn`（358–386）、`complexInfo_typeTableName_determinesType`（394–402） | V2007/V2010 样本均有版本历史、多值和附件三列；各自的 flat table 含 AutoNumber 主键及标记的复杂值外键。复杂列类型由关联表名识别。本机读探针计数相同 | C++ 可识别 raw type `0x12`，解析 ComplexID 与概念表/类型表/flat table/AutoNumber 主键/复杂值外键列，并按 5.2.0 命名和 schema 规则分类附件、多值、版本历史及 unsupported；`raw_complex_values()` 可按父行 RowId 读取关联 flat-table 行，`complex_values()` 提供多值/附件/版本历史 typed payload 并保留 unknown 原始行；外键标记缺失时按固定版选取最后一个额外的非自动编号 LONG 列，并由合成目录单测覆盖；真实非标记样本及坏结构行为仍待验证；样本与哈希见[基线记录](04-P0基线核验记录.md#2026-11-20-ace-复杂列样本与键关系) |
| 附件、多值和版本历史 | `ComplexColumnTest.versions`（68–183）、`attachments`（187–278）、`multiValues`（282–350）；`AttachmentColumnInfoImplTest`；`impl/complex/MultiValueColumnPropertyMapTest` | 测试覆盖关联值读取/新增/更新/删除/清空、附件普通与编码载荷、文件元数据、多值属性保存，以及版本追加；已有版本项的修改/删除按 API 约定拒绝。本机只读样本分别计数为 5/6/3 个值 | C++ 提供类型化只读值，固定样本核对多值/版本文本与顺序、附件名和解码文件数据；复杂值写入、附件文件/属性保存和完整 OLE 流接口未实现 |
| 未知复杂类型 | `ComplexColumnTest.unsupported`（407–439，`UNSUPPORTED_FIELDS`） | 固定版本将 `UnknownComplex` 归为 `UNSUPPORTED`，可遍历原始值；附件/多值/版本专用访问会拒绝。本机 V2007 样本关联值数为 1、2、0 | C++ 对 `unsupported-fields-v2007.accdb` 中的 `UnknownComplex` 已分类为 `unsupported`，`raw_complex_values()` 按三行读取关联计数 1、2、0；未知类型返回 unsupported typed tag，flat-table 原始行中的未知字段通过 `UnsupportedValue::raw_bytes` 保留；值编辑未实现 |

这些样本只覆盖 V2007 与 V2010；复杂列在其他 ACE 版本的写入限制、文件兼容性和客户工作流仍需验证。

## P0 抽查记录：格式识别与 codec

以下行为来自固定 5.2.0 的 `src/main/java/io/github/spannm/jackcess/` 源码及 `src/test/java/io/github/spannm/jackcess/impl/` 测试；测试行号仍以固定提交为准。

| 能力 | 源码与测试入口 | 已确认行为 | 当前 C++ 对照与缺口 |
|---|---|---|---|
| FileFormat 识别 | `impl/JetFormat.java`（`PossibleFileFormats`，105–136）；`impl/JetFormatTest.getFormat/fileFormat1/fileFormat2`（48–94） | Jet 头版本与 `MSysDb.AccessVersion` 联合区分 V1997、Generic Jet4、V2000、V2003；ACE 版本号映射 V2007/V2010/V2016/V2019；MSISAM 有独立格式。`fileFormat2` 单独覆盖 Generic Jet4 | C++ 头识别返回引擎族；V2000/V2003 写门禁另读 `AccessVersion=08.50`，但 `FormatInfo` 尚不公开完整 FileFormat 分类，也未逐类比对所有版本 |
| 只读格式和 SQL 类型 | `impl/JetFormatTest.readOnlyFormat`（58–78）、`jet4SqlTypes/sqlTypes`（96–119） | 对只读格式尝试保存属性会以 `NonWritableChannelException` 失败，可写格式不抛该错误。Jet4 支持类型做 SQL 类型往返；JDBC `BIGINT` 默认映射 LONG、V2016 映射 BIG_INT，长 VARCHAR 映射 MEMO | C++ 有 Jet3 只读标记和显式 `read_write` 打开，但尚未按 Jackcess 格式矩阵逐项验证写拒绝；公开 API 尚无 JDBC SQL 类型映射等价接口 |
| 自定义页面 codec | `impl/CodecProvider.java`（29–38）、`CodecHandler.java`（24–57）、`DefaultCodecProvider.java`（25–68）；`impl/CodecHandlerTest.codecHandlerSimple/codecHandlerNotSimple`（45–90） | provider 按数据库通道和字符集创建 handler。handler 声明能否部分页编码、能否原地解码，并逐页编码/解码；默认 provider 对未编码库使用 dummy handler，对有编码键的 Jet/Office 库和 MSISAM 返回拒绝编码/解码的 handler | C++ 核心没有 codec provider/handler 扩展点；`DatabaseFile::read_page` 明确返回未解码的原始页，不应把头部可识别误认为支持加密页 |
| codec 行为及错误边界 | `impl/CodecHandlerTest.codecHandlerSimple/codecHandlerNotSimple`（45–135、201–269）；测试的 `@FileFormatSource` provider（`test/source/FileFormatSource.java`，37–79） | 两种自定义 handler 分别覆盖部分页与整页编码；打开编码副本后建两张带主键/索引的表，写入记录，按索引正反向遍历并删除，最后各验证删除 100 行。`FileFormatSource` 默认仅提供非只读且非 MSISAM 格式；这些 codec 测试不证明默认 provider 能解码密文 | 目前无 codec 测试或扩展接口；加密页和 MSISAM 页尚无可配置拒绝/API 错误映射，也未建立外部加密插件边界 |

`DefaultCodecProvider` 的默认拒绝规则是源码契约；`CodecHandlerTest` 只验证自定义 handler 的编码与解码流程。两类证据不能互相替代。该抽查也显示，当前 C++ `EngineVersion` 与 Jackcess `FileFormat` 不是一一对应关系；冻结矩阵时需将头格式、`AccessVersion`、codec 状态和操作结果分列记录。

## P0 抽查记录：属性、关系、链接表与保存查询

| 能力 | Jackcess 5.2.0 测试方法 | 从测试确认的预期 | 当前 C++ 对照与差距 |
|---|---|---|---|
| 属性映射和值 | `PropertiesTest.propertyMaps/inferTypes/readProperties/parseProperties/writeProperties/modifyProperties`（48–312）；`impl/PropertyMapsTest.read_flaggedProperty_keepsWholeFlagByte/write_flaggedProperty_keepsWholeFlagByte/put_newProperty_getsDdlBitOnly/write_sameNameColumnAndIndexBlock_keepsBothSeparate` | 默认映射与具名映射分开；属性名不区分大小写；按值推导类型；读取库/表/列/摘要/用户属性；序列化往返保留旗标和同名但不同块；未显式 `save()` 的修改不持久，保存后重开可读到更新/删除结果 | **部分实现**：C++ 的 `Database::property_maps()`、`Table::property_maps()` 读取原始属性块并解码已支持的值；`save_property_maps()` 保存原始记录并保留标志字节，支持较大的长值属性块，列 `Required` 更新即时反映在 nullable 和写入校验中。CTest `database_create` 覆盖 V2000/V2003 保存、重开和大属性块（[database_create_test.cpp](../../tests/database_create_test.cpp)）。`PropertyMapInfo` 支持基于 OpenJDK 21.0.12 的 Locale.ROOT 完整大写映射查找与替换、带默认值读取、自动/显式 Jet 类型写入及 `put_all()`；批量复制保留完整 flags。自动推导覆盖固定基线的 21 个内建属性和常见值类型，`PropertyInfo::set_value()` 支持 SHORT_DATE_TIME 编码，null 值记录保存时省略；CTest `catalog` 覆盖推导、标志、序列化和 Unicode 名称查找（[catalog_test.cpp](../../tests/catalog_test.cpp)）。大小写映射数据来自 OpenJDK 21.0.12；Numeric 等未支持类型推导、计算表达式及完整属性语义仍未实现 |
| 关系读取与过滤 | `RelationshipTest.twoTables/oneTable/noTables`（36–110） | 能按两个表、单表或全库枚举关系；保留 from/to 表及列顺序、名称、引用完整性、级联标记；同一表作为两端会抛 `IllegalArgumentException` | **部分实现**：C++ `Database::relationships()` 读取本地表关系、列映射、参照完整性、级联与联接类型；`create_relationship()` 可写目录行和两端逻辑索引描述，自动创建缺少的子端索引并检查已有子行；行写入路径执行外键约束及更新、删除、置空级联，支持多级级联和非级联自引用。缺表/列、非法 ASCII 标识符、类型不匹配、父端无唯一索引和非法级联组合按 `std::invalid_argument` 映射基线 `IllegalArgumentException`。非 ASCII 关系名仍属能力限制；无效 `RelationshipJoinType` 按非法参数拒绝；固定基线允许级联删除与删除置空标志共存；无引用完整性的关系不受十列索引上限限制，启用引用完整性时最多十列。CTest `relationship_create`、`foreign_key_v2000/v2003` 覆盖创建、重开、约束和级联。其他错误分类及关系过滤/格式行为仍待核对 |
| 文件链接和链接解析 | `LinkedTableTest.linkedTables`（36–144） | 无 resolver 时访问文件链接可抛 `AccessDeniedException`；元数据仍报告链接名称、目标表、数据库路径和类型。配置 resolver 后可读取目标表，并能创建新的 linked table；遍历器分别支持常规表、仅本地用户表和系统表筛选 | **部分实现**：C++ 的 `linked_tables()` 暴露文件/ODBC 链接元数据，`create_linked_table()` 可写入文件链接；`open_linked_table()` 接受调用方路径 resolver 打开文件链接，支持多跳、按源状态缓存目标（键为存储的目标文件名）、循环拒绝，并由返回 `Table` 保持目标状态；`CustomLinkedSourceResolver` 按外部源缓存源对象，`CustomLinkedSource` 在首次请求目标表时惰性加载快照；返回空源的文件链接回退到路径 resolver，已识别源缺表时返回空表结果且不回退。源对象随相关表句柄保活，表快照分别物化到临时 Jet4 V2000 数据库并按表缓存。受现有 `TableBuilder` 列类型、ASCII 表/列名和行写入规则约束；内存库/自定义临时目录选项、同源多表共享临时库和完整 Jackcess 错误语义未实现。CTest `database_create` 覆盖 V2000/V2003 的源缓存、表惰性加载/缓存、缺表空结果/不回退、路径回退、源对象生命周期及只读/读写访问模式继承与文件链接写穿。默认 resolver 安全策略和完整目录遍历筛选仍未实现 |
| ODBC 链接表 | `LinkedTableTest.odbcLinkedTables`（146–171） | 读取目标表名、连接串、20 列和 5 个索引；`toString()` 不泄漏连接串中的密码；迭代和写入明确抛 `UnsupportedOperationException` | **部分实现**：C++ `linked_tables()` 暴露 ODBC 链接类型、目标表名和存储的连接串；尚不解析本地列/索引 schema，不提供内建 ODBC 驱动或密码脱敏；自定义 resolver 可提供表快照，未实现 Jackcess 风格的 ODBC 行迭代/写入拒绝接口 |
| 保存查询 | `query/QueryTest.readQueries`（169–222）、`badQueries`（148–167）；查询类型构造覆盖 `unionQuery/passthroughQuery/dataDefinitionQuery/updateQuery/selectQuery/appendQuery` | 从查询对象目录解析 SELECT、DELETE、APPEND、UPDATE、MAKE TABLE、CROSSTAB、UNION、PASSTHROUGH 和 DATA DEFINITION；SQL 输出按解析出的字段、JOIN、条件、排序和参数组合；未知或结构不完整查询按操作抛明确异常 | **部分实现**：C++ `Database::queries()` 读取 Jet4 保存查询目录，`QueryInfo::to_sql()` 重建九类 SQL；CTest `saved_query_v2000/v2003` 将查询类型、对象元数据及 SQL 与固定 Jackcess 5.2.0 输出逐项比较（[query_test.cpp](../../tests/query_test.cpp)）。查询执行、完整错误分类及所有畸形目录行行为仍未实现 |

以上测试分别依赖 `COMMON1`、`INDEX`、`LINKED`、`LINKED_ODBC`、`QUERY` 语料或合成对象。后续登记需核对这些测试 source 实际返回的格式、样本哈希和逐项预期；目前不以测试类名或源码检查代替当前 C++ 的格式回归与互操作证据。

`impl` 包中的 `CodecProvider`/`CodecHandler` 虽位于实现命名空间，仍是 Jackcess 的扩展接口，应纳入能力范围；不得因为命名为 `impl` 而漏项。反之，内部类和私有行为不自动成为 C++ 公开 API，除非其影响可观察文件行为或公开扩展契约。

## 格式与操作放行矩阵

格式初始范围保留在[验收与发布门槛 §2](02-验收与发布门槛.md#2-格式矩阵)。对上表每个适用能力，仍需把格式矩阵具体化为：

| 格式组 | 打开/读取 | 创建 | 已有库修改 | 拒绝/不支持行为 |
|---|---|---|---|---|
| Jet3 V1997 | 待从 5.2.0 源码和用例逐项核对 | 计划为拒绝，需固定拒绝 API、错误和文件状态 | 计划只读，需验证每类修改入口拒绝 | 待建负例与原文件哈希断言 |
| Generic Jet4 | 待核实 | 计划为拒绝 | 计划支持，需区分允许的改动 | 待从对应 fixture/test 映射 |
| Jet4 V2000/V2003 | V2000 有限样本试验，其他行为待矩阵 | **部分实现**：可新建空 V2000/V2003 库；V2000 与 V2003 均支持创建 BOOLEAN/BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/GUID/TEXT/BINARY/MEMO/OLE 列空表 | **部分实现**：V2000/V2003 本地表支持数值、MONEY、BOOLEAN、GUID、TEXT/BINARY 与 Memo/OLE 长值追加、更新和删除，并在插入、键变更更新和删除时维护受支持的单列升序/降序索引条目与去重键计数、增长行 overflow 迁移和缩短行压实；MONEY 校验四位小数精度和 Jet Currency 的有符号 64 位边界，BOOLEAN 使用行尾 NULL mask 位（列定义 `size` 为 1 但不占定长区，NULL 读回为 FALSE），长值使用 LVAL 页链和列级 Usage Map，更新/删除后释放不再被存活行引用的页并标记全局空闲，共享页保留；插入可分裂叶页并向上递归分裂节点、提升根层（已验证三层树）、删除可摘除并释放清空的叶页、映射扩展与条目再分配未实现 | 部分验证；格式读写矩阵与已有库修改未验收 |
| ACE V2007/V2010/V2016/V2019 | V2007/V2010 固定样本：普通 schema/行、复杂列关联/分类及类型化复杂值只读；附件 wrapper 可解码。V2010 `calcFieldV2010` 固定样本覆盖计算列 `ResultType` 与已存结果读取。V2019 固定样本验证扩展日期列和 `DateNormalCalc` 已存计算结果读取；V2016 与其余 V2019 schema/行行为未验收 | 未实现 | ACE `read_write` 对 V2007/V2010/V2019 固定样本以 `unsupported_operation` 拒绝 | 按版本补齐 schema、typed values、复杂列与写入证据后再验收 |
| MSISAM/有编码 Jet/ACE | 头/编码识别和拒绝边界待核实 | 待核实 | codec 接口和默认拒绝待核实 | 待补独立样本与自定义 codec 用例 |

上述“计划为拒绝”来自现行项目门槛，不是已确认的 Jackcess 5.2.0 行为；冻结前须检查对应 `FileFormat`、Builder 和测试。

## P0 难点和证据缺口

| 难点 | 现有证据 | 尚缺验证 | 状态 |
|---|---|---|---|
| 索引排序键、复合键、分裂与查询顺序 | 仓库 `tools/validation/` 在固定 Jet4 样本上比较 138 个用户索引的完整 RowId 序列，结果一致；已有合成多层 B-tree/叶链测试；固定 Jackcess 5.2.0 的 V2000 BIG_INDEX 副本插入 1,000 行后重开，根页 level=2、90 个叶页、8 个 node 页且游标升序 | 多格式排序规则、复合键、升降序/NULL/重复键/长文本、删除后重开、精确查找和范围边界；C++ 写入导致的页分裂 | 参考行为已有单格式试验；C++ 只读扫描，不能放行索引能力 |
| 已有库更新、迁移行和长值变更 | C++ writer 支持 Jet4 V2000/V2003 本地表的受支持类型追加、更新和删除，并维护已实现的索引；Memo/OLE 长值更新/删除会回收不再被存活行引用的页。本仓库 `database_create` 在 V2000/V2003 合成无索引表上检查列级拥有/空闲 Usage Map 与全局空闲位；固定 Jackcess 5.2.0 曾独立回读 Memo/OLE 更新值，但未独立检查本轮回收后的映射 | 固定 Jackcess 对回收后 Usage Map 的独立重开检查、大库性能、已有索引库回收以及其他格式验证；C++ 合成 fixture 和命令/哈希见[基线记录](04-P0基线核验记录.md) | 回收用例限合成无索引表；更新/删除按次扫描表内长值描述及链页，尚无大库耗时基准。已有索引库与其他格式回收仍未验证 |
| ACE 附件、多值、版本历史等复杂列 | 固定 Jackcess 测试源码覆盖 V2007/V2010 复杂列 CRUD 和 V2007 未知类型；只读探针对两个版本确认复杂表主外键关系及版本历史/多值/附件计数 | 复放固定源码 CRUD 预期、核对 ACE 其他版本与删除/失败状态，设计 typed complex value 和未知类型字段访问策略并建立产品/GDAL 对照 | 参考样本审计部分完成；C++ 已解析关联元数据、分类类别并提供类型化只读读取及附件 OLE wrapper 解码；异常 schema、ACE 写入和其他版本验证未完成 |
| 原生目标与估算 | 当前仓库有本机 CMake/CTest 结果；计划存有旧的总体估算范围 | 原生 Linux x86_64/ARM64 环境、Access/ArcGIS 目标版本、性能门槛、逐包工量和关键路径 | 未核实；旧总体估算不作为本版分包估算 |

## P0 缺口标记与关闭记录（2026-11-22）

接手时四项缺口只写在[待办清单](06-待办清单.md)，矩阵没有对应条目。以下把它们登记进矩阵，并记录关闭证据；状态均为**已实现并带回归**，仍属部分实现，不等于 P0 冻结。

| 缺口 | 影响 | 关闭证据 | 状态 |
|---|---|---|---|
| 多页表定义修改 | 表定义超过一页时 `Table::add_index` 与关系创建直接拒绝，宽表无法加索引或建关系（后续补充：初始建表也受限） | 定义按逻辑流重组后重新分页，续页头 `02 01 <free> <next>`、`definition_size`、名字区与新增续页的全局页图同步；CTest `multipage_definition_v2000/v2003`（55 列、30 个长名索引，跨越续页后继续加索引）；固定 Jar 在 3 页定义上读到 30 个索引、按索引查回行并可继续追加行 | 已关闭 |
| 写盘中断的恢复 | 进程被杀或掉电后可能留下半写页面，且会被当成正常库继续写 | `WriteJournal` 在改页前把原始页镜像 `fsync` 到 `<库>.reajournal`，批次 flush 后删除；读写打开时回放并截断新增页，只读打开拒绝并提示；CTest `crash_recovery_v2000/v2003`（页回滚、追加页截断、只读拒绝、恢复后可写、重建清理陈旧日志）；关闭回放或关闭提交两处变异都会让测试失败 | 已关闭 |
| 日期/时间列 | `TableBuilder` 拒绝 `short_date_time`，写路径无日期编码，CSV 无法交换日期 | `SHORT_DATE_TIME` 进入定长类型表（8 字节 Access 序列值），`jet4_date_serial` 统一解析并做日历校验，支持 year 100 与 1765 的纪元前日期；V2000/V2003 写入后由固定 5.2.0 JAR 独立重开核对；日期文本改为 Java `LocalDateTime.toString()` 形式（零秒省略、小数 3/6/9 位分组）；CSV 导出与固定 `ExportUtil` 逐字节一致；CTest `date_time_v2000/v2003` | 已关闭 |
| 叶页合并 | 删除只摘空叶页，长期删改后索引碎片化 | 叶页低于半页且与同一父节点下的兄弟页合并后放得下时合并，更新叶链并释放源页；CTest `index_merge_v2000/v2003` 删除 1200 行中的 1170 行后索引页从多页降到 1 页，禁用合并在测试中立即失败；固定 Jar 复核叶链顺序、行数与索引查找 | 已关闭 |

尚未消除的相关边界：写盘日志的持久性依赖 `fsync`，未覆盖介质损坏；叶页合并只在同一父节点内进行，节点页不合并；建表时仍拒绝单表列数超过 Jet4 上限、以及需要收缩定义（删除列/索引）的改动。

## 冻结条件

本矩阵目前只完成 API/测试入口初盘。冻结 P0 还要：审阅上表引用的 5.2.0 测试；把每类公开 overload、错误/拒绝语义和适用格式映射到可复现用例；确认样本和工具哈希；补齐三个难点的实验记录；核实原生环境、许可与资源门槛；据此修订逐包估算和关键路径。冻结后新增能力按同样结构补记录，不覆盖历史结果。
