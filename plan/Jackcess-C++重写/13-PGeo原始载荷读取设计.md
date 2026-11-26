# PG-01 PGeo 原始载荷读取设计

## 目标与边界

本阶段在核心只读 API 上增加独立的 `rea_jackcess::pgeo` 适配层，用于枚举 PGeo Feature Class，并逐要素返回 OBJECTID、类型化属性和原始 SHAPE 字节。核心库不增加 PGeo 或 GDAL 依赖；几何记录的解析、解压、解码以及 GDAL 图层组装留在后续工作包。

首个实现切片按固定本机样本验证，不声称覆盖所有 ArcGIS/Access 版本。固定样本 SHA-256 为 `0bc0acb9c732de1e17c43c2d0a6019f7b5a62ccf83c5818a1bc42db7dbc460c6`。

## 目录关系

固定样本中：

- `GDB_ItemTypes.Name = "Feature Class"` 对应的 `UUID` 是 Feature Class 类型标识。
- `GDB_Items.Type` 与该 UUID 关联；`PhysicalName` 指向 Access 表，`DatasetInfo1` 给出几何字段名。
- `GDB_GeomColumns.TableName` 与 `GDB_Items.PhysicalName`、`FieldName` 与 `DatasetInfo1` 对应，并给出 `ShapeType`、`SRID`、`HasZ`、`HasM`。
- `GDB_Items` 自身在 `GDB_GeomColumns` 中有一行，但没有相应 Feature Class 目录项；只按几何目录枚举会把目录表误认为图层。
- 样本有 18 条几何目录记录，其中 17 条对应 17 个 Feature Class；Feature Class 表使用 `OBJECTID` LONG 和 OLE 类型的 `SHAPE`。
- `GDB_SpatialRefs.SRID` 对应几何目录的 `SRID`，`SRTEXT` 是可空的原始文本。样本中的 GUID 形式定义也按原文返回，不尝试转换成 EPSG 或 WKT。

## 首期 API 与规则

1. `PGeoDataset::open(path)` 以核心库只读方式打开数据库。必须同时存在 `GDB_ItemTypes`、`GDB_Items` 和 `GDB_GeomColumns`；三者都不存在时以 `unsupported_operation` 报告“不是可识别的 PGeo 库”，只缺其中一部分时以 `corrupted_file` 报告不完整目录。
2. 只枚举 `GDB_Items.Type` 解析为 `Feature Class` 的目录项，再按物理表名和几何字段名关联 `GDB_GeomColumns` 与本地表。普通 Access 表不会仅因有 `OBJECTID` 或 `SHAPE` 列而成为图层。
3. 首期 OBJECTID 规则为不区分 ASCII 大小写的 `OBJECTID` 列，类型必须是 LONG (`int32`)。几何字段名来自两份目录并须一致；对应列须为 OLE 或 BINARY。缺表、缺字段、重复关联或目录值类型错误均以带表/列上下文的 `corrupted_file` 报告。
4. `PGeoLayerInfo` 暴露图层名、物理表名、几何字段名、OBJECTID 字段名、ShapeType、HasZ/HasM、属性列的 `ColumnInfo`、目录 SRID 和可用的空间参考文本。`GDB_SpatialRefs` 可缺失；SRID 没有匹配行时保留目录 SRID，但空间参考对象为空。`SRTEXT` 原样返回，不解释内容。
5. `PGeoLayerReader::next()` 逐行扫描，不预先物化图层。OBJECTID 返回 `int32`；属性列排除 OBJECTID 和 SHAPE，保留核心库 `Value` 类型；SHAPE 为 NULL 时返回 `nullopt`，空 BLOB 返回存在但长度为零的字节向量。
6. 图层按 `GDB_Items` 记录顺序返回，打开图层使用目录中的图层名称精确匹配。一个数据库或图层句柄关闭后，已打开的图层读取器通过核心 `Table`/游标共享状态继续持有底层只读状态。

## 验收切片

- 由内存生成的 Jet4 数据库验证缺少 PGeo 目录时明确拒绝。
- 合成 PGeo 目录验证图层关联、属性类型、OBJECTID、SRID 缺失映射，以及 NULL SHAPE 与空 SHAPE 的区别；坏目录引用须明确失败。
- 固定样本验证只枚举 17 个 Feature Class，不包含 `GDB_Items`，并检查逐要素返回的 ID、属性和原始 SHAPE 字节。测试仅读取样本并核对源文件哈希不变。
- 本切片不验证 Linux 双架构、独立来源 PGeo、GDAL 几何解码、SRS 语义转换或产品导入入口；这些仍按路线图的独立门槛记录。
