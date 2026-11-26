# C++ 源码架构

本文说明实现包边界、公开接口分类和目标依赖。当前只为已有代码建立构建包；规划中的包在有对应工作包时再创建源文件和目标。

## 公开接口分类

`include/rea_jackcess/` 是面向调用方的 C++17 接口层，按职责分为：

| 类别 | 主要类型 | 当前状态 |
|---|---|---|
| 文件与数据库 | `DatabaseFile`、`Database`、`FormatInfo` | 文件页读取、Jet4 只读/显式读写打开和空 V2000/V2003 创建已实现；读写句柄持有排他 advisory 文件锁 |
| 表与 schema | `Table`、`ColumnInfo`、`IndexInfo` | 本地表、列和逻辑索引定义读取已实现；`rows_by_index()` 支持按名称扫描索引顺序，并提供精确查找与区间扫描；ACE 复杂列可按父行读取 flat-table 原始关联行和类型化只读值 |
| 行访问 | `RowCursor`、`Row`、`RowId` | 顺序只读访问、按 Jet4 索引叶链顺序扫描和物理行标识已实现 |
| 字段值 | `Value` 及类型化载荷 | Jet4 基础类型和 Memo/OLE 原始载荷已实现 |
| 错误 | `DatabaseError`、`DatabaseDiagnostic` | 带文件、页、表、行、列上下文的只读错误与诊断已实现 |
| 构建与修改 | `Database::create`、`TableBuilder`、行写入 | 可建空 V2000/V2003 文件；可在 Jet4 V2000/V2003 新建 BOOLEAN/BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/GUID/TEXT/BINARY/MEMO/OLE 列空表；显式读写句柄可向本地 Jet4 V2000/V2003 表追加这些值，MONEY 用四位小数的 `DecimalValue` 精确映射到有符号 64 位存储，BOOLEAN 使用行尾 NULL mask 位（不占定长区，NULL 读回为 FALSE），包括跨页 Memo/OLE 长值，更新行并删除行；带单列升序/降序（含唯一）索引的表在插入、键变更更新和删除时维护条目 |
| 关系、属性、查询 | 关系/属性访问、过滤与索引游标 | 按对应行为矩阵逐步补齐 |

公开头文件不暴露 `detail` 类型、文件页结构或第三方验证工具类型。未实现行为先留在实现计划中，不作为可链接 API 承诺。

## 内部实现包

| 包 | 源码位置 | 边界与状态 |
|---|---|---|
| API 门面 | `src/api/` | 组合内部实现并提供公开类方法；当前支持读写打开、空库创建、带表 schema 的 Jet4 V2000/V2003 创建、受限行追加、等长原位更新、增长行 overflow 更新、缩短行压实和行删除，并对单列升序/降序（含唯一）索引维护条目；行写入根据 `MSysDb.AccessVersion` 接受 V2000 `08.50` 与 V2003 `09.50`，文件缺该属性时按 Jet4 版式处理；写入上下文通过轻量结构传给 `write`，不形成包循环 |
| 格式 | `src/format/` | Jet/ACE 版本识别、格式布局和格式特有编码规则；当前实现版本头识别 |
| 页 | `src/page/` | 有界页 I/O、页结构、缓存和分配；当前提供只读页访问、Jet4 数据页行目录、Jet4 原始页覆盖/追加/刷新、跨句柄 advisory 写锁，以及改页前记录原始镜像的写盘日志（`WriteJournal`，批次刷新后删除，下次读写打开时回放） |
| 目录 | `src/catalog/` | 系统目录、表定义、schema、Usage Map、行发现/读取及其物理元数据解析；当前支持 Jet4 读取 |
| 值 | `src/value/` | 类型映射、标量/长值解码与编码；当前实现 Jet4 基础值读取、日期序列转换和 ACE 附件 OLE wrapper 解码；deflate 解码依赖 zlib，不承担表/页布局解释 |
| 元数据 | `src/metadata/`（规划） | Jet4 关系目录读取在 `src/api/relationships.cpp`，属性块与索引定义解析在目录层。关系创建在 `write/jet4_relationship_creator.cpp`：向两端表的定义追加逻辑索引描述（定义跨页时按续页链重组）并写入 `MSysRelationships` 目录行，要求父表参照列已有唯一索引，子表外键索引可复用或自动构建；已有本地外键索引的写入前校验和级联执行同在 `write` 包，覆盖复合键与 TEXT/BINARY/GUID 键 |
| 索引 | `src/index/` | Jet4 B-tree 页解析、从根定位最左叶页并沿叶链返回行位置已实现；按编码键下降定位、起始扫描与开区间上界已实现；键语义解码待实施。写入侧的叶页分裂/合并与叶链维护在 `write` 包 |
| 表达式 | `src/expression/` | 默认值、校验、计算列和查询表达式；待实施 |
| 查询 | `src/query/` | 行过滤、组合游标和公开查询流程；待实施 |
| 写入 | `src/write/` | 当前嵌入官方 V2000/V2003 模板并执行同目录暂存、C++ 回读校验和发布；初始建表与既有表定义修改都按续页链写出（定义超过一页时自动分页）；可在既有表定义上追加关系所需的逻辑索引描述并写入关系目录行；Jet4 V2000/V2003 初始空表写入支持 BOOLEAN/BYTE/SHORT/LONG/SINGLE/DOUBLE/MONEY/SHORT_DATE_TIME/GUID/TEXT/BINARY/MEMO/OLE 列并更新 `MSysObjects`、系统索引和模板内联全局 Usage Map；行写入按 `MSysDb.AccessVersion` 接受 V2000/V2003 标记，可向本地表追加定长数值、BOOLEAN、MONEY、GUID、TEXT/BINARY 和 Memo/OLE 长值，BOOLEAN 只写 NULL mask 位且定长偏移保持 0，MONEY 按四位小数缩放为有符号 64 位整数，长值使用 LVAL 单页或链页及列级 Usage Map；建表可声明单列升序/降序非唯一/唯一索引并写入索引定义块与空根叶页，插入、键变更更新和删除时维护这些索引的数值（含 MONEY 的十进制缩放）、布尔、GUID、BINARY 与 General Legacy/General/General 97 TEXT 键（含固定 Jackcess 5.2.0 对 LCID 1049/1055/1058 version 0 的编码回退）与去重键计数（唯一索引拒绝重复的非 NULL 键）；插入会分裂写满的叶页并把根叶页升级为节点页，节点满则向上递归分裂并提升根层（已验证三层树），删除会摘除被清空的叶页并释放其页面（条目始终指向逻辑 RowId，行迁移到 overflow 行后仍保持一致）；支持等长原位更新、压实缩短行、增长行迁移到 overflow 行、行删除和更新行数。长值更新优先复用列内仍有空闲空间的 LVAL 页；更新/删除后会从列级拥有页和空闲页 Usage Map 移除不再被存活行引用的 LVAL 页，并在全局 Usage Map 标记空闲，共享页保留；行写入落盘前按文件已有的本地外键索引校验受支持的键，孤儿行和未声明级联时仍有依赖行的主端改键或删除以 `constraint_violation` 拒绝，声明了级联的关系在父行落盘后改写或删除依赖行，级联批次共享回滚记录并在失败时回滚到操作前；回滚自身失败时句柄停写。页图/表图扩展、节点页合并和 Jackcess 可选的 `writeBrokenIndex` 写入策略待实施；MEMO 索引沿用文本排序编码并实现 255 UTF-16 单元与 510 字节键截断。固定版的六种文本排序描述符均已支持；其他 LCID 在 Jackcess 5.2.0 中默认只读 |
| 通用工具 | `src/util/` | 与具体格式无关的内部帮助代码；当前提供 SHA-256 和共享小端读写辅助函数 |

`src/CMakeLists.txt` 将已有实现编译为 `api`、`format`、`page`、`catalog`、`value`、`index`、`write`、`util` 内部对象包，再合并进一个 `rea_jackcess` 静态库。CMake 将受 Apache License 2.0 覆盖的两个空库模板嵌入静态库；运行时无需查找模板文件。调用方只链接 `rea_jackcess::core`；对象包不作为公开链接目标。新增包应先确定职责与依赖，再加入对象包列表。

## 依赖方向

以下是完整目标结构的依赖方向。当前 CMake 只为已有实现建立对象包；尚未实现的包暂不创建空目标。现有对象包边界用于构建分组，后续拆出更多模块时再收紧头文件可见范围并让 CMake 依赖与此图一致。

```text
api ──> query, metadata, index, catalog, write, value, page, format
query ──> expression, index, metadata, catalog, value
write ──> metadata, index, catalog, value, page, format, util
index ──> metadata, catalog, value, page, format
metadata ──> catalog, value, format
catalog ──> value, page, format
value ──> page, format, util
page ──> format
```

依赖只从上层指向下层，不以回调或头文件引用形成包循环。`adapters/pgeo/` 构建为独立目标 `rea_jackcess::pgeo`，当前只通过公开 API 使用核心库，负责图层目录映射和原始 SHAPE 读取；目标产品的几何解码可另行依赖 GDAL。核心库不依赖 GDAL、JVM 或 libmdb。`tools/cli/` 和 `tests/` 是独立消费者；canonical 诊断工具目前可直接读取内部目录结构，以保留解析阶段和字段级诊断。

## 构建与实现约定

- 核心实现使用 C++17；公开 API 使用 `rea_jackcess` 命名空间，内部实现使用 `rea_jackcess::detail`。
- 格式解析先校验长度、边界、引用和整数运算，再解码；错误尽量携带可获得的文件、页、表、行、列上下文。
- 对不支持或不可解释的数据保留原始字节或显式诊断，不静默改写。
- 新公开能力先有 Jackcess 5.2.0 行为证据和相应工作包，再进入公开头文件。
- 核心库和 PGeo/验证工具保持依赖隔离；格式对照依赖不得进入核心库传递依赖。
