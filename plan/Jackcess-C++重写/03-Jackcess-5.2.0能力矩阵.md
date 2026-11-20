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
| 打开、创建与生命周期 | `DatabaseBuilder`、`Database`、`Database.FileFormat` | 既有库打开/只读模式；Jet/ACE 版本选择和新建；刷新、关闭、自动同步；密码、文件通道、字符集、时区、日期类型及打开选项 | `BuilderTest`、`DatabaseTest`、`DatabaseReadWriteTest`、`LocalDateTimeTest` | **部分实现**：`Database::open` 只读打开当前支持的 Jet4；文件头/页读取和关闭由 RAII 管理。创建、修改、配置项与跨格式行为未实现。 |
| 格式识别、编码和 codec | `Database.FileFormat`、`DataType`、`CodecProvider`、`CodecHandler` | Jet3、Generic Jet4、V2000/V2003、各 ACE 版本、MSISAM、有编码页的识别和逐操作拒绝；codec 调用顺序、部分页编码和错误传播 | `JetFormatTest`、`CodecHandlerTest`、`DatabaseTest` | **部分实现**：Jet/ACE 头识别；行读取目前限 Jet4。各引擎版本行为、MSISAM 和 codec 扩展未实现或未验证。 |
| 数据库目录与库级元数据 | `Database`、`TableMetaData`、`PropertyMap`、`Relationship`、`query.Query` | 枚举系统/用户/隐藏/链接表、库属性、关系、已保存查询、linked database 与 resolver；对象名称、类型和错误语义 | `DatabaseTest`、`PropertiesTest`、`PropertyMapsTest`、`RelationshipTest`、`LinkedTableTest`、`QueryTest` | **部分实现**：Jet4 系统目录与普通本地表发现、部分属性/索引定义读取。库属性、关系、查询定义、链接表和完整对象分类未实现。 |
| 表、列和结构构建 | `Table`、`TableDefinition`、`Column`、`TableBuilder`、`ColumnBuilder` | schema/属性/创建时间；列类型、大小、精度、可空性、默认值、自动编号、计算列、超链接、多值和版本列；建表、加列和名称转义/拒绝 | `TableTest`、`ColumnBuilderTest`、`BuilderTest`、`TableUpdaterTest`、`CalcFieldTest` | **部分实现**：Jet4 普通本地表 schema；列 `Required` 推导可空性；读取逻辑/物理索引定义。其他属性、结构构建与修改未实现。 |
| 行值和 RowId | `Row`、`RowId`、`DataType` | 类型化值和 NULL；各类型便捷取值；字段名匹配；行 ID 相等/排序；复杂值与字节载荷的资源生命周期 | `TableTest`、`BigIntTest`、`ExtendedDateTest`、`LongValueTest`、`ComplexColumnTest` | **部分实现**：Jet4 基础标量、日期、GUID、文本、Binary、Memo/OLE 原始字节；`Row::id()` 暴露页/行位置。Java 各便捷转换、完整复杂类型和边界错误仍待补齐。 |
| 表扫描、游标和过滤 | `Cursor`、`CursorBuilder`、`util.RowFilter`、`util.TableIterableBuilder` | 前后移动、定位 RowId、保存/恢复位置、列投影、查找/匹配、更新/删除当前行、过滤与单次/重复迭代语义 | `CursorTest`、`CursorBuilderTest`、`RowFilterTest`、`TableTest` | **部分实现**：只读单遍 `rows()`；游标导航、定位、投影、过滤和修改未实现。 |
| 索引定义、查询和维护 | `Index`、`IndexBuilder`、`IndexCursor`、`CursorBuilder` | 唯一/必需/忽略 NULL、复合键和升降序；键编码、排序规则、精确/最近查找、范围端点；页分裂、删除、重开后的可达性 | `IndexTest`、`BigIndexTest`、`IndexLevelTest`、`IndexCodesTest`、`CursorBuilderTest` | **部分实现**：Jet4 索引定义及叶链顺序扫描；一个 MDB 样本中 138 个用户逻辑索引的完整 RowId 序列与 Jackcess 5.2.0 一致。键语义、点/范围查询、其他排序规则及写维护未实现；样本结果不等于验收。 |
| 属性、约束和关系 | `PropertyMap`、`Relationship`、`RelationshipBuilder`、列/表属性 API | 库/表/列属性读写；默认值/校验；主外键约束、引用完整性、级联更新/删除/置空、关系方向和连接类型 | `PropertiesTest`、`PropertyMapsTest`、`RelationshipTest`、`FKEnforcerTest`、`PropertyExpressionTest` | **部分实现**：仅读取列 `Required` 并推导可空性。其余属性 API、关系和约束执行未实现。 |
| 查询定义与表达式 | `query.*`、`expr.Expression`、`EvalConfig`、`EvalContext` | 各类保存查询的读取/SQL 表达；表达式解析与计算、内置函数、区域/数字/日期配置、错误传播 | `QueryTest`、`ExpressionatorTest`、`DefaultFunctionsTest`、`NumberFormatterTest`、`FormatUtilTest`、`CalcFieldTest` | **未实现**：当前 CLI `canonical` 是诊断导出，不是 Access 查询引擎。 |
| 复杂列、OLE 与长值 | `complex.*`、`util.OleBlob` | 附件、多值、历史记录、OLE 封装/流式读取；跨页长值读写、所有权和关闭行为 | `ComplexColumnTest`、`AttachmentColumnInfoImplTest`、`MultiValueColumnPropertyMapTest`、`LongValueTest`、`OleUtilTest`、`OleBlobTest` | **部分实现**：Memo/OLE 长值保留原始字节；附件、多值/历史记录、OLE 包装和完整流接口未实现。 |
| 错误处理和扩展点 | `JackcessException`、`ConstraintViolationException`、`ErrorHandler`、`ColumnValidator`、`ColumnMatcher`、`LinkResolver` | 公开错误类型/上下文、可恢复错误、校验器/匹配器替换、链接解析、调用顺序和失败传播 | `ErrorHandlerTest`、`ColumnValidatorTest`、`CustomLinkResolverTest`、`LinkResolverTest` | **部分实现**：`DatabaseError` 和诊断可带文件/页/表/行/列上下文；Jackcess 错误分类及各扩展 hook 未实现。 |
| 导入导出、通道和帮助工具 | `ImportUtil`、`ExportUtil`、filters、`ReadOnlyFileChannel`、`MemFileChannel`、格式化与 OLE helpers | CSV/对象导入导出、过滤器、文件通道、OLE 构造与读写、辅助格式和异常行为 | `ImportTest`、`ExportTest`、`ImportExportUtilTest`、`FileChannelTest`、`MemFileChannelTest`、`OleBlobBuilderTest`、`ColumnFormatterTest` | **未实现**：目前只有诊断 CLI `inspect`/`canonical` 和独立验证探针，不等同于 Jackcess 工具能力。 |

`impl` 包中的 `CodecProvider`/`CodecHandler` 虽位于实现命名空间，仍是 Jackcess 的扩展接口，应纳入能力范围；不得因为命名为 `impl` 而漏项。反之，内部类和私有行为不自动成为 C++ 公开 API，除非其影响可观察文件行为或公开扩展契约。

## 格式与操作放行矩阵

格式初始范围保留在[验收与发布门槛 §2](02-验收与发布门槛.md#2-格式矩阵)。对上表每个适用能力，仍需把格式矩阵具体化为：

| 格式组 | 打开/读取 | 创建 | 已有库修改 | 拒绝/不支持行为 |
|---|---|---|---|---|
| Jet3 V1997 | 待从 5.2.0 源码和用例逐项核对 | 计划为拒绝，需固定拒绝 API、错误和文件状态 | 计划只读，需验证每类修改入口拒绝 | 待建负例与原文件哈希断言 |
| Generic Jet4 | 待核实 | 计划为拒绝 | 计划支持，需区分允许的改动 | 待从对应 fixture/test 映射 |
| Jet4 V2000/V2003 | V2000 有限样本试验，其他行为待矩阵 | 未实现 | 未实现 | 未完成 |
| ACE V2007/V2010/V2016/V2019 | 头识别已实现；schema/值/复杂列逐版待核实 | 未实现 | 未实现 | 未完成 |
| MSISAM/有编码 Jet/ACE | 头/编码识别和拒绝边界待核实 | 待核实 | codec 接口和默认拒绝待核实 | 待补独立样本与自定义 codec 用例 |

上述“计划为拒绝”来自现行项目门槛，不是已确认的 Jackcess 5.2.0 行为；冻结前须检查对应 `FileFormat`、Builder 和测试。

## P0 难点和证据缺口

| 难点 | 现有证据 | 尚缺验证 | 状态 |
|---|---|---|---|
| 索引排序键、复合键、分裂与查询顺序 | 仓库 `tools/validation/` 在固定 Jet4 样本上比较 138 个用户索引的完整 RowId 序列，结果一致；已有合成多层 B-tree/叶链测试 | 多格式排序规则、升降序/NULL/重复键/长文本、页分裂/删除、精确查找和范围边界 | 部分试验；不能放行索引能力 |
| 已有库更新、迁移行和长值变更 | 尚无 C++ writer | 在副本上覆盖普通/溢出行更新、行迁移、长值链更新、索引键变化、删除、失败后的文件状态，并由 Java 回读 | 未开始 |
| ACE 附件、多值、版本历史等复杂列 | 尚无 C++ 解码/写入实现 | 按 ACE 版本取得样本，追踪关联表/属性/删除行为，建立 GDAL/Jackcess 对照 | 未开始 |
| 原生目标与估算 | 当前仓库有本机 CMake/CTest 结果；计划存有旧的总体估算范围 | 原生 Linux x86_64/ARM64 环境、Access/ArcGIS 目标版本、性能门槛、逐包工量和关键路径 | 未核实；旧总体估算不作为本版分包估算 |

## 冻结条件

本矩阵目前只完成 API/测试入口初盘。冻结 P0 还要：审阅上表引用的 5.2.0 测试；把每类公开 overload、错误/拒绝语义和适用格式映射到可复现用例；确认样本和工具哈希；补齐三个难点的实验记录；核实原生环境、许可与资源门槛；据此修订逐包估算和关键路径。冻结后新增能力按同样结构补记录，不覆盖历史结果。
