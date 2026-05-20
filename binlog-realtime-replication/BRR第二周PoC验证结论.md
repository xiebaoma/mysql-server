# BRR 第二周 PoC 验证结论

## 1. 总体结论

第二周 PoC 从源码层面验证了 BRR 一期关键路径的可行性。结论是：BRR 方案可以继续进入第 3-4 周的协议和传输链路实现，但必须把“提前 GTID”和“DDL commit 前暂停/清理”作为最高风险点继续用插桩和故障注入验证。

总体判断：

| PoC | 验证项 | 结论 | 是否阻塞主线 |
|---|---|---|---|
| PoC 1 | 源端提前 GTID | 有条件可行。不能依赖 automatic GTID 的最终序号预测，需要在 BRR DDL 进入 prepare 前显式保留/绑定最终 GTID。 | 不阻塞，但必须先做最小源码 PoC |
| PoC 2 | BRR event 非持久传输 | 可行。source dump thread 可以额外发送非持久 event，replica IO thread 可以在写 relay log 前分流。 | 不阻塞 |
| PoC 3 | 备库独立 worker 执行 DDL | 可行。应复用 applier 的 SQL 执行路径，但必须构造 replica worker 语义的 THD/RLI 上下文。 | 不阻塞，但实现复杂 |
| PoC 4 | SQL worker GTID auto-skip | 可行。现有 `gtid_executed` + `gtid_pre_statement_checks()` 已能跳过已执行 GTID。 | 不阻塞 |
| PoC 5 | fallback | prepare 后、replica commit 前可证明 fallback；replica commit 后结果不明确时不能自动 fallback，应进入 `ABORTED`。 | 不阻塞，但边界必须收紧 |

一期可以继续优先支持普通 InnoDB inplace `ALTER TABLE ... ADD INDEX`。暂不建议在 PoC 通过前扩展 Copy DDL 或 rebuild DDL。

## 2. PoC 1：源端提前 GTID

### 代码依据

相关源码：

- `sql/binlog.cc`
  - `MYSQL_BIN_LOG::assign_automatic_gtids_to_flush_group()`
  - `MYSQL_BIN_LOG::write_transaction()`
- `sql/rpl_gtid_execution.cc`
  - `set_gtid_next()`
- `sql/rpl_gtid_state.cc`
  - `Gtid_state::generate_automatic_gtid()`
  - `Gtid_state::acquire_ownership()`
  - `Gtid_state::update_on_commit()`
  - `Gtid_state::update_on_rollback()`

关键代码事实：

- `AUTOMATIC_GTID` 的 GNO 在 binlog flush group 阶段才由 `assign_automatic_gtids_to_flush_group()` 生成。
- `write_transaction()` 注释明确认为 THD 的 GTID 已经在 `assign_automatic_gtids_to_flush_group()` 阶段确定。
- 如果会话使用 `ASSIGNED_GTID`，`assign_automatic_gtids_to_flush_group()` 不再重新生成 GTID，而是要求 `thd->owned_gtid.sidno > 0`。
- `set_gtid_next()` 对未执行且未被其他线程占有的 GTID 会走 ownership 获取；对已执行 GTID 不占有，后续由 skip 逻辑处理。

### 可行性判断

提前 GTID 可行，但方式不能是“预测 automatic GTID”。可行路径应该是：

1. BRR DDL eligibility 通过后，source 在 DDL 长耗时阶段前显式分配或保留一个最终 GTID。
2. 将该 GTID 设置到当前 DDL THD，使后续原始 binlog 写出的 `GTID_EVENT` 使用同一个 GTID。
3. BRR prepare event 携带这个 GTID 给 replica BRR worker。
4. 若 DDL 失败或 BRR prepare 前 fallback，需要通过现有 rollback 路径释放 ownership，不能把 GTID 写入 `executed_gtids`。

### 风险

- 直接使用 `AUTOMATIC_GTID` 无法在 DDL 开始前稳定知道最终 GNO。
- 提前保留 GTID 需要避免与并发事务分配冲突。
- 必须确认 DDL 失败、kill、prepare 发送失败时不会留下 source 侧 GTID ownership。
- 需要确认提前 GTID 不影响普通事务的 group commit 和 binlog cache 逻辑。

### PoC 建议

第 3 周前建议做一个最小源码 PoC：

- 只对 BRR DDL 调用内部 GTID reserve/assign 逻辑。
- 在 `assign_automatic_gtids_to_flush_group()` 和 `write_transaction()` 打点确认原始 binlog 使用同一 GTID。
- 在 DDL 失败路径打点确认 `update_on_rollback()` 释放 ownership 且不进入 `executed_gtids`。

## 3. PoC 2：BRR event 非持久传输

### 代码依据

相关源码：

- `sql/rpl_source.cc`
  - `mysql_binlog_send()`
- `sql/rpl_binlog_sender.cc`
  - `Binlog_sender::run()`
  - `Binlog_sender::send_events()`
  - `Binlog_sender::send_packet()`
  - `Binlog_sender::send_heartbeat_event_v2()`
- `sql/rpl_replica.cc`
  - replica IO thread 主循环
  - `queue_event()`
- `sql/replication.h`
  - `Binlog_relay_IO_observer`

关键代码事实：

- source dump thread 在 `Binlog_sender::send_events()` 中从 binlog 文件读取 event 后调用 `send_packet()` 发给 replica。
- heartbeat event 是一个现有的“非 binlog 文件读取、直接构造网络包发送”的参考模式。
- replica IO thread 在读到 event 后，先执行 `after_read_event` hook，然后调用 `queue_event()`。
- `queue_event()` 是写 relay log 的集中入口，最终调用 `rli->relay_log.write_buffer()`。
- 现有 observer 没有 `before_queue_event`，单靠插件 hook 不能阻止 event 写 relay log，需要在核心路径增加分流。

### 可行性判断

非持久传输可行。建议的 PoC 路径：

1. source 在 `Binlog_sender::send_events()` 中，在原始 binlog event 前额外发送一个 BRR event 网络包。
2. replica IO thread 在 `after_read_event` 之后、`queue_event()` 之前识别 BRR event。
3. 如果是 BRR event，写入内存 BRR queue/cache，跳过 `queue_event()`。
4. 普通 event 保持现有 `queue_event()` 路径，继续写 relay log。

### 风险

- 未协商 BRR capability 的 replica 不能收到 BRR event，否则可能解析失败或写入 relay log。
- BRR event 即使不写 relay log，也要明确是否推进 source 位点，以及如何维护 `Master_info::master_log_pos`。
- 如果 BRR event 参与 transaction parser，必须避免破坏 GTID transaction 边界判断。
- event checksum、Format_description_event 版本、unknown ignorable event 行为需要单独测试。

### PoC 建议

- 先用内部临时 event type + magic payload 验证链路，不急于冻结最终协议。
- source 侧参考 heartbeat 构造 event buffer 和 checksum。
- replica 侧在 IO thread 主循环短路，先只入内存 queue 并记录计数。
- 测试必须确认 BRR event 不出现在 relay log，普通 DDL 的原始 `GTID_EVENT + QUERY_EVENT` 仍正常写入 relay log。

## 4. PoC 3：备库独立 worker 执行 DDL

### 代码依据

相关源码：

- `sql/log_event.cc`
  - `Gtid_log_event::do_apply_event()`
  - `Query_log_event::do_apply_event()`
- `sql/sql_parse.cc`
  - `dispatch_sql_command()`
  - `mysql_execute_command()`
- `sql/sql_alter.cc`
  - `Sql_cmd_alter_table::execute()`
- `sql/sql_table.cc`
  - `mysql_alter_table()`
  - `mysql_inplace_alter_table()`
- `sql/rpl_replica.cc`
  - `init_replica_thread()`
  - `handle_slave_worker()`
  - `handle_slave_sql()`
- `sql/rpl_rli.cc`
  - `Relay_log_info::post_commit()`

关键代码事实：

- `Query_log_event::do_apply_event()` 设置 db、charset、session variables 后，通过 `dispatch_sql_command()` 执行 SQL，注释明确绕过 `dispatch_command()`。
- `ALTER TABLE` 最终进入 `Sql_cmd_alter_table::execute()`，再调用 `mysql_alter_table()`。
- InnoDB inplace DDL 的主体阶段在 `mysql_inplace_alter_table()` 中，核心阶段是 `ha_prepare_inplace_alter_table()`、`ha_inplace_alter_table()`、`ha_commit_inplace_alter_table(commit=true)`。
- replica worker THD 初始化会设置 `system_thread`、`slave_thread`、`rli_slave`、`require_row_format` 等 applier 上下文。
- DDL commit 过程中 `Disable_slave_info_update_guard` 依赖 `thd->rli_slave->current_event` 和 `Query_log_event::has_ddl_committed`。

### 可行性判断

备库独立 worker 执行 DDL 可行，但不能做成普通客户端 THD。BRR worker 应该复用 replica applier 的执行语义：

1. 创建独立 THD。
2. 调用或复用 `init_replica_thread(..., SLAVE_THD_WORKER)` 的初始化逻辑。
3. 设置 `thd->rli_slave`，确保 `rli->info_thd == thd` 或 worker 上下文满足同等断言。
4. 复用 `Gtid_log_event::do_apply_event()` 或等价 `set_gtid_next()` 路径占有 GTID。
5. 构造 Query event 或复用 `Query_log_event::do_apply_event()` 所需的 session context，再执行 `dispatch_sql_command()`。

### ADD INDEX 阶段边界

普通二级索引 `ADD INDEX` 的首选暂停点在 `mysql_inplace_alter_table()`：

- prepare 前：适合 source 发送 BRR prepare。
- `ha_inplace_alter_table()` 后：DDL 主体完成。
- `ha_commit_inplace_alter_table(commit=true)` 前：适合 replica 等待 source result。

这与一期“备库提前执行主体、等待主库结果后再提交”的模型一致。

### 风险

- `thd->rli_slave`、`rli->current_event`、`has_ddl_committed` 不完整会破坏 DDL commit 和 slave info 更新语义。
- commit 前后有 kill 忽略逻辑；进入 commit 后结果不明确时不能随意回滚。
- 非 atomic DDL 或 copy DDL 的中间状态更复杂，一期不应默认支持。
- MTS 下独立 worker 和普通 SQL worker 的调度关系复杂，一期应限制单 channel、单 active BRR DDL。

### PoC 建议

- PoC 只跑单线程复制和单个 `ALTER TABLE ... ADD INDEX`。
- 优先用现有 `DEBUG_SYNC` 点验证 `ha_inplace_alter_table()` 后、`ha_commit_inplace_alter_table()` 前的暂停能力。
- 在 BRR worker 前后断言 `thd->rli_slave != nullptr`、`thd->rli_slave->info_thd == thd`、GTID ownership 状态和 `has_ddl_committed` 状态。

## 5. PoC 4：SQL worker GTID auto-skip

### 代码依据

相关源码：

- `sql/log_event.cc`
  - `Gtid_log_event::do_apply_event()`
- `sql/rpl_gtid_execution.cc`
  - `is_already_logged_transaction()`
  - `gtid_pre_statement_checks()`
  - `skip_statement()`
- `sql/sql_parse.cc`
  - `mysql_execute_command()`
- `sql/rpl_gtid_state.cc`
  - `Gtid_state::update_gtids_impl_own_gtid()`

关键代码事实：

- `Gtid_log_event::do_apply_event()` 调用 `set_gtid_next()` 后，再调用 `gtid_pre_statement_checks()`。
- 当 GTID 已在 `executed_gtids` 中且当前 THD 没有 ownership 时，`is_already_logged_transaction()` 返回 true。
- `gtid_pre_statement_checks()` 对这种情况返回 `GTID_STATEMENT_SKIP`。
- `mysql_execute_command()` 收到 `GTID_STATEMENT_SKIP` 后返回 OK，并调用 `binlog_gtid_end_transaction()`，不会执行 SQL 正文。
- `Gtid_state::update_gtids_impl_own_gtid(..., is_commit=true)` 会把 owned GTID 加入 `executed_gtids` 并释放 ownership。

### 可行性判断

SQL worker auto-skip 无需大改。BRR worker 只要在成功 commit 后正确把同一个 DDL GTID 写入 `gtid_executed`，原始 SQL worker 后续读到 relay log 中的 `GTID_EVENT + QUERY_EVENT` 时就会走现有 skip 逻辑。

这个结论是 BRR 一期可以保持“原始 relay log 坐标仍由 SQL worker 推进”的关键。

### 风险

- 如果 SQL worker 先读到原始 GTID，而 BRR worker 还持有 ownership 未提交，SQL worker 会等待 ownership 释放。
- 如果 BRR worker 错误写入 `gtid_executed` 但 DDL 未真正提交，会造成数据未变但 GTID 已跳过。
- MTS 下需要验证 skip group 的 checkpoint 和 relay log 坐标推进。

### PoC 建议

- 构造一个已提前写入 `gtid_executed` 的 DDL GTID，确认原始 `Query_log_event` 不执行且 relay log 坐标推进。
- 构造 BRR worker 持有 ownership 未释放场景，确认 SQL worker 等待后在 commit 时 skip、在 rollback/fallback 时执行原始 DDL。

## 6. PoC 5：fallback

### 代码依据

相关源码：

- `sql/rpl_gtid_state.cc`
  - `Gtid_state::update_gtids_impl_own_gtid()`
  - `Gtid_state::update_on_rollback()`
- `sql/log_event.cc`
  - `Gtid_log_event::do_apply_event()`
  - `Query_log_event::do_apply_event()`
- `sql/rpl_replica.cc`
  - IO thread 断连和重连逻辑
  - `queue_event()`
- `sql/sql_table.cc`
  - `mysql_inplace_alter_table()` rollback/commit 边界

关键代码事实：

- GTID rollback 路径释放 ownership，但不把 GTID 加入 `executed_gtids`。
- 原始 binlog/relay log 仍存在，BRR event 不持久化。
- SQL worker 在 GTID 未 executed 的情况下会正常执行原始 DDL。
- `mysql_inplace_alter_table()` 在进入 commit 以后，有些错误无法简单安全回滚；源码注释也说明某些阶段后被 kill 仍应继续。

### 可行性判断

fallback 可证明成立的范围是：

- prepare 发送前失败：不进入 BRR，原始复制照常。
- prepare 已发送但 replica 尚未 commit：BRR worker rollback/cleanup，释放 ownership，不写 `executed_gtids`，原始 SQL worker 接管。
- BRR worker 执行主体失败且中间对象可清理：释放 ownership，原始 SQL worker 执行原始 DDL。
- IO 断连或等待 source result 超时：清理后 fallback。

不能自动 fallback 的范围是：

- replica 已经进入本地 DDL commit 且结果不明确。
- DDL 中间对象无法确认清理。
- GTID ownership 释放失败。
- DDL 已提交但 `gtid_executed` 未更新。

这些场景应该进入 `ABORTED`，停止复制并要求人工处理。

### PoC 建议

- 验证 BRR worker 人工失败后，GTID 未进入 `executed_gtids`。
- 验证 SQL worker 后续能执行原始 relay log DDL。
- 验证 cleanup 失败时不释放给 SQL worker 自动执行。
- 验证 source crash、replica crash、IO 断连后不会依赖 BRR 内存状态恢复。

## 7. 第二周完成判定

第二周必须确认的 4 个问题结论如下：

| 问题 | 结论 |
|---|---|
| 提前 GTID 是否可控 | 可控，但需要显式 reserve/assign，不能预测 automatic GTID。 |
| BRR worker 是否可复用现有 SQL 执行路径 | 可以复用 `Query_log_event::do_apply_event()`/`dispatch_sql_command()` 路径，但必须补齐 applier THD/RLI 上下文。 |
| SQL worker skip 是否无需大改 | 无需大改，复用现有 GTID skip。 |
| fallback 是否可证明正确 | 在 replica commit 前可证明；commit 结果不明确后必须停止复制，不能自动 fallback。 |

## 8. 对后续排期的影响

第 3-4 周可以继续实现 BRR event 与实时传输，但建议同步增加以下技术约束：

- source 发送 BRR event 前必须完成 BRR capability 协商。
- BRR event 一期只允许发送给已协商支持的 replica。
- replica 侧分流必须发生在 `queue_event()` 写 relay log 之前。
- BRR DDL 的 GTID 生成需要独立设计最小改造点，并在第 3 周前完成源码 PoC。
- 第 7-8 周的 BRR worker 不能使用普通客户端线程语义，必须按 replica applier worker 语义构造 THD。
- 第 10 周异常恢复需要明确区分 `FALLBACK` 和 `ABORTED`，不能把 commit 结果不明确的场景交给原始 SQL worker 自动执行。
