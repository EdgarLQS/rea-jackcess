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
| 打开、创建与生命周期 | `DatabaseBuilder`、`Database`、`Database.FileFormat` | 既有库打开/只读模式；Jet/ACE 版本选择和新建；刷新、关闭、自动同步；密码、文件通道、字符集、时区、日期类型及打开选项 | `BuilderTest`、`DatabaseTest`、`DatabaseReadWriteTest`、`LocalDateTimeTest` | **部分实现**：Jet4 只读/显式读写打开；读写句柄排他 advisory 锁；官方模板可创建 V2000/V2003 空库；配置信息与跨格式行为未实现。 |
| 格式识别、编码和 codec | `Database.FileFormat`、`DataType`、`CodecProvider`、`CodecHandler` | Jet3、Generic Jet4、V2000/V2003、各 ACE 版本、MSISAM、有编码页的识别和逐操作拒绝；codec 调用契约、部分页编码和错误传播 | `JetFormatTest.getFormatNull/getFormat/readOnlyFormat/fileFormat1/fileFormat2/jet4SqlTypes/sqlTypes`；`CodecHandlerTest.codecHandlerSimple/codecHandlerNotSimple` | **部分实现**：头部识别 Jet/ACE 引擎版本、码页和 MSISAM；尚不能仅从公开 `FormatInfo` 区分 Generic Jet4、V2000 与 V2003。核心无 codec hook，原始 `DatabaseFile` 页读取不解码。 |
| 数据库目录与库级元数据 | `Database`、`TableMetaData`、`PropertyMap`、`Relationship`、`query.Query` | 枚举系统/用户/隐藏/链接表、库属性、关系、已保存查询、linked database 与 resolver；对象名称、类型和错误语义 | `DatabaseTest`、`PropertiesTest`、`PropertyMapsTest`、`RelationshipTest`、`LinkedTableTest`、`QueryTest` | **部分实现**：Jet4 系统目录与普通本地表发现、部分属性/索引定义读取。库属性、关系、查询定义、链接表和完整对象分类未实现。 |
| 表、列和结构构建 | `Table`、`TableDefinition`、`Column`、`TableBuilder`、`ColumnBuilder` | schema/属性/创建时间；列类型、大小、精度、可空性、默认值、自动编号、计算列、超链接、多值和版本列；建表、加列和名称转义/拒绝 | `TableTest`、`ColumnBuilderTest`、`BuilderTest`、`TableUpdaterTest`、`CalcFieldTest` | **部分实现**：Jet4 普通本地表 schema、列 `Required` 推导可空性、逻辑/物理索引定义读取；V2000 空库可创建 BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/GUID/TEXT/BINARY/MEMO/OLE 列空表；其他字段属性和表结构修改未实现。仅 Jet4 V2000 无索引本地表可追加这些类型并读写 MONEY 与 Memo/OLE 值；MONEY 使用十进制 `DecimalValue` 精确存四位小数并校验有符号 64 位范围。行更新、迁移和删除仍限无用户索引表。V2003 或未知 `AccessVersion` 写入会拒绝。
| 行值和 RowId | `Row`、`RowId`、`DataType` | 类型化值和 NULL；各类型便捷取值；字段名匹配；行 ID 相等/排序；复杂值与字节载荷的资源生命周期 | `TableTest`、`BigIntTest`、`ExtendedDateTest`、`LongValueTest`、`ComplexColumnTest` | **部分实现**：Jet4 基础标量、日期、GUID、文本、Binary、Memo/OLE 原始字节；`Row::id()` 暴露页/行位置。Java 各便捷转换、完整复杂类型和边界错误仍待补齐。 |
| 表扫描、游标和过滤 | `Cursor`、`CursorBuilder`、`util.RowFilter`、`util.TableIterableBuilder` | 前后移动、定位 RowId、保存/恢复位置、列投影、查找/匹配、更新/删除当前行、过滤与单次/重复迭代语义 | `CursorTest`、`CursorBuilderTest`、`RowFilterTest`、`TableTest` | **部分实现**：只读单遍 `rows()`；游标导航、定位、投影、过滤和修改未实现。 |
| 索引定义、查询和维护 | `Index`、`IndexBuilder`、`IndexCursor`、`CursorBuilder` | 唯一/必需/忽略 NULL、复合键和升降序；键编码、排序规则、精确/最近查找、范围端点；页分裂、删除、重开后的可达性 | `IndexTest`、`BigIndexTest`、`IndexLevelTest`、`IndexCodesTest`、`CursorBuilderTest` | **部分实现**：Jet4 索引定义及叶链顺序扫描；一个 MDB 样本中 138 个用户逻辑索引的完整 RowId 序列与 Jackcess 5.2.0 一致。键语义、点/范围查询、其他排序规则及写维护未实现；样本结果不等于验收。 |
| 属性、约束和关系 | `PropertyMap`、`Relationship`、`RelationshipBuilder`、列/表属性 API | 库/表/列属性读写；默认值/校验；主外键约束、引用完整性、级联更新/删除/置空、关系方向和连接类型 | `PropertiesTest`、`PropertyMapsTest`、`RelationshipTest`、`FKEnforcerTest`、`PropertyExpressionTest` | **部分实现**：仅读取列 `Required` 并推导可空性。其余属性 API、关系和约束执行未实现。 |
| 查询定义与表达式 | `query.*`、`expr.Expression`、`EvalConfig`、`EvalContext` | 各类保存查询的读取/SQL 表达；表达式解析与计算、内置函数、区域/数字/日期配置、错误传播 | `QueryTest`、`ExpressionatorTest`、`DefaultFunctionsTest`、`NumberFormatterTest`、`FormatUtilTest`、`CalcFieldTest` | **未实现**：当前 CLI `canonical` 是诊断导出，不是 Access 查询引擎。 |
| 复杂列、OLE 与长值 | `complex.*`、`util.OleBlob` | 附件、多值、历史记录、OLE 封装/流式读取；跨页长值读写、所有权和关闭行为 | `ComplexColumnTest`、`AttachmentColumnInfoImplTest`、`MultiValueColumnPropertyMapTest`、`LongValueTest`、`OleUtilTest`、`OleBlobTest` | **部分实现**：Memo/OLE 可读原始负载；无索引本地 Jet4 V2000 表支持写入 LVAL 单页/链页并更新列级内联 Usage Map。固定 Jackcess 5.2.0 已回读 20,000 字符 Memo 和 8,192 字节 OLE 更新 fixture，以及 Unicode、空值和 NULL。旧 LVAL 页回收、Usage Map 扩展、附件、多值/历史记录、OLE 包装和完整流接口未实现。
| 错误处理和扩展点 | `JackcessException`、`ConstraintViolationException`、`ErrorHandler`、`ColumnValidator`、`ColumnMatcher`、`LinkResolver` | 公开错误类型/上下文、可恢复错误、校验器/匹配器替换、链接解析、调用顺序和失败传播 | `ErrorHandlerTest`、`ColumnValidatorTest`、`CustomLinkResolverTest`、`LinkResolverTest` | **部分实现**：`DatabaseError` 和诊断可带文件/页/表/行/列上下文；Jackcess 错误分类及各扩展 hook 未实现。 |
| 导入导出、通道和帮助工具 | `ImportUtil`、`ExportUtil`、filters、`ReadOnlyFileChannel`、`MemFileChannel`、格式化与 OLE helpers | CSV/对象导入导出、过滤器、文件通道、OLE 构造与读写、辅助格式和异常行为 | `ImportTest`、`ExportTest`、`ImportExportUtilTest`、`FileChannelTest`、`MemFileChannelTest`、`OleBlobBuilderTest`、`ColumnFormatterTest` | **未实现**：目前只有诊断 CLI `inspect`/`canonical` 和独立验证探针，不等同于 Jackcess 工具能力。 |

## P0 抽查记录：建库、建表与行修改

以下测试方法已从固定提交 `6159c680933625ffa952334b5e0e6b24f37bfdfe` 的源码逐项查看。这里只记录一个与当前 C++ 实现直接相关的切片，不代表整张矩阵已完成审阅或冻结。源码路径均相对于 Jackcess 仓库根目录 `src/test/java/io/github/spannm/jackcess/`。

| 能力 | Jackcess 5.2.0 测试方法 | 从测试确认的预期 | 当前 C++ 对照与差距 |
|---|---|---|---|
| 新库创建 | `DatabaseTest.create`（179–185，`@FileFormatSource`）；`BuilderTest.databaseBuilderCreateStatic`（204–210） | 新建库初始没有用户表；静态 create 调用 V2000 重载并检查返回格式属于 `FileFormat` 枚举。前者按 `FileFormatSource` 参数化，具体格式由该 source provider 决定 | C++ 能用固定模板创建空 V2000/V2003，并通过独立 Java 检查；目前没有逐个 Jackcess `FileFormatSource` 格式对照，不能外推到其他格式 |
| 表定义校验 | `DatabaseTest.invalidTableDefs`（88–116，`@FileFormatSource`） | 拒绝无列表、大小写不敏感的重名列/表、不合法列长度和过长列名；有效单列定义可创建 | C++ 建表限制为 ASCII 名称及 BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/GUID/TEXT/BINARY/MEMO/OLE；拒绝名称超出子集，但 Jackcess 的标识符转义和完整长度规则尚未对齐 |
| 构建器语义 | `BuilderTest.tableBuilderEscaping`（36–51）、`tableBuilderCollections`（53–72）、`databaseBuilderProperties`（178–202）；`ColumnBuilderTest` 的 `maxPrecisionAndScale`、`maxLength`、`flags`、`properties`、`validate`、`validateCalculated`（31–204） | 覆盖保留字/标识符转义、批量列和索引添加、数据库属性、列长度/精度/scale、压缩 Unicode、超链接、AutoNumber、列属性及格式相关定义校验 | 当前 C++ builder 只有表名与列名/类型，未提供索引、数据库/列属性、精度、scale、AutoNumber、计算列或标识符转义；这些是完整 API 对齐缺口 |
| 行追加和批量追加 | `impl/DatabaseReadWriteTest.writeAndRead`（49–90）、`writeAndReadInMem`（57–63）、`writeAndReadInBatch`（92–117，均为 `@FileFormatSource` 参数化） | 写入单行循环与 1,000 行批次后逐字段回读；样例含 NULL，并比较各列值 | C++ 仅支持 Jet4 V2000 无索引本地表和受限类型；本地批次/Jackcess 对照记录见[工作包 P2](01-实施工作包.md)，不能覆盖测试 source 中的其他格式或类型 |
| 行更新 | `impl/DatabaseReadWriteTest.updateRow`（119–220，`@FileFormatSource`） | 支持 `KEEP_VALUE`、AutoNumber 语义、变长文本更新、当前行更新、Row 对象更新后回读；测试包含更长的新文本。[本机参考探针](04-P0基线核验记录.md#2026-11-20已有-v2000-库变长行与索引探针)确认 V2000 变长行更新会写 overflow 数据行并保留逻辑表头 RowId | C++ 已支持 Jet4 V2000 无索引本地表等长原位更新、增长行 overflow 迁移、缩短行压实并保留逻辑 RowId；Memo/OLE 更新会分配新的 LVAL 页链。公开 Cursor/Row 更新接口、旧长值页回收、索引表更新和完整格式矩阵仍缺失；CTest 与固定 Jackcess 5.2.0 回读证据见[基线记录](04-P0基线核验记录.md)。
| 行删除 | `DatabaseTest.deleteCurrentRow`（187–250）、`deleteRow`（252–282，均为 `@FileFormatSource` 参数化） | 覆盖按游标删除、按 Row 删除、行数和剩余顺序、删除后再插入，以及重复删除后数据不变。本机参考探针还确认一次索引 TEXT 键变更、索引表删除和重开后的顺序 | C++ 仅支持 Jet4 V2000 无索引表的 RowId 删除；无游标删除或索引维护接口。参考探针只证明固定 Jackcess 5.2.0 在一个 V2000 副本上的行为，不代表 C++ 已实现，也不覆盖全部格式 |

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
| 复杂列关系发现 | `ComplexColumnTest.complexInfo_flatTable_usesFlaggedForeignKeyColumn`（358–386）、`complexInfo_typeTableName_determinesType`（394–402） | V2007/V2010 样本均有版本历史、多值和附件三列；各自的 flat table 含 AutoNumber 主键及标记的复杂值外键。复杂列类型由关联表名识别。本机读探针计数相同 | C++ 不识别复杂列、关联表或复杂值外键；样本与哈希见[基线记录](04-P0基线核验记录.md#2026-11-20-ace-复杂列样本与键关系) |
| 附件、多值和版本历史 | `ComplexColumnTest.versions`（68–183）、`attachments`（187–278）、`multiValues`（282–350）；`AttachmentColumnInfoImplTest`；`impl/complex/MultiValueColumnPropertyMapTest` | 测试覆盖关联值读取/新增/更新/删除/清空、附件普通与编码载荷、文件元数据、多值属性保存，以及版本追加；已有版本项的修改/删除按 API 约定拒绝。本机只读样本分别计数为 5/6/3 个值 | C++ 尚无复杂列解码与写入；本机探针只读了 V2007/V2010 样本，未重放复杂值写入或文件/属性保存流程 |
| 未知复杂类型 | `ComplexColumnTest.unsupported`（407–439，`UNSUPPORTED_FIELDS`） | 固定版本将 `UnknownComplex` 归为 `UNSUPPORTED`，可遍历原始值；附件/多值/版本专用访问会拒绝。本机 V2007 样本关联值数为 1、2、0 | C++ 尚未定义可保留的未知复杂值表示或明确拒绝行为 |

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
| 属性映射和值 | `PropertiesTest.propertyMaps/inferTypes/readProperties/parseProperties/writeProperties/modifyProperties`（48–312）；`impl/PropertyMapsTest.read_flaggedProperty_keepsWholeFlagByte/write_flaggedProperty_keepsWholeFlagByte/put_newProperty_getsDdlBitOnly/write_sameNameColumnAndIndexBlock_keepsBothSeparate` | 默认映射与具名映射分开；属性名不区分大小写；按值推导类型；读取库/表/列/摘要/用户属性；序列化往返保留旗标和同名但不同块；未显式 `save()` 的修改不持久，保存后重开可读到更新/删除结果 | C++ 当前仅读取 `Required` 并推导 nullable；库/表/列属性映射、类型转换、持久化和保存语义未实现 |
| 关系读取与过滤 | `RelationshipTest.twoTables/oneTable/noTables`（36–110） | 能按两个表、单表或全库枚举关系；保留 from/to 表及列顺序、名称、引用完整性、级联标记；同一表作为两端会抛 `IllegalArgumentException` | C++ 仅读取部分索引元数据，没有关系发现 API、关系构建或外键/级联执行 |
| 文件链接和链接解析 | `LinkedTableTest.linkedTables`（36–144） | 无 resolver 时访问文件链接可抛 `AccessDeniedException`；元数据仍报告链接名称、目标表、数据库路径和类型。配置 resolver 后可读取目标表，并能创建新的 linked table；遍历器分别支持常规表、仅本地用户表和系统表筛选 | C++ 当前把链接表记录为诊断并跳过 schema；无 `TableMetaData` 完整对象分类、resolver、linked database 生命周期或遍历筛选 |
| ODBC 链接表 | `LinkedTableTest.odbcLinkedTables`（146–171） | 读取目标表名、连接串、20 列和 5 个索引；`toString()` 不泄漏连接串中的密码；迭代和写入明确抛 `UnsupportedOperationException` | C++ 尚不公开 ODBC 链接元数据，也没有凭据脱敏或按操作拒绝的对应接口 |
| 保存查询 | `query/QueryTest.readQueries`（169–222）、`badQueries`（148–167）；查询类型构造覆盖 `unionQuery/passthroughQuery/dataDefinitionQuery/updateQuery/selectQuery/appendQuery` | 从查询对象目录解析 SELECT、DELETE、APPEND、UPDATE、MAKE TABLE、CROSSTAB、UNION、PASSTHROUGH 和 DATA DEFINITION；SQL 输出按解析出的字段、JOIN、条件、排序和参数组合；未知或结构不完整查询按操作抛明确异常 | C++ 尚无 query 目录/API、SQL 表达或错误分类；CLI `canonical` 不能代替查询解释 |

以上测试分别依赖 `COMMON1`、`INDEX`、`LINKED`、`LINKED_ODBC`、`QUERY` 语料或合成对象。后续登记需核对这些测试 source 实际返回的格式、样本哈希和逐项预期；目前不以测试类名或源码检查代替当前 C++ 的格式回归与互操作证据。

`impl` 包中的 `CodecProvider`/`CodecHandler` 虽位于实现命名空间，仍是 Jackcess 的扩展接口，应纳入能力范围；不得因为命名为 `impl` 而漏项。反之，内部类和私有行为不自动成为 C++ 公开 API，除非其影响可观察文件行为或公开扩展契约。

## 格式与操作放行矩阵

格式初始范围保留在[验收与发布门槛 §2](02-验收与发布门槛.md#2-格式矩阵)。对上表每个适用能力，仍需把格式矩阵具体化为：

| 格式组 | 打开/读取 | 创建 | 已有库修改 | 拒绝/不支持行为 |
|---|---|---|---|---|
| Jet3 V1997 | 待从 5.2.0 源码和用例逐项核对 | 计划为拒绝，需固定拒绝 API、错误和文件状态 | 计划只读，需验证每类修改入口拒绝 | 待建负例与原文件哈希断言 |
| Generic Jet4 | 待核实 | 计划为拒绝 | 计划支持，需区分允许的改动 | 待从对应 fixture/test 映射 |
| Jet4 V2000/V2003 | V2000 有限样本试验，其他行为待矩阵 | **部分实现**：可新建空 V2000/V2003 库；仅 V2000 支持创建 BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/GUID/TEXT/BINARY/MEMO/OLE 列空表 | **部分实现**：V2000 无索引本地表支持数值、MONEY、GUID、TEXT/BINARY 与 Memo/OLE 长值追加和更新、增长行 overflow 迁移、缩短行压实及行删除；MONEY 校验四位小数精度和 Jet Currency 的有符号 64 位边界，长值使用 LVAL 页链和列级 Usage Map，旧长值页不回收；索引维护和映射扩展未实现 | 部分验证；格式读写矩阵与已有库修改未验收 |
| ACE V2007/V2010/V2016/V2019 | 头识别已实现；schema/值/复杂列逐版待核实 | 未实现 | 未实现 | 未完成 |
| MSISAM/有编码 Jet/ACE | 头/编码识别和拒绝边界待核实 | 待核实 | codec 接口和默认拒绝待核实 | 待补独立样本与自定义 codec 用例 |

上述“计划为拒绝”来自现行项目门槛，不是已确认的 Jackcess 5.2.0 行为；冻结前须检查对应 `FileFormat`、Builder 和测试。

## P0 难点和证据缺口

| 难点 | 现有证据 | 尚缺验证 | 状态 |
|---|---|---|---|
| 索引排序键、复合键、分裂与查询顺序 | 仓库 `tools/validation/` 在固定 Jet4 样本上比较 138 个用户索引的完整 RowId 序列，结果一致；已有合成多层 B-tree/叶链测试；固定 Jackcess 5.2.0 的 V2000 BIG_INDEX 副本插入 1,000 行后重开，根页 level=2、90 个叶页、8 个 node 页且游标升序 | 多格式排序规则、复合键、升降序/NULL/重复键/长文本、删除后重开、精确查找和范围边界；C++ 写入导致的页分裂 | 参考行为已有单格式试验；C++ 只读扫描，不能放行索引能力 |
| 已有库更新、迁移行和长值变更 | 有限 C++ writer 支持 Jet4 V2000 无索引本地表的数值/GUID/TEXT/BINARY/Memo/OLE 行追加、等长更新、增长行 overflow 更新、缩短行压实和删除；Memo/OLE 长值使用 LVAL 单页/链页并更新列级 Usage Map。固定 Jackcess 5.2.0 已重开校验 20,000 字符 Memo 与 8,192 字节 OLE 的更新 fixture | 继续验证已有库副本、非空/引用式列 Usage Map、边界值和 I/O 失败状态；实现旧长值页回收与 Usage Map 扩展，覆盖 V2003 拒绝和索引维护，并由固定 Jackcess 独立回读。C++ 合成 fixture 和命令/哈希见[基线记录](04-P0基线核验记录.md) | 本轮验证限合成 V2000 无索引表；旧 LVAL 页回收、已有索引库写入和跨格式验证仍未完成 |
| ACE 附件、多值、版本历史等复杂列 | 固定 Jackcess 测试源码覆盖 V2007/V2010 复杂列 CRUD 和 V2007 未知类型；只读探针对两个版本确认复杂表主外键关系及版本历史/多值/附件计数 | 复放固定源码 CRUD 预期、核对 ACE 其他版本与删除/失败状态，设计 C++ 未知类型保留策略并建立产品/GDAL 对照 | 参考样本审计部分完成；C++ 解码/写入未开始 |
| 原生目标与估算 | 当前仓库有本机 CMake/CTest 结果；计划存有旧的总体估算范围 | 原生 Linux x86_64/ARM64 环境、Access/ArcGIS 目标版本、性能门槛、逐包工量和关键路径 | 未核实；旧总体估算不作为本版分包估算 |

## 冻结条件

本矩阵目前只完成 API/测试入口初盘。冻结 P0 还要：审阅上表引用的 5.2.0 测试；把每类公开 overload、错误/拒绝语义和适用格式映射到可复现用例；确认样本和工具哈希；补齐三个难点的实验记录；核实原生环境、许可与资源门槛；据此修订逐包估算和关键路径。冻结后新增能力按同样结构补记录，不覆盖历史结果。
