# rea-jackcess

[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

`rea-jackcess` 正在用 C++17 重写 Java [Jackcess](https://github.com/spannm/jackcess)，目标是原生 Linux x86_64 和 ARM64 核心库，按固定基线 Jackcess 5.2.0 对齐 Access MDB/ACCDB 的公开能力和可观察行为。首个里程碑是 Jet4 V2000/V2003 与 ACE V2007/V2010/V2016/V2019 普通库的只读验收。

## 当前状态

目前已实现 Jet/ACE 版本头识别和只读页访问；Jet4 可读取普通本地表，固定 ACE V2007/V2010 样本可读取普通表 schema 与行，ACE 复杂列 ID、概念表/类型表/flat table 关系、AutoNumber 主键和外键列名已映射到 `ColumnInfo`，并按固定 5.2.0 规则分类附件、多值、版本历史和 unsupported；`Table::raw_complex_values()` 可按父行 `RowId` 读取 flat-table 原始行，覆盖附件、多值、版本历史和 `UnknownComplex` 样本。已新增只读类型化复杂值读取，支持多值、附件、版本历史和 unsupported；附件保留 OLE wrapper 字节并解出文件载荷，unsupported 字段保留原始编码字节。复杂列写入和完整 OLE 流接口仍未实现。基础字段解码覆盖布尔值、整数、浮点、日期、Money/Numeric、文本、GUID、Binary、Memo/OLE 长值；Memo/OLE 保留原始负载字节，无法表示的短日期 serial 输出为 `$dateOutOfRange`。加密库的只读访问通过 `DatabaseOptions::page_codec` 接入 `PageCodec`：只解码页 1 及以后，页 0 保持原样，缺处理器时以 `unsupported_operation` 拒绝而不是返回乱码，真实加密样本尚未对照。`DatabaseError` 可携带文件、页、表、行和列信息；`ErrorHandler` 可在 Database/Table/RowCursor 级对列解码异常返回替代值或重抛。公开 API 提供只读或显式读写 `Database::open`、空 V2000/V2003 数据库创建，以及通过 `TableBuilder` 创建 Jet4 V2000/V2003 BOOLEAN、BYTE、SHORT、LONG、SINGLE、DOUBLE、MONEY、GUID、TEXT、BINARY、MEMO、OLE 列表和索引，完整字段类型和结构修改仍待补齐。

GUID 写入使用 `GuidValue{"00112233-4455-6677-8899-aabbccddeeff"}` 一类规范 8-4-4-4-12 十六进制文本，外侧大括号可省略；读取结果统一为小写且带大括号。BOOLEAN 列使用 `bool` 行值：TRUE/FALSE 存于行尾 NULL mask 位，列定义 `size` 为 1 但定长数据区不占字节；Jet4 无法区分 NULL 与 FALSE，写入 NULL 后读回为 FALSE。

固定 ACE V2010 `calcFieldV2010` 样本还覆盖了 11 个计算列的 `ResultType` 逻辑类型映射及四行已存 Boolean、TEXT、MEMO、LONG、MONEY、FLOAT 和 NUMERIC 结果读取；固定版独立检查器逐行比较结果。计算表达式尚未求值，其他 ACE 版本的计算列行为也未由此样本验证。

当前 P1/P2 仍在开发中，不能视为阶段或 Jackcess 兼容性验收完成。索引读取已能从 B-tree 定位最左叶页并沿叶页链返回行；建表可声明单列升序/降序非唯一/唯一索引，插入、更新与删除会维护其条目和去重键计数；单列 TEXT 索引支持 General Legacy（1033/0）、General（1033/1）和 General 97（1033/-1），包括建表、维护和按键查找；固定 Jackcess 5.2.0 对俄语、土耳其语和乌克兰语（LCID 1049/1055/1058，version 0）回退到 General Legacy 键编码，C++ 与之保持一致。固定 Jackcess 5.2.0 的俄语/土耳其语/乌克兰语描述符也委托 General Legacy，其他 LCID 默认将索引标为只读；C++ 覆盖六种基线描述符并拒绝不支持的排序写入；MEMO 列索引沿用相同文本排序编码，支持建表、回填、增删改维护、查找和升降序扫描；文本键先截到 255 个 UTF-16 单元，完整索引键超过 510 字节时按 Jackcess 5.2.0 计算 CRC-16 摘要。索引页按共享键前缀计算容量并写入压缩条目。超出容量的叶页会分裂，根叶页升级为节点页；节点写满后继续向上分裂并提升根层，已通过三层索引树回归。删除使叶页低于半页时，与同一父节点下放得下的兄弟叶页合并并释放源页，避免长期删改后碎片化。`Table::find_row_by_index` 可按索引键精确取行，`rows_by_index_from` 从首个 ≥ 给定键的条目开始扫描，`rows_by_index_range` 扫描 `[start, stop)`；`rows_by_index_reverse` 与 `rows_by_index_range_reverse` 按相同区间从最大键向最小键反向扫描；五者共用与写入一致的键编码，因此可直接定位节点页下的目标叶页。`IndexBuilder` 支持声明最多 10 列的复合索引及其逐列方向，也支持 `with_required()`、`with_ignore_nulls()` 和 `with_primary_key()`。单列和复合索引均可随行维护。复合索引支持完整键精确查找、起始扫描、正反向范围及前导列前缀扫描。节点页合并和 Jackcess 可选的 `writeBrokenIndex` 写入策略仍待实现。BOOLEAN/BYTE/SHORT/LONG/MONEY/SINGLE/DOUBLE/GUID/BINARY 键已支持升序或降序；BINARY 区分 NULL 与空字节串，并按 8 字节分段编码。列 `Required` 属性已用于推导公开 `ColumnInfo::nullable`；`Database::create(path, format, tables)` 可在 V2000/V2003 新库中创建一个或多个 BOOLEAN、BYTE、SHORT、LONG、SINGLE、DOUBLE、MONEY、SHORT_DATE_TIME、GUID、TEXT、BINARY、MEMO、OLE 列空表，并更新 `MSysObjects` 的目录行、`Id`/`ParentIdName` 索引和全局 Usage Map，已通过 C++ 重开和固定 Jackcess 5.2.0 独立打开检查。C++ 行写入需显式以 `read_write` 打开，目前覆盖本地 Jet4 V2000/V2003 表（带单列升序/降序索引时同步维护索引）的 BOOLEAN/BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/GUID/TEXT/BINARY/MEMO/OLE；MONEY 接受可精确缩放为四位小数的 `DecimalValue`，并检查 Jet Currency 的有符号 64 位边界；写入门禁按 `MSysDb.AccessVersion` 识别 `08.50`（V2000）与 `09.50`（V2003）；第三方驱动（如 ArcGIS）生成的 Jet4 文件可能根本没有该属性，此时以文件头判定的 Jet4 版式为准，只有已知但陌生的标记才拒绝；批量追加和更新先检查每一行能否放进单个数据页。Memo/OLE 数据写入外部 LVAL 页，长值超过单页容量时使用链式页；Memo 可从 UTF-8 文本编码或直接提供原始字节，OLE 使用原始二进制载荷。C++ 回归覆盖单列升序索引建表（定义块/索引名/使用图/空根叶页、未知列与 OLE 列拒绝）、插入时的索引键维护与升序读回、更新与删除时的条目移除及去重键计数、行增长/缩短后条目仍指向原 RowId、GUID 索引键的存储字节序与更新/删除、索引清空后重建、删除低端键触发叶页摘除并释放页面后索引仍有序且可再增长、700 键批量的叶页分裂与多叶维护、多层树上的精确查找/起始扫描/区间扫描与 NULL、重复键、BOOLEAN/SHORT/DOUBLE 键与 NULL/重复键排序、MONEY 键的十进制缩放与正负排序、BINARY 的分段边界、空字节串、跨页重复键和混合长度分裂、跨页批量追加、MONEY 精度、指数形式和正负边界及更新、其他数值类型边界、GUID 字节序和更新、BOOLEAN 位值与 TRUE→FALSE 更新、跨 NULL mask 字节、仅 BOOLEAN 列的表、BOOLEAN 与 Memo/OLE 混排、UTF-8/UTF-16LE、空值与 NULL 区分、无效类型/编码/长度及超页批次预检、等长原位更新、变长行增长及再次迁移到新 overflow 行、overflow 数据行等长更新、inline/overflow 行缩短压实、RowId/行数稳定、Memo/OLE 多页链追加与更新、长值空闲页复用、Unicode、空值、NULL、V2003 建表与行写入、未知 AccessVersion 拒绝、行删除和重开读取。固定 Jackcess 5.2.0 已逐行回读 500 行跨页 LONG 批次和 NULL、MONEY 精度/边界/更新与 20,000 字符 Memo 及 8,192 字节 OLE 长值；Java 写入 MONEY 后 C++ canonical 也已逐行核对精确金额，BOOLEAN 亦完成双向校验：Jackcess 回读 C++ 文件的位值和尾部 LONG 位置，Java 写入 TRUE/FALSE/NULL 后由 C++ canonical 核对读回为 FALSE。其他类型由 Jackcess 核对相应字段值、GUID 字节序、TEXT/BINARY 类型、Unicode、空值与 NULL，并分别重开读取变长行增长和缩短结果。逻辑 RowId 稳定由 CTest 检查，行数及读回字段由 CTest 与 Jackcess 独立检查；也确认删除中间行后剩余值及顺序。`read_write` 句柄持有数据库文件的 OS advisory 排他锁，避免合作的进程同时获取写句柄；替换已存在且正被写入的数据库也会被拒绝，同一句柄内的操作另行串行化。全局页图支持内联图升级和引用式位图页扩容。表、索引和长值 Usage Map 也支持内联图升级和新增引用式位图页；写入要求表拥有页/空闲页 Usage Map 共用声明页且 Memo/OLE 列含可解析的列级 Usage Map。长值更新优先复用列内仍有空闲空间的 LVAL 页（与固定 5.2.0 的 `findFreeRowSpace` 一致）；更新和删除后会从列级 Usage Map 移除不再被存活行引用的 LVAL 页，并在全局 Usage Map 标记空闲，共享页保留。该写入路径目前不创建 ACE 权限行；索引维护覆盖单列升序/降序数值（含 MONEY）、布尔、GUID、BINARY 与 General Legacy/General/General 97 TEXT 键（含固定 Jackcess 5.2.0 对 LCID 1049/1055/1058 version 0 的 General Legacy 编码回退）（唯一索引拒绝重复的非 NULL 键）；插入会分裂写满的叶页（根叶页升级为节点页，节点满则向上递归分裂并提升根层，已验证三层树），删除把叶页清空时，该叶页会从叶链摘除、从父页条目中删掉并释放页面（索引使用图与全局页图同时更新）；若根节点因此没有子页，根页退回为空叶页，等于新建索引的初始状态。更新与删除仍不追加页面，也不做条目再分配。其他字段类型、ACE 写入（V2007/V2010 固定样本的普通表、复杂列关联及类型化复杂值均只读；V2016/V2019 未验收）、节点页合并、俄语/土耳其语/乌克兰语专属 TEXT 权重、其他 LCID、表与索引删除和查询执行仍待实施。外部非合作写入者、单批页写入失败时尝试恢复覆盖页的原始内容并截去新增页。回滚失败后当前句柄拒绝后续写入。级联和关系创建的多个写盘批次共用内存回滚记录。进程崩溃恢复由 `<库>.reajournal` 写盘日志覆盖，读写打开时回放日志；本机制不是数据库事务。`src/page/` 的内部 Jet4 原始页写入器支持既有文件的整页覆盖、末尾追加和显式刷新。尚未冻结的 P0 行为矩阵及上游同步 CI 也仍待实施。复杂列或无法识别的字段类型会保留为不支持值并记录在 `errors` 中。

已有的本机只读样本 `test_data_only_read/test_mdb.mdb`（SHA-256 `0bc0acb9c732de1e17c43c2d0a6019f7b5a62ccf83c5818a1bc42db7dbc460c6`）曾用于有限对照：系统目录 51 行与 MDBTools 1.0.1 导出在 5 个字段上匹配；Jackcess 5.2.0 canonical 对照的 43 张共同用户表无 schema 或行值差异；138 个用户逻辑索引的逐索引 `page:row` 遍历序列与 Jackcess 完全一致。该样本上的 BOOLEAN 列（`GDB_GeomColumns.HasZ/HasM`、`MSysACEs.FInheritable`）现作为可选 CTest `real_sample` 的读取与样本副本写入基准：样本存在时注册并校验固定哈希，缺失时该测试不进入套件。该样本不属于仓库测试资产，这些结果也不替代本仓库回归或产品验收。

当前先收口 Jet4 读取，再逐版本验收 ACE 读取。PGeo 的 PG-01 已实现：独立适配层可枚举 Feature Class 并逐要素返回 OBJECTID、类型化属性、原始 SHAPE 字节及可用的 SRS 文本；两种目录布局、固定样本、本地真实库和 GDAL v3.10.0 公开语料均已按 OBJECTID 与固定 Jackcess 5.2.0 对照。GDAL 几何解码、样本克隆和产品导入验收不在本次范围。只读路线图已按缩减范围发布到 20/21，剩余 R3.1 仅依赖真实 AR-10 语料（独立 ACE V2016/V2019 已取得并与固定 JAR 对照）；证据、缺口与发布承诺见[只读发布记录](plan/Jackcess-C++重写/17-只读发布记录.md)和[可读特性与已知差异](plan/Jackcess-C++重写/16-可读特性与已知差异.md)。详见[只读优先实施计划](plan/Jackcess-C++重写/09-只读优先实施计划.md)、[只读优先开发交接](plan/Jackcess-C++重写/10-只读优先开发交接.md)和[PG-01 设计](plan/Jackcess-C++重写/13-PGeo原始载荷读取设计.md)。

`Database::relationships()` 读取 Jet4 本地表间关系，包括按序的列映射、参照完整性、级联标志和联接类型。已有的本地外键索引可在落盘前校验受支持的键（含复合键，覆盖 TEXT/BINARY/GUID 列），防止孤儿记录和未声明级联时仍有依赖行的主端改键或删除，违规时抛 `DatabaseErrorCode::constraint_violation` 且不改动文件。声明了级联的关系在父行落盘后继续执行：级联更新改写依赖行的外键列，级联删除删除依赖行，删除置空把外键列写成 NULL；级联沿用同一写入路径，因此会继续向孙表级联，深度上限 16 层，操作失败时尝试恢复所有已写入页。同一物理索引同时被主键和外键描述时只重写一次。回归样本在 `tests/fixtures/`，登记为 CTest `foreign_key_v2000` 和 `foreign_key_v2003`。`RelationshipBuilder` 加 `Database::create_relationship()` 可在读写句柄上新建关系：它向两端表的定义页各追加一条逻辑索引描述（子表外键、父表关系别名；同表关系在一个定义流中原子追加两条），再向 `MSysRelationships` 写入按列对拆分的目录行；不声明引用完整性的关系只写目录行。声明引用完整性时会在写盘前检查已有子行，外键全为 NULL 的行跳过检查，其余行须有匹配的父键，否则以 `constraint_violation` 拒绝。当前要求父表在参照列上已有唯一索引，可复用子表外键列上的升序索引，或自动新建并回填。关系名为 ASCII，跨页表定义可继续修改；非级联自引用可创建，级联自引用会作为级联环拒绝，与固定 5.2.0 测试预期一致。创建关系前会拒绝传递级联成环；缺表/列、非法 ASCII 标识符、类型不匹配、缺父端唯一索引和无引用完整性却启用级联按 `std::invalid_argument` 报错；非 ASCII 关系名暂报 `unsupported_operation`。级联删除和删除置空标志可共存；无引用完整性的关系不受十列索引上限限制，启用引用完整性时最多十列。空名称由表名生成，重名时自动追加数字，外键索引使用最终关系名。写盘失败时尝试恢复关系目录、表定义和索引页。

`Database::create` 建表时若定义超过一页（列或索引很多），会直接写出续页链而不是拒绝。读写句柄上的 `Table::add_index(IndexBuilder)` 可为既有 Jet4 本地表建索引并回填已有行。它支持单列和复合键、各列排序方向以及唯一、必填、忽略全 NULL 键和主键选项。相同物理定义可共享索引树。表定义超过一页时会按续页链重组（`definition_size`、名字区与续页头同步改写，新增续页登记进全局页图），因此宽表也能继续加索引；ASCII 索引名及现有 Jet4 写入器支持的键类型不变。索引构建和约束检查先在内存完成，单批页写入出错时尝试恢复写入前的页；进程中断留下的半写批次由写盘日志在下次读写打开时回放。

Jet4 的 `Database::property_maps()` 和 `Table::property_maps()` 可读取已存储的属性块，包括默认块及具名列和索引块。返回块类型、属性类型、完整标志字节、原始值和可解码的类型化值。读写句柄的 `Database::save_property_maps()` 可显式保存库属性的原始记录。较大的属性块通过 Jet4 长值页保存。本地用户表可通过 `Table::save_property_maps()` 保存原始记录，列 `Required` 更新即时生效。`PropertyMapInfo` 提供大小写不敏感的 `find()`/`get_value()`、带缺省值读取、按值推导类型或显式指定 Jet 类型的 `put()`，以及保留完整 flags 的 `put_all()`。自动推导遵循固定 Jackcess 5.2.0 规则，覆盖 21 个内建属性和常见值类型；`PropertyInfo::set_value()` 可编码受支持的标量、文本、二进制、GUID 与 SHORT_DATE_TIME。null 属性值保留在内存映射中，但保存时省略值记录。Numeric 等未支持类型的推导和完整属性语义仍待实现。

Jet4 行追加可应用列的 `DefaultValue`。当前支持整数、布尔值、带引号的文本和 NULL 字面量。添加行时，省略值和显式 NULL 都会触发默认值，显式非 NULL 值优先。需要执行但尚不支持的表达式会在写盘前报错。列 `ValidationRule` 支持整数比较和 `IS NULL` / `IS NOT NULL`，在追加和更新前执行。

`DatabaseFile::file_format()` 和 `Database::file_format()` 提供 Jackcess 风格的格式分类。Jet4 额外读取库属性 `AccessVersion`，区分 V2000、V2003 和通用 Jet4。Jet3、ACE、MSISAM 的分类来自文件头；ACE V2007/V2010 固定样本已覆盖普通表只读、复杂列关联/类别和按父行读取 flat-table 原始值，多值/附件/版本历史/unsupported 复杂值已支持只读类型化读取，附件 OLE wrapper 与文件载荷已解码；V2019 固定样本覆盖扩展日期列及 DateNormalCalc 计算列已存结果的投影读取；后者仅解包存储值，不执行公式。canonical 可读取该样本全部表（含 Table1 九行），并与固定 Jackcess 5.2.0 转储逐表一致。复杂值写入仍不支持；独立来源 V2016/V2019 均已与固定 JAR 逐表对照（V2019 覆盖 BIGINT 与标准 EXT_DATE_TIME，非标准“第二形态”明确拒绝）。

Jet4 的 `Database::linked_tables()` 枚举系统目录中的文件链接和 ODBC 链接，返回已存储的目标信息。`Database::open_linked_table()` 可通过调用方显式提供的路径 resolver 逐跳解析文件链接，也可通过自定义 resolver 按目标源创建可保活的 `CustomLinkedSource`，在首次请求每张表时加载其 schema 和行快照。源对象按外部源缓存，表快照分别写入临时 Jet4 V2000 数据库并按表缓存；返回的 `Table` 继承链接源数据库的只读或读写模式，并在源句柄关闭后仍持有目标状态；最后一个相关句柄释放后销毁源对象并删除临时文件。自定义 resolver 返回空源时，文件链接回退到路径 resolver；已识别的自定义源若没有请求表则返回空结果，不执行路径回退。快照受现有 Jet4 `TableBuilder` 列类型、ASCII 表/列名及行写入限制约束。两种解析均拒绝循环。读写句柄的 `Database::create_linked_table()` 可登记文件链接，写入目录、索引及继承自 Tables 容器 SID 的访问控制条目。ODBC 链接也可把目录中的目标信息交给自定义 resolver；合成 Jet4 fixture 覆盖 V2000/V2003 元数据传递、文件 resolver 不回退、快照读取及缓存。不读取 ODBC 本地 schema/索引元数据，不提供内建 ODBC 驱动或向 ODBC 数据源写回。

`Database::queries()` 可读取 Jet4 保存查询的名称、类型和原始查询目录行。`QueryInfo::to_sql()` 支持九种已识别的查询类型，包括 SELECT、操作查询、交叉表、DDL、Pass-through 和 UNION。查询执行仍待实现。

`<rea_jackcess/csv.hpp>` 提供表级 CSV 导入导出：`export_table_csv()` 按固定 Jackcess 5.2.0 `ExportUtil` 的文本形式写出（空字段表示 NULL、BOOLEAN 为 true/false、MONEY 四位小数、浮点按 Java 规则、GUID 带花括号大写、BINARY/OLE 为大写十六进制并按 4 字节分组），并可通过 `CsvOptions::columns` 按指定顺序选择列、通过 `row_filter` 跳过行（谓词接收投影后的行）；`import_table_csv()` 支持表头名称映射、引号与嵌入换行、双引号转义，并把空字段写成 NULL，也可用 `import_row_filter` 按解析出的源字段跳过记录（表头不传给谓词，类型转换前执行）。CLI 对应 `export <database> <table> <csv> [--header]` 与 `import <database> <table> <csv> [--header]`。`SHORT_DATE_TIME` 列按 Java `LocalDateTime.toString()` 的文本交换（零秒与零小数省略、小数按三位分组），与固定 `ExportUtil` 逐字节一致；复杂列仍显式拒绝；查询执行与表达式引擎按计划后置。

## 本地构建

需要 CMake 3.16 或以上、支持 C++17 的编译器以及 zlib。

```sh
cmake -S . -B build
cmake --build build
(cd build && ctest --output-on-failure)
./build/rea-jackcess --version
```

安装后可供 CMake 和 pkg-config 下游使用：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build
cmake --install build
```

```cmake
find_package(rea_jackcess CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE rea_jackcess::core)
```

PGeo 适配器对应 `rea_jackcess::pgeo`。可用 `-DREA_JACKCESS_BUILD_CLI=OFF`、
`-DREA_JACKCESS_BUILD_PGEO=OFF`、`-DREA_JACKCESS_INSTALL=OFF` 控制构建内容。Fuzz 目标默认不构建，
见 [`fuzz/README.md`](fuzz/README.md)。

Windows 只用于本机开发验证，不在发布平台范围内。使用 MSYS2 UCRT64 的 GCC 与 zlib，并把 `C:\msys64\ucrt64\bin` 放进 `PATH`（编译器子进程依赖其中的 DLL）：

```sh
cmake -S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/ninja.exe \
    -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe \
    -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe
cmake --build build
(cd build && ctest --output-on-failure)
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
| `src/index/` | Jet4 B-tree 页遍历；支持沿叶链顺序读取行位置，也支持按编码键从根下降定位首个 ≥ 目标键的叶页条目，并可带开区间上界。索引写入与页分裂在 `write` 包实现。 |
| `src/expression/` | 默认值、校验、计算列和查询表达式；待实现。 |
| `src/query/` | 行过滤、组合游标和公开查询流程；待实现。 |
| `src/write/` | 数据库创建和后续表/行写入；当前可从随核心库嵌入的 V2000/V2003 官方模板创建空库，也可写入第三方驱动创建、`MSysDb` 无 `AccessVersion` 的 Jet4 文件（真实 ArcGIS 样本副本已验证插入与删除），支持 Jet4 V2000/V2003 BOOLEAN/BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/GUID/TEXT/BINARY/MEMO/OLE 列空表 schema 与单列升序/降序非唯一/唯一索引定义、索引名和空根叶页，并在行插入/更新/删除时按 Jet4 键编码维护这些索引的条目与去重键计数，维护系统目录和两个系统索引，并在同目录暂存、C++ 回读校验后发布；行写入门禁按 `MSysDb.AccessVersion` 接受 V2000 `08.50` 与 V2003 `09.50`，可向无索引本地表追加这些类型，MONEY 按四位小数缩放到有符号 64 位整数，Memo/OLE 使用列级 Usage Map 和 LVAL 单页/链页；支持等长原位更新、将增长行写入 overflow 数据行、压实缩短行和标记删除。更新或删除后会回收不再被存活行引用的 LVAL 页，共享页保留；页内孤儿行空间不压实，文件大小不会因此缩小。其他字段类型、通用/扩展页分配、ACL 和事务恢复待实现。 |
| `src/util/` | 与具体格式无关的内部帮助代码；当前提供 SHA-256 文件摘要和供读写模块共用的小端整数编解码。 |
| `tools/cli/` | 诊断和互操作对照工具；当前提供帮助、版本、Jet4 `inspect` 摘要和含行值的 `canonical` JSON v1。 |
| `tools/validation/` | 手动互操作探针；`compare_all_samples.sh` 一键把 `canonical` 与固定 JAR 转储逐表对照；当前提供索引行序列对照、空库回读、固定及变长列空表检查，以及 Jackcess 5.2.0 对跨页批量写入、MONEY 精度/边界/更新、定长数值、GUID、BOOLEAN、TEXT/BINARY、Memo/OLE 多页追加和更新与长值页复用、等长更新、overflow 行增长与行缩短结果及行删除的回读，和对索引建表、索引插入/更新/删除后按键顺序、MONEY 索引金额顺序、MEMO 升降序索引增删改和长文本回读、`--index-split` 分裂后多叶索引遍历与 Jackcess 回写 700 行树，以及 seek/range 语义（真实样本与分裂树）、合并后叶页释放（`--index-merge`，含 Jackcess 追加）与 GUID 索引顺序（`--index-guid`）的回读，以及在真实样本副本上写入 `GDB_SpatialRefs` 后由 Jackcess 回读新增行与索引顺序；另有 Java 写 MONEY 与 BOOLEAN 后由 C++ canonical 读取的反向验证。 |
| `tests/` | CTest 单元与格式回归入口；当前覆盖头识别、页 I/O、Jet4 空库和支持列类型的空表创建、模板替换保护、目录/值读取、列属性/索引定义、公开只读与读写接口、MONEY 精度/范围/更新、整数/浮点边界、GUID、BOOLEAN、TEXT/BINARY 及 Memo/OLE 行追加和更新、长值多页链、等长更新、overflow 行增长/更新及行缩短压实，以及带索引表的插入、键变更更新、删除与去重键计数，TEXT/MEMO 键截断、唯一性和回填，还有 700 行分裂后的多叶索引顺序与删除/重加；ACE V2007/V2010 固定样本检查普通 schema/行只读、ComplexID 关联、flat table 键列、附件/多值/版本历史分类；ACE V2019 固定样本检查 EXT_DATE_TIME 与 DateNormalCalc 已存结果读取，所有 ACE 用例检查写打开拒绝。本机存在 `test_data_only_read/test_mdb.mdb` 时另注册 `real_sample` 和 `pgeo_sample_read`；后者检查 Feature Class 枚举、OBJECTID、SRS 和原始 SHAPE 载荷，合成 `pgeo_layer_reader` 覆盖空层、属性类型、NULL/空 BLOB 和坏目录引用。 |
| `adapters/pgeo/` | 独立目标 `rea_jackcess::pgeo`，仅依赖核心公开 API；当前支持 PGeo Feature Class 枚举、OBJECTID/属性读取、原始 SHAPE BLOB 和可用的 SRTEXT 元数据。几何解码、产品导入和样本克隆待实施。 |

C++ 以目录划分内部模块：公开接口使用 `rea_jackcess` 命名空间，内部实现使用 `rea_jackcess::detail`。现有 `api`、`format`、`page`、`catalog`、`value`、`index`、`write`、`util` 由 `src/CMakeLists.txt` 编译为内部对象包，并合并进单一 `rea_jackcess::core` 静态库。构建目标还包括 `rea_jackcess_cli`（产物名 `rea-jackcess`）、页写入/数据库创建/目录回归目标。完整包边界和依赖方向见[源码架构](src/ARCHITECTURE.md)。

### 公开接口分类

以下是目标 API 的职责边界，接口会随对应行为矩阵和 Jackcess 5.2.0 证据逐步定型，未实现的类型不会提前作为可链接 API 发布。

| API 类别 | 目标职责 |
|---|---|
| `DatabaseFile` | 当前接口：以 RAII 方式打开文件、返回格式信息和原始页。 |
| `Database` / `DatabaseBuilder` | `Database::open` 打开 Jet4 只读库或显式读写库；`Database::create` 创建 V2000/V2003 空库，或按 builder 创建 Jet4 V2000/V2003 BOOLEAN/BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/GUID/TEXT/BINARY/MEMO/OLE 列空表，并返回只读句柄。`property_maps()` 可读库属性块，读写句柄可用 `save_property_maps()` 保存原始记录。 |
| `Table` / `TableBuilder` | 当前读取表/列 schema 和逻辑索引定义；ACE `raw_complex_values(column_name, parent_row_id)` 可读取对应 flat table 的原始关联行，`complex_values()` 返回类型化的只读值和原始行；V2010 固定样本的计算列按 `ResultType` 暴露逻辑类型并读取已存结果，V2019 的 `EXT_DATE_TIME` 和 `DateNormalCalc` 已存结果可投影读取（均不执行表达式）；builder 可声明初始表名、BOOLEAN/BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/GUID/TEXT/BINARY/MEMO/OLE 列与单列升序/降序非唯一/唯一索引；显式读写库可对这些本地 Jet4 V2000/V2003 表追加这些值，MONEY 以 `DecimalValue` 精确存储四位小数，Memo/OLE 使用长值页链，并可更新行和删除行；带受支持的单列升序/降序索引时插入、键变更更新和删除都会维护条目与去重键计数；TEXT/MEMO 可使用已支持的 General Legacy、General 和 General 97 排序编码。表属性块可读写原始记录。复杂值写入和完整 OLE 流接口仍待实现。
| `RowCursor` / `Row` / `RowId` | 当前支持前进/后退、`first`/`last`、按 `RowId` 定位、保存位置、重读当前行和按列模式查找；自定义 `ColumnMatcher` 可替换比较逻辑。默认遵循 Jackcess `SimpleColumnMatcher`，支持布尔、数值、Money/Numeric、文本、Memo 与 GUID 的类型转换；唯一索引游标的文本列按 OpenJDK 21.0.12 字符映射执行 Java `String.equalsIgnoreCase` 比较。SHORT_DATE_TIME 的 ISO 日期文本转换已支持；BINARY 列支持 `BlobValue` 字节相等和 String 对象序列化匹配；其他二进制对象与复杂值转换待实现。表扫描与索引叶链扫描可指定列投影与行过滤；字段名便捷访问待实现。 |
| `Value` | 当前表示已实现类型、NULL、超范围日期、Memo/OLE 负载及不支持类型。 |
| 属性、索引与关系 API | 可读库/表属性块并由读写句柄保存原始记录；列 `Required` 会更新可空性与写入校验。关系支持读取、创建和外键级联，`Table::add_index()` 支持复合定义与回填；索引游标支持扫描、精确查找及 `[start, stop)` 范围扫描。完整属性表达式语义、Jackcess 可选的 `writeBrokenIndex` 策略与更多索引行为仍待实现。 |
| `DatabaseError` | 以稳定错误码报告失败，并携带可获得的文件、页、表、行和列上下文。 |

依赖从高层流向低层：`api` 组合 `query`、`metadata`、`index`、`catalog`、`write` 和 `value`；`query` 使用 `expression`、`index`、`metadata`、`catalog` 和 `value`；`write` 后续使用 `metadata`、`index`、`catalog`、`value`、`page` 和 `format`；`index` 使用 `metadata`、`catalog`、`value`、`page` 和 `format`；`metadata` 使用 `catalog`、`value` 和 `format`；`catalog` 使用 `value`、`page` 和 `format`；`value` 使用 `page` 和 `format`；`page` 使用 `format`。详细职责见[源码架构](src/ARCHITECTURE.md)。canonical CLI 仍直接调用内部解析器，以保留阶段和列级诊断。PGeo 适配器通过公开接口使用核心库，不进入核心库依赖图；核心库不依赖 JVM、libmdb 或 GDAL。

## 从哪里开始

- [重写计划](plan/Jackcess-C++重写/README.md)列出范围、固定版本、JAR 哈希和外部资料位置。
- [实施工作包](plan/Jackcess-C++重写/01-实施工作包.md)列出开发顺序、依赖和上游同步流程。
- [验收与发布门槛](plan/Jackcess-C++重写/02-验收与发布门槛.md)列出格式矩阵、双向回读、故障测试和发布条件。
- [Jackcess 5.2.0 能力矩阵](plan/Jackcess-C++重写/03-Jackcess-5.2.0能力矩阵.md)按行为记录 Jackcess 公开 API、REA 实现状态和具体测试证据。P0 尚未冻结。
- [详细开发计划](plan/Jackcess-C++重写/06-待办清单.md)列出工作包优先级、依赖和验收命令。
- [近期接口契约](plan/Jackcess-C++重写/08-接口契约.md)记录写入、属性、ACE 读取和链接源的签名及行为边界。
- [AI 协作指南](AGENTS.md)说明从哪里查证、如何实施和记录结果。

## 许可证与参与

本项目以 [Apache License 2.0](LICENSE) 发布，并非 Jackcess 官方项目；上游归属和第三方资源见
[`NOTICE`](NOTICE)。参与方式见 [`CONTRIBUTING.md`](CONTRIBUTING.md)；安全问题请按
[`SECURITY.md`](SECURITY.md) 私下报告，不要提交公开 issue。
