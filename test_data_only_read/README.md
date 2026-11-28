# 真实样本目录

本目录只保留一个入库的真实数据库样本，用于在本机和 CI 上跑 `real_sample` 及与固定 Jackcess
5.2.0 JAR 的全表对照。其他临时样本不要放进来。

| 文件 | 格式 | 字节 | SHA-256 |
|---|---|---|---|
| `test_mdb.mdb` | Jet4（PGeo） | 1548288 | `0bc0acb9c732de1e17c43c2d0a6019f7b5a62ccf83c5818a1bc42db7dbc460c6` |

`tests/real_sample_test.cpp` 固定上面的哈希；哈希不符时用例直接失败，避免样本被替换或改动。

## 本地验证样本（未入库）

`test_mdb_2.mdb`（Jet4 PGeo，4,882,432 字节，SHA-256 `a3d77555362404f4042e12bb7f0f7b44c6214100fec193ff91233a70733bb7bf`）只存在于开发者本机，**不入库**（`.gitignore` 已排除）。它是一份独立的真实 PGeo 库，使用较新的 `GDB_FeatureClasses`+`GDB_ObjectClasses` 目录布局（区别于 `test_mdb.mdb` 的 `GDB_Items`+`GDB_ItemTypes`），用于验证 PGeo 适配层对两种布局的支持：

- 48 张表对固定 Jackcess 5.2.0 JAR 逐表一致；
- 暴露 2 个图层 `报批数据_1`（16 要素）与 `China面地图_100_4326`（2371 要素），逐要素按 OBJECTID 与 SHAPE 字节与核心表完全一致。

对应回归：合成用例 `pgeo_layer_reader`（含 GDB_FeatureClasses 布局）、设计见[PG-01 设计](../plan/Jackcess-C++重写/13-PGeo原始载荷读取设计.md)。该样本不参与 CI，也不在 `real_sample`/`pgeo_sample_read` 用例里固定哈希。

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
