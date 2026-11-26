# 真实样本目录

本目录只保留一个入库的真实数据库样本，用于在本机和 CI 上跑 `real_sample` 及与固定 Jackcess
5.2.0 JAR 的全表对照。其他临时样本不要放进来。

| 文件 | 格式 | 字节 | SHA-256 |
|---|---|---|---|
| `test_mdb.mdb` | Jet4（PGeo） | 1548288 | `0bc0acb9c732de1e17c43c2d0a6019f7b5a62ccf83c5818a1bc42db7dbc460c6` |

`tests/real_sample_test.cpp` 固定上面的哈希；哈希不符时用例直接失败，避免样本被替换或改动。

## 来源与使用边界

- 该文件是真实项目导出的 PGeo 库，含 43 张用户表（中文表名、`SHAPE` 空间 BLOB、`GDB_*` 目录表）
  与 4 张系统表。
- **仓库必须保持私有（private）**：样本属于项目数据，不得推送到公开仓库，也不得用于演示或分发。
- 对外发布核心库前应确认该样本的授权与脱敏要求；必要时用合成语料替换并同步移除本文件与
  `real_sample` 的固定哈希。

## 对照方式

```sh
build/rea-jackcess canonical test_data_only_read/test_mdb.mdb > /tmp/cpp.json

javac -cp "$JACKCESS_JAR" -d /tmp tools/validation/JackcessTableDump.java
java -cp "/tmp:$JACKCESS_JAR" JackcessTableDump test_data_only_read/test_mdb.mdb /tmp/java.json

python3 -I tools/validation/compare_table_dump.py /tmp/cpp.json /tmp/java.json
```

2026-11-26 的结果是 `Compared 47 tables: schema and rows match.`，详见
[首期只读矩阵与语料盘点](../plan/Jackcess-C++重写/11-首期只读矩阵与语料盘点.md)。
