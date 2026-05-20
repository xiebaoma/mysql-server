# BRR 状态机

## 1. 状态机目标

BRR 状态机用于约束一个 DDL 从 source 进入 BRR，到 replica 预执行、提交、回滚或 fallback 的全过程。

一期状态机只处理：

- 单 channel。
- 单个 in-flight BRR DDL。
- 一个 DDL id 对应一个 GTID。
- BRR event 非持久化。
- 原始 `GTID_EVENT + QUERY_EVENT` 始终保留为最终兜底链路。

## 2. 源码约束

状态机设计基于以下源码事实：

- DDL 执行入口是 `mysql_alter_table()`，InnoDB inplace DDL 的 prepare/main/commit 阶段在 `mysql_inplace_alter_table()` 中串行发生。
- Inplace DDL 主体阶段是 `ha_inplace_alter_table()`，最终提交是 `ha_commit_inplace_alter_table(commit=true)`。
- 原始 DDL 成功后仍由 `write_bin_log()` 写入 binlog。
- Replica IO thread 默认通过 `queue_event()` 写 relay log；BRR event 必须在该路径前分流。
- 原 SQL worker 通过 `Gtid_log_event::do_apply_event()` 设置 `gtid_next`，再由 `gtid_pre_statement_checks()` 判断是否 skip。
- GTID ownership 由 `Gtid_state::acquire_ownership()` 占有，并在 commit/rollback 的 GTID 更新路径释放。

## 3. 全局状态

### 3.1 `INIT`

含义：当前没有 active BRR DDL。

进入条件：

- channel 启动。
- 上一个 BRR DDL 完成、fallback 或 aborted 后清理完成。

允许转换：

- source eligible DDL -> `PREPARE_SENDING`
- replica 收到 prepare -> `PREPARE_RECEIVED`

### 3.2 `FALLBACK`

含义：该 DDL 不再由 BRR 完成，等待或已经交给原始 SQL worker。

进入条件：

- 白名单不通过。
- source prepare 前失败。
- BRR event 解析失败。
- replica worker 执行失败。
- IO 断连。
- dependency 等待超时且策略选择 fallback。
- GTID ownership 释放后不写 `gtid_executed`。

退出条件：

- 原 SQL worker 执行原始 DDL 成功或按原逻辑报错。
- 清理 BRR context。

### 3.3 `ABORTED`

含义：BRR DDL 无法安全 fallback，需要停止复制或人工介入。

进入条件：

- GTID ownership 无法释放。
- DDL 中间对象无法清理且继续执行原始 DDL 可能破坏一致性。
- BRR worker 已提交本地 DDL 但未能正确写入 `gtid_executed`。
- 检测到 source/replica 元数据不一致，不能继续自动执行。

退出条件：

- 人工处理。
- 修复后重新启动复制。

## 4. Source 状态机

### 4.1 Source 状态列表

- `SRC_INIT`
- `SRC_ELIGIBILITY_CHECK`
- `SRC_GTID_RESERVED`
- `SRC_PREPARE_SENDING`
- `SRC_PREPARE_SENT`
- `SRC_EXECUTING`
- `SRC_COMMIT_SENDING`
- `SRC_COMMIT_SENT`
- `SRC_ROLLBACK_SENDING`
- `SRC_ROLLBACK_SENT`
- `SRC_FALLBACK`
- `SRC_DONE`
- `SRC_ABORTED`

### 4.2 Source 转换

| 当前状态 | 事件 | 下一状态 | 动作 |
|---|---|---|---|
| `SRC_INIT` | DDL 进入 `mysql_alter_table()` | `SRC_ELIGIBILITY_CHECK` | 创建临时 BRR 判定上下文 |
| `SRC_ELIGIBILITY_CHECK` | 白名单不通过 | `SRC_FALLBACK` | 记录 fallback reason，不发送 BRR event |
| `SRC_ELIGIBILITY_CHECK` | 白名单通过 | `SRC_GTID_RESERVED` | 提前确定 DDL GTID |
| `SRC_GTID_RESERVED` | GTID 失败 | `SRC_FALLBACK` | 释放上下文，原始 DDL 继续 |
| `SRC_GTID_RESERVED` | GTID 成功 | `SRC_PREPARE_SENDING` | 采集 prepare dependency GTID set |
| `SRC_PREPARE_SENDING` | prepare 入队成功 | `SRC_PREPARE_SENT` | 标记 in-flight DDL |
| `SRC_PREPARE_SENDING` | prepare 入队失败 | `SRC_FALLBACK` | 不创建 in-flight 状态 |
| `SRC_PREPARE_SENT` | 进入长耗时阶段 | `SRC_EXECUTING` | 正常执行 DDL 主体 |
| `SRC_EXECUTING` | DDL 成功 | `SRC_COMMIT_SENDING` | 采集 commit dependency GTID set |
| `SRC_EXECUTING` | DDL 失败或可回滚 kill | `SRC_ROLLBACK_SENDING` | 记录 source error |
| `SRC_COMMIT_SENDING` | commit 入队成功 | `SRC_COMMIT_SENT` | 等待普通 DDL 成功返回 |
| `SRC_COMMIT_SENDING` | commit 入队失败 | `SRC_DONE` | 原始 binlog 仍兜底，replica 超时 fallback |
| `SRC_COMMIT_SENT` | 原始 DDL 成功完成 | `SRC_DONE` | 清理 source BRR context |
| `SRC_ROLLBACK_SENDING` | rollback 入队成功 | `SRC_ROLLBACK_SENT` | 允许 replica 清理 |
| `SRC_ROLLBACK_SENDING` | rollback 入队失败 | `SRC_DONE` | 依赖 replica 断连/超时清理 |
| `SRC_ROLLBACK_SENT` | DDL 返回错误 | `SRC_DONE` | 清理 source BRR context |
| `SRC_FALLBACK` | 原始 DDL 完成 | `SRC_DONE` | 记录 fallback 统计 |

### 4.3 Source 不变量

- `SRC_PREPARE_SENT` 之后，source 必须最终发送 commit 或 rollback；若无法发送，必须保证 replica 可通过断连或超时进入 fallback。
- prepare event 不写 binlog。
- commit/rollback event 不写 binlog。
- 原始 DDL 成功时仍写原始 `GTID_EVENT + QUERY_EVENT`。
- prepare 前 fallback 不得影响原始 DDL 执行。
- source 不等待 replica BRR worker 完成，避免把优化路径变成提交依赖。

## 5. Replica BRR worker 状态机

### 5.1 Replica 状态列表

- `RPL_INIT`
- `RPL_PREPARE_RECEIVED`
- `RPL_VALIDATE`
- `RPL_GTID_OWNING`
- `RPL_WAIT_PREPARE_DEP`
- `RPL_EXECUTING`
- `RPL_WAIT_SOURCE_RESULT`
- `RPL_COMMIT_RECEIVED`
- `RPL_WAIT_COMMIT_DEP`
- `RPL_COMMITTING`
- `RPL_COMMITTED`
- `RPL_ROLLBACK_RECEIVED`
- `RPL_ROLLING_BACK`
- `RPL_FALLBACK`
- `RPL_ABORTED`

### 5.2 Replica 转换

| 当前状态 | 事件 | 下一状态 | 动作 |
|---|---|---|---|
| `RPL_INIT` | IO thread 分流 prepare | `RPL_PREPARE_RECEIVED` | 写入 BRR queue |
| `RPL_PREPARE_RECEIVED` | worker 取到 prepare | `RPL_VALIDATE` | 校验 version、DDL id、GTID、白名单 |
| `RPL_VALIDATE` | 校验失败 | `RPL_FALLBACK` | 记录 reason，不占有 GTID |
| `RPL_VALIDATE` | 校验成功 | `RPL_GTID_OWNING` | 设置 `gtid_next` 并尝试占有 GTID |
| `RPL_GTID_OWNING` | GTID 已 executed | `RPL_COMMITTED` | 说明原始链路已完成，清理 BRR context |
| `RPL_GTID_OWNING` | GTID 被其他线程占有 | `RPL_GTID_OWNING` | 等待或超时 fallback |
| `RPL_GTID_OWNING` | ownership 成功 | `RPL_WAIT_PREPARE_DEP` | 等待 prepare dependency |
| `RPL_WAIT_PREPARE_DEP` | dependency 满足 | `RPL_EXECUTING` | 构造 THD，执行 DDL 主体 |
| `RPL_WAIT_PREPARE_DEP` | stop/timeout | `RPL_FALLBACK` | 释放 ownership |
| `RPL_EXECUTING` | 主体执行成功并暂停 | `RPL_WAIT_SOURCE_RESULT` | 保留 DDL context |
| `RPL_EXECUTING` | 主体执行失败 | `RPL_FALLBACK` | 清理中间对象，释放 ownership |
| `RPL_WAIT_SOURCE_RESULT` | 收到 commit | `RPL_COMMIT_RECEIVED` | 记录 commit dependency |
| `RPL_WAIT_SOURCE_RESULT` | 收到 rollback | `RPL_ROLLBACK_RECEIVED` | 记录 source error |
| `RPL_WAIT_SOURCE_RESULT` | IO 断连/timeout/stop | `RPL_FALLBACK` | 清理并释放 ownership |
| `RPL_COMMIT_RECEIVED` | commit event 校验成功 | `RPL_WAIT_COMMIT_DEP` | 等待 commit dependency |
| `RPL_COMMIT_RECEIVED` | commit event 校验失败 | `RPL_FALLBACK` | 清理并释放 ownership |
| `RPL_WAIT_COMMIT_DEP` | dependency 满足 | `RPL_COMMITTING` | 执行 DDL commit |
| `RPL_WAIT_COMMIT_DEP` | stop/timeout | `RPL_FALLBACK` 或 `RPL_ABORTED` | 按安全策略处理 |
| `RPL_COMMITTING` | DDL commit 成功且 GTID executed 更新成功 | `RPL_COMMITTED` | 释放 ownership，唤醒 SQL worker |
| `RPL_COMMITTING` | DDL commit 失败 | `RPL_ABORTED` | 停止复制，避免重复执行 |
| `RPL_ROLLBACK_RECEIVED` | 可回滚 | `RPL_ROLLING_BACK` | 执行 rollback/cleanup |
| `RPL_ROLLING_BACK` | 清理成功 | `RPL_FALLBACK` | 释放 ownership，等待原始 DDL |
| `RPL_ROLLING_BACK` | 清理失败 | `RPL_ABORTED` | 停止复制 |
| `RPL_FALLBACK` | 原始 SQL worker 接管 | `RPL_INIT` | 清理 BRR queue/context |
| `RPL_COMMITTED` | 原始 SQL worker skip 完成 | `RPL_INIT` | 清理状态 |

### 5.3 Replica 不变量

- 只有 `RPL_GTID_OWNING` 到 `RPL_COMMITTING` 期间可以持有 GTID ownership。
- `RPL_COMMITTED` 必须满足 DDL 已提交且 GTID 在 `gtid_executed` 中。
- `RPL_FALLBACK` 必须满足 GTID 不在 `gtid_executed` 中。
- `RPL_ABORTED` 下不允许原 SQL worker 继续自动执行同一 DDL，除非人工确认中间状态可清理。
- BRR worker 退出前必须释放 ownership 或停止复制。

## 6. SQL worker 协作状态

原 SQL worker 不需要感知完整 BRR 状态，只需要依赖 GTID 现有行为。

### 6.1 SQL worker 读到原始 GTID

可能情况：

- BRR worker 尚未占有 GTID：SQL worker 占有 GTID，BRR worker 后续发现已 owned/executed 后 fallback 或退出。
- BRR worker 正在占有 GTID：`set_gtid_next()` 等待该 GTID 释放。
- BRR worker 已 commit：GTID 已在 `gtid_executed` 中，SQL worker 进入 auto-skip。
- BRR worker 已 fallback：GTID 未 executed，SQL worker 正常执行原始 DDL。

### 6.2 SQL worker 不变量

- 不新增“按 DDL id skip”的特殊逻辑。
- 只以 GTID ownership 和 `gtid_executed` 为准。
- relay log 坐标仍由原 SQL worker 推进。

## 7. Event 顺序

成功路径：

1. Source DDL eligibility 通过。
2. Source 提前确定 GTID。
3. Source 发送 `BRR_DDL_PREPARE_EVENT`。
4. Replica IO thread 分流 prepare 到 BRR queue。
5. BRR worker 占有 GTID，等待 prepare dependency。
6. BRR worker 执行 DDL 主体并暂停。
7. Source DDL 主体成功。
8. Source 发送 `BRR_DDL_COMMIT_EVENT`。
9. BRR worker 等待 commit dependency。
10. BRR worker 提交 DDL，写入 `gtid_executed`，释放 ownership。
11. Source 原始 `GTID_EVENT + QUERY_EVENT` 到达 relay log。
12. SQL worker 读到原始 GTID 并 auto-skip，推进 relay log 坐标。

Source 失败路径：

1. Source 已发送 prepare。
2. Replica BRR worker 可能正在执行或等待。
3. Source DDL 失败，发送 `BRR_DDL_ROLLBACK_EVENT`。
4. BRR worker rollback/cleanup。
5. BRR worker 释放 ownership，不写 `gtid_executed`。
6. 原 SQL worker 等待原始复制链路；如果 source DDL 没有成功 binlog，则不会执行成功 DDL。

Replica 失败路径：

1. Replica 收到 prepare。
2. BRR worker 执行失败或超时。
3. BRR worker 清理中间对象。
4. BRR worker 释放 ownership，不写 `gtid_executed`。
5. 原 SQL worker 后续执行原始 DDL。

## 8. 超时策略

一期建议配置：

- `binlog_realtime_replication_ddl_timeout`
- `binlog_realtime_replication_fallback_on_error`

超时点：

- 等待 GTID ownership。
- 等待 prepare dependency。
- 等待 source commit/rollback。
- 等待 commit dependency。
- DDL rollback/cleanup。

策略：

- 尚未提交本地 DDL：优先 fallback。
- 已进入本地 commit 且结果不明确：停止复制并进入 `ABORTED`。
- ownership 释放失败：停止复制。

## 9. 状态展示

建议展示字段：

- `channel_name`
- `brr_enabled`
- `brr_capability_negotiated`
- `active_ddl_id`
- `active_gtid`
- `source_state`
- `replica_state`
- `ddl_query_digest`
- `prepare_dependency_gtid_set`
- `commit_dependency_gtid_set`
- `owned_gtid`
- `last_fallback_reason`
- `last_error_code`
- `last_error_message`
- `last_state_change_time`

## 10. 第一阶段验收

状态机设计冻结时必须确认：

- 每个状态只有一个明确 owner：source、replica BRR worker 或 SQL worker。
- 每个失败分支都能走到 `FALLBACK` 或 `ABORTED`。
- 进入 `FALLBACK` 前 GTID ownership 处理规则明确。
- 进入 `COMMITTED` 前 `gtid_executed` 更新规则明确。
- source 不因为 replica BRR worker 慢而阻塞 DDL 成功返回。
- SQL worker 推进 relay log 坐标的职责不被 BRR worker 接管。
