# 真实样本目录

本目录只有 `test_mdb.mdb` 一个入库的真实数据库样本，用于在本机和 CI 上跑 `real_sample` 及与固定
Jackcess 5.2.0 JAR 的全表对照；其余本地或下载样本均被 `.gitignore` 排除。

| 文件 | 格式 | 字节 | SHA-256 |
|---|---|---|---|
| `test_mdb.mdb` | Jet4（PGeo） | 1548288 | `0bc0acb9c732de1e17c43c2d0a6019f7b5a62ccf83c5818a1bc42db7dbc460c6` |

`tests/real_sample_test.cpp` 固定上面的哈希；哈希不符时用例直接失败，避免样本被替换或改动。

## 本地验证样本（未入库）

`test_mdb_2.mdb`（Jet4 PGeo，4,882,432 字节，SHA-256 `a3d77555362404f4042e12bb7f0f7b44c6214100fec193ff91233a70733bb7bf`）只存在于开发者本机，**不入库**（`.gitignore` 已排除）。它是一份独立的真实 PGeo 库，使用较新的 `GDB_FeatureClasses`+`GDB_ObjectClasses` 目录布局（区别于 `test_mdb.mdb` 的 `GDB_Items`+`GDB_ItemTypes`），用于验证 PGeo 适配层对两种布局的支持：

- 48 张表对固定 Jackcess 5.2.0 JAR 逐表一致；
- 暴露 2 个图层 `报批数据_1`（16 要素）与 `China面地图_100_4326`（2371 要素），逐要素按 OBJECTID 与 SHAPE 字节与核心表完全一致。

对应回归：合成用例 `pgeo_layer_reader`（含 GDB_FeatureClasses 布局）、设计见[PG-01 设计](../plan/Jackcess-C++重写/13-PGeo原始载荷读取设计.md)。该样本不参与 CI，也不在 `real_sample`/`pgeo_sample_read` 用例里固定哈希。

### Access 2024 实机另存副本

以下副本由 Microsoft Access 2024（16.0.17928）从 `test_mdb.mdb` 另存，只存在于本机，**不入库**。`access_version_read_test` 只在文件存在时由 CMake 注册，分别断言文件格式、43 张用户表及 `GDB_GeomColumns` / `GDB_ItemTypes` / `GDB_SpatialRefs` 的关键行。

| 文件 | 格式 | 字节 | SHA-256 |
|---|---|---|---|
| `test_mdb_2000.mdb` | Jet4 V2000 | 1,576,960 | `16a51a1f700e23b6d27d2f935d5210f26d3e34055b0b93cdbc4adc515ff93352` |
| `test_mdb_2003.mdb` | Jet4 V2003 | 1,572,864 | `7e112de3c462868d31ed457c0ebfa9799e2287265a6e40f5e1f0d31d2c67a31c` |
| `test_mdb_2003.accdb` | ACE V2007 | 2,314,240 | `e4de60cdb030ba6b33d5c695ad256fcd773698e52be5ee57fa8c055bce433ea3` |
| `test2007_2016.accdb` | ACE V2007（空库探针） | 278,528 | `c2db912021c7241f77ff6a25f2f2688464fa40b55bac4d991cd25735b02ddb19` |

`test2007_2016.accdb` 是 Access 选择"2007-2016 文件格式"后新建的空库，引擎字节仍为 ACE2007，说明该选项不能生成独立 V2016/V2019 语料。

### GDAL 公开 PGeo 语料

`gdal-pgeo-sample-v3.10.0.mdb` 来自 GDAL 仓库标签 `v3.10.0` 的 `autotest/ogr/data/pgeo/sample.mdb`，经 GitHub Contents API（`application/vnd.github.raw`）下载，**不入库**：

- 765,952 字节，SHA-256 `f126ec09aa8bfc305a6dd62898f359a28630883adf17673c82ef9944b6766dae`；
- Jet4 V2003，27 张表，`GDB_Items`+`GDB_ItemTypes` 目录布局；
- 3 个图层 `lines`（6 要素）、`points`（17 要素）、`polys`（10 要素），均解析出有效 SRS；
- GDAL 仓库为 MIT 许可；该测试数据的再分发授权仍需在对外发布前复核，本仓库只登记来源与哈希，不提交文件。

对应回归：`pgeo_gdal_sample_read`（文件存在时注册），断言 27 表、格式、图层顺序/行数、SRS 可用性，并按 OBJECTID 与核心表逐行核对属性与 SHAPE。固定 JAR 对照结果：`Compared 27 tables: schema and rows match.`、`Compared 3 PGeo layers and 33 features by OBJECTID.`

### 上游公开语料（未入库）

以下文件经 GitHub Contents API（`application/vnd.github.raw`）从公开仓库下载，放在本目录子文件夹中，**不入库**。它们用于补齐真实 Jet3 936/932 代码页、独立 ACE V2016/V2019 与真实加密库证据；下载文件的再分发授权仍需在对外发布前复核。

| 文件 | 来源路径（固定分支 `master`/`main`） | 格式 | 字节 | SHA-256 |
|---|---|---|---|---|
| `upstream-jetdb/japaneseTestV1997.mdb` | `dominion525/jetdb` `testdata/V1997/` | Jet3，代码页 932 | 53,248 | `3b0c73573b770abf4149faa1187e193407ed6cc9c734bb680b3ddbef77e08210` |
| `upstream-ucanaccess-v2/charsetGBK.mdb` | `spannm/ucanaccess` `src/test/resources/testdbs/` | Jet3，代码页 936 | 67,584 | `3c8d7d25e45df1bb23b988a0089ac1e88a6b62ff261ed0702024888abecfef89` |
| `upstream-ucanaccess-v2/functionsV2016.accdb` | 同上 | ACE2016（22 表） | 405,504 | `31248687b75f28e7f8a233ae9b452121d455f5321d5c5f356328512d33ac1550` |
| `upstream-jetdb/bigIntTestV2016.accdb` | `dominion525/jetdb` `testdata/V2016/` | ACE2016（BigInt） | 376,832 | `dba5d460bd8143c13c0dfb62a9030a7dd0b042b379fdea9bf84e72cfd31230eb` |
| `upstream-jackcess-v2/empty2016.accdb` | `spannm/jackcess` `src/main/resources/io/github/spannm/jackcess/` | ACE2016（空库） | 364,544 | `cde272fdb4b1cbf467ec58883f68b2f45ef0000cd440e85a09961dd17eea5c81` |
| `upstream-jackcess-v2/empty2019.accdb` | 同上 | ACE2019（空库） | 434,176 | `c20059a6924fcfe3480e1999544d59c7a9bb0b2e64648764187b26d0d90abd35` |
| `upstream-jetdb/extDateTestV2019.accdb` | `dominion525/jetdb` `testdata/V2019/` | ACE2019（与仓内 `ext-date-v2019.accdb` 相同，均源自 `spannm/jackcess`） | 557,056 | `bb6d7af8e5275c1143b5f68447b2f96bd11e460fbec6856de7b3e339ccb53cf6` |
| `upstream-jetdb/db2007-enc.accdb` | `dominion525/jetdb` `testdata/`（源自 jackcessencrypt） | ACE2010，Agile 加密，密码 `Test123` | 430,080 | `fa6cd4ed7639dee40ad288ff6c530356ea050bde36dbde5e4328fee6aa07f306` |
| `upstream-jetdb/db2007-rc4cryptoapi.accdb` | 同上 | ACE2007，RC4 CryptoAPI | 434,176 | `174ba7ff6349d4f929adf555f5035c0722923776662f30a523f2933827d9558e` |
| `upstream-jetdb/db2013-enc.accdb` | 同上 | ACE2010，Agile 加密 | 389,120 | `3e9ee219a7ca9bf7dee261f6455f298887603d41c82ea0c5bc06d8831f09637a` |
| `upstream-jackcess-dotnet/access97-cp932.mdb` | `markmario/jackcess-dotnet` `tests/JackcessDotNet.Tests/Assets/` | Jet3，代码页 932（Jet 4.0/DAO 3.6 在 CP932 进程内创建） | 51,200 | `df2854a88a203848edefc74e4601a24ed91f7550603cb6ac525644c40b8b399f` |
| `upstream-jackcess-dotnet/ace-large-numbers-extended-dates.accdb` | 同上 | ACE2019，BIGINT 与 EXT_DATE_TIME（DAO `CreateDatabase`，由 Access 数据库引擎生成） | 249,856 | `1c494e390aac10fb6490eca641fb507f85408f2aee5ae02e70ead8ae415e9fa5` |

- `dominion525/jetdb` 为 MIT OR Apache-2.0；其 `testdata/SOURCES.md` 说明 `japaneseTestV1997.mdb` 由 DAO 3.6 以 `dbLangJapanese`（`CP=932`）独立创建，`bigIntTestV2016.accdb` 由 Access for Microsoft 365 独立创建，V2019 文件复制自 Jackcess 上游。
- `spannm/ucanaccess` 测试库的来源与许可需按其仓库说明复核；`charsetGBK.mdb` 的期望值由 `CharsetGBKTest` 固定为 `GBK编码`。
- `markmario/jackcess-dotnet` 仓库许可为 Apache-2.0；其 `tests/JackcessDotNet.Tests/AccessLargeNumberExtendedDateTests.cs` 与 `tools/large-number-extended-date-fixture.ps1` 记录了 V2019 样本的生成方式，`Access97ReadTests.cs` 记录了 CP932 样本由 32 位 DAO 3.6 在代码页 932 进程内创建。两者均不来自固定 Jackcess 5.2.0 基线，按独立来源登记。
- 加密库的密码仅登记 `db2007-enc.accdb`（`Test123`）；其余两份用于验证“有 EncryptionInfo、无 codec 时明确拒绝”，不用于解密。
- 同目录另有仅作探测、未纳入发布对照的文件（`ace-divergent-keys`、`ace-extended-date-forms`、`ace-extended-date-table-type`、`ace-second-form-foreign-key`、`ace-text-marker`、`dao-boundary-definitions`、`dao-odd-names` 及 `upstream-ucanaccess-complex/` 下的 `complex/githubIssue16/aliasNaming`）。这些文件用于检查非标记复杂外键与扩展日期第二形态；探测结果见[可读特性与已知差异](../plan/Jackcess-C++重写/16-可读特性与已知差异.md) §4。`upstream-ucanaccess-complex/` 的许可同样按其仓库说明复核。

对应回归：`jet3_codepage_932`、`jet3_codepage_936`（文件存在时运行，缺失返回 77）。`japaneseTestV1997.mdb` 用 `--code-page 932` 读取 5 张表，表名/列名/索引名与三行文本及 MEMO 均与生成器说明一致，索引用 `氏名索引` 扫描得到 `Smith`、`ﾃｽﾄ`、`山田太郎`；与固定 JAR（`windows-31J`）逐表一致。`access97-cp932.mdb` 用 `--code-page 932` 读取 5 张表，同样与固定 JAR（`windows-31J`）逐表一致。`charsetGBK.mdb` 用 `--code-page 936` 读取 7 张表，`table1` 的 `GBK编码` 与固定 JAR（`GBK`）逐表一致，`PrimaryKey` 数值查找命中。`ace-large-numbers-extended-dates.accdb` 的 16 张表（含 `BySql`/`ByDao` 各 12 行、BIGINT 与 EXT_DATE_TIME）与固定 JAR 逐表一致。以上样本对照前后哈希不变。

### 派生 NULL 复杂外键语料

`build/fixtures/complex-null-v2007.accdb` 不放进本目录也不入库。它由 `rea_jackcess_csv_tests complex-null-fixture tests/fixtures/complex-data-v2007.accdb <目标>` 确定性派生：复制上游固定样本并清除 `Table1` 首行 `multi-value-data` 的 NULL-mask 位。SHA-256 `4aa32c391aa8e4341aef7244ceb11b0b65f981833dba93ab2fb8a7a7d6731669`；固定 JAR 读取 29 表一致、`ExportUtil` 全表 CSV 内容一致。Access 2024 的 DAO/ADO/SQL/UI 与固定 JAR 公共 API 均无法写出 NULL 复杂外键，因此该语料只作为合法 NULL 编码的读取/导出回归，不声称是 Access 保存产物。

## 来源与使用边界

- 该文件是真实项目导出的 PGeo 库，含 43 张用户表（中文表名、`SHAPE` 空间 BLOB、`GDB_*` 目录表）
  与 4 张系统表。
- 仓库已调整为公开，本样本因此同样对外可见。样本属于项目数据，建议在对外发布核心库前确认授权与
  脱敏要求；如需收紧，用脱敏或合成样本替换本文件，并同步更新 `test_data_only_read/README.md`、
  `tests/real_sample_test.cpp` 的固定哈希以及 `tests/fixtures/README.md` 的对照记录。
- 不得把样本再分发到其他仓库或用于演示。

## 对照方式

```sh
build/rea-jackcess canonical test_data_only_read/test_mdb.mdb > /tmp/cpp.json

javac -cp "$JACKCESS_JAR" -d /tmp tools/validation/JackcessTableDump.java
java -cp "/tmp:$JACKCESS_JAR" JackcessTableDump test_data_only_read/test_mdb.mdb /tmp/java.json

python3 -I tools/validation/compare_table_dump.py /tmp/cpp.json /tmp/java.json

python3 tools/validation/compare_pgeo_layer_dump.py /tmp/cpp.json /tmp/java.json \
  --source test_data_only_read/test_mdb.mdb
```

2026-11-26 的全表对照结果是 `Compared 47 tables: schema and rows match.`。PGeo 专项对照结果是
`Compared 17 PGeo layers and 30 features by OBJECTID`，图层目录、属性定义和值、SHAPE 长度与 SHA-256
一致；源文件对照前后 SHA-256 均为
`0bc0acb9c732de1e17c43c2d0a6019f7b5a62ccf83c5818a1bc42db7dbc460c6`。专用比较器按 OBJECTID 配对核心
canonical 与固定 JAR 的表行；`pgeo_sample_read` 逐项验证适配层图层目录映射、完整属性定义和逐行返回值与核心目录/表一致。详见
[首期矩阵与语料盘点](../plan/Jackcess-C++重写/11-首期只读矩阵与语料盘点.md)。
