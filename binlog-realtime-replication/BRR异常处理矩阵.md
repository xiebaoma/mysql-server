# BRR 异常处理矩阵

## 1. 处理原则

BRR 一期异常处理的优先级：

1. 保证复制正确性。
2. 保证原始 binlog/relay log 链路可兜底。
3. 能自动清理则 fallback。
4. 不能证明自动清理安全则停止复制并报错。

核心规则：

- prepare 前失败：不进入 BRR。
- prepare 后、replica commit 前失败：优先 rollback/cleanup 并 fallback。
- replica commit 后失败：如果 GTID/DDL 提交状态不明确，进入 `ABORTED`，停止复制。
- fallback 时不得把 DDL GTID 加入 `gtid_executed`。
- committed 时必须把 DDL GTID 加入 `gtid_executed`。

## 2. 源码相关风险点

BRR 异常处理必须贴合以下源码行为：

- `mysql_inplace_alter_table()` 在 `ha_inplace_alter_table()` 后进入最终 `ha_commit_inplace_alter_table(commit=true)`，本地 commit 后再发生错误时清理复杂。
- InnoDB 支持 `HTON_SUPPORTS_ATOMIC_DDL`，但 BRR 不能假设所有错误都能跨 source/replica 对称回滚。
- Replica IO thread 默认 `queue_event()` 后 event 已进入 relay log；BRR event 必须在写 relay log 前分流，分流失败不能污染 relay log。
- `set_gtid_next()` 遇到已 owned GTID 会等待，replica stop 时依赖 `abort_slave` 退出。
- `gtid_pre_statement_checks()` 对已 executed GTID 返回 skip，SQL worker skip 是 BRR 成功后的兜底推进机制。
- `Gtid_state::update_gtids_impl_own_gtid()` commit 时写 executed，rollback 时释放 ownership 但不写 executed。

## 3. Source 侧异常矩阵

| 阶段 | 异常 | 处理 | GTID 处理 | 原始复制影响 | 观测信息 |
|---|---|---|---|---|---|
| capability 协商 | replica 不支持 BRR | 不发送 BRR event | 不提前占有 | 原始复制 | `BRR_CAPABILITY_NOT_NEGOTIATED` |
| eligibility | 白名单不通过 | prepare 前 fallback | 不提前占有 | 原始复制 | 具体 fallback reason |
| eligibility | 判断信息不完整 | prepare 前 fallback | 不提前占有 | 原始复制 | `UNSAFE_DDL_PHASE` |
| 提前 GTID | GTID 分配失败 | prepare 前 fallback；必要时报错 | 不保留 BRR GTID | 原始复制或语句失败 | `GTID_PREALLOCATE_FAILED` |
| prepare 构造 | query/session/dependency 过大 | prepare 前 fallback | 释放 BRR context | 原始复制 | `QUERY_TOO_LARGE` 或 `DEPENDENCY_GTID_SET_TOO_LARGE` |
| prepare 发送 | BRR buffer 入队失败 | prepare 前 fallback | 释放 BRR context | 原始复制 | `PREPARE_SEND_FAILED` |
| prepare 已发送 | source DDL 主体失败 | 发送 rollback | source 按原 DDL 失败处理 | 成功 DDL 不写 binlog | source error code |
| prepare 已发送 | source DDL 被 kill，仍可回滚 | 发送 rollback | source 按 kill/rollback 处理 | 原始链路不写成功 DDL | `SOURCE_DDL_KILLED` |
| prepare 已发送 | source crash | 无法发送 rollback | replica 断连/超时 fallback | crash recovery 后按原始 binlog | `SOURCE_DISCONNECT` |
| commit 发送 | commit event 入队失败 | 不阻塞 source；replica 超时 fallback 或 SQL worker 等待后执行原始 DDL | source 正常提交 | 原始 DDL 仍写 binlog | `COMMIT_SEND_FAILED` |
| commit 已发送 | source 后续发现 DDL 失败 | 一期设计禁止此发送点；若发生视为 bug | 状态不可信 | 停止相关 channel | `SOURCE_COMMIT_PROTOCOL_VIOLATION` |
| rollback 发送 | rollback 入队失败 | 依赖 replica 断连/超时清理 | source 按失败处理 | 原始链路兜底 | `ROLLBACK_SEND_FAILED` |

## 4. Replica IO thread 异常矩阵

| 阶段 | 异常 | 处理 | GTID 处理 | Relay log 影响 | 观测信息 |
|---|---|---|---|---|---|
| 读取网络包 | 普通网络错误 | 走现有 reconnect | 无 BRR 特殊处理 | 普通 event 按现有逻辑 | 现有 IO error |
| BRR event 识别 | event type/version 不兼容 | 丢弃 BRR event，标记 DDL fallback | 不占有 | 不写 relay log | `BRR_PROTOCOL_VERSION_MISMATCH` |
| BRR event 解码 | length/checksum/payload 错误 | 丢弃 BRR event，标记 fallback | 不占有或通知 worker 清理 | 不写 relay log | `BRR_EVENT_DECODE_ERROR` |
| BRR queue | 内存不足 | 标记 fallback；必要时停止 IO | 不占有或释放 | 不写 relay log | `BRR_QUEUE_OOM` |
| BRR queue | 已有 active DDL | 新 DDL fallback | 不占有 | 不写 BRR event 到 relay log | `ANOTHER_BRR_DDL_RUNNING` |
| 普通 event | `queue_event()` 失败 | 现有复制报错 | 由原逻辑处理 | relay log 写失败 | 现有 relay log error |
| IO 断连 | prepare 已收到但 result 未收到 | 唤醒 worker fallback | worker 释放 ownership | 原始 relay log 后续重连兜底 | `SOURCE_DISCONNECT` |

## 5. Replica BRR worker 异常矩阵

| 阶段 | 异常 | 处理 | GTID 处理 | SQL worker 后续 | 观测信息 |
|---|---|---|---|---|---|
| validate | DDL id 不匹配 | fallback | 不占有 | 原始 DDL 执行 | `DDL_ID_MISMATCH` |
| validate | GTID 缺失或非法 | fallback 或停止复制 | 不占有 | 原始 DDL 执行或报错 | `INVALID_GTID` |
| validate | 本地白名单不通过 | fallback | 不占有 | 原始 DDL 执行 | `REPLICA_UNSUPPORTED_DDL` |
| GTID owning | GTID 已 executed | 直接完成并清理 | 不占有 | SQL worker 已完成或将 skip | `GTID_ALREADY_EXECUTED` |
| GTID owning | GTID 被其他线程占有 | 等待；超时 fallback | 不抢占 | 原线程决定结果 | `GTID_OWNERSHIP_WAIT` |
| GTID owning | ownership 获取失败 | fallback | 不占有 | 原始 DDL 执行 | `GTID_OWNERSHIP_FAILED` |
| prepare dependency | 等待超时 | fallback 或停止复制 | 释放 ownership | 原始 DDL 执行 | `PREPARE_DEPENDENCY_TIMEOUT` |
| prepare dependency | STOP REPLICA | 退出并 fallback | 释放 ownership | START 后原始 DDL 执行 | `REPLICA_STOPPED` |
| DDL 主体 | 执行报错 | cleanup 后 fallback | 释放 ownership，不写 executed | 原始 DDL 执行 | replica error code |
| DDL 主体 | 被 kill/stop | rollback/cleanup 后 fallback | 释放 ownership | 原始 DDL 执行 | `BRR_WORKER_KILLED` |
| DDL 主体 | 中间对象清理失败 | `ABORTED` | 尽量释放；不确定则停止复制 | 禁止自动执行 | `BRR_CLEANUP_FAILED` |
| 等 source result | 收到 rollback | rollback/cleanup，fallback | 释放 ownership | 视原始 binlog 是否存在 | source error code |
| 等 source result | timeout | rollback/cleanup，fallback | 释放 ownership | 原始 DDL 执行 | `SOURCE_RESULT_TIMEOUT` |
| 等 source result | IO 断连 | rollback/cleanup，fallback | 释放 ownership | 重连后原始 DDL 执行 | `SOURCE_DISCONNECT` |
| commit dependency | 等待超时 | 若未 commit，fallback；否则 aborted | 按阶段处理 | fallback 或停止复制 | `COMMIT_DEPENDENCY_TIMEOUT` |
| commit | DDL commit 失败 | `ABORTED` | 不写 executed；状态可能不确定 | 停止复制 | replica error code |
| GTID executed | 更新失败 | `ABORTED` | 状态不可信 | 停止复制 | `GTID_EXECUTED_UPDATE_FAILED` |
| cleanup | ownership 释放失败 | `ABORTED` | 状态不可信 | 停止复制 | `GTID_OWNERSHIP_LEAK` |

## 6. SQL worker 异常矩阵

| 阶段 | 异常 | 处理 | BRR 关系 |
|---|---|---|---|
| 读到原始 GTID | BRR worker 持有同一 GTID | 等待现有 GTID ownership 释放 | 不新增特殊逻辑 |
| 读到原始 GTID | BRR 已 commit | `gtid_pre_statement_checks()` skip | 推进 relay log 坐标 |
| 读到原始 GTID | BRR 已 fallback | 正常执行原始 DDL | 兜底执行 |
| 执行原始 DDL | 原始 DDL 报错 | 按现有复制错误处理 | BRR 不吞错 |
| STOP REPLICA | SQL worker 等待 GTID | 通过现有 `abort_slave` 退出等待 | BRR worker 也必须退出 |

## 7. Crash/restart 矩阵

| 场景 | crash 点 | 恢复策略 | GTID 处理 | 风险等级 |
|---|---|---|---|---|
| source crash | prepare 前 | 无 BRR 状态 | 无 | 低 |
| source crash | prepare 后、DDL 主体中 | replica 断连后 cleanup/fallback | replica 释放 ownership | 中 |
| source crash | commit event 前 | replica timeout/fallback；source recovery 按原始 binlog | replica 不写 executed | 中 |
| source crash | commit event 后 | replica 可能已提交；原始 binlog recovery 必须与 source 成功状态一致 | 已 commit 则 executed | 高，需故障注入 |
| replica crash | prepare 前 | 无 BRR 状态 | 无 | 低 |
| replica crash | prepare dependency 等待中 | 启动后内存状态丢失，原始 relay log 兜底 | 未写 executed | 中 |
| replica crash | DDL 主体执行中 | 启动时清理 BRR 中间对象，原始 relay log 兜底 | 未写 executed | 高 |
| replica crash | commit 前暂停 | 启动时清理 BRR 中间对象或停止复制 | 不确定则人工处理 | 高 |
| replica crash | commit 后、GTID executed 前 | 状态不确定，停止复制 | 需要人工确认 | 最高 |
| replica crash | GTID executed 后 | 原始 SQL worker skip | executed 已持久化 | 中 |

## 8. 中间对象清理

BRR worker 可能在 replica 产生 DDL 中间对象。设计要求：

- 所有 BRR 专用临时对象命名必须带 `ddl_id` 或可从 DD/SE 元数据识别。
- cleanup 必须幂等。
- cleanup 成功前不得释放给 SQL worker 自动执行同一 DDL，除非确认原始 DDL 可覆盖该中间状态。
- server startup 时不恢复 in-flight BRR DDL，只扫描并清理可识别中间对象。
- 无法识别或无法清理时进入 `ABORTED`。

普通 InnoDB inplace `ADD INDEX` 优先进入一期，是因为它的源码阶段相对明确；但仍必须通过故障注入验证 `ha_inplace_alter_table()` 成功后、`ha_commit_inplace_alter_table()` 前 crash/kill 的清理路径。

## 9. Timeout 策略

建议默认：

- 等待 GTID ownership：短超时，可 fallback。
- 等待 prepare dependency：中等超时，可 fallback。
- 等待 source result：长超时，可 fallback。
- 等待 commit dependency：中等超时；如果尚未本地 commit，可 fallback。
- commit 执行中：不允许简单超时 kill；结果不明确时停止复制。

配置项：

- `binlog_realtime_replication_ddl_timeout`
- `binlog_realtime_replication_dependency_timeout`
- `binlog_realtime_replication_source_result_timeout`
- `binlog_realtime_replication_fallback_on_error`

## 10. 错误等级

### 10.1 Warning 并 fallback

- 白名单不通过。
- capability 未协商。
- BRR event 解码失败但 GTID 未占有。
- replica 本地不支持该 DDL。
- dependency 等待超时且尚未执行主体。
- BRR worker 主体执行失败且 cleanup 成功。

### 10.2 Error 并停止复制

- GTID ownership 泄漏。
- 本地 DDL commit 结果不明确。
- DDL 已提交但 `gtid_executed` 未更新。
- cleanup 失败且原始 DDL 可能重复修改对象。
- 检测到 source/replica 表元数据不一致。

### 10.3 Fatal / 需要人工介入

- replica crash 后发现 BRR 中间状态不可判定。
- source commit event 与原始 binlog 最终结果矛盾。
- BRR worker 已提交 DDL，但 SQL worker 后续未能 skip 同一 GTID。

## 11. 故障注入点

Source：

- prepare 发送前失败。
- prepare 发送后 crash。
- prepare 发送后 DDL 主体失败。
- commit event 发送失败。
- rollback event 发送失败。
- `write_bin_log()` 前后失败。

Replica IO：

- BRR event 半包/坏 checksum。
- BRR event version 不兼容。
- BRR queue OOM。
- prepare 后 IO 断连。
- commit/rollback 丢失。

Replica worker：

- GTID ownership 等待超时。
- prepare dependency 永不满足。
- `ha_inplace_alter_table()` 失败。
- 主体成功后 crash。
- commit 前 crash。
- commit 失败。
- `gtid_executed` 更新失败。
- cleanup 失败。

SQL worker：

- 等待 BRR ownership 时 STOP REPLICA。
- BRR commit 后原始 GTID 到达。
- BRR fallback 后原始 Query event 到达。

## 12. 一期验收标准

异常处理矩阵冻结时必须满足：

- 每个异常都有明确 fallback、rollback、停止复制或人工介入策略。
- 每个 fallback 分支都说明 GTID 是否写入 `gtid_executed`。
- 每个需要停止复制的分支都有明确原因，不能静默 fallback。
- 每个 source prepare 后异常都能让 replica 最终解除等待。
- 每个 replica worker 异常都不会污染持久 relay log。
- crash 后不依赖非持久 BRR event 恢复正确性。
