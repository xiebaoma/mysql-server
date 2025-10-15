# SHOW MASTER STATUS 执行流程与调用栈

## 概述

`SHOW MASTER STATUS` 用于显示MySQL主服务器的二进制日志（binlog）状态，包括当前binlog文件名、位置、过滤规则和已执行的GTID集合。

---

## 完整函数调用栈

```
1. dispatch_command()                           [sql/sql_parse.cc]
   ↓
2. mysql_parse()                                [sql/sql_parse.cc]
   ↓
3. mysql_execute_command()                      [sql/sql_parse.cc]
   ↓
4. lex->m_sql_cmd->execute(thd)                [sql/sql_parse.cc:4724]
   ↓
5. Sql_cmd_show_master_status::execute()       [sql/sql_show.h]
   ├─> check_privileges()                       [sql/sql_show.cc:555]
   │   └─> check_global_access(SUPER_ACL | REPL_CLIENT_ACL)
   │
   └─> execute_inner()                          [sql/sql_show.cc:559]
       ↓
6. show_master_status()                         [sql/rpl_source.cc:1267]
   ├─> get_executed_gtids()                     [获取GTID集合]
   ├─> send_result_metadata()                   [发送列元数据]
   ├─> mysql_bin_log.get_current_log()          [获取binlog信息]
   │   └─> raw_get_current_log()                [sql/binlog.cc:5447]
   │       ├─> strmake(log_file_name)           [复制文件名]
   │       └─> m_binlog_file->position()        [获取当前位置]
   │
   ├─> binlog_filter->get_do_db()               [获取过滤规则]
   ├─> binlog_filter->get_ignore_db()           [获取过滤规则]
   ├─> protocol->store()                        [发送数据行]
   └─> my_eof()                                 [发送结束标记]
```

---

## 详细执行流程

### 1. SQL解析与分发层

#### 1.1 词法/语法分析

**文件位置**: `sql/sql_yacc.yy`

```yacc
// sql/sql_yacc.yy: 13850-13853
show_master_status_stmt:
  SHOW_SYM MASTER_SYM STATUS_SYM
  {
    $$ = NEW_PTN PT_show_master_status(@$);
  }
```

**解析树节点**:
```cpp
// sql/parse_tree_nodes.h: 3504-3512
class PT_show_master_status final : public PT_show_base {
public:
  PT_show_master_status(const POS &pos)
      : PT_show_base(pos, SQLCOM_SHOW_MASTER_STAT) {}

  Sql_cmd *make_cmd(THD *thd) override;

private:
  Sql_cmd_show_master_status m_sql_cmd;
};
```

**make_cmd实现**:
```cpp
// sql/parse_tree_nodes.cc: 2706-2711
Sql_cmd *PT_show_master_status::make_cmd(THD *thd) {
  LEX *lex = thd->lex;
  lex->sql_command = m_sql_command;  // SQLCOM_SHOW_MASTER_STAT
  
  return &m_sql_cmd;
}
```

**关键点**:
- SQL命令类型: `SQLCOM_SHOW_MASTER_STAT`
- 创建执行器: `Sql_cmd_show_master_status`

---

### 2. 命令执行入口

#### 2.1 mysql_execute_command

**文件位置**: `sql/sql_parse.cc`

```cpp
// sql/sql_parse.cc: 4694-4726
case SQLCOM_SHOW_MASTER_STAT:
case SQLCOM_SHOW_SLAVE_STAT:
case SQLCOM_SHOW_PRIVILEGES:
// ... 其他SHOW命令
{
  assert(lex->m_sql_cmd != nullptr);
  
  // ★ 调用Sql_cmd的execute方法 ★
  res = lex->m_sql_cmd->execute(thd);
  
  break;
}
```

**命令标志**:
```cpp
// sql/sql_parse.cc: 653
sql_command_flags[SQLCOM_SHOW_MASTER_STAT] = CF_STATUS_COMMAND;
```

- `CF_STATUS_COMMAND`: 标记为状态查询命令
- 不修改数据，只读操作

---

### 3. 权限检查与执行

#### 3.1 Sql_cmd_show_master_status

**文件位置**: `sql/sql_show.h` 和 `sql/sql_show.cc`

**类定义**:
```cpp
// sql/sql_show.h: 442-445
class Sql_cmd_show_master_status : public Sql_cmd_show_noplan {
public:
  Sql_cmd_show_master_status() 
    : Sql_cmd_show_noplan(SQLCOM_SHOW_MASTER_STAT) {}

  bool check_privileges(THD *thd) override;
  bool execute_inner(THD *thd) override;
};
```

**权限检查**:
```cpp
// sql/sql_show.cc: 555-557
bool Sql_cmd_show_master_status::check_privileges(THD *thd) {
  // 需要 SUPER 或 REPL_CLIENT 权限
  return check_global_access(thd, SUPER_ACL | REPL_CLIENT_ACL);
}
```

**执行入口**:
```cpp
// sql/sql_show.cc: 559-561
bool Sql_cmd_show_master_status::execute_inner(THD *thd) {
  return show_master_status(thd);
}
```

---

### 4. 核心实现：show_master_status

**文件位置**: `sql/rpl_source.cc`

```cpp
// sql/rpl_source.cc: 1267-1317
bool show_master_status(THD *thd) {
  Protocol *protocol = thd->get_protocol();
  char *gtid_set_buffer = nullptr;
  int gtid_set_size = 0;

  DBUG_TRACE;

  // ========================================
  // Step 1: 获取GTID集合
  // ========================================
  global_sid_lock->wrlock();
  const Gtid_set *gtid_set = gtid_state->get_executed_gtids();
  if ((gtid_set_size = gtid_set->to_string(&gtid_set_buffer)) < 0) {
    global_sid_lock->unlock();
    my_eof(thd);
    my_free(gtid_set_buffer);
    return true;
  }
  global_sid_lock->unlock();

  // ========================================
  // Step 2: 定义结果集列
  // ========================================
  mem_root_deque<Item *> field_list(thd->mem_root);
  field_list.push_back(new Item_empty_string("File", FN_REFLEN));
  field_list.push_back(
      new Item_return_int("Position", 20, MYSQL_TYPE_LONGLONG));
  field_list.push_back(new Item_empty_string("Binlog_Do_DB", 255));
  field_list.push_back(new Item_empty_string("Binlog_Ignore_DB", 255));
  field_list.push_back(
      new Item_empty_string("Executed_Gtid_Set", gtid_set_size));

  // ========================================
  // Step 3: 发送列元数据
  // ========================================
  if (thd->send_result_metadata(field_list,
                                Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF)) {
    my_free(gtid_set_buffer);
    return true;
  }
  
  protocol->start_row();

  // ========================================
  // Step 4: 获取binlog信息并发送数据行
  // ========================================
  if (mysql_bin_log.is_open()) {
    LOG_INFO li;
    
    // ★ 获取当前binlog文件和位置 ★
    mysql_bin_log.get_current_log(&li);
    
    // 去除目录前缀，只保留文件名
    size_t dir_len = dirname_length(li.log_file_name);
    
    // 发送各列数据
    protocol->store(li.log_file_name + dir_len, &my_charset_bin);  // File
    protocol->store((ulonglong)li.pos);                            // Position
    store(protocol, binlog_filter->get_do_db());                   // Binlog_Do_DB
    store(protocol, binlog_filter->get_ignore_db());               // Binlog_Ignore_DB
    protocol->store(gtid_set_buffer, &my_charset_bin);             // Executed_Gtid_Set
    
    if (protocol->end_row()) {
      my_free(gtid_set_buffer);
      return true;
    }
  }
  
  // ========================================
  // Step 5: 发送结束标记
  // ========================================
  my_eof(thd);
  my_free(gtid_set_buffer);
  return false;
}
```

**返回的5列数据**:
1. **File**: binlog文件名（如 `binlog.000003`）
2. **Position**: 当前写入位置（字节偏移）
3. **Binlog_Do_DB**: binlog过滤规则（包含的数据库）
4. **Binlog_Ignore_DB**: binlog过滤规则（忽略的数据库）
5. **Executed_Gtid_Set**: 已执行的GTID集合

---

### 5. 获取Binlog当前状态

#### 5.1 get_current_log

**文件位置**: `sql/binlog.cc`

```cpp
// sql/binlog.cc: 5439-5445
int MYSQL_BIN_LOG::get_current_log(LOG_INFO *linfo,
                                   bool need_lock_log /*true*/) {
  // 加锁保护
  if (need_lock_log) 
    mysql_mutex_lock(&LOCK_log);
  
  int ret = raw_get_current_log(linfo);
  
  if (need_lock_log) 
    mysql_mutex_unlock(&LOCK_log);
  
  return ret;
}
```

#### 5.2 raw_get_current_log

```cpp
// sql/binlog.cc: 5447-5453
int MYSQL_BIN_LOG::raw_get_current_log(LOG_INFO *linfo) {
  // 复制当前binlog文件名
  strmake(linfo->log_file_name, log_file_name,
          sizeof(linfo->log_file_name) - 1);
  
  // 获取当前写入位置
  linfo->pos = m_binlog_file->position();
  
  // 获取加密头大小（如果启用了binlog加密）
  linfo->encrypted_header_size = m_binlog_file->get_encrypted_header_size();
  
  return 0;
}
```

**关键变量**:
- `log_file_name`: MYSQL_BIN_LOG的成员变量，存储当前binlog文件名
- `m_binlog_file`: Binlog_ofile对象，管理binlog文件IO
- `position()`: 返回当前文件的写入位置（字节偏移）

---

### 6. 获取GTID信息

#### 6.1 get_executed_gtids

**文件位置**: `sql/rpl_gtid_set.cc` 和 `sql/rpl_gtid_state.cc`

```cpp
// 获取全局已执行的GTID集合
global_sid_lock->wrlock();
const Gtid_set *gtid_set = gtid_state->get_executed_gtids();

// 转换为字符串格式
int gtid_set_size = gtid_set->to_string(&gtid_set_buffer);

global_sid_lock->unlock();
```

**GTID格式示例**:
```
3E11FA47-71CA-11E1-9E33-C80AA9429562:1-5,
4E11FA47-71CA-11E1-9E33-C80AA9429562:1-3
```

---

### 7. 获取Binlog过滤规则

#### 7.1 binlog_filter

**文件位置**: `sql/rpl_filter.h` 和 `sql/rpl_filter.cc`

```cpp
// 获取需要记录binlog的数据库列表
binlog_filter->get_do_db()

// 获取需要忽略binlog的数据库列表
binlog_filter->get_ignore_db()
```

**配置参数**:
- `--binlog-do-db=db_name`: 只记录指定数据库
- `--binlog-ignore-db=db_name`: 忽略指定数据库

---

### 8. 数据结构

#### 8.1 LOG_INFO

**文件位置**: `sql/binlog.h`

```cpp
// sql/binlog.h: 115-133
struct LOG_INFO {
  char log_file_name[FN_REFLEN] = {0};  // binlog文件名
  my_off_t index_file_offset;           // index文件偏移
  my_off_t index_file_start_offset;     
  my_off_t pos;                         // binlog文件中的位置
  bool fatal;                           // 致命错误标志
  int entry_index;                      // 用于purge_logs
  int encrypted_header_size;            // 加密头大小
  my_thread_id thread_id;               // 线程ID
  
  LOG_INFO()
      : index_file_offset(0),
        index_file_start_offset(0),
        pos(0),
        fatal(false),
        entry_index(0),
        encrypted_header_size(0),
        thread_id(0) {
    memset(log_file_name, 0, FN_REFLEN);
  }
};
```

#### 8.2 MYSQL_BIN_LOG

**文件位置**: `sql/binlog.h`

```cpp
// sql/binlog.h: 139-963 (简化)
class MYSQL_BIN_LOG : public TC_LOG {
private:
  mysql_mutex_t LOCK_log;           // 保护binlog操作的互斥锁
  char *name;                       // binlog basename
  char log_file_name[FN_REFLEN];   // 当前binlog文件全路径
  Binlog_ofile *m_binlog_file;     // binlog文件IO对象
  
public:
  int get_current_log(LOG_INFO *linfo, bool need_lock_log = true);
  int raw_get_current_log(LOG_INFO *linfo);
  bool is_open() const;
  my_off_t position();
  // ...
};
```

---

## 完整时序图

```
客户端                SQL Parser            Sql_cmd              show_master_status      MYSQL_BIN_LOG
  |                      |                     |                         |                      |
  | SHOW MASTER STATUS   |                     |                         |                      |
  |--------------------->|                     |                         |                      |
  |                      |                     |                         |                      |
  |                      | 词法/语法分析        |                         |                      |
  |                      | SQLCOM_SHOW_MASTER_STAT                       |                      |
  |                      |-------------------->|                         |                      |
  |                      |                     |                         |                      |
  |                      |                     | check_privileges()      |                      |
  |                      |                     | (SUPER_ACL | REPL_CLIENT_ACL)                  |
  |                      |                     |                         |                      |
  |                      |                     | execute_inner()         |                      |
  |                      |                     |------------------------>|                      |
  |                      |                     |                         |                      |
  |                      |                     |                         | 获取GTID集合          |
  |                      |                     |                         | gtid_state->get_executed_gtids()
  |                      |                     |                         |                      |
  |                      |                     |                         | 发送列元数据          |
  |                      |                     |                         | send_result_metadata()|
  |                      |                     |                         |                      |
  |                      |                     |                         | get_current_log()    |
  |                      |                     |                         |--------------------->|
  |                      |                     |                         |                      |
  |                      |                     |                         |   lock(&LOCK_log)    |
  |                      |                     |                         |   复制log_file_name   |
  |                      |                     |                         |   position()         |
  |                      |                     |                         |   unlock(&LOCK_log)  |
  |                      |                     |                         |<---------------------|
  |                      |                     |                         |   return LOG_INFO    |
  |                      |                     |                         |                      |
  |                      |                     |                         | 获取过滤规则          |
  |                      |                     |                         | get_do_db()          |
  |                      |                     |                         | get_ignore_db()      |
  |                      |                     |                         |                      |
  |                      |                     |                         | 发送数据行            |
  |                      |                     |                         | protocol->store()    |
  |                      |                     |                         | protocol->end_row()  |
  |                      |                     |                         |                      |
  |                      |                     |                         | 发送EOF              |
  |                      |                     |                         | my_eof()             |
  |                      |                     |<------------------------|                      |
  |                      |<--------------------|                         |                      |
  |<---------------------|                     |                         |                      |
  | 结果集返回            |                     |                         |                      |
```

---

## 关键代码路径

### 路径1: SQL解析到执行

```
sql_yacc.yy (Bison语法文件)
  ↓
PT_show_master_status::make_cmd()
  ↓
Sql_cmd_show_master_status::execute()
  ├─> check_privileges()
  └─> execute_inner()
      ↓
show_master_status()
```

### 路径2: 获取Binlog状态

```
show_master_status()
  ↓
mysql_bin_log.get_current_log()
  ↓
MYSQL_BIN_LOG::get_current_log()
  ├─> mysql_mutex_lock(&LOCK_log)
  ├─> raw_get_current_log()
  │   ├─> strmake(log_file_name)
  │   └─> m_binlog_file->position()
  └─> mysql_mutex_unlock(&LOCK_log)
```

### 路径3: 获取GTID状态

```
show_master_status()
  ↓
global_sid_lock->wrlock()
  ↓
gtid_state->get_executed_gtids()
  ↓
gtid_set->to_string()
  ↓
global_sid_lock->unlock()
```

---

## 实际执行示例

### 输入SQL

```sql
SHOW MASTER STATUS;
```

### 输出结果

```
+------------------+----------+--------------+------------------+-------------------------------------------+
| File             | Position | Binlog_Do_DB | Binlog_Ignore_DB | Executed_Gtid_Set                         |
+------------------+----------+--------------+------------------+-------------------------------------------+
| binlog.000003    | 157      |              |                  | 3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5  |
+------------------+----------+--------------+------------------+-------------------------------------------+
```

### 字段含义

1. **File**: `binlog.000003`
   - 当前正在写入的binlog文件名
   - 来源: `MYSQL_BIN_LOG::log_file_name`

2. **Position**: `157`
   - 当前binlog文件的写入位置（字节）
   - 来源: `m_binlog_file->position()`
   - 这是下一条事件将要写入的位置

3. **Binlog_Do_DB**: (空)
   - 需要记录binlog的数据库列表
   - 来源: `binlog_filter->get_do_db()`

4. **Binlog_Ignore_DB**: (空)
   - 需要忽略binlog的数据库列表
   - 来源: `binlog_filter->get_ignore_db()`

5. **Executed_Gtid_Set**: `3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5`
   - 已执行的GTID集合
   - 来源: `gtid_state->get_executed_gtids()`
   - 格式: `server_uuid:transaction_id_ranges`

---

## 锁机制

### 1. LOCK_log (binlog锁)

```cpp
// 保护binlog文件操作
mysql_mutex_lock(&LOCK_log);
// 读取log_file_name和position
mysql_mutex_unlock(&LOCK_log);
```

**保护内容**:
- binlog文件名
- binlog写入位置
- binlog文件切换操作

### 2. global_sid_lock (GTID锁)

```cpp
// 保护GTID状态读取
global_sid_lock->wrlock();
// 读取executed_gtids
global_sid_lock->unlock();
```

**保护内容**:
- GTID集合
- Server UUID
- GTID状态更新

---

## 性能考虑

### 1. 命令特点

- **只读操作**: 不修改任何数据
- **快速执行**: 通常 < 1ms
- **轻量级锁**: 短时间持有锁

### 2. 锁的影响

```
LOCK_log 持有时间:
  - 复制文件名: ~1μs
  - 读取position: ~1μs
  - 总计: < 10μs

global_sid_lock 持有时间:
  - 复制GTID集合: ~100μs (取决于GTID数量)
  - 格式化为字符串: ~1ms
```

### 3. 并发影响

- **读读不互斥**: 多个线程可以同时执行 SHOW MASTER STATUS
- **读写互斥**: 在持有 LOCK_log 期间，binlog写入会短暂等待
- **影响极小**: 因为持锁时间极短

---

## 使用场景

### 1. 主从复制

```bash
# 在主库执行
mysql> SHOW MASTER STATUS;
+------------------+----------+
| File             | Position |
+------------------+----------+
| binlog.000003    | 157      |
+------------------+----------+

# 在从库配置
CHANGE MASTER TO
  MASTER_LOG_FILE='binlog.000003',
  MASTER_LOG_POS=157;
```

### 2. 备份一致性

```bash
# 在备份前记录binlog位置
FLUSH TABLES WITH READ LOCK;
SHOW MASTER STATUS;
# 记录File和Position
# 执行物理备份
UNLOCK TABLES;
```

### 3. GTID复制

```sql
-- 查看已执行的GTID
SHOW MASTER STATUS;
-- 使用Executed_Gtid_Set配置从库
```

### 4. 监控告警

```bash
# 定期检查binlog位置
while true; do
  mysql -e "SHOW MASTER STATUS\G" | grep Position
  sleep 5
done
```

---

## 错误处理

### 1. 权限不足

```sql
mysql> SHOW MASTER STATUS;
ERROR 1227 (42000): Access denied; you need (at least one of) 
the SUPER or REPL_CLIENT privilege(s) for this operation
```

**解决方案**:
```sql
GRANT REPL_CLIENT ON *.* TO 'user'@'host';
```

### 2. Binlog未开启

```sql
mysql> SHOW MASTER STATUS;
Empty set (0.00 sec)
```

**原因**: `log_bin=OFF`

**解决方案**:
```ini
[mysqld]
log_bin=ON
```

### 3. 崩溃恢复后

如果服务器崩溃后重启，Position可能重置到上一个checkpoint位置。

---

## 相关命令对比

| 命令 | 功能 | 返回内容 |
|------|------|---------|
| **SHOW MASTER STATUS** | 显示master状态 | 当前binlog文件和位置 |
| **SHOW BINARY LOGS** | 列出所有binlog文件 | 所有binlog文件列表 |
| **SHOW BINLOG EVENTS** | 显示binlog事件 | 具体binlog事件内容 |
| **SHOW SLAVE STATUS** | 显示slave状态 | 复制状态和位置 |

---

## 调试技巧

### 1. GDB断点

```bash
# 在show_master_status入口设置断点
(gdb) break show_master_status
(gdb) run

# 执行 SHOW MASTER STATUS
mysql> SHOW MASTER STATUS;

# 查看调用栈
(gdb) backtrace

# 单步执行
(gdb) next
```

### 2. Performance Schema跟踪

```sql
-- 启用语句跟踪
UPDATE performance_schema.setup_instruments 
SET ENABLED = 'YES', TIMED = 'YES' 
WHERE NAME LIKE 'statement/%';

-- 执行命令
SHOW MASTER STATUS;

-- 查看跟踪信息
SELECT * FROM performance_schema.events_statements_history_long
WHERE SQL_TEXT = 'SHOW MASTER STATUS'
ORDER BY EVENT_ID DESC LIMIT 1\G
```

### 3. 慢查询日志

```ini
[mysqld]
slow_query_log=ON
long_query_time=0
log_slow_admin_statements=ON
```

---

## 源码文件清单

### 核心文件

- `sql/sql_yacc.yy`: SQL语法定义
- `sql/parse_tree_nodes.h/cc`: 解析树节点
- `sql/sql_show.h/cc`: SHOW命令实现
- `sql/rpl_source.h/cc`: 主库复制相关功能
- `sql/binlog.h/cc`: Binlog核心实现
- `sql/rpl_gtid_set.h/cc`: GTID集合管理
- `sql/rpl_gtid_state.h/cc`: GTID状态管理
- `sql/rpl_filter.h/cc`: Binlog过滤器

### 辅助文件

- `sql/sql_parse.cc`: SQL命令分发
- `sql/protocol.h/cc`: 客户端协议
- `sql/sql_class.h/cc`: THD和会话管理

---

## 总结

`SHOW MASTER STATUS` 的执行流程体现了MySQL的几个设计特点：

1. **分层清晰**: SQL解析 → 权限检查 → 业务逻辑 → 数据获取
2. **锁机制精细**: 使用细粒度的锁，持锁时间极短
3. **高性能**: 只读操作，无磁盘IO，响应快速
4. **复制关键**: 主从复制的核心命令之一
5. **GTID支持**: 完整支持GTID复制模式

**调用栈深度**: 约6-7层
**执行时间**: 通常 < 1ms
**锁持有时间**: < 100μs

---

## 参考资料

- MySQL 8.0 官方文档: Binary Log
- MySQL 8.0 官方文档: Replication
- MySQL源码注释

---

**文档版本**: 1.0  
**生成时间**: 2025-10-15  
**MySQL版本**: 8.0.30+

