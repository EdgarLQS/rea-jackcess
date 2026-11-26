# AI 协作指南

## 项目是什么

本仓库用 C++17 和 CMake 原生重写 Java Jackcess，目标是对齐固定基线 Jackcess 5.2.0 的公开能力与可观察行为，首期目标平台为 Linux x86_64 和 ARM64。首个里程碑是 Jet4 V2000/V2003 和 ACE V2007/V2010/V2016/V2019 普通库只读验收。按[只读优先实施计划](plan/Jackcess-C++重写/09-只读优先实施计划.md)和[只读优先开发交接](plan/Jackcess-C++重写/10-只读优先开发交接.md)推进。暂停新增写入能力，核心只读放行后独立推进 PGeo 导入，样本克隆随写入工作后移。核心库不依赖 JVM、libmdb 或 GDAL。

## 开工前

1. 阅读[README](README.md)了解项目与当前实现，[源码架构](src/ARCHITECTURE.md)了解模块边界，再读[计划总览](plan/Jackcess-C++重写/README.md)、[能力矩阵](plan/Jackcess-C++重写/03-Jackcess-5.2.0能力矩阵.md)、[实施工作包](plan/Jackcess-C++重写/01-实施工作包.md)和[验收门槛](plan/Jackcess-C++重写/02-验收与发布门槛.md)确认任务范围及验收证据。能力矩阵当前仍是 P0 初版，不能当作冻结的完整规格。
2. 先检查仓库实际代码、测试和未提交改动。当前 P0 能力矩阵尚未冻结，P1/P2 均未验收。已实现 Jet/ACE 头识别、只读页访问、Jet4 表/列和普通本地表读取，以及固定 ACE V2007/V2010 样本的普通表 schema/行读取（复杂列 ID/关联表、AutoNumber 主键/外键列名、附件/多值/版本历史/unsupported 分类、`Table::raw_complex_values()` 原始行读取和 `Table::complex_values()` 类型化只读读取可用；附件 OLE wrapper 可保留并解出文件载荷，unsupported 字段保留原始编码字节；复杂列写入未实现；固定 V2010 `calcFieldV2010` 样本按 `ResultType` 暴露计算列逻辑类型并读取 Boolean、TEXT、MEMO、LONG、MONEY、FLOAT、NUMERIC 已存结果、不执行公式；V2019 固定样本的 DateExt 与 DateNormalCalc 已投影读取，后者仅解包计算列已存结果、不执行公式（各九行；含 NULL、纳秒精度和 100/1265/1765 年）；V2016 与 V2019 其他 schema/行行为未验收）；Jet4 Usage Map、行目录与溢出行、基础值和 Memo/OLE 原始负载解码、列 `Required` 属性及逻辑/物理索引定义读取。公开 API 支持只读及显式读写 `Database::open`、创建空 V2000/V2003 数据库的 `Database::create`，以及用 `TableBuilder` 在 Jet4 V2000/V2003 初始创建带单列升序/降序非唯一/唯一索引（`IndexBuilder`）的 BOOLEAN、BYTE、SHORT、LONG、SINGLE、DOUBLE、MONEY、GUID、TEXT、BINARY、MEMO、OLE 列空表；MONEY 使用四位小数的十进制值，TEXT/BINARY 列容量默认分别为 255 个 UTF-16 字符和 255 字节，Memo/OLE 使用长值页，BOOLEAN 存在行尾 NULL mask 位且不占定长区字节。表/列名仅支持 ASCII 字母、数字、空格和下划线，创建后返回的 `Database` 句柄仍只读。
3. Jackcess 行为以[计划总览](plan/Jackcess-C++重写/README.md)固定的 5.2.0 源码提交、JAR 哈希及对应测试为准。按计划中的 `MDB_REFERENCE_DIR` 查找逆向笔记、比较器和外部语料；这些历史材料提供参考，不能替代本仓库当前回归或产品验收。

## 实现约定

- 先沿用[源码架构](src/ARCHITECTURE.md)中的模块职责和依赖方向。当前已有 `api`、`format`、`page`、`catalog`、`value`、`index`、`write`、`util` 包；未实现模块先留在计划中，只有出现实际代码时才加入构建目标。
- 公开接口使用 `rea_jackcess`，内部实现使用 `rea_jackcess::detail`。新 API 应对应 Jackcess 5.2.0 的明确行为和工作包；不发布没有实现支撑的空接口。
- 解析不可信文件时先检查长度、边界、页/行引用、整数溢出和循环；错误尽量带文件、页、表、行、列上下文。遇到未知或不支持结构要显式报错或保留原始字节，不静默重写。
- 核心格式和行为按 Jet/ACE 版本分别核对。写入验证使用副本；保留输入样本，记录哈希、工具版本、命令和结果。标明已验证、部分验证和待验证，不从单个样本推导普遍兼容性。
- 不提交新造的二进制数据库样本（`.mdb`、`.accdb` 等）。语料由 `tools/validation/` 的生成器写到构建目录（CMake 变量 `REA_JACKCESS_FIXTURE_DIR`，默认 `build/fixtures`）；文件缺失时对应用例返回 77，由 CTest 记为跳过。既有上游与基线语料保持不动。
- 唯一例外是已入库的真实样本 `test_data_only_read/test_mdb.mdb`：`tests/real_sample_test.cpp` 固定其 SHA-256，不得替换或再新增同类样本；仓库必须保持私有。
- 上游版本升级按工作包的同步流程执行；不要在未完成差异评审和回归前更换 5.2.0 基线。

## 构建与验证

本地构建和 CTest 命令：

```sh
cmake -S . -B build
cmake --build build
(cd build && ctest --output-on-failure)
```

按变更涉及的格式和工作包选择验证；文档修改至少核对链接、文件路径、能力状态和 `git diff --check`。不要把通过的合成测试、样本对照或本机实验写成完整格式验收。

多步骤任务开始前，用不超过 5 句话说明执行和验证策略；按最小改动实施，只改任务相关内容，并清理本次修改产生的无用代码或内容。需求存在歧义时，列出至少两种合理解释并请求确认；要求相互矛盾时立即停止并说明矛盾。报告异常时说明原因和可行处理建议。完成后按“计划、执行、验证”汇报。
