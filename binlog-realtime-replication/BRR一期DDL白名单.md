# BRR 一期 DDL 白名单

## 1. 设计原则

BRR 一期白名单不以 SQL 字符串为准，而以 MySQL 源码实际解析、表打开、storage engine 判定后的结构化信息为准。

白名单判断必须发生在以下信息可用之后：

- `Sql_cmd_alter_table::execute()` 已进入 `mysql_alter_table()`。
- `table_list` 已解析为目标表。
- `HA_CREATE_INFO` 和 `Alter_info` 已准备完成。
- 对 inplace 路径，`fill_alter_inplace_info()` 已设置 `Alter_inplace_info::handler_flags`。
- `table->file->check_if_supported_inplace_alter()` 已返回实际 `enum_alter_inplace_result`。
- storage engine 已确认为 InnoDB。

原则：

- 能证明源码阶段边界、依赖等待和失败清理都正确，才进入白名单。
- 任何混合复杂能力的 DDL 直接 fallback。
- 用户指定更强锁不自动扩大白名单。
- Instant DDL 本身耗时低，一期不通过 BRR 优化。

## 2. 源码依据

关键源码：

- `sql/sql_parse.cc`：`SQLCOM_ALTER_TABLE` 进入 `lex->m_sql_cmd->execute(thd)`。
- `sql/sql_alter.cc`：`Sql_cmd_alter_table::execute()` 调用 `mysql_alter_table()`。
- `sql/sql_cmd_ddl_table.cc`：`CREATE INDEX`/`DROP INDEX` 通过 `mysql_alter_table()` 实现。
- `sql/sql_table.cc`：`mysql_alter_table()`、`fill_alter_inplace_info()`、`mysql_inplace_alter_table()`、copy 分支。
- `sql/handler.h`：`enum_alter_inplace_result`。
- `storage/innobase/handler/handler0alter.cc`：`ha_innobase::check_if_supported_inplace_alter()`。
- `storage/innobase/handler/ha_innodb.cc`：InnoDB handlerton 声明 `HTON_SUPPORTS_ATOMIC_DDL`。

## 3. 全局准入条件

所有 BRR DDL 必须同时满足：

- `binlog_realtime_replication=ON`。
- `binlog_realtime_replication_ddl=ON`。
- `GTID_MODE=ON`。
- 当前语句会写 binlog。
- source/replica 已完成 BRR capability 协商。
- 单 channel。
- 当前没有其他 in-flight BRR DDL。
- SQL command 为 `SQLCOM_ALTER_TABLE`。
- 单表 DDL。
- 目标对象是 base table，不是 view。
- 非 temporary table。
- 非系统表。
- 非 DD 表。
- schema 不是 `mysql`、`sys`、`performance_schema`、`information_schema`。
- storage engine 为 InnoDB。
- 表不是 discard/import tablespace 相关状态。
- DDL query 长度、dependency GTID set 长度未超过 BRR 配置上限。

任一条件不满足，fallback reason 使用明确枚举值，不进入 BRR。

## 4. 一期明确支持

### 4.1 普通二级索引 ADD INDEX

用户形态：

- `ALTER TABLE t ADD INDEX idx(c)`
- `ALTER TABLE t ADD KEY idx(c)`
- `ALTER TABLE t ADD INDEX idx(c1, c2)`

源码判定条件：

- `Alter_inplace_info::handler_flags` 只包含普通二级索引新增及可忽略的元数据标志。
- 必须包含 `Alter_inplace_info::ADD_INDEX`。
- 不能包含 `ADD_UNIQUE_INDEX`。
- 不能包含 `ADD_PK_INDEX`。
- 不能包含 `ADD_SPATIAL_INDEX`。
- 不能包含 `ADD_FOREIGN_KEY`。
- 不能包含 column add/drop/change/order 相关 flags。
- 不能包含 partition、rename、tablespace、encryption、charset conversion 等 flags。
- InnoDB `ha_innobase::check_if_supported_inplace_alter()` 返回：
  - `HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE`，优先支持。
  - `HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE`，一期默认暂缓；如果测试证明普通非 FULLTEXT 场景可控，可作为灰度子白名单。
- 用户未显式要求 `ALGORITHM=INSTANT`。
- 用户未显式要求 `LOCK=EXCLUSIVE`。

表和索引限制：

- 非 partition 表。
- 无 foreign key 参与。
- 新增索引不是 FULLTEXT。
- 新增索引不是 SPATIAL。
- 新增索引不是 functional index。
- 新增索引不引用 generated column。
- 新增索引列均为已有普通列。
- 不新增 auto_increment 列。
- 不涉及 invisible generated primary key 行为变化。

BRR 执行策略：

- prepare dependency：source 进入 DDL 主体前的 `gtid_executed`。
- commit dependency：source DDL 成功路径采集的 `gtid_executed`。
- replica 在 `ha_inplace_alter_table()` 后、`ha_commit_inplace_alter_table(commit=true)` 前等待 source result。

### 4.2 等价 CREATE INDEX 语法

源码中 `CREATE INDEX` 会调用 `mysql_alter_table()`，理论上可以复用普通二级索引 ADD INDEX 白名单。

一期用户文档默认不承诺 `CREATE INDEX`，实现可选择：

- 内部允许：当最终 `Alter_inplace_info` 与 `ALTER TABLE ... ADD INDEX` 完全等价时进入 BRR。
- 用户可见仍标记为实验性。

如果纳入，需要测试原始 query replay、错误消息、权限路径与 `ALTER TABLE ADD INDEX` 一致。

## 5. 一期候选但默认暂缓

这些 DDL 可能有收益，但第一周设计不把它们放入默认白名单，需要第二周 PoC 或后续阶段补充源码证明和故障注入后再打开。

### 5.1 InnoDB rebuild inplace DDL

候选：

- 某些 `ALTER TABLE ... ADD COLUMN` 非 instant 场景。
- 某些 `ALTER TABLE ... DROP COLUMN` 非 instant 场景。
- 某些 row format / create option 变化。

暂缓原因：

- InnoDB `innobase_need_rebuild()` 参与判断，实际路径复杂。
- DD 元数据变化和 SE commit 后错误处理更复杂。
- 需要确认 replica 能安全暂停并清理中间对象。
- 与 instant fallback 边界容易误判。

### 5.2 Copy DDL

候选：

- 明确 `ALGORITHM=COPY` 且单表 InnoDB 普通表。

暂缓原因：

- copy 分支涉及临时表、数据拷贝、rename/replace、DD 更新。
- crash 后中间对象清理复杂。
- 与 FK、trigger、view metadata 更新路径耦合。
- 执行前必须等待 prepare dependency，对延迟收益和并行度影响需要压测。

## 6. 一期明确不支持

### 6.1 对象类型

- 非 InnoDB 表。
- 临时表。
- view。
- system view。
- DD 表。
- log table。
- `mysql` schema 表。
- `performance_schema` 表。
- `information_schema` 表。
- `sys` schema 表。
- partition 表。

### 6.2 DDL 类型

- 多表 DDL。
- `RENAME TABLE`。
- `ALTER TABLE ... RENAME`。
- `ALTER TABLE ... EXCHANGE PARTITION`。
- `ALTER TABLE ... DISCARD TABLESPACE`。
- `ALTER TABLE ... IMPORT TABLESPACE`。
- `TRUNCATE TABLE`。
- `OPTIMIZE TABLE`。
- `REPAIR TABLE`。
- `ANALYZE TABLE`。
- `ALTER TABLESPACE`。

### 6.3 索引能力

- `ADD PRIMARY KEY`。
- `DROP PRIMARY KEY`。
- `ADD UNIQUE INDEX`。
- `DROP INDEX`。
- `RENAME INDEX`。
- `ALTER INDEX` visible/invisible。
- FULLTEXT index。
- SPATIAL index。
- functional index。
- generated column 上的 index。
- 与虚拟列 add/drop/change 混合的 index DDL。

### 6.4 列和表结构能力

- `ADD COLUMN`。
- `DROP COLUMN`。
- `CHANGE COLUMN`。
- `MODIFY COLUMN`。
- `ALTER COLUMN SET/DROP DEFAULT`。
- column rename。
- generated column 相关 DDL。
- column order 变化。
- charset/collation conversion。
- row format 变化。
- compression/encryption 变化。
- tablespace 变化。
- storage engine 变化。

### 6.5 约束与依赖

- foreign key add/drop/change。
- check constraint add/drop/change。
- table rename 且存在触发器或 view 引用更新。
- histogram 或 metadata 更新路径无法确认的组合 DDL。

### 6.6 运行环境

- `GTID_MODE != ON`。
- source 未开启 binlog。
- replica 未开启 BRR capability。
- source/replica BRR protocol version 不一致。
- 多 source channel。
- 并行 in-flight BRR DDL。
- source 或 replica 处于 read only / super read only 导致 DDL 行为不同。
- replica 表结构已经与 source 不一致。

## 7. Eligibility 决策流程

1. 检查全局开关和 capability。
2. 检查 `GTID_MODE=ON`、binlog enabled。
3. 检查 command 为 `SQLCOM_ALTER_TABLE`。
4. 检查单表、普通 InnoDB base table。
5. 排除临时表、系统表、partition、FK、rename 等对象级风险。
6. 调用原有 DDL 准备路径，让源码填充 `Alter_inplace_info`。
7. 调用 InnoDB `check_if_supported_inplace_alter()` 获取实际算法结果。
8. 检查 handler flags 是否精确匹配普通二级索引新增。
9. 检查算法结果是否为 `HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE`。
10. 检查无其他 in-flight BRR DDL。
11. 提前确定 GTID。
12. 创建 BRR context 并发送 prepare。

任一步失败都记录 fallback reason，并走原始复制链路。

## 8. Fallback reason 枚举建议

- `BRR_DISABLED`
- `BRR_CAPABILITY_NOT_NEGOTIATED`
- `BRR_PROTOCOL_VERSION_MISMATCH`
- `GTID_MODE_NOT_ON`
- `BINLOG_DISABLED`
- `UNSUPPORTED_SQL_COMMAND`
- `MULTI_TABLE_DDL`
- `TEMPORARY_TABLE`
- `SYSTEM_SCHEMA`
- `NON_BASE_TABLE`
- `NON_INNODB_TABLE`
- `PARTITION_TABLE`
- `FOREIGN_KEY_RELATED`
- `UNSUPPORTED_ALTER_FLAGS`
- `UNSUPPORTED_INPLACE_RESULT`
- `INSTANT_DDL`
- `COPY_DDL_NOT_ENABLED`
- `FULLTEXT_INDEX`
- `SPATIAL_INDEX`
- `FUNCTIONAL_INDEX`
- `GENERATED_COLUMN`
- `PRIMARY_KEY_CHANGE`
- `UNIQUE_INDEX`
- `RENAME_RELATED`
- `ANOTHER_BRR_DDL_RUNNING`
- `GTID_PREALLOCATE_FAILED`
- `DEPENDENCY_GTID_SET_TOO_LARGE`
- `QUERY_TOO_LARGE`
- `UNSAFE_DDL_PHASE`

## 9. 验收用例

必须进入 BRR：

- 普通 InnoDB 表执行 `ALTER TABLE t ADD INDEX idx(c)`。
- 普通 InnoDB 表执行 `ALTER TABLE t ADD INDEX idx(c1, c2)`。
- 大表普通二级索引新增，replica 预执行主体阶段。

必须 fallback：

- MyISAM 表 `ADD INDEX`。
- InnoDB partition 表 `ADD INDEX`。
- InnoDB 表 `ADD UNIQUE INDEX`。
- InnoDB 表 `ADD FULLTEXT INDEX`。
- InnoDB 表 `ADD SPATIAL INDEX`。
- InnoDB 表 `ADD PRIMARY KEY`。
- InnoDB 表 `ADD INDEX` 同时 `ADD COLUMN`。
- InnoDB 表 `DROP INDEX`。
- InnoDB 表 `RENAME INDEX`。
- 临时表 `ADD INDEX`。
- `mysql` schema 表 DDL。
- `GTID_MODE=OFF`。
- source 开启 BRR，replica 未协商 BRR capability。

边界验证：

- `LOCK=NONE` 的普通 `ADD INDEX`。
- 默认 `LOCK` 的普通 `ADD INDEX`。
- 显式 `ALGORITHM=INPLACE` 的普通 `ADD INDEX`。
- 显式 `ALGORITHM=INSTANT` fallback。
- source/replica 表存在同名索引时，BRR worker 失败后 SQL worker 能按原始错误策略处理。
