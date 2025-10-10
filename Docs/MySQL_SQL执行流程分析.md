# MySQL 8.0 SQL执行流程分析

本文档详细梳理MySQL 8.0源码中一条SQL语句从接收到返回结果的完整执行流程和关键函数调用堆栈。

## 一、整体架构概览

MySQL的SQL执行流程可以分为以下几个主要阶段：

```
客户端连接 → 接收命令 → 分发命令 → SQL解析 → 优化器 → 执行器 → 存储引擎 → 返回结果
```

## 二、核心函数调用堆栈

### 1. 连接处理和命令接收

#### 主要文件
- `sql/conn_handler/connection_handler_one_thread.cc` - 连接处理
- `sql/sql_parse.cc` - 命令解析和分发

#### 关键函数调用链

```
线程主循环:
  └─ do_command()                                    [sql/sql_parse.cc:1308]
      ├─ 设置网络超时和事务状态
      ├─ thd->get_protocol()->get_command()          获取客户端命令
      └─ dispatch_command()                          [sql/sql_parse.cc:1688]
```

**do_command() 核心逻辑:**
```cpp
bool do_command(THD *thd) {
  // 1. 清理上一条语句的状态
  thd->lex->set_current_query_block(nullptr);
  thd->clear_error();
  thd->get_stmt_da()->reset_diagnostics_area();
  
  // 2. 设置网络超时
  net = thd->get_protocol_classic()->get_net();
  my_net_set_read_timeout(net, thd->variables.net_wait_timeout);
  
  // 3. 从网络读取客户端命令（阻塞等待）
  rc = thd->get_protocol()->get_command(&com_data, &command);
  
  // 4. 分发命令到具体处理函数
  return_value = dispatch_command(thd, &com_data, command);
}
```

### 2. 命令分发阶段

#### dispatch_command() - 命令分发器
**位置:** `sql/sql_parse.cc:1688`

```
dispatch_command(THD *thd, const COM_DATA *com_data, enum_server_command command)
  ├─ 性能计数器初始化（Performance Schema）
  ├─ 设置查询开始时间 thd->set_time()
  ├─ 分配新的query_id
  │
  ├─ 根据command类型分发:
  │   ├─ COM_QUERY:    处理普通SQL查询              [行2011]
  │   ├─ COM_STMT_PREPARE: 预处理语句
  │   ├─ COM_STMT_EXECUTE: 执行预处理语句
  │   ├─ COM_QUIT:     断开连接
  │   └─ 其他命令...
  │
  └─ 针对COM_QUERY的处理流程:
      ├─ alloc_query()                              分配查询内存
      ├─ Parser_state parser_state                   初始化解析器状态
      ├─ parser_state.init()                        
      └─ dispatch_sql_command(thd, &parser_state)   [行2055] → SQL解析和执行
```

**COM_QUERY处理核心代码:**
```cpp
case COM_QUERY: {
  // 1. 初始化digest用于性能监控
  thd->m_digest = &thd->m_digest_state;
  thd->m_digest->reset(thd->m_token_array, max_digest_length);
  
  // 2. 分配查询字符串内存
  if (alloc_query(thd, com_data->com_query.query,
                  com_data->com_query.length))
    break;
  
  // 3. 初始化Parser状态
  Parser_state parser_state;
  if (parser_state.init(thd, thd->query().str, thd->query().length)) 
    break;
  
  // 4. 设置二级存储引擎优化选项
  thd->set_secondary_engine_optimization(
      Secondary_engine_optimization::PRIMARY_TENTATIVELY);
  
  // 5. 分发到SQL命令处理
  dispatch_sql_command(thd, &parser_state);
  break;
}
```

### 3. SQL解析阶段

#### dispatch_sql_command() - SQL命令分发
**位置:** `sql/sql_parse.cc:5254`

```
dispatch_sql_command(THD *thd, Parser_state *parser_state)
  ├─ mysql_reset_thd_for_next_command(thd)          重置THD状态
  ├─ lex_start(thd)                                 初始化词法分析器
  │
  ├─ invoke_pre_parse_rewrite_plugins(thd)          预解析重写插件
  │
  ├─ parse_sql(thd, parser_state, nullptr)          [行5282] ★★★核心解析★★★
  │   └─ THD::sql_parser()                          [sql/sql_class.cc:3073]
  │       └─ MYSQLparse(thd, &root)                 YACC生成的语法解析器
  │           ├─ 词法分析 (Lexer)
  │           ├─ 语法分析 (Parser) 
  │           └─ 生成解析树 (Parse Tree)
  │
  ├─ invoke_post_parse_rewrite_plugins(thd)         后解析重写插件
  │
  ├─ mysql_rewrite_query(thd)                       查询重写（如密码脱敏）
  │
  └─ mysql_execute_command(thd, true)               [行5376] → 执行命令
```

**parse_sql() 核心逻辑:**
```cpp
bool parse_sql(THD *thd, Parser_state *parser_state, 
               Object_creation_ctx *creation_ctx) {
  // 1. 设置parser状态
  thd->m_parser_state = parser_state;
  
  // 2. 配置digest（用于性能监控和查询缓存）
  if (parser_state->m_input.m_has_digest) {
    parser_state->m_digest_psi = MYSQL_DIGEST_START(thd->m_statement_psi);
    parser_state->m_lip.m_digest = thd->m_digest;
  }
  
  // 3. 调用YACC生成的解析器
  ret_value = thd->sql_parser();  // 调用MYSQLparse()
  
  // 4. 完成digest
  MYSQL_DIGEST_END(parser_state->m_digest_psi, 
                   &thd->m_digest->m_digest_storage);
  
  return ret_value;
}
```

**MYSQLparse() - YACC语法解析器:**
```cpp
// sql/sql_class.cc:3073
bool THD::sql_parser() {
  extern int MYSQLparse(class THD *thd, class Parse_tree_root **root);
  
  Parse_tree_root *root = nullptr;
  
  // 调用YACC生成的解析函数，生成语法树
  if (MYSQLparse(this, &root) || is_error()) {
    cleanup_after_parse_error();
    return true;
  }
  
  // 将解析树转换为SQL命令对象
  if (root != nullptr && lex->make_sql_cmd(root)) {
    return true;
  }
  
  return false;
}
```

### 4. 命令执行阶段

#### mysql_execute_command() - SQL命令执行
**位置:** `sql/sql_parse.cc:2948`

这是MySQL执行层的核心入口，根据不同的SQL类型（SELECT/INSERT/UPDATE/DELETE等）分发到不同的执行逻辑。

```
mysql_execute_command(THD *thd, bool first_level)
  ├─ LEX *lex = thd->lex                            获取词法分析结果
  ├─ Query_block *query_block = lex->query_block    获取查询块
  │
  ├─ 权限检查
  ├─ 表锁检查
  │
  ├─ 根据sql_command类型分发:
  │   │
  │   ├─ SQLCOM_SELECT:                             SELECT语句
  │   │   └─ execute_sqlcom_select(thd, all_tables)
  │   │       └─ Query_expression::execute()
  │   │           ├─ Query_block::optimize()        [行2056] → 查询优化
  │   │           └─ ExecuteIteratorQuery()         → 执行查询
  │   │
  │   ├─ SQLCOM_INSERT:                             INSERT语句
  │   │   └─ mysql_insert()
  │   │
  │   ├─ SQLCOM_UPDATE:                             UPDATE语句
  │   │   └─ mysql_update()
  │   │       └─ Sql_cmd_update::execute_inner()
  │   │
  │   ├─ SQLCOM_DELETE:                             DELETE语句
  │   │   └─ mysql_delete()
  │   │       └─ Sql_cmd_delete::execute_inner()
  │   │
  │   └─ 其他DML/DDL/DCL语句...
  │
  └─ 事务处理和清理
```

### 5. 查询优化阶段 (针对SELECT)

#### Query_block::optimize() - 查询块优化
**位置:** `sql/sql_select.cc:1986`

```
Query_block::optimize(THD *thd, bool finalize_access_paths)
  └─ JOIN *join = new JOIN(thd, this)               创建JOIN对象
      └─ JOIN::optimize(finalize_access_paths)      [sql/sql_optimizer.cc:337]
          │
          ├─ 逻辑优化阶段:
          │   ├─ 外连接转内连接
          │   ├─ 常量传播和等价谓词推导
          │   ├─ 分区裁剪 (Partition Pruning)
          │   ├─ COUNT/MIN/MAX优化
          │   └─ ORDER BY优化
          │
          ├─ 代价优化阶段:
          │   └─ FindBestQueryPlan()                 [sql/join_optimizer/join_optimizer.cc:6392]
          │       ├─ 构建超图 (Hypergraph)
          │       ├─ make_join_hypergraph()          生成连接超图
          │       ├─ 枚举连接顺序
          │       ├─ 计算各路径代价
          │       └─ 选择最优访问路径 (AccessPath)
          │
          ├─ 后优化阶段:
          │   ├─ 创建表条件
          │   ├─ 注入外连接保护条件
          │   ├─ 调整数据访问方法
          │   └─ 优化ORDER BY/DISTINCT
          │
          └─ 代码生成阶段:
              ├─ 设置数据访问函数
              ├─ 优化排序/去重
              └─ 设置临时表（GROUP BY/ORDER BY需要）
```

**JOIN::optimize() 核心流程:**
```cpp
bool JOIN::optimize(bool finalize_access_paths) {
  // 1. 处理窗口函数
  if (has_windows && Window::setup_windows2(thd, &m_windows))
    return true;
  
  // 2. 获取WHERE和HAVING条件
  if (query_block->get_optimizable_conditions(thd, &where_cond, &having_cond))
    return true;
  
  // 3. 运行所有派生表/视图的优化
  // optimize phase for all derived tables/views
  
  // 4. 应用半连接/子查询优化
  if (Optimize_table_order(thd, this, nullptr).choose_table_order())
    return true;
  
  // 5. 生成执行计划 (使用超图优化器)
  AccessPath *path = FindBestQueryPlan(thd, query_block, &trace);
  
  // 6. 完成访问路径，创建迭代器
  if (finalize_access_paths) {
    m_root_access_path = path;
    create_iterators();
  }
  
  return false;
}
```

### 6. 查询执行阶段

#### Query_expression::ExecuteIteratorQuery() - 迭代器执行
**位置:** `sql/sql_union.cc:1681`

MySQL 8.0使用迭代器模型执行查询，这是火山模型(Volcano Model)的实现。

```
Query_expression::ExecuteIteratorQuery(THD *thd)
  ├─ ClearForExecution()                            清理执行状态
  │
  ├─ query_result->start_execution(thd)             开始执行
  │
  ├─ query_result->send_result_set_metadata()       发送结果集元数据
  │   └─ Protocol::send_result_set_metadata()       发送字段信息到客户端
  │
  ├─ m_root_iterator->Init()                        初始化根迭代器
  │
  ├─ 执行循环 (Volcano模型):
  │   └─ for (;;) {
  │       ├─ int error = m_root_iterator->Read()   ★读取一行数据★
  │       │   │
  │       │   └─ 迭代器链式调用 (示例: Nested Loop Join):
  │       │       NestedLoopIterator::Read()
  │       │         ├─ outer_iterator->Read()      读取外表一行
  │       │         └─ inner_iterator->Read()      读取内表匹配行
  │       │             └─ TableScanIterator::Read()
  │       │                 └─ handler::ha_rnd_next() → 存储引擎接口
  │       │
  │       ├─ if (error < 0) break;                 没有更多数据，退出循环
  │       ├─ if (error > 0) return true;           错误，返回
  │       │
  │       └─ query_result->send_data(thd, *fields) ★发送数据到客户端★
  │           └─ Protocol::send_result_set_row()
  │   }
  │
  └─ query_result->send_eof(thd)                    发送EOF包，完成查询
```

**核心执行逻辑:**
```cpp
bool Query_expression::ExecuteIteratorQuery(THD *thd) {
  // 1. 发送结果集元数据（列名、类型等）
  if (query_result->send_result_set_metadata(
          thd, *fields, Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF)) {
    return true;
  }
  
  // 2. 初始化迭代器树
  if (m_root_iterator->Init()) {
    return true;
  }
  
  // 3. 火山模型执行：循环调用Read()获取每一行
  for (;;) {
    int error = m_root_iterator->Read();  // 读取一行
    
    if (error > 0 || thd->is_error())     // 错误
      return true;
    else if (error < 0)                   // 没有更多数据
      break;
    else if (thd->killed)                 // 用户取消
      return true;
    
    ++*send_records_ptr;
    
    // 4. 发送这一行数据到客户端
    if (query_result->send_data(thd, *fields)) {
      return true;
    }
  }
  
  // 5. 发送EOF包
  return query_result->send_eof(thd);
}
```

### 7. 存储引擎接口层

#### Handler接口 - 与存储引擎交互
**位置:** `sql/handler.h` 和 `storage/innobase/handler/ha_innodb.cc`

```
迭代器调用存储引擎:
  TableScanIterator::Read()
    └─ handler::ha_rnd_next(record)                 顺序扫描下一行
        ├─ ha_statistic_increment()                 统计信息
        └─ rnd_next(record)                         虚函数，由存储引擎实现
            └─ ha_innobase::rnd_next()              [InnoDB实现]
                └─ general_fetch()
                    └─ row_search_mvcc()            InnoDB MVCC读取

  IndexScanIterator::Read()
    └─ handler::ha_index_next(record)               索引扫描下一行
        └─ index_next(record)
            └─ ha_innobase::index_next()            [InnoDB实现]
                └─ row_search_mvcc()
```

**关键存储引擎方法:**
- `ha_rnd_init()` - 初始化表扫描
- `ha_rnd_next()` - 读取下一行（全表扫描）
- `ha_index_init()` - 初始化索引扫描
- `ha_index_read()` - 按索引读取
- `ha_index_next()` - 索引扫描下一行
- `ha_write_row()` - 插入行
- `ha_update_row()` - 更新行
- `ha_delete_row()` - 删除行

### 8. 结果返回阶段

#### Protocol层 - 网络协议处理
**位置:** `sql/protocol_classic.cc`

```
数据发送到客户端:
  query_result->send_data(thd, fields)
    └─ Query_result_send::send_data()               [sql/query_result.cc]
        └─ thd->send_result_set_row(fields)
            └─ Protocol_classic::send_result_set_row()
                ├─ start_row()                      开始发送一行
                ├─ store(field)                     存储每个字段值
                │   └─ Field::send_to_protocol()    字段序列化
                └─ end_row()                        结束这行
                    └─ net_write_packet()           写入网络缓冲区
                        └─ my_net_write()           TCP发送
```

## 三、不同SQL类型的执行路径

### SELECT语句
```
dispatch_command() [COM_QUERY]
  → dispatch_sql_command()
    → parse_sql() [MYSQLparse]
      → mysql_execute_command()
        → execute_sqlcom_select()
          → Query_expression::optimize()
            → Query_block::optimize()
              → JOIN::optimize()
                → FindBestQueryPlan()  // 生成最优执行计划
          → Query_expression::execute()
            → ExecuteIteratorQuery()
              → m_root_iterator->Read()  // 循环读取数据
                → handler::ha_rnd_next() / ha_index_next()  // 存储引擎
              → send_data()  // 发送到客户端
```

### INSERT语句
```
dispatch_command() [COM_QUERY]
  → dispatch_sql_command()
    → parse_sql()
      → mysql_execute_command()
        → mysql_insert()
          → Sql_cmd_insert::execute_inner()
            → write_record()
              → handler::ha_write_row()  // 调用存储引擎插入
```

### UPDATE语句
```
dispatch_command() [COM_QUERY]
  → dispatch_sql_command()
    → parse_sql()
      → mysql_execute_command()
        → mysql_update()
          → Sql_cmd_update::execute_inner()
            → 初始化迭代器扫描符合WHERE条件的行
            → handler::ha_update_row()  // 更新每一行
```

### DELETE语句
```
dispatch_command() [COM_QUERY]
  → dispatch_sql_command()
    → parse_sql()
      → mysql_execute_command()
        → mysql_delete()
          → Sql_cmd_delete::execute_inner()
            → 初始化迭代器扫描符合WHERE条件的行
            → handler::ha_delete_row()  // 删除每一行
```

## 四、关键数据结构

### 1. THD (Thread Handler)
**位置:** `sql/sql_class.h`

THD是MySQL中最重要的数据结构，代表一个客户端连接的线程上下文。

```cpp
class THD {
  LEX *lex;                    // 词法和语法分析结果
  Query_arena *query_arena;    // 查询内存分配arena
  Protocol *protocol;          // 客户端通信协议
  Security_context *security_ctx;  // 安全上下文
  Transaction_state transaction;   // 事务状态
  Diagnostics_area *stmt_da;   // 诊断区域（错误/警告）
  // ... 还有数百个成员
};
```

### 2. LEX (Lexical Context)
**位置:** `sql/sql_lex.h`

保存SQL语句的词法和语法分析结果。

```cpp
class LEX {
  enum_sql_command sql_command;  // SQL命令类型（SELECT/INSERT等）
  Query_block *query_block;      // 查询块链表
  Query_expression *unit;        // 查询表达式树根
  List<Table_ref> query_tables;  // 查询涉及的表
  // ...
};
```

### 3. Query_block (查询块)
**位置:** `sql/sql_lex.h`

代表一个SELECT查询块（可能有子查询）。

```cpp
class Query_block {
  Table_ref *leaf_tables;        // 叶子表（实际的表）
  Item *where_cond;              // WHERE条件
  Item *having_cond;             // HAVING条件
  ORDER *order_list;             // ORDER BY列表
  JOIN *join;                    // JOIN对象
  // ...
};
```

### 4. JOIN (连接操作)
**位置:** `sql/sql_optimizer.h`

管理表连接和优化。

```cpp
class JOIN {
  THD *thd;                      // 线程句柄
  Query_block *query_block;      // 所属查询块
  table_map const_table_map;     // 常量表位图
  AccessPath *m_root_access_path; // 根访问路径
  RowIterator *m_root_iterator;  // 根迭代器
  // ...
};
```

### 5. AccessPath (访问路径)
**位置:** `sql/access_path.h`

优化器生成的执行计划节点。

```cpp
struct AccessPath {
  AccessPathType type;  // TABLE_SCAN, INDEX_SCAN, NESTED_LOOP_JOIN等
  double num_output_rows;  // 预估输出行数
  double cost;             // 代价
  // 根据类型有不同的union成员
  union {
    TableScanAccessPath table_scan;
    IndexScanAccessPath index_scan;
    NestedLoopJoinAccessPath nested_loop_join;
    // ...
  };
};
```

### 6. RowIterator (行迭代器)
**位置:** `sql/row_iterator.h`

执行器的迭代器接口（火山模型）。

```cpp
class RowIterator {
  virtual int Init() = 0;      // 初始化
  virtual int Read() = 0;      // 读取下一行（核心）
  virtual void UnlockRow() {}  // 解锁行
};
```

常见迭代器类型:
- `TableScanIterator` - 全表扫描
- `IndexScanIterator` - 索引扫描
- `NestedLoopIterator` - 嵌套循环连接
- `HashJoinIterator` - 哈希连接
- `SortIterator` - 排序
- `FilterIterator` - 过滤（WHERE条件）
- `AggregateIterator` - 聚合（GROUP BY）

## 五、关键优化技术

### 1. 超图优化器 (Hypergraph Optimizer)
**位置:** `sql/join_optimizer/`

MySQL 8.0引入的新优化器，使用超图表示连接关系。

**主要特点:**
- 动态规划枚举连接顺序
- 支持多表连接的全局优化
- 考虑各种物理算子（Nested Loop, Hash Join等）
- 代价模型驱动

**核心函数:**
- `make_join_hypergraph()` - 构建超图
- `FindBestQueryPlan()` - 查找最优计划
- `EnumerateAllConnectedPartitions()` - 枚举连接顺序

### 2. 谓词下推 (Predicate Pushdown)
将WHERE条件尽可能推到数据源附近执行，减少数据传输。

### 3. 索引条件下推 (Index Condition Pushdown, ICP)
将过滤条件推到存储引擎层，减少引擎和Server层的交互。

### 4. 多范围读取 (Multi-Range Read, MRR)
优化索引扫描的随机IO，将随机IO转换为顺序IO。

### 5. 批量键访问 (Batched Key Access, BKA)
批量处理连接操作，减少函数调用开销。

## 六、性能监控点

### 1. Performance Schema埋点
- `MYSQL_START_STATEMENT` - 语句开始
- `MYSQL_EXECUTE_PS` - 预处理语句执行
- `MYSQL_DIGEST_START/END` - 查询指纹计算
- `MYSQL_END_STATEMENT` - 语句结束

### 2. Profiling点
```cpp
#if defined(ENABLED_PROFILING)
thd->profiling->start_new_query();
thd->profiling->set_query_source(query, length);
#endif
```

### 3. 慢查询日志
- 查询执行时间统计
- 扫描行数、返回行数
- 锁等待时间

## 七、重要配置参数

### 优化器相关
- `optimizer_switch` - 优化器开关（控制各种优化策略）
- `optimizer_search_depth` - 优化器搜索深度
- `optimizer_prune_level` - 优化器剪枝级别

### 执行器相关
- `join_buffer_size` - 连接缓冲区大小
- `sort_buffer_size` - 排序缓冲区大小
- `tmp_table_size` - 内存临时表大小
- `max_heap_table_size` - MEMORY表最大大小

### 解析器相关
- `parser_max_mem_size` - 解析器最大内存
- `max_digest_length` - 查询摘要最大长度

## 八、总结

MySQL 8.0的SQL执行流程可以总结为：

1. **连接层:** `do_command()` 接收客户端命令
2. **分发层:** `dispatch_command()` 根据命令类型分发
3. **解析层:** `parse_sql()` → `MYSQLparse()` 词法语法分析
4. **执行层:** `mysql_execute_command()` 根据SQL类型执行
5. **优化层:** `JOIN::optimize()` → `FindBestQueryPlan()` 生成执行计划
6. **迭代器层:** `ExecuteIteratorQuery()` 火山模型执行
7. **存储引擎层:** `handler::ha_*()` 接口访问数据
8. **协议层:** `Protocol::send_*()` 返回结果给客户端

整个流程体现了MySQL的模块化设计：
- **清晰的分层:** 网络层、SQL层、优化器、执行器、存储引擎各司其职
- **可扩展性:** 插件式存储引擎、优化器可替换
- **高性能:** 迭代器模型、代价优化、丰富的优化技术

掌握这个流程对于理解MySQL内部机制、性能调优、故障排查都至关重要。

