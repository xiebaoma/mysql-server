# BRR 一期详细设计

## 1. 目标与边界

Binlog Realtime Replication（BRR）一期目标是为部分长耗时 InnoDB DDL 提供一条非持久的实时预执行通道，使备库在原始 binlog DDL 到达前提前完成主要耗时阶段，最终由原始 `GTID_EVENT + QUERY_EVENT` 推进复制坐标并兜底正确性。

一期只解决 DDL 复制延迟，不做 DML 实时复制，不改变持久 binlog/relay log 格式，不要求 crash 后继续接管 in-flight BRR DDL。

一期范围：

- `GTID_MODE=ON`。
- InnoDB 普通非临时基表。
- 单 channel。
- 同一时刻最多一个 in-flight BRR DDL。
- 优先支持 `ALTER TABLE ... ADD INDEX` 中可明确走 InnoDB inplace 主体阶段的子类。
- 对少量 rebuild/copy 类 `ALTER TABLE` 只在源码阶段边界和清理路径明确后纳入白名单。

默认原则：

- 白名单宁窄勿宽。
- 任何判断不确定、运行失败、断连、版本不兼容，均 fallback 到原始 relay log DDL。
- BRR 只做优化路径，不能成为唯一正确性路径。

## 2. 源码依据

### 2.1 DDL 入口与执行主线

源码路径：

- `sql/sql_parse.cc`
- `sql/sql_alter.cc`
- `sql/sql_table.cc`
- `sql/sql_cmd_ddl_table.cc`

`ALTER TABLE` 在 `mysql_execute_command()` 的 `SQLCOM_ALTER_TABLE` 分支中进入 `lex->m_sql_cmd->execute(thd)`。`Sql_cmd_alter_table::execute()` 完成权限、语义和上下文准备后调用 `mysql_alter_table()`。

`CREATE INDEX` 和 `DROP INDEX` 也会被转换为 `mysql_alter_table()` 调用，因此一期如果支持 `ADD INDEX`，需要同时决定是否把等价的 `CREATE INDEX` 语法纳入用户可见白名单。为降低范围，一期用户文档只承诺 `ALTER TABLE ... ADD INDEX`，实现上可以复用同一 eligibility 判断。

### 2.2 InnoDB inplace DDL 阶段

源码路径：

- `sql/sql_table.cc`
- `sql/handler.h`
- `sql/handler.cc`
- `storage/innobase/handler/handler0alter.cc`

`mysql_alter_table()` 在非强制 `ALGORITHM=COPY` 时会构造 `Alter_inplace_info`，调用 `fill_alter_inplace_info()`，再通过 `handler::check_if_supported_inplace_alter()` 判断实际算法。InnoDB 的判断在 `ha_innobase::check_if_supported_inplace_alter()`。

源码中 inplace 结果枚举为：

- `HA_ALTER_INPLACE_NOT_SUPPORTED`
- `HA_ALTER_INPLACE_EXCLUSIVE_LOCK`
- `HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE`
- `HA_ALTER_INPLACE_SHARED_LOCK`
- `HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE`
- `HA_ALTER_INPLACE_NO_LOCK`
- `HA_ALTER_INPLACE_INSTANT`

`mysql_inplace_alter_table()` 的核心阶段是：

- MDL 升级，必要时到 `MDL_EXCLUSIVE` 或 `MDL_SHARED_NO_WRITE`。
- `ha_prepare_inplace_alter_table()`。
- 对 `*_AFTER_PREPARE` 类型降级 MDL。
- `ha_inplace_alter_table()`，这是长耗时主体阶段。
- 提交前等待并重新阻止并发访问。
- `ha_commit_inplace_alter_table(commit=true)`。
- 更新 DD，写 binlog。

InnoDB 普通二级索引 `ADD INDEX` 通常返回 `HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE`，但 FULLTEXT、SPATIAL、虚拟列组合、rebuild、加主键等路径会改变锁和算法结果。因此 BRR 一期不能只看 SQL 文本，必须以 `Alter_inplace_info::handler_flags`、InnoDB `check_if_supported_inplace_alter()` 返回值和实际表元数据共同判断。

### 2.3 Copy DDL 阶段

源码路径：

- `sql/sql_table.cc`

当用户指定 `ALGORITHM=COPY`，或 storage engine 返回 `HA_ALTER_INPLACE_NOT_SUPPORTED` 且允许 fallback 到 copy 时，`mysql_alter_table()` 进入 copy 分支。copy 分支在源码里明确不支持 `LOCK=NONE`，会升级到 `MDL_SHARED_NO_WRITE` 或更强锁，再执行创建临时表、拷贝数据、替换表、写 binlog。

Copy DDL 与 Online/Inplace DDL 的依赖时机不同：Copy DDL 在主库执行主体前已经阻止并发写入，因此备库必须在执行主体前等待 DDL 之前的事务全部完成。

### 2.4 Binlog 发送与 relay log 写入

源码路径：

- `sql/binlog.cc`
- `sql/log_event.h`
- `sql/log_event.cc`
- `libs/mysql/binlog/event/binlog_event.h`
- `sql/rpl_source.cc`
- `sql/rpl_binlog_sender.cc`
- `sql/rpl_replica.cc`
- `sql/rpl_handler.h`

普通 DDL 成功后仍通过 `write_bin_log()` 写出 `GTID_EVENT + QUERY_EVENT`。在 source dump 侧，`mysql_binlog_send()` 创建 `Binlog_sender`，`Binlog_sender::run()` 读取 binlog 文件并通过 `send_packet()` 写网络包。`Binlog_transmit_delegate` 现有 hook 可以观察发送事件，但 BRR 一期需要在原始 binlog event 生成前发送 prepare，因此只靠现有 `before_send_event` 不够，需要新增 source 侧 BRR buffer 与 dump thread 取数逻辑。

Replica IO thread 在 `handle_slave_io()` 中从网络读取 event，经 `after_read_event` hook 后调用 `queue_event()`。`queue_event()` 默认会把 event 写入 `Relay_log_info::relay_log`，底层是 `MYSQL_BIN_LOG::write_event()` 或 `write_buffer()`。因此 BRR event 如果要“不进入 relay log”，必须在 IO thread 识别后分流到 BRR queue/cache，而不是沿用 `queue_event()`。

### 2.5 SQL applier 与 DDL 回放

源码路径：

- `sql/rpl_replica.cc`
- `sql/log_event.cc`

SQL thread/coordinator 通过 `exec_relay_log_event()` 和 `apply_event_and_update_pos()` 调用 `ev->apply_event(rli)`。DDL 最终进入 `Query_log_event::do_apply_event()`，在备库 SQL 线程上下文里按普通 SQL 执行。因此 fallback 的目标是：让原始 `Query_log_event` 保持可执行；BRR 成功时让这条原始 DDL 走 GTID skip。

### 2.6 GTID ownership 与 auto-skip

源码路径：

- `sql/rpl_gtid_execution.cc`
- `sql/rpl_gtid_state.cc`
- `sql/rpl_gtid_owned.cc`
- `sql/sql_parse.cc`
- `sql/binlog.cc`
- `sql/log_event.cc`
- `sql/sql_class.h`

`Gtid_log_event::do_apply_event()` 会调用 `set_gtid_next()`。当 GTID 未执行且未被其他线程占有时，`set_gtid_next()` 通过 `Gtid_state::acquire_ownership()` 占有 GTID；当 GTID 已在 `gtid_executed` 中时，它不再占有 GTID，而是在 `gtid_pre_statement_checks()` 返回 `GTID_STATEMENT_SKIP`。

`Gtid_state::update_gtids_impl_own_gtid()` 在 commit 时把 owned GTID 加入 `executed_gtids`，并释放 ownership；rollback/fallback 时释放 ownership 但不加入 executed。

BRR 一期复用这套机制：

- BRR worker 在 prepare 阶段提前占有 DDL GTID。
- BRR worker commit 成功后将 GTID 加入 `gtid_executed`。
- 原 SQL worker 后续读到原始 GTID 时，若 BRR 仍占有则等待；若已 executed 则 auto-skip。
- BRR fallback/rollback 时释放 ownership，不写 executed，让 SQL worker 正常执行原始 DDL。

## 3. 总体架构

BRR 一期新增四个逻辑模块：

- Source DDL hook：在 `mysql_alter_table()`/`mysql_inplace_alter_table()` 的安全阶段识别白名单 DDL，构建 BRR context，发送 prepare/commit/rollback。
- Source BRR buffer：保存待发送的非持久 BRR event，由 dump thread 优先发送给已协商 BRR capability 的 replica。
- Replica BRR queue/cache：IO thread 分流 BRR event 到内存队列，不写 relay log。
- Replica BRR worker：独立线程消费 BRR event，复用 DDL 执行路径预执行主体阶段，并通过 GTID ownership 与 SQL worker 协作。

普通复制链路保持不变：

- Source 仍写原始 binlog。
- Replica IO thread 仍写原始 relay log。
- SQL worker 仍读取原始 `GTID_EVENT + QUERY_EVENT`。
- BRR 成功时原始 Query event 被 GTID skip；BRR 失败时原始 Query event 兜底执行。

## 4. Source 侧设计

### 4.1 能力开关

Source 侧新增全局/会话可读配置：

- `binlog_realtime_replication`
- `binlog_realtime_replication_ddl`

Source dump thread 只有在以下条件同时满足时发送 BRR event：

- source 开关启用。
- replica 在 dump 请求阶段声明 BRR capability。
- channel 未被标记为 BRR incompatible。

一期不做跨版本协议兼容。协议 version 不一致时，source 不发送 BRR event 或 replica 忽略并 fallback。

### 4.2 DDL eligibility 判断

Source 侧 eligibility 必须在源码实际算法确认之后做最终判断，不能仅靠 SQL 文本。

必要输入：

- `SQLCOM_ALTER_TABLE`。
- `table_list` 指向单个普通基表。
- `create_info->db_type` 最终为 InnoDB。
- 非临时表、非系统库、非 DD 表、非 view。
- `Alter_inplace_info::handler_flags`。
- `enum_alter_inplace_result inplace_supported`。
- 是否存在 FK、partition、rename、generated column、FULLTEXT/SPATIAL index 等复杂能力。
- 当前 channel 是否已有 in-flight BRR DDL。

建议插入点：

- Inplace DDL：`mysql_alter_table()` 已完成 `fill_alter_inplace_info()` 和 `check_if_supported_inplace_alter()` 后，进入 `mysql_inplace_alter_table()`；在 `mysql_inplace_alter_table()` 完成必要 MDL 升级、进入 `ha_prepare_inplace_alter_table()` 前发送 prepare。
- Copy DDL：copy 分支完成 MDL 升级后、开始长耗时 copy 前发送 prepare。

如果无法在源码中明确“长耗时主体开始前”的安全点，直接 fallback。

### 4.3 提前 GTID

BRR prepare event 必须携带最终原始 DDL GTID。因此 source 需要在进入 BRR 前提前确定该 DDL 的 GTID，并保证后续 `write_bin_log()` 写出的原始 DDL 使用同一个 GTID。

第一期只允许 BRR DDL 使用提前 GTID，不改变普通事务 GTID 生成路径。

提前 GTID 的约束：

- `GTID_MODE=ON`。
- 语句必须会写 binlog。
- DDL 失败或 BRR fallback 时，不能把 GTID 错误加入 source/replica executed。
- 如果无法证明提前 GTID 与现有 group commit、atomic DDL、binlog cache 交互正确，第二周 PoC 必须停止主实现并调整方案。

### 4.4 BRR event

一期定义三类非持久 event：

- `BRR_DDL_PREPARE_EVENT`
- `BRR_DDL_COMMIT_EVENT`
- `BRR_DDL_ROLLBACK_EVENT`

这些 event 只走网络实时通道，不写 source binlog，不写 replica relay log。事件类型可以复用内部协议类型或扩展 `Log_event_type`，但必须保证未协商 BRR 的 replica 不会收到它们。

统一字段：

- `event_version`
- `channel_id`
- `ddl_id`
- `source_server_uuid`
- `source_server_id`
- `gtid`
- `schema_name`
- `table_name`
- `query`
- `ddl_type`
- `ddl_algorithm`
- `ddl_lock_type`
- `prepare_dependency_gtid_set`
- `commit_dependency_gtid_set`
- `session_variables`
- `source_result`
- `source_error_code`
- `source_error_message`

prepare event 必须包含：

- DDL GTID。
- 原始 query。
- 执行上下文。
- prepare dependency GTID set。
- 白名单判定结果摘要。

commit event 必须包含：

- DDL GTID。
- commit dependency GTID set。
- source 成功结果。

rollback event 必须包含：

- DDL GTID。
- source 失败原因。
- 是否允许 replica fallback 到原始 DDL。

### 4.5 Source 发送时机

prepare：

- 完成解析、权限、基础语义检查。
- 完成表打开、storage engine 确认和白名单判断。
- Inplace 路径在长耗时 `ha_inplace_alter_table()` 前。
- Copy 路径在长耗时 copy 前。

commit：

- 一期采用保守策略：source DDL 已经进入成功路径，并且不会再把该 DDL 当作失败返回给用户之后，才发送 BRR commit。
- 若实现能证明 `ha_commit_inplace_alter_table()` 后的 atomic DDL rollback 与 BRR rollback 完全闭环，可以把 commit 发送点前移到最终提交前；否则保持保守发送，牺牲少量时延收益换正确性。

rollback：

- source DDL 在 prepare 已发送后失败。
- source DDL 被 kill 且仍处于可回滚阶段。
- source 中途发现无法继续 BRR。
- BRR buffer/send 失败且 replica 可能已收到 prepare。

### 4.6 Source fallback

Source fallback 分为两类：

- prepare 前 fallback：不创建 in-flight BRR 状态，不发送 BRR event，原始复制照常。
- prepare 后 fallback：发送 rollback；如果 rollback 发送失败，依赖 IO 断连/超时让 replica 清理，原始 relay log 兜底。

Source 必须记录 fallback reason，供状态变量、错误日志和排查文档使用。

## 5. Replica 侧设计

### 5.1 IO thread 分流

Replica IO thread 在 `after_read_event` 之后、`queue_event()` 之前识别 BRR event。

处理规则：

- 普通 binlog event：保持现有 `queue_event()` 写 relay log。
- BRR event：校验 version、length、checksum、capability、channel 后写入 BRR queue/cache，不调用 `queue_event()`。
- BRR event 解析失败：记录错误，丢弃该 BRR event，并让该 DDL fallback；不得影响普通 event 写 relay log。

BRR queue/cache 一期为内存结构，不做 crash recovery 持久化。

### 5.2 BRR worker 生命周期

每个 channel 最多一个 BRR worker。

生命周期：

- `START REPLICA` 或 channel start 时启动。
- `STOP REPLICA`、IO 断连、SQL thread stop、server shutdown 时唤醒退出。
- worker 退出前必须释放 GTID ownership，清理 in-memory DDL context。

一期只允许一个 active DDL，避免多个 BRR DDL 与 MTS 调度交叉。

### 5.3 BRR worker 执行上下文

BRR worker 需要构造独立 THD，并设置为 replica applier 语义：

- `system_thread`
- `slave_thread`
- `rli_slave`
- `server_id`
- `gtid_next`
- 当前 db
- sql mode
- charset/collation
- time zone
- `foreign_key_checks`
- `unique_checks`
- `explicit_defaults_for_timestamp`
- 其他 Query event 中会影响 DDL 行为的 session 变量

这些字段必须来自 BRR prepare event，而不是使用 replica 当前会话默认值。

### 5.4 DDL 预执行与暂停

BRR worker 不应重新实现一套 DDL。它应复用 `mysql_alter_table()`、`mysql_inplace_alter_table()` 和 InnoDB handler DDL 路径，但需要新增 BRR 执行模式：

- 正常 DDL 模式：保持现有行为。
- BRR source 模式：在 source 安全点发送 prepare/commit/rollback。
- BRR replica 模式：执行 prepare 和主体阶段，在最终 commit 前暂停等待 source result。

Inplace 路径的暂停点：

- `ha_inplace_alter_table()` 成功之后。
- `ha_commit_inplace_alter_table(commit=true)` 之前。

Copy 路径的暂停点：

- 数据 copy/临时表构建完成之后。
- 原表替换、DD 提交、binlog 语义提交之前。

如果某类 DDL 无法在源码中保持上下文并安全暂停，不纳入一期白名单。

### 5.5 依赖等待

BRR prepare event 携带 `prepare_dependency_gtid_set`。BRR worker 在执行 DDL 主体前等待：

`replica.gtid_executed` 是 `prepare_dependency_gtid_set` 的超集。

BRR commit event 携带 `commit_dependency_gtid_set`。BRR worker 在提交 DDL 前等待：

`replica.gtid_executed` 是 `commit_dependency_gtid_set` 的超集。

Online/Inplace DDL：

- 主体阶段允许部分并发 DML。
- 提交前必须等待源端 commit dependency。

Copy DDL：

- 主体开始前必须等待 prepare dependency。
- 因为 copy 阶段阻止并发写，commit dependency 通常不会额外引入长等待，但仍按协议校验。

### 5.6 GTID 协作

Prepare 阶段：

- BRR worker 调用现有 GTID 设置/ownership 逻辑占有 DDL GTID。
- 如果 GTID 已 executed，说明原始 SQL worker 或恢复路径已完成，BRR worker 直接忽略该 DDL。
- 如果 GTID 被其他线程占有，等待或按超时策略 fallback。

Commit 阶段：

- BRR worker 完成 DDL commit。
- 通过现有 GTID commit 路径把 GTID 加入 `gtid_executed`。
- 释放 ownership 并广播等待线程。

Fallback/Rollback：

- 释放 ownership。
- 不加入 `gtid_executed`。
- 清理 DDL 中间状态。
- 等待 SQL worker 执行原始 relay log DDL。

SQL worker 行为不新增特殊 skip 分支，优先复用现有 `set_gtid_next()` 和 `gtid_pre_statement_checks()`。

## 6. 正确性设计

### 6.1 不丢

原始 binlog/relay log 不变。即使 BRR event 全部丢失，原始 SQL worker 仍能执行 DDL。

### 6.2 不重

BRR 成功提交后，GTID 已加入 `gtid_executed`。原始 Query event 到达时，现有 GTID skip 机制跳过执行。

### 6.3 不乱序

BRR 不使用 relay log logical clock 作为预执行依据，因为 logical clock 在事务提交后才形成。BRR 使用 source 在 prepare/commit 时采集的 GTID set 表达依赖，replica 以本地 `gtid_executed` 是否为超集作为执行和提交门槛。

### 6.4 不破坏生态

BRR event 不持久化：

- source binlog 格式不变。
- replica relay log 格式不变。
- CDC、`mysqlbinlog`、级联复制继续消费原始事件。

## 7. 一期不支持场景

- 非 InnoDB。
- 临时表。
- view。
- 系统表、DD 表、`mysql` schema 表。
- 多表 DDL。
- partition 表。
- FK 相关 DDL。
- rename table/rename column/rename index 与 ADD INDEX 混合。
- generated column 或 functional index 相关 DDL。
- FULLTEXT/SPATIAL index。
- primary key 变更。
- 多个 BRR DDL 并发。
- 跨版本 BRR 协议兼容。
- failover 后继续接管 in-flight BRR DDL。
- DML 实时复制。

## 8. 可观测性设计

一期需要提供：

- 当前 BRR 是否启用。
- channel 是否协商 BRR。
- 当前 in-flight DDL id。
- 当前 BRR 状态。
- 当前 DDL query 摘要。
- 最近一次 fallback reason。
- 最近一次 source/replica error。
- 统计计数：attempt、success、fallback、rollback、error。

错误日志至少记录：

- BRR capability 协商结果。
- DDL 进入 BRR。
- prepare/commit/rollback event 发送结果。
- replica BRR worker 执行结果。
- fallback reason。
- cleanup 结果。

## 9. 测试要求

第一期设计对应的测试必须覆盖：

- 支持 DDL 进入 BRR 并降低延迟。
- 不支持 DDL fallback。
- source prepare 前失败。
- source prepare 后失败并发送 rollback。
- replica BRR worker 执行失败。
- IO 断连后清理并 fallback。
- source crash。
- replica crash。
- STOP/START REPLICA。
- GTID ownership 等待。
- BRR commit 后 SQL worker auto-skip。
- BRR fallback 后 SQL worker 执行原始 DDL。
- BRR event 不进入 relay log。
- 原始 binlog 与 BRR 关闭时一致。

## 10. 第二周 PoC 必须验证的问题

以下问题不能留到主实现阶段：

- 提前 GTID 是否能只作用于 BRR DDL，且后续 `write_bin_log()` 使用同一个 GTID。
- BRR worker 是否能复用 `mysql_alter_table()` 并在 commit 前安全暂停。
- InnoDB `ADD INDEX` 的 BRR replica 模式是否能保留足够上下文到 commit/rollback event 到达。
- BRR worker 提交后，原 SQL worker 是否无需改动即可通过现有 GTID 机制 skip。
- prepare 已收到但 commit/rollback 丢失时，replica 是否能清理中间对象并 fallback。
- source commit event 的最早安全发送点在哪里；若不能证明前移安全，一期采用保守 commit-after-success 策略。
