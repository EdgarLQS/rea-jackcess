# ACE 只读读取与复杂列实施计划

## 目标和边界

在现有 Jet4 读写能力之外，逐步实现 Jackcess 5.2.0 对 ACE（V2007、V2010、V2016、V2019）的读取与复杂列行为。固定基线为提交 `6159c680933625ffa952334b5e0e6b24f37bfdfe` 和 JAR SHA-256 `9451d6f336cfaa82f5af178d5f6668b933cda4f27e261f29a1faa83630f95775`。所有新增能力按文件版本分别声明证据，不把一个 ACE 样本推及其他版本；ACE 写入仅在独立实现和回读验证后开放。

截至 2026-11-23，阶段 A 已完成多个只读切片：ACE 与 Jet4 共用页布局的只读 catalog、表定义、Usage Map 和行读取路径已开放。固定 V2007/V2010 样本各可枚举 29 张表，`Table1` 有 6 列、4 行；三个 raw type `0x12` 列映射为 `DataType::complex`，普通行可遍历。固定 V2010 `calcFieldV2010` 样本按 `ResultType` 读取 11 个计算列的逻辑类型与四行已存 Boolean、TEXT、MEMO、LONG、MONEY、FLOAT、NUMERIC 结果，不执行表达式。固定 V2019 样本验证 `Table1.DateExt` 的 `EXT_DATE_TIME` schema 与九行值，以及 `DateNormalCalc` calculated 标记和九行已存结果；计算列只解包 stored value，不执行表达式。CTest `ace2007_read`、`ace2010_read`、`ace2019_read` 同时确认 ACE `read_write` 打开以 `unsupported_operation` 拒绝。`ColumnInfo` 已暴露 ComplexType ID、概念表/类型表/flat table 页与名称、complex-value foreign key 列名，并按 Jackcess 5.2.0 规则分类；`Table::raw_complex_values()` 可按父行 `RowId` 读取 flat-table 原始行；`Table::complex_values()` 提供多值、附件、版本历史和 unsupported 的类型化只读记录，并保留原始行及未知字段编码字节。附件字段包含元数据、原始 wrapper 和解码后的文件载荷；ACE 写入仍未实现；V2016 尚未用样本验证，V2019 仅验证该固定样本的日期 schema 与已存结果读取，`canonical` 可读取该固定样本 `Table1` 的普通与计算列九行；当前唯一错误为 `MSysResources.Data` 类型映射未定义。样本哈希、固定版本预期和回归见[基线核验记录](04-P0基线核验记录.md)。

## 阶段与验收

### A. ACE 只读页、目录和普通行

- 核实 ACE 与 Jet4 共用的页面、表定义、Usage Map、目录及行布局；格式差异使用独立格式参数表达，不把 ACE 写入路径误开放。
- 分别对 V2007、V2010 固定样本核对文件格式、系统/用户表、列 schema、普通值、行数和错误位置；Jet4 的 V2000/V2003 回归保持通过。
- CTest 使用固定、哈希登记的只读 fixture；`read_write` 在 ACE 暂不支持时明确拒绝。

### B. 复杂列 schema 和关联关系

- 已从 ACE 列定义读取 ComplexType ID/foreign-key 标记，并将其与 `MSysComplexColumns` 中的概念表、复杂类型表、flat table、AutoNumber 主键和复杂值外键列对应；ACE V2007/V2010 固定样本回归覆盖该关系。外键标记缺失时的选择规则已按固定版实现并由合成目录单测覆盖；真实 ACE 非标记 LONG 外键样本及坏关联的拒绝/恢复行为仍待补齐。
- 已实现 Jackcess 5.2.0 的类型判定优先级：精确附件表名、版本历史名前缀、通用复杂类型名前缀加单列多值 schema；未知表名再按多值、附件、版本历史 schema 形状回退，其余分类为 `unsupported`。固定 V2007/V2010 样本覆盖三种已知类型，另有 V2007 `UnknownComplex` fixture 覆盖 unsupported 分类。
- 对固定 V2007/V2010 样本逐列对照复杂类型、关联表、主外键列、类型表 schema 和关联键。

### C. 复杂值读取

- `Table::raw_complex_values(column_name, parent_row_id)` 已读取复杂列对应的 flat-table 原始行，优先按 foreign-key 前缀索引扫描，无索引时回退为表扫描；
- 已实现类型化多值单项、附件文件名/类型/URL/时间/标志及原始/解码载荷、版本值与修改时间；未知类型保留 raw flat-table 行及其未知字段字节。附件 wrapper 解码区分存储载荷和实际文件内容。复杂值编辑仍待实现。
- 固定样本现比较三类已知值的关联计数、多值/版本文本与最新优先顺序、附件元数据和解码文件内容，并比较未知类型关联计数；时间/标志和未知字段载荷的逐字段对照仍待补齐。

### D. OLE 公共包装与资源语义

- 对照 `OleBlob`、流和关闭行为实现流式读取、重置/释放语义及异常分类；测试多页载荷和游标/数据库句柄生命周期。

### E. ACE 复杂值写入

- 依据固定版写入实现和公开测试逐项覆盖附件、多值、版本追加及各类允许/拒绝的 update/delete 操作；在同目录副本写入，失败可回滚。
- 固定 Jackcess 独立重开验证所有变更，检查复杂主外键、关联表索引、行数和 payload；未覆盖的 ACE 版本继续标记待验证。

### F. 核心库验收

- 按格式逐项完成 [验收门槛](02-验收与发布门槛.md)，保持普通构建、CTest、Sanitizer 和独立 Jackcess 回读证据；在 Linux x86_64 与 ARM64 完成回归前，不将 ACE 阶段标为发布验收完成。
