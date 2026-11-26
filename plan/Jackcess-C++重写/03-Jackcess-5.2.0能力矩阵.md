# Jackcess 5.2.0 能力矩阵

## 状态与证据

Jackcess 标签 `jackcess-5.2.0`，源码提交 `6159c680933625ffa952334b5e0e6b24f37bfdfe`，发布 JAR SHA-256 `9451d6f336cfaa82f5af178d5f6668b933cda4f27e261f29a1faa83630f95775`。REA 盘点快照为 `81afd3d`。P0 工作包矩阵尚未冻结，P1/P2 尚未放行。

- **实现**使用已完成、部分实现、未实现、待核实。**验证**使用通过、失败、未运行。两列同时为「已完成」和「通过」时，只确认该行写明的操作与证据范围。
- `C` 表示 2026-11-23 在 `81afd3d` 上复跑的 macOS ARM64 CTest，31/31 通过；2026-11-26 在 `only-read` 分支复跑：`f321d3f` 上 32/32（新增 `jet4_data_read_v2000/v2003`，ACE 用例增加只读打开前后字节比较），`62f6480` 上 36/36，其后加入 `corrupt_read_v2007/v2010/jet4`、`long_value_corrupt`、`perf_read_200k`、`complex_link_corrupt` 后为 42/42，接到本机真实样本后为 43/43（`real_sample`）（`jet4_*`、`ace_extra_*`、`corrupt_read_jet4` 依赖本地生成的语料，缺件时跳过）。其后是 CTest 用例名，括号内说明具体切片。样本来源见[语料清单](../../tests/fixtures/README.md)。`J` 表示[固定 JAR 历史探针](04-P0基线核验记录.md)中的对应章节。历史记录缺具体提交或样本哈希时保留缺口，由 W02 补跑并记录。Linux x86_64、Linux ARM64 和 Access 均待单独登记。
- 「两版」表示 V2000 与 V2003 的同一行为及同一证据范围。建库、建表、行增删改、索引、外键和恢复按需要分列两版。共用的长值行为保留一个编号。工作包编号见[开发计划](06-待办清单.md)，签名和边界见[接口契约](08-接口契约.md)。

## 数据库与格式

| ID | Jackcess 公开行为与格式 | REA 接口与位置 | 实现 | 验证与证据 | 工作包 |
|---|---|---|---|---|---|
| DB-01 | DatabaseBuilder 识别 Jet3、Jet4、ACE、MSISAM 格式头 | DatabaseFile / format | 已完成 | 通过 · C database_file（头识别） | W02 |
| DB-02 | DatabaseBuilder.create，V2000 空库可重开 | Database::create / write | 已完成 | 通过 · C cli_create_v2000、database_create | W02 |
| DB-03 | DatabaseBuilder.create，V2003 空库可重开 | 同 DB-02 | 已完成 | 通过 · C cli_create_v2003、database_create | W02 |
| DB-04 | DatabaseBuilder.open，Jet3 只读数据访问 | 目录、表定义、行、索引与保存查询均已实现（只读）/ format、catalog、value、index、queries | 已完成 | 通过 · C jet3_read、jet3_table_definition、jet3_index_read、jet3_query_read；6 份样本与固定 JAR 全表一致，9 条查询 SQL 逐字一致 | W08 |
| DB-05 | Generic Jet4 既有库打开及修改；创建拒绝 | Database / api，读取和写入边界待分格式核实 | 部分实现 | 未运行 · 独立 Generic Jet4 语料 | W02 |
| DB-06 | Database 生命周期、自动同步、刷新与打开选项 | Database / api、page，写锁与日志已有，选项对齐待补 | 部分实现 | 通过 · C database_create、crash_recovery_v2000/v2003（写入与恢复切片） | W09 |
| DB-07 | 默认 codec 拒绝编码页，MSISAM 识别，扩展处理器接入 | `PageCodec`/`DatabaseOptions::page_codec` 只读解码契约；`DatabaseFile::is_encoded`；format 读掩码后的编码键，page 逐页原地解码（页 0 原样）| 部分实现 | 通过 · C database_file（识别）、page_codec（置换样本：已配置 codec 解码全库并与原库逐行一致、内存打开一致、页 0 不进 codec、缺处理器与工厂拒答拒绝、MSISAM 容器拒绝、处理器异常与自带页号透传）；真实 Access 加密库对照未做 | W09 |
| DB-08 | 文件通道和内存打开，字符集、时区及日期模式 | Database::open(bytes)、DatabaseOptions::code_page（1252/819）已实现；时区与日期模式按只读范围不适用（存储串无时区，格式化由调用方决定）/ Database、page | 已完成 | 通过 · C memory_open（内存与按路径一致、写入被拒、损坏缓冲）、database_options（代码页覆盖与不支持值拒绝） | W09 |

## 表结构、行与游标

| ID | Jackcess 公开行为与格式 | REA 接口与位置 | 实现 | 验证与证据 | 工作包 |
|---|---|---|---|---|---|
| TB-01 | Database/Table 读取 Jet4 本地表、列和类型 | Database::tables / catalog | 部分实现 | 通过 · C catalog、real_sample（真实 PGeo 库 47 张表与固定 JAR 全表一致）、jet4_data_read（13 种列类型与 6 行值） | W02 |
| TB-02 | TableBuilder 创建 V2000 支持类型的空表及跨页表定义 | Database::create、TableBuilder / write | 已完成 | 通过 · C database_create（多页定义） | W02 |
| TB-03 | TableBuilder 创建 V2003 已覆盖类型的空表；既有跨页定义可修改 | 同 TB-02 | 已完成 | 通过 · C database_create（V2003）、multipage_definition_v2003（后续改定义） | W02 |
| TB-04 | TableUpdater、列类型和标识符规则 | TableBuilder、Table::add_index / write；加列及更广标识符规则待补 | 部分实现 | 通过 · C database_create（当前结构写入范围） | W10 |
| TB-05 | TableMetaData 与系统、隐藏、链接对象分类 | Database::tables、Table::is_hidden、linked_tables / catalog | 已完成 | 通过 · C catalog、real_sample、hidden_table（MSysObjects 0x08 翻转后仍列出且标记隐藏） | W10 |
| RW-01 | Table.addRow/addRows，V2000 支持类型写入并回读 | Table::add_row/add_rows / write | 已完成 | 通过 · C database_create（固定、变长及批量） | W02 |
| RW-02 | Table.addRow/addRows，V2003 支持类型写入并回读 | 同 RW-01 | 已完成 | 通过 · C database_create（V2003 写入切片） | W02 |
| RW-03 | Table.updateRow/deleteRow，V2000 已支持表形态的行更新、迁移、删除 | Table::update_row/delete_row / write | 已完成 | 通过 · C database_create（更新、迁移及删除） | W02 |
| RW-04 | Table.updateRow/deleteRow，V2003 已支持表形态的行更新、删除 | 同 RW-03 | 已完成 | 通过 · C database_create（V2003 写入切片） | W02 |
| RW-05 | Jet4 Memo/OLE 多页载荷读写及旧页处理，两版 | Table / value、write；读写及未引用页回收已有 | 已完成 | 通过 · C database_create（长值更新、共享页保留）；J「长值旧页回收」 | W02 |
| RW-06 | 两版 SHORT_DATE_TIME 读写及纪元前值 | Table / value、write | 已完成 | 通过 · C date_time_v2000/v2003、jet4_data_read（1899-12-29 读出）；J「纪元前值写入与回读」 | W02 |
| RW-07 | Cursor/RowId 定位、投影、过滤、前后向游标，两版 | RowCursor、Table::rows / api、catalog | 已完成 | 通过 · C cursor_v2000/v2003（所列操作）、jet4_data_read（顺序、正/反序索引与迁移行定位） | W02 |
| RW-08 | DataType 与 AutoNumber 生成、完整 Numeric 和类型覆盖 | Value、ColumnInfo / value、write；基线各类型要单独补齐 | 部分实现 | 通过 · C database_create（已支持的类型） | W10 |
| RW-09 | V2000 写入被中断后恢复 | Database::open / page；写前日志为 REA 自身契约 | 已完成 | 通过 · C crash_recovery_v2000（模拟中断） | W02 |
| RW-10 | V2003 写入被中断后恢复 | 同 RW-09 | 已完成 | 通过 · C crash_recovery_v2003（模拟中断） | W02 |

## 索引

| ID | Jackcess 公开行为与格式 | REA 接口与位置 | 实现 | 验证与证据 | 工作包 |
|---|---|---|---|---|---|
| IX-01 | IndexBuilder/IndexCursor，V2000 单列或合成复合键建索引、维护、查找 | Table::add_index、find_row_by_index / index、write | 已完成 | 通过 · C database_create、cursor_v2000（当前键集合）、jet4_data_read_v2000（索引遍历与按键查找） | W02 |
| IX-02 | 同 IX-01，V2003 | 同 IX-01 | 已完成 | 通过 · C database_create、cursor_v2003（当前键集合）、jet4_data_read_v2003（索引遍历与按键查找） | W02 |
| IX-03 | 两版单列 TEXT/MEMO 六种已覆盖排序描述符，含截断及升降序 | IndexBuilder、Table::rows_by_index / index、write | 已完成 | 通过 · C database_create（排序与 MEMO）、jet4_index_read（255 单元截断的 MEMO 索引正/反序）；J「MEMO 列索引」 | W02 |
| IX-04 | 两版复合 TEXT/MEMO 键的一致排序、前缀、唯一约束与特殊空值 | Table::rows_by_index_prefix、add_index / index | 已完成（读取侧） | 通过 · C jet4_index_read_v2000/v2003（复合 TEXT 与 (TEXT, MEMO) 排序、前缀、区间、精确查找、截断、NULL 键位置、unique 标志与条目数）；生成器探测固定 JAR 拒绝重复键，写入侧强制不在只读范围 | W02 |
| IX-05 | 两版清空叶页时维护叶链及回收页 | Table::delete_row / write | 已完成 | 通过 · C index_merge_v2000/v2003 | W02 |
| IX-06 | 删除跨多级节点后的可达性、节点页合并与空间回收 | Table::delete_row / write，仅叶页回收 | 部分实现 | 未运行 · 节点合并 | W02 |
| IX-07 | 既有索引异常策略及切换选项 | 索引拒绝规则已有；writeBrokenIndex 选项待补 | 部分实现 | 未运行 · 策略对照 | W10 |

## 属性与表达式、关系与链接

| ID | Jackcess 公开行为与格式 | REA 接口与位置 | 实现 | 验证与证据 | 工作包 |
|---|---|---|---|---|---|
| PR-01 | PropertyMap 名称匹配、类型/DDL 推导、NULL 和 flags | PropertyMapInfo::find/put/put_all / catalog | 已完成 | 通过 · C catalog（上述操作） | W03 |
| PR-02 | 两版库/表属性读写和未知记录保留 | Database/Table::save_property_maps / api、catalog | 部分实现 | 通过 · C database_create（已覆盖的属性块）；完整独立回读待补 | W03 |
| PR-03 | 两版字面量默认值与基础校验 | Table::add_rows/update_row / write | 部分实现 | 通过 · C database_create；J「数值 BETWEEN」与「日期字面量默认值」等探针 | W03 |
| PR-04 | 计算列和默认值的表达式求值、函数及区域行为 | 已支持部分规则与 ACE 已存值读取；完整求值待实现 | 未实现 | 未运行 · 完整求值 | W11 |
| RL-01 | V2000 读取关系，写入时拒绝孤儿行 | Database::relationships、Table / api、write | 已完成 | 通过 · C foreign_key_v2000；J「已有外键的写入检查」 | W02 |
| RL-02 | 同 RL-01，V2003 | 同 RL-01 | 已完成 | 通过 · C foreign_key_v2003；J「已有外键的写入检查」 | W02 |
| RL-03 | RelationshipBuilder 创建关系及支持键的级联，两版 | Database::create_relationship / api、write | 部分实现 | 通过 · C relationship_create、foreign_key_v2000/v2003；J「外键级联执行」「关系创建」 | W02 |
| RL-04 | LinkResolver 显式打开、CustomLinkResolver 惰性加载 | Database::open_linked_table / api；自定义快照按表临时物化，当前支持的列类型及完整 Jackcess 语义待扩展 | 部分实现 | 通过 · C database_create（V2000/V2003 文件链与 custom source、缺失 custom 表、缺失文件 `io_error`/目标路径、循环拒绝；`feece9c` 定向回归 8/8） | W04 |
| RL-05 | Database.createLinkedTable 创建链接目录条目 | Database::create_linked_table / api | 部分实现 | 通过 · C database_create（当前文件链接） | W04 |

## ACE、复杂列和 OLE

| ID | Jackcess 公开行为与格式 | REA 接口与位置 | 实现 | 验证与证据 | 工作包 |
|---|---|---|---|---|---|
| AC-01 | V2007 固定样本普通表 schema 和行 | Database::tables、Table::rows / catalog | 已完成 | 通过 · C ace2007_read（complex-data-v2007.accdb） | W05 |
| AC-02 | V2010 固定样本普通表 schema 和行 | 同 AC-01 | 已完成 | 通过 · C ace2010_read（complex-data-v2010.accdb） | W05 |
| AC-03 | V2007 固定样本的附件、多值、历史类型化读取 | Table::complex_values / api、value | 已完成 | 通过 · C ace2007_read、attachment_value；J「复杂值类型化只读读取」 | W05 |
| AC-04 | 同 AC-03，V2010 固定样本 | 同 AC-03 | 已完成 | 通过 · C ace2010_read、attachment_value | W05 |
| AC-05 | V2007 固定未知类型样本的关联行与未识别值保留 | Table::raw_complex_values / api | 已完成 | 通过 · C ace2007_unsupported_complex | W05 |
| AC-06 | V2010 固定样本计算列逻辑类型及已存结果 | ColumnInfo、Table::rows / catalog、value | 已完成 | 通过 · C ace2010_calculated_fields；J「ResultType 与已存结果读取」 | W05 |
| AC-07 | V2019 固定样本扩展日期值（NULL、纳秒及历史日期） | Table::rows / value | 已完成 | 通过 · C ace2019_read；J「扩展日期与计算列结果读取」 | W05 |
| AC-08 | V2019 固定样本计算日期列已存结果 | Table::rows / catalog、value | 已完成 | 通过 · C ace2019_read；同 AC-07 的 J 记录 | W05 |
| AC-09 | V2016 普通表和 V2019 扩展语料读取 | Database、Table / format、catalog | 已完成 | 通过 · C ace_extra_read_v2016/v2019（固定 JAR 生成语料；独立来源待补）；`MSysResources.Data` 复合外键按 LONG 读出 | W05 |
| AC-10 | 非标记 LONG 外键回退和异常关联结构 | Table::complex_values / catalog、api | 部分实现 | 通过 · C catalog（合成回退）；真实样本未运行 | W05 |
| AC-11 | V2007/V2010/V2016/V2019 创建及既有库普通行写入 | Database、Table / write；当前拒绝 ACE 写打开 | 未实现 | 未运行 · 写入 | W06 |
| AC-12 | 复杂值附件、多值和版本历史的写入操作 | ComplexValueInfo / api、write | 未实现 | 未运行 · 写入 | W07 |
| AC-13 | OleBlob 包装、流式读取及资源生命周期 | OleStream / Table::ole_stream 只读取流（seek/read/read_range） | 已完成 | 通过 · C attachment_value（解码）、ole_stream（9000 字节跨页 OLE 的顺序读、定位读、尾部截断与非 OLE 列拒绝） | W09 |

## 保存查询、导入导出与扩展点

| ID | Jackcess 公开行为与格式 | REA 接口与位置 | 实现 | 验证与证据 | 工作包 |
|---|---|---|---|---|---|
| QU-01 | Database.getQueries / Query.toSQLString，V2000/V2003 固定样本 SQL 重建 | Database::queries、QueryInfo::to_sql / api | 已完成 | 通过 · C saved_query_v2000/v2003；J「保存查询的 SQL 重建」 | W10 |
| IO-01 | ImportUtil/ExportUtil 普通表 CSV，含日期文本切片；复杂列导出其外键文本 | CSV 接口 / io | 部分实现 | 通过 · C `csv_round_trip`、`csv_complex_export`（多值/附件/版本历史以十进制外键输出并过滤，逐字匹配固定 5.2.0 JAR）、`csv_unsupported_complex_export`（未知复杂类型输出外键、未知标量明确拒绝；`b2e6b1e`）；暂无 NULL 复杂外键样本 | W10 |
| EX-01 | ErrorHandler、ColumnValidator、ColumnMatcher 扩展点 | Database、Table、RowCursor / api；错误码与上下文映射见[错误分类](14-只读错误分类与上下文.md) | 部分实现 | 通过 · C database_create、cursor_v2000/v2003、error_context（七类码的可依赖字段与表/列上下文）；复杂值匹配的自定义异常仍原样透传 | W10 |

**额外能力判定。** Jackcess 5.2.0 的 `Database::getQueries` 和 `Query::toSQLString` 提供查询定义及 SQL 文本。通用 SQL 执行属于额外能力，单独决策；PR-04 的表达式求值保留在核心对齐范围。

## 目标平台验收

| ID | 放行证据 | 实现 | 验证 | 工作包 |
|---|---|---|---|---|
| QA-01 | 原生 Linux x86_64 构建和当前完整测试集 | 待核实 | 未运行 · CI 已配置 `ubuntu-24.04`，待首次运行登记 | W02、W12 |
| QA-02 | 原生 Linux ARM64 构建和当前完整测试集 | 待核实 | 未运行 · CI 已配置 `ubuntu-24.04-arm`，待首次运行登记 | W02、W12 |
| QA-03 | 目标 Access 实机打开、读取、修改、查询并再次核对 | 待核实 | 未运行 | W12 |

## PGeo 适配矩阵

| ID | 交付与边界 | REA 接口与位置 | 实现 | 验证与证据 | 工作包 |
|---|---|---|---|---|---|
| PG-01 | 读取 SHAPE、属性、OBJECTID/SRS 并组装图层 | `rea_jackcess::pgeo::Dataset`、`LayerReader` / `adapters/pgeo` | 部分实现 | 通过 · C `pgeo_layer_reader`（合成目录、坏引用、NULL/空 BLOB、缺失 SRS）、`pgeo_sample_read`（固定样本 17 图层/30 要素，完整逐层元数据/属性定义及逐行适配器输出对照核心目录/表）、`compare_pgeo_layer_dump.py`（固定 JAR 按图层/OBJECTID 对照属性定义和值及 SHAPE 长度/SHA-256）；GDAL 几何解码待验 | W13 |
| PG-02 | 克隆既有样本、保留几何载荷与空间索引关系 | 计划中的 PGeo 适配层 | 未实现 | 未运行 · GDAL/ArcGIS 回读 | W13 |
