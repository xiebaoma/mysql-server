# MySQL SELECT语句详细调用堆栈示例

本文档展示一条简单的SELECT语句 `SELECT * FROM users WHERE id = 100` 的完整函数调用堆栈。

## 一、完整调用堆栈（从网络到返回结果）

```
┌─────────────────────────────────────────────────────────────────────────┐
│ 1. 网络层 - 接收客户端请求                                                │
└─────────────────────────────────────────────────────────────────────────┘

main()                                                    [sql/mysqld.cc]
  └─ mysqld_main()
      └─ 创建线程池/为每个连接创建线程
          └─ handle_connection()                          [sql/conn_handler/connection_handler_one_thread.cc]
              └─ thd_prepare_connection()                 认证、初始化连接
                  └─ while(!connection_closed) {
                      ┌─────────────────────────────────────────────────────┐
                      │ 循环处理客户端命令                                    │
                      └─────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│ 2. 命令接收层                                                             │
└─────────────────────────────────────────────────────────────────────────┘

                      do_command(THD *thd)                [sql/sql_parse.cc:1308]
                        │
                        ├─ 作用: 从客户端读取一条命令
                        ├─ 重要操作:
                        │   ├─ thd->lex->set_current_query_block(nullptr)
                        │   ├─ thd->clear_error()
                        │   ├─ thd->get_stmt_da()->reset_diagnostics_area()
                        │   └─ my_net_set_read_timeout(net, thd->variables.net_wait_timeout)
                        │
                        ├─ rc = thd->get_protocol()->get_command(&com_data, &command)
                        │   └─ Protocol_classic::get_command()    [sql/protocol_classic.cc]
                        │       └─ my_net_read()                  阻塞等待网络数据
                        │           └─ net_read_packet()          从socket读取
                        │               └─ recv() / read()        系统调用
                        │
                        └─ return dispatch_command(thd, &com_data, command)
                            │
                            │ 此时: command = COM_QUERY
                            │      com_data.com_query.query = "SELECT * FROM users WHERE id = 100"
                            │      com_data.com_query.length = 37
                            ↓

┌─────────────────────────────────────────────────────────────────────────┐
│ 3. 命令分发层                                                             │
└─────────────────────────────────────────────────────────────────────────┘

dispatch_command(THD *thd, const COM_DATA *com_data,     [sql/sql_parse.cc:1688]
                 enum_server_command command)
  │
  ├─ 作用: 根据命令类型(COM_QUERY/COM_STMT_PREPARE等)分发处理
  │
  ├─ 初始化性能监控:
  │   ├─ thd->m_statement_psi = MYSQL_REFINE_STATEMENT(...)  // Performance Schema
  │   ├─ thd->set_command(command)
  │   └─ thd->set_time()                                     // 记录查询开始时间
  │
  ├─ thd->set_query_id(next_query_id())                      // 分配query_id
  ├─ thd_manager->inc_thread_running()
  │
  └─ switch(command) {
      case COM_QUERY: {                                      [sql/sql_parse.cc:2011]
        │
        ├─ 初始化digest（查询指纹，用于性能统计）:
        │   ├─ thd->m_digest = &thd->m_digest_state
        │   └─ thd->m_digest->reset(thd->m_token_array, max_digest_length)
        │
        ├─ alloc_query(thd, com_data->com_query.query,       分配查询字符串内存
        │              com_data->com_query.length)
        │
        ├─ 记录查询日志:
        │   └─ query_logger.general_log_write(thd, command, thd->query().str, ...)
        │
        ├─ 初始化Parser状态:
        │   ├─ Parser_state parser_state
        │   ├─ parser_state.init(thd, thd->query().str, thd->query().length)
        │   ├─ parser_state.m_input.m_has_digest = true
        │   └─ parser_state.m_input.m_compute_digest = true
        │
        └─ dispatch_sql_command(thd, &parser_state)          [sql/sql_parse.cc:2055]
            ↓

┌─────────────────────────────────────────────────────────────────────────┐
│ 4. SQL解析准备                                                            │
└─────────────────────────────────────────────────────────────────────────┘

dispatch_sql_command(THD *thd, Parser_state *parser_state) [sql/sql_parse.cc:5254]
  │
  ├─ 作用: 解析SQL语句并执行
  │
  ├─ mysql_reset_thd_for_next_command(thd)                  重置THD状态
  ├─ lex_start(thd)                                         初始化LEX结构
  │   └─ 分配新的LEX对象，重置所有词法分析器状态
  │
  ├─ invoke_pre_parse_rewrite_plugins(thd)                  预解析插件（如查询重写）
  │
  ├─ 核心解析:
  │   └─ err = parse_sql(thd, parser_state, nullptr)        [sql/sql_parse.cc:5282]
  │       ↓

┌─────────────────────────────────────────────────────────────────────────┐
│ 5. 词法和语法分析（Parser）                                                │
└─────────────────────────────────────────────────────────────────────────┘

parse_sql(THD *thd, Parser_state *parser_state,           [sql/sql_parse.cc:7066]
          Object_creation_ctx *creation_ctx)
  │
  ├─ 作用: 调用YACC生成的解析器，将SQL文本转换为语法树
  │
  ├─ 设置parser状态:
  │   ├─ thd->m_parser_state = parser_state
  │   ├─ parser_state->m_digest_psi = MYSQL_DIGEST_START(thd->m_statement_psi)
  │   └─ parser_state->m_lip.m_digest = thd->m_digest
  │
  ├─ 设置内存限制:
  │   └─ thd->mem_root->set_max_capacity(thd->variables.parser_max_mem_size)
  │
  ├─ 切换诊断区域:
  │   ├─ thd->push_diagnostics_area(parser_da)
  │   └─ thd->mem_root->set_error_handler(&poomh)          OOM处理器
  │
  └─ ret_value = thd->sql_parser()                         [sql/sql_class.cc:3073]
      │
      ├─ 作用: 执行YACC解析器
      │
      └─ extern int MYSQLparse(THD *thd, Parse_tree_root **root)
          │
          ├─ Parse_tree_root *root = nullptr
          │
          ├─ if (MYSQLparse(this, &root) || is_error()) {  [sql/sql_yacc.yy 生成]
          │   │
          │   │ YACC解析器核心流程:
          │   │
          │   ├─ 词法分析 (Lexer):
          │   │   └─ Lexer::next_token()                   [sql/sql_lex.cc]
          │   │       ├─ 逐字符扫描SQL文本
          │   │       ├─ 识别关键字: SELECT, FROM, WHERE
          │   │       ├─ 识别标识符: users, id
          │   │       ├─ 识别操作符: =, *
          │   │       └─ 识别字面值: 100
          │   │
          │   ├─ 语法分析 (Parser - YACC规则):
          │   │   │
          │   │   └─ 应用文法规则构建语法树:
          │   │       │
          │   │       ├─ query: select_stmt
          │   │       │   └─ select_stmt: SELECT select_item_list FROM table_ref opt_where_clause
          │   │       │       ├─ select_item_list: '*'
          │   │       │       ├─ table_ref: table_ident
          │   │       │       │   └─ table_ident: 'users'
          │   │       │       └─ opt_where_clause: WHERE expr
          │   │       │           └─ expr: id = 100
          │   │       │               ├─ 创建 Item_field('id')
          │   │       │               ├─ 创建 Item_int(100)
          │   │       │               └─ 创建 Item_func_eq(Item_field, Item_int)
          │   │       │
          │   │       └─ 构建Parse_tree_root对象
          │   │
          │   └─ 生成的结果存储在 thd->lex 中
          │
          └─ if (root != nullptr && lex->make_sql_cmd(root)) {
              └─ 将Parse Tree转换为可执行的Sql_cmd对象
                  └─ Sql_cmd_select::make_cmd()
                      ├─ 创建 Query_expression（查询表达式树）
                      └─ 创建 Query_block（查询块）
                          ├─ fields: List<Item*>  // SELECT *展开的字段列表
                          ├─ table_list: users
                          └─ where_cond: Item_func_eq(id, 100)

  │ 解析完成后的LEX结构:
  │ lex->sql_command = SQLCOM_SELECT
  │ lex->query_block->table_list = {Table_ref("users")}
  │ lex->query_block->where_cond = Item_func_eq(...)
  │ lex->query_block->item_list = {Item_field("id"), Item_field("name"), ...}
  │
  └─ invoke_post_parse_rewrite_plugins(thd, false)        后解析插件
  │
  └─ mysql_rewrite_query(thd)                             查询重写（如密码脱敏）
      │
      └─ 继续 dispatch_sql_command():
          └─ mysql_execute_command(thd, true)             [sql/sql_parse.cc:5376]
              ↓

┌─────────────────────────────────────────────────────────────────────────┐
│ 6. 命令执行层                                                             │
└─────────────────────────────────────────────────────────────────────────┘

mysql_execute_command(THD *thd, bool first_level)         [sql/sql_parse.cc:2948]
  │
  ├─ 作用: 根据SQL命令类型执行相应操作
  │
  ├─ LEX *lex = thd->lex
  ├─ Query_block *query_block = lex->query_block
  ├─ Table_ref *first_table = query_block->get_table_list()  // users表
  │
  ├─ 权限检查:
  │   └─ check_table_access(thd, SELECT_ACL, all_tables, ...)
  │       └─ check_access(thd, want_access, db, ...)
  │
  ├─ 打开表和获取锁:
  │   └─ open_tables_for_query(thd, all_tables, flags)
  │       └─ open_tables(thd, start, ...)                  [sql/sql_base.cc]
  │           ├─ open_and_process_table()
  │           │   ├─ open_table()
  │           │   │   ├─ get_table_share()                从表定义缓存获取
  │           │   │   └─ open_table_from_share()
  │           │   │       └─ handler::ha_open()           调用存储引擎打开表
  │           │   │           └─ ha_innobase::open()      [storage/innobase/handler/ha_innodb.cc]
  │           │   │
  │           │   └─ mysql_lock_tables()                  加表锁
  │           │       └─ lock_tables()
  │           │           └─ thr_multi_lock()
  │           │
  │           └─ 表已打开，TABLE对象已创建
  │
  └─ switch(lex->sql_command) {
      case SQLCOM_SELECT:
        │
        └─ execute_sqlcom_select(thd, all_tables)         [sql/sql_select.cc]
            │
            ├─ Query_expression *unit = lex->unit
            │
            ├─ 优化查询:
            │   └─ unit->optimize(thd, /*materialize_destination=*/nullptr,
            │                     /*create_iterators=*/true,
            │                     /*finalize_access_paths=*/true)
            │       ↓

┌─────────────────────────────────────────────────────────────────────────┐
│ 7. 查询优化阶段                                                           │
└─────────────────────────────────────────────────────────────────────────┘

Query_expression::optimize()                              [sql/sql_union.cc]
  └─ Query_block::optimize(thd, finalize_access_paths)    [sql/sql_select.cc:1986]
      │
      ├─ 作用: 生成最优执行计划
      │
      ├─ JOIN *join = new JOIN(thd, this)                 创建JOIN对象
      ├─ this->join = join
      │
      └─ join->optimize(finalize_access_paths)            [sql/sql_optimizer.cc:337]
          │
          ├─ 作用: JOIN优化的核心入口
          │
          ├─ 阶段1: 逻辑优化
          │   │
          │   ├─ count_field_types(query_block, ...)
          │   ├─ alloc_func_list()
          │   ├─ query_block->get_optimizable_conditions(thd, &where_cond, &having_cond)
          │   │   └─ 获取WHERE条件: id = 100
          │   │
          │   ├─ setup_tables(thd, ...)                   设置表信息
          │   ├─ setup_wild(thd, ...)                     展开 SELECT *
          │   ├─ setup_fields(thd, ...)                   解析字段
          │   └─ setup_conds(thd, ...)                    设置条件
          │       └─ 常量折叠、谓词简化
          │
          ├─ 阶段2: 访问路径优化（代价优化）
          │   │
          │   └─ AccessPath *path = FindBestQueryPlan(thd, query_block, &trace)
          │       ↓                                       [sql/join_optimizer/join_optimizer.cc:6392]
          │
          │       ├─ 作用: 使用超图优化器生成最优执行计划
          │       │
          │       ├─ 构建超图:
          │       │   └─ JoinHypergraph graph = MakeJoinHypergraph(thd, error_msg, query_block)
          │       │       ├─ 为每个表创建节点
          │       │       ├─ 为每个JOIN条件创建超边
          │       │       └─ 分析WHERE条件，标记可用索引
          │       │
          │       ├─ 收集访问路径（单表扫描方式）:
          │       │   └─ for each table {
          │       │       ├─ 全表扫描: CreateTableScanAccessPath()
          │       │       ├─ 索引扫描: 
          │       │       │   └─ 检查 WHERE id = 100
          │       │       │       └─ 发现id列有主键索引
          │       │       │           └─ CreateIndexScanAccessPath(table, index, ranges)
          │       │       │               ├─ index = PRIMARY KEY
          │       │       │               └─ ranges = [100, 100] (单点查询)
          │       │       │
          │       │       └─ 计算代价:
          │       │           ├─ 全表扫描: 假设1000行 × 1.0 = 1000
          │       │           └─ 索引扫描: 1行 × 1.0 = 1.0  ← 选择这个
          │       │   }
          │       │
          │       ├─ 枚举连接顺序（本例只有一个表，跳过）
          │       │
          │       ├─ 选择最优路径:
          │       │   └─ 单表索引扫描 users.PRIMARY WHERE id=100
          │       │
          │       └─ 添加额外操作:
          │           ├─ 如果有ORDER BY → AddSortPath()
          │           ├─ 如果有LIMIT → AddLimitPath()
          │           └─ 如果有GROUP BY → AddGroupByPath()
          │
          │       返回 AccessPath *path
          │
          ├─ 阶段3: 完成优化
          │   │
          │   ├─ m_root_access_path = path                保存访问路径
          │   │
          │   └─ 创建迭代器:
          │       └─ create_iterators()
          │           └─ CreateIteratorFromAccessPath()
          │               └─ 根据AccessPath类型创建对应的Iterator:
          │                   │
          │                   └─ IndexScanAccessPath →
          │                       └─ new IndexScanIterator(
          │                             table = users,
          │                             index = PRIMARY,
          │                             ranges = [100, 100],
          │                             reverse = false
          │                          )
          │
          └─ 优化完成，执行计划已生成
              │
              │ 执行计划示例:
              │ IndexScan on users.PRIMARY
              │   Key: id = 100
              │   Rows: 1
              │   Cost: 1.0
              │
              └─ 返回到 execute_sqlcom_select()

            └─ 执行查询:
                └─ unit->execute(thd)                      [sql/sql_union.cc:1821]
                    ↓

┌─────────────────────────────────────────────────────────────────────────┐
│ 8. 查询执行阶段（迭代器模型 - Volcano Model）                              │
└─────────────────────────────────────────────────────────────────────────┘

Query_expression::execute(THD *thd)                       [sql/sql_union.cc:1821]
  └─ ExecuteIteratorQuery(thd)                            [sql/sql_union.cc:1681]
      │
      ├─ 作用: 使用迭代器模型执行查询，逐行返回结果
      │
      ├─ Query_result *query_result = this->query_result()
      │   └─ 通常是 Query_result_send（发送结果到客户端）
      │
      ├─ 开始执行:
      │   └─ query_result->start_execution(thd)
      │
      ├─ 发送元数据（列定义）:
      │   └─ query_result->send_result_set_metadata(thd, *fields, Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF)
      │       └─ Protocol_classic::send_result_set_metadata()  [sql/protocol_classic.cc]
      │           ├─ start_result_metadata()
      │           ├─ for each field {
      │           │   └─ send_field_metadata()
      │           │       ├─ store_string(field->name)     字段名
      │           │       ├─ store_long(field->type)       字段类型
      │           │       └─ store_long(field->length)     字段长度
      │           │   }
      │           └─ end_result_metadata()
      │               └─ net_write_packet()                发送到客户端
      │
      ├─ 初始化迭代器:
      │   └─ if (m_root_iterator->Init()) {
      │       │
      │       └─ IndexScanIterator::Init()                [sql/iterators/basic_row_iterators.cc]
      │           ├─ 作用: 初始化索引扫描
      │           │
      │           ├─ m_record = table()->record[0]        设置记录缓冲区
      │           │
      │           └─ table()->file->ha_index_init(m_idx, m_sorted)
      │               └─ handler::ha_index_init()         [sql/handler.cc]
      │                   ├─ innobase_reset_autoinc()
      │                   └─ index_init(idx, sorted)      虚函数
      │                       └─ ha_innobase::index_init() [storage/innobase/handler/ha_innodb.cc]
      │                           ├─ 定位到索引
      │                           ├─ 初始化搜索游标
      │                           └─ 设置MVCC一致性读
      │       }
      │
      ├─ 执行循环（Volcano模型核心）:
      │   │
      │   └─ for (;;) {
      │       │
      │       ├─ int error = m_root_iterator->Read()     ★★★ 读取一行 ★★★
      │       │   │
      │       │   └─ IndexScanIterator::Read()           [sql/iterators/basic_row_iterators.cc]
      │       │       │
      │       │       ├─ 作用: 从索引读取下一行
      │       │       │
      │       │       ├─ if (m_first) {                   第一次读取
      │       │       │   │
      │       │       │   ├─ 构建索引查找key:
      │       │       │   │   └─ key_copy(m_key, m_record, key_info, key_len)
      │       │       │   │       └─ 将 id=100 编码为索引key格式
      │       │       │   │
      │       │       │   └─ error = table()->file->ha_index_read_map(
      │       │       │         m_record,              // 输出缓冲区
      │       │       │         m_key,                 // 查找key: id=100
      │       │       │         keypart_map,           // 使用的key部分
      │       │       │         HA_READ_KEY_EXACT      // 精确匹配
      │       │       │       )
      │       │       │       │
      │       │       │       └─ handler::ha_index_read_map()  [sql/handler.cc]
      │       │       │           ├─ ha_statistic_increment()  统计
      │       │       │           └─ index_read_map()          虚函数
      │       │       │               │
      │       │       │               └─ ha_innobase::index_read()  [storage/innobase/handler/ha_innodb.cc]
      │       │       │                   │
      │       │       │                   └─ row_search_mvcc()      [storage/innobase/row/row0sel.cc]
      │       │       │                       │
      │       │       │                       ├─ 在B+树中定位key: id=100
      │       │       │                       ├─ 读取页面（可能从Buffer Pool）
      │       │       │                       ├─ 二分查找定位记录
      │       │       │                       ├─ MVCC可见性判断
      │       │       │                       ├─ 如果可见，复制记录到m_record
      │       │       │                       └─ 返回 0（成功）
      │       │       │
      │       │       │   m_first = false
      │       │       │
      │       │       │ } else {                         后续读取
      │       │       │   │
      │       │       │   └─ error = table()->file->ha_index_next(m_record)
      │       │       │       └─ handler::ha_index_next()
      │       │       │           └─ ha_innobase::index_next()
      │       │       │               └─ general_fetch()
      │       │       │                   └─ row_search_mvcc()
      │       │       │                       └─ 移动到下一条记录
      │       │       │ }
      │       │       │
      │       │       └─ return error
      │       │           // 0  = 成功读取一行
      │       │           // -1 = 没有更多数据（HA_ERR_END_OF_FILE）
      │       │           // >0 = 错误
      │       │
      │       ├─ 检查返回值:
      │       │   ├─ if (error > 0 || thd->is_error())   // 错误
      │       │   │     return true;
      │       │   ├─ else if (error < 0)                 // 没有更多数据
      │       │   │     break;                           // 退出循环
      │       │   └─ else if (thd->killed)               // 用户取消
      │       │         return true;
      │       │
      │       ├─ ++*send_records_ptr                     记录已发送行数
      │       │
      │       └─ 发送这一行数据:
      │           │
      │           └─ if (query_result->send_data(thd, *fields)) {
      │               │
      │               └─ Query_result_send::send_data()   [sql/query_result.cc]
      │                   │
      │                   └─ thd->send_result_set_row(fields)
      │                       │
      │                       └─ Protocol_classic::send_result_set_row()  [sql/protocol_classic.cc]
      │                           │
      │                           ├─ start_row()          开始一行
      │                           │
      │                           ├─ for each field {     遍历所有字段
      │                           │   │
      │                           │   └─ field->send_to_protocol(this)
      │                           │       └─ Field::send_to_protocol()  [sql/field.cc]
      │                           │           │
      │                           │           └─ switch(field_type) {
      │                           │               case INT:
      │                           │                 └─ store_long()    存储整数
      │                           │               case VARCHAR:
      │                           │                 └─ store_string()  存储字符串
      │                           │               case DATETIME:
      │                           │                 └─ store_datetime() 存储日期时间
      │                           │               // ...
      │                           │             }
      │                           │   }
      │                           │
      │                           └─ end_row()            结束这行
      │                               └─ net_write_packet()  写入网络缓冲
      │                                   └─ my_net_write()
      │                                       └─ memcpy to net->buff
      │               }
      │       }
      │
      │   }  // end of for(;;)
      │
      ├─ 设置统计信息:
      │   └─ thd->current_found_rows = *send_records_ptr
      │
      └─ 发送EOF包，完成查询:
          └─ query_result->send_eof(thd)
              └─ Query_result_send::send_eof()            [sql/query_result.cc]
                  └─ thd->get_protocol()->send_eof()
                      └─ Protocol_classic::send_eof()     [sql/protocol_classic.cc]
                          ├─ write_eof_packet()
                          │   ├─ 包含服务器状态标志
                          │   ├─ 包含警告数量
                          │   └─ 包含affected rows（本例为1）
                          └─ net_flush()                  刷新网络缓冲区
                              └─ my_net_write()
                                  └─ send() / write()     系统调用，发送到socket

┌─────────────────────────────────────────────────────────────────────────┐
│ 9. 清理和返回                                                             │
└─────────────────────────────────────────────────────────────────────────┘

  ↓ 返回到 do_command()
  │
  ├─ 记录慢查询日志（如果需要）
  ├─ 更新统计信息
  ├─ MYSQL_END_STATEMENT(thd->m_statement_psi, thd->get_stmt_da())
  │
  └─ return false  // 继续处理下一条命令
      │
      └─ 回到 handle_connection() 的循环
          └─ 等待下一条客户端命令...
```

## 二、关键数据流转

### 1. SQL字符串 → 解析树
```
"SELECT * FROM users WHERE id = 100"
  ↓ Lexer (词法分析)
Token流: [SELECT] [*] [FROM] [users] [WHERE] [id] [=] [100]
  ↓ Parser (语法分析)
Parse Tree:
  PT_query_specification
    ├─ PT_select_item_list: [PT_asterisk]
    ├─ PT_table_factor_table_ident: users
    └─ PT_where_clause
        └─ PT_comp_expr: =
            ├─ PT_ident: id
            └─ PT_literal: 100
  ↓ make_sql_cmd()
LEX结构:
  lex->sql_command = SQLCOM_SELECT
  lex->query_block
    ├─ fields: [Item_field(id), Item_field(name), ...]
    ├─ table_list: [Table_ref(users)]
    └─ where_cond: Item_func_eq(Item_field(id), Item_int(100))
```

### 2. LEX → 执行计划
```
LEX
  ↓ JOIN::optimize()
  ↓ FindBestQueryPlan()
AccessPath:
  IndexScanAccessPath
    ├─ table: users
    ├─ index: PRIMARY
    ├─ ranges: [100, 100]
    ├─ cost: 1.0
    └─ rows: 1
  ↓ create_iterators()
RowIterator:
  IndexScanIterator
    ├─ m_table: users
    ├─ m_idx: 0 (PRIMARY)
    ├─ m_key: [100]
    └─ m_first: true
```

### 3. 执行计划 → 结果集
```
IndexScanIterator::Read()
  ↓ ha_index_read_map()
  ↓ row_search_mvcc()
  ↓ B+树查找
TABLE::record[0] = [100, "Alice", "alice@example.com", ...]
  ↓ send_data()
  ↓ Protocol::send_result_set_row()
Network Packet:
  [field1: 100]
  [field2: "Alice"]
  [field3: "alice@example.com"]
  [...]
  ↓ net_write_packet()
  ↓ TCP send()
→ 客户端接收
```

## 三、关键性能点

### 1. 索引选择
```
位置: sql/join_optimizer/join_optimizer.cc
函数: FindBestQueryPlan()
  
代价计算:
- 全表扫描: table_rows × cpu_cost
- 索引扫描: (索引层数 × io_cost) + (返回行数 × cpu_cost)

本例: id=100是主键，索引扫描只需1次IO，返回1行
选择: IndexScan on PRIMARY
```

### 2. 存储引擎交互
```
位置: storage/innobase/row/row0sel.cc
函数: row_search_mvcc()

关键操作:
1. 定位记录: btr_pcur_open_on_user_rec() - B+树搜索
2. 读取页面: buf_page_get_gen() - Buffer Pool
3. MVCC判断: lock_clust_rec_cons_read_sees() - 可见性检查
4. 复制数据: row_sel_store_mysql_rec() - 转换为MySQL格式

优化:
- Buffer Pool缓存减少磁盘IO
- 自适应哈希索引加速B+树查找
- Read Ahead预读优化
```

### 3. 网络传输
```
位置: sql/protocol_classic.cc
函数: Protocol_classic::send_result_set_row()

优化:
- 批量发送: net_write_packet() 累积到缓冲区
- 压缩: 如果启用协议压缩
- 延迟刷新: 不是每行都flush，而是缓冲区满或查询结束时flush

本例: 1行数据，立即发送EOF包
```

## 四、调试技巧

### 1. GDB断点设置
```bash
# 命令接收
b do_command
b dispatch_command

# 解析
b parse_sql
b MYSQLparse

# 优化
b JOIN::optimize
b FindBestQueryPlan

# 执行
b Query_expression::ExecuteIteratorQuery
b IndexScanIterator::Read
b ha_innobase::index_read

# 协议
b Protocol_classic::send_result_set_row
```

### 2. 查看执行计划
```sql
EXPLAIN FORMAT=TREE SELECT * FROM users WHERE id = 100;
-- 显示迭代器树

EXPLAIN FORMAT=JSON SELECT * FROM users WHERE id = 100;
-- 显示详细的访问路径和代价

SET optimizer_trace='enabled=on';
SELECT * FROM users WHERE id = 100;
SELECT * FROM information_schema.OPTIMIZER_TRACE;
-- 查看优化器完整trace
```

### 3. 性能分析
```sql
SET profiling = 1;
SELECT * FROM users WHERE id = 100;
SHOW PROFILES;
SHOW PROFILE FOR QUERY 1;
-- 查看各阶段耗时

SELECT * FROM performance_schema.events_statements_history
WHERE SQL_TEXT LIKE '%users%';
-- Performance Schema统计
```

## 五、总结

这条简单的SELECT语句经过了：

1. **网络接收** - 50+行代码
2. **命令分发** - 300+行代码
3. **SQL解析** - 10000+行代码（YACC生成）
4. **权限检查** - 200+行代码
5. **打开表** - 500+行代码
6. **查询优化** - 5000+行代码
7. **执行器** - 1000+行代码
8. **存储引擎** - 3000+行代码（InnoDB）
9. **结果返回** - 500+行代码

总计约 **20000+行核心代码** 的执行路径！

这体现了MySQL的：
- **高度模块化**: 每层职责清晰
- **可扩展性**: 存储引擎、优化器、协议都可替换
- **高性能**: 每层都有大量优化
- **复杂性**: 支持SQL标准、事务、MVCC、复制等

理解这个流程，对于：
- 性能调优（知道瓶颈在哪）
- 故障排查（知道在哪层出问题）
- 源码贡献（知道如何修改）

都至关重要！

