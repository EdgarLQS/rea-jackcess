# C++ 源码架构

本文说明实现包边界、公开接口分类和目标依赖。当前只为已有代码建立构建包；规划中的包在有对应工作包时再创建源文件和目标。

## 公开接口分类

`include/rea_jackcess/` 是面向调用方的 C++17 接口层，按职责分为：

| 类别 | 主要类型 | 当前状态 |
|---|---|---|
| 文件与数据库 | `DatabaseFile`、`Database`、`FormatInfo` | 文件页读取和 Jet4 只读数据库入口已实现 |
| 表与 schema | `Table`、`ColumnInfo`、`IndexInfo` | 本地表、列和逻辑索引定义读取已实现；`rows_by_index()` 支持按名称扫描索引顺序 |
| 行访问 | `RowCursor`、`Row`、`RowId` | 顺序只读访问、按 Jet4 索引叶链顺序扫描和物理行标识已实现 |
| 字段值 | `Value` 及类型化载荷 | Jet4 基础类型和 Memo/OLE 原始载荷已实现 |
| 错误 | `DatabaseError`、`DatabaseDiagnostic` | 带文件、页、表、行、列上下文的只读错误与诊断已实现 |
| 构建与修改 | `DatabaseBuilder`、`TableBuilder`、写入游标 | 后续按写入工作包定型，当前不发布空接口 |
| 关系、属性、查询 | 关系/属性访问、过滤与索引游标 | 按对应行为矩阵逐步补齐 |

公开头文件不暴露 `detail` 类型、文件页结构或第三方验证工具类型。未实现行为先留在实现计划中，不作为可链接 API 承诺。

## 内部实现包

| 包 | 源码位置 | 边界与状态 |
|---|---|---|
| API 门面 | `src/api/` | 组合内部实现并提供公开类方法；当前已实现只读 API |
| 格式 | `src/format/` | Jet/ACE 版本识别、格式布局和格式特有编码规则；当前实现版本头识别 |
| 页 | `src/page/` | 有界页 I/O、页结构、缓存和分配；当前提供只读页访问、Jet4 数据页行目录，以及仅供内部使用的 Jet4 原始页覆盖/追加/刷新 |
| 目录 | `src/catalog/` | 系统目录、表定义、schema、Usage Map、行发现/读取及其物理元数据解析；当前支持 Jet4 读取 |
| 值 | `src/value/` | 类型映射、标量/长值解码与编码；当前实现 Jet4 基础读取，不承担表/页布局解释 |
| 元数据 | `src/metadata/` | 库/表/列属性、关系、约束和逻辑索引语义；待实施。现有 Jet4 属性块与索引定义解析暂留在目录层 |
| 索引 | `src/index/` | Jet4 B-tree 页解析、从根定位最左叶页并沿叶链返回行位置已实现；键语义解码、查找/范围游标和索引维护待实施 |
| 表达式 | `src/expression/` | 默认值、校验、计算列和查询表达式；待实施 |
| 查询 | `src/query/` | 行过滤、组合游标和公开查询流程；待实施 |
| 写入 | `src/write/` | 构建器、页分配、表/行创建修改删除和写入事务边界；待实施 |
| 通用工具 | `src/util/` | 与具体格式无关的内部帮助代码；当前提供 SHA-256 |

`src/CMakeLists.txt` 将已有实现编译为 `api`、`format`、`page`、`catalog`、`value`、`index`、`util` 内部对象包，再合并进一个 `rea_jackcess` 静态库。调用方只链接 `rea_jackcess::core`；对象包不作为公开链接目标。新增包应先确定职责与依赖，再加入对象包列表。

## 依赖方向

以下是完整目标结构的依赖方向。当前 CMake 只为已有实现建立对象包；尚未实现的包暂不创建空目标。现有对象包边界用于构建分组，后续拆出更多模块时再收紧头文件可见范围并让 CMake 依赖与此图一致。

```text
api ──> query, metadata, index, catalog, value, page, format
query ──> expression, index, metadata, catalog, value
write ──> metadata, index, catalog, value, page, format
index ──> metadata, catalog, value, page, format
metadata ──> catalog, value, format
catalog ──> value, page, format
value ──> page, format
page ──> format
```

依赖只从上层指向下层，不以回调或头文件引用形成包循环。PGeo 适配器通过公开 API 使用核心库，并单独依赖 GDAL；核心库不依赖 GDAL、JVM 或 libmdb。`tools/cli/` 和 `tests/` 是独立消费者；canonical 诊断工具目前可直接读取内部目录结构，以保留解析阶段和字段级诊断。

## 构建与实现约定

- 核心实现使用 C++17；公开 API 使用 `rea_jackcess` 命名空间，内部实现使用 `rea_jackcess::detail`。
- 格式解析先校验长度、边界、引用和整数运算，再解码；错误尽量携带可获得的文件、页、表、行、列上下文。
- 对不支持或不可解释的数据保留原始字节或显式诊断，不静默改写。
- 新公开能力先有 Jackcess 5.2.0 行为证据和相应工作包，再进入公开头文件。
- 核心库和 PGeo/验证工具保持依赖隔离；格式对照依赖不得进入核心库传递依赖。
