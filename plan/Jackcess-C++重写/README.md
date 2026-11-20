# Jackcess C++ 重写计划

## 1. 首期目标与当前进展

首期在原生 Linux x86_64 和 ARM64 上交付 C++ 核心库，完整对齐 Jackcess 5.2.0 的公开能力和可观察行为。采用 C++ 接口表达数据库、表、游标、类型化值、构建器和扩展点。Jet4 读写是第一个内部里程碑，核心库在全量行为矩阵通过后发布。

当前已建立 C++17/CMake 构建、诊断 CLI、测试入口和公开读取接口。P1 已实现 Jet/ACE 引擎头识别、只读页访问，以及 Jet4 表定义、Usage Map、数据页行目录、溢出行跟随、普通本地表 schema 发现、流式逐行读取、基础类型值/长值解码、从 `MSysObjects.LvProp` 读取列 `Required` 属性以推导可空性、逻辑/物理索引定义读取，以及按 Jet4 B-tree 叶链顺序扫描索引记录。公开 `Database`、`Table`、`RowCursor`、`Row`、`RowId`、`Value` 可由核心库直接使用；`Database::create` 已可从固定模板创建空 V2000/V2003 文件，且经 C++ 和 Jackcess 5.2.0 回读，表/行修改尚未实现。`Row::id()` 提供页号和行号，`Table::indexes()` 提供逻辑索引元数据，`Table::rows_by_index()` 提供索引顺序扫描。`inspect` 输出内部诊断摘要；`canonical` 支持 Jet4 canonical JSON v1，输出 schema、列可空性、逻辑/物理索引定义、已支持类型的行值和行集哈希。其他列/表属性、索引键语义解码、按键查找/范围游标、表结构构建器、行写入，以及 P0 矩阵和关键难点核验仍待完成；上游同步 CI 待实施。以上是部分实现状态，不代表 P1/P2 验收完成。

本机只读样本 `test_data_only_read/test_mdb.mdb`（SHA-256 `0bc0acb9c732de1e17c43c2d0a6019f7b5a62ccf83c5818a1bc42db7dbc460c6`）经 MDBTools 1.0.1 导出对照，系统目录 51 行的 `Id`、`ParentId`、`Name`、`Type`、`Flags` 均匹配。样本不纳入仓库测试资产；可重复的合成格式用例纳入 CTest。

| 交付 | 范围 | 放行条件 |
|---|---|---|
| C++ 核心库 | 按格式对齐 MDB、ACCDB、公开读写操作、元数据、约束、表达式、工具及扩展接口 | 完成 [行为矩阵和核心库验收](02-验收与发布门槛.md) |
| PGeo 导入 | 读取属性、OBJECTID、SRS、SHAPE，经 GDAL 解码并组装图层 | 核对 GDAL 对照结果并通过目标产品入口验证 |
| PGeo 样本克隆 | 按清单复制已有库的表结构、元数据、原始 SHAPE 和空间索引相关数据 | GDAL 与目标 ArcGIS 回读通过，空间索引与实际要素一致 |

PGeo 导入和样本克隆各自发布。由产品图层新建 PGeo 另行规划。Access 窗体、报表等应用对象按 5.2.0 的可观察行为记录在矩阵中。

## 2. 基线、实施顺序与估算

参考基线为 `io.github.spannm:jackcess:5.2.0`，标签 `jackcess-5.2.0`，源码提交 `6159c680933625ffa952334b5e0e6b24f37bfdfe`。本地核对的发布 JAR SHA-256 为 `9451d6f336cfaa82f5af178d5f6668b933cda4f27e261f29a1faa83630f95775`。历史逆向笔记引用 5.3.0-SNAPSHOT 提交 `ba736087316e71439b57b30e6cea01bfdf8a9993`，涉及行为差异时以固定版本的源码和实测为准。 [5.2.0 源码](https://github.com/spannm/jackcess/tree/6159c680933625ffa952334b5e0e6b24f37bfdfe)

| 阶段 | 交付 | 依赖 |
|---|---|---|
| P0 · 行为与估算 | 版本与语料冻结、行为矩阵、关键难点验证、各包估算 | — |
| P1–P2 · Jet4 读写 | 从只读库和验证 CLI 贯通到新建、批量插入和已有库更新删除 | P0 |
| P3–P4 · 全量对齐 | 扩展格式，补齐约束、复杂列、表达式、查询定义及其余公开能力 | P1–P2，按能力并行 |
| P5 · 核心库发布 | 两种原生 Linux 架构回归、Access 互操作、产物审查 | P1–P4，验证滚动进行 |
| P6 · PGeo | 导入依赖所需读取能力；克隆依赖写入、索引和元数据能力 | 按适配能力分别启动 |

原 **92–140 人日**估算覆盖普通 Jet4 读取、新库导出和 PGeo 实验。P0 根据完整矩阵和索引、已有库更新、ACE 复杂列的验证结果，逐包重估实现、格式实验、评审和回归工量，列出可并行任务及关键路径。客户样本、原生运行环境和实机窗口单列为外部依赖。

## 3. 实施与证据入口

- [实施工作包与上游同步](01-实施工作包.md)
- [验收与发布门槛](02-验收与发布门槛.md)
- [Jackcess 5.2.0 能力矩阵（P0 初版，未冻结）](03-Jackcess-5.2.0能力矩阵.md)

`MDB_REFERENCE_DIR` 指向 `daydaydaywork/dailyWork/schemedesign/20261116_MDB 读写问题设计` 的本地目录。下列路径相对于该目录，P0 在验收清单记录所用资料和脚本的版本或哈希。

| 证据 | 专题内路径 |
|---|---|
| 格式研究 | `docs/Jackcess逆向研究/README.md` |
| Linux 产品集成背景 | `014-竞品分析-总报告与Linux产品集成决策.md` |
| 路线 B 历史验收与接口口径 | `008-路线B落地验收报告.md`、`decisions/ADR-001-路线B口径.md` |
| canonical 规范、比较器和公共语料清单 | `resource/test-mdb-validation/` |
| PGeo 解码实验 | `resource/test-mdb-gdal/reports/2026-11-18-jackcess-shapebin.md` |
