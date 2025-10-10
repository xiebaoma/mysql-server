# MySQL INSERT语句与InnoDB Redo Log写入完整流程

## 文档概述

本文档详细分析一条INSERT语句在MySQL 8.0中的执行流程，重点关注InnoDB存储引擎如何写入redo log，确保数据的持久性。

## 一、整体流程概览

```
客户端发送INSERT语句
  ↓
SQL层解析和执行
  ↓
调用存储引擎接口 handler::ha_write_row()
  ↓
InnoDB: ha_innobase::write_row()
  ↓
InnoDB: row_insert_for_mysql()
  ↓
InnoDB: 插入到聚簇索引/二级索引
  ↓
InnoDB: 在mini-transaction(mtr)中记录redo log
  ↓
InnoDB: mtr提交，将redo log写入log buffer
  ↓
InnoDB: 后台线程或事务提交时，将log buffer刷入磁盘
  ↓
返回成功给客户端
```

## 二、SQL层处理 (Server Layer)

### 2.1 INSERT语句的入口

**文件**: `sql/sql_parse.cc`

```
mysql_execute_command(THD *thd, bool first_level)  [行2948]
  ↓
switch (lex->sql_command) {
  case SQLCOM_INSERT:
    ↓
    res = Sql_cmd_insert::execute(thd);
      ↓
      文件: sql/sql_insert.cc
      Sql_cmd_insert::execute_inner(THD *thd)
        ↓
        mysql_insert(thd, table_list)
          ↓
          循环处理每一行:
            write_record(thd, table, &info, &update)
}
```

### 2.2 write_record() - 写入记录的核心

**文件**: `sql/sql_insert.cc:1749`

```cpp
bool write_record(THD *thd, TABLE *table, COPY_INFO *info, COPY_INFO *update) {
  int error;
  
  // 增加统计计数
  info->stats.records++;
  
  // 处理重复键的情况 (DUP_REPLACE / DUP_UPDATE)
  if (duplicate_handling == DUP_REPLACE || duplicate_handling == DUP_UPDATE) {
    while ((error = table->file->ha_write_row(table->record[0]))) {
      // 处理重复键错误...
    }
  } else {
    // 普通INSERT，直接调用存储引擎
    error = table->file->ha_write_row(table->record[0]);  [行2119]
  }
  
  // 触发AFTER INSERT触发器
  if (table->triggers) {
    table->triggers->process_triggers(thd, TRG_EVENT_INSERT, 
                                      TRG_ACTION_AFTER, true);
  }
  
  return false;
}
```

### 2.3 handler::ha_write_row() - 存储引擎接口

**文件**: `sql/handler.cc:8004`

```cpp
int handler::ha_write_row(uchar *buf) {
  int error;
  
  // 标记事务为读写事务
  mark_trx_read_write();
  
  // 性能监控点
  MYSQL_TABLE_IO_WAIT(PSI_TABLE_WRITE_ROW, MAX_KEY, error,
                      { error = write_row(buf); })  // 调用虚函数
  
  // 记录binlog（用于复制）
  if (unlikely((error = binlog_log_row(table, nullptr, buf, log_func))))
    return error;
  
  return 0;
}
```

**关键点:**
- `write_row(buf)` 是虚函数，由具体存储引擎实现
- InnoDB的实现是 `ha_innobase::write_row()`

## 三、InnoDB存储引擎层

### 3.1 ha_innobase::write_row() - InnoDB入口

**文件**: `storage/innobase/handler/ha_innodb.cc:9015`

```cpp
int ha_innobase::write_row(uchar *record) {
  dberr_t error;
  
  // 增加写计数统计
  ha_statistic_increment(&System_status_var::ha_write_count);
  
  // 获取当前事务对象
  trx_t *trx = thd_to_trx(m_user_thd);  [行9030]
  
  // 检查只读模式
  if (high_level_read_only) {
    return HA_ERR_TABLE_READONLY;
  }
  
  // 处理AUTO_INCREMENT列
  if (table->next_number_field && record == table->record[0]) {
    error_result = update_auto_increment();  [行9067]
    auto_inc_used = true;
  }
  
  // 构建INSERT操作的模板（第一次时）
  if (m_prebuilt->mysql_template == nullptr ||
      m_prebuilt->template_type != ROW_MYSQL_WHOLE_ROW) {
    build_template(true);  [行9097]
  }
  
  // 进入InnoDB并发控制
  error = innobase_srv_conc_enter_innodb(m_prebuilt);
  
  // 执行真正的INSERT操作
  error = row_insert_for_mysql((byte *)record, m_prebuilt);  [行9107]
  
  return error;
}
```

### 3.2 row_insert_for_mysql() - InnoDB行插入

**文件**: `storage/innobase/row/row0mysql.cc:1340+`

对于普通表（非临时表），会调用常规插入流程：

```cpp
dberr_t row_insert_for_mysql(byte *mysql_rec, row_prebuilt_t *prebuilt) {
  trx_t *trx = prebuilt->trx;
  ins_node_t *node = prebuilt->ins_node;
  que_thr_t *thr;
  dberr_t err;
  
  // 步骤1: 获取插入节点的引用
  row_get_prebuilt_insert_row(prebuilt);
  node = prebuilt->ins_node;
  thr = que_fork_get_first_thr(prebuilt->ins_graph);
  
  // 步骤2: 转换MySQL格式到InnoDB格式
  row_mysql_to_innobase(node->row, prebuilt, mysql_rec);
  
  // 步骤3: 分配row_id（如果没有主键）
  dict_index_t *clust_index = node->table->first_index();
  if (!dict_index_is_unique(clust_index)) {
    dict_sys_write_row_id(node->row_id_buf,
                          dict_table_get_next_table_sess_row_id(node->table));
  }
  
  // 写入trx_id
  trx_write_trx_id(node->trx_id_buf,
                   dict_table_get_next_table_sess_trx_id(node->table));
  
  // 步骤4: 遍历所有索引，插入索引项
  dict_index_t *inserted_upto = nullptr;
  node->entry = UT_LIST_GET_FIRST(node->entry_list);
  
  for (dict_index_t *index = UT_LIST_GET_FIRST(node->table->indexes);
       index != nullptr; 
       index = UT_LIST_GET_NEXT(indexes, index)) {
    
    node->index = index;
    
    // 设置索引项的值
    err = row_ins_index_entry_set_vals(node->index, node->entry, node->row);
    
    // 插入到聚簇索引或二级索引
    if (index->is_clustered()) {
      err = row_ins_clust_index_entry(node->index, node->entry, thr, false);
    } else {
      err = row_ins_sec_index_entry(node->index, node->entry, thr, false);
    }
    
    if (err == DB_SUCCESS) {
      inserted_upto = index;
    } else {
      break;  // 出错则回滚
    }
  }
  
  return err;
}
```

### 3.3 row_ins_clust_index_entry() - 插入聚簇索引

**文件**: `storage/innobase/row/row0ins.cc`

```cpp
dberr_t row_ins_clust_index_entry(
    dict_index_t *index,
    dtuple_t *entry,
    que_thr_t *thr,
    bool dup_chk_only) {
  
  dberr_t err;
  mtr_t mtr;
  btr_pcur_t pcur;
  
  // 启动一个mini-transaction (mtr)
  mtr_start(&mtr);  // ★★★ 重要：mtr开始 ★★★
  
  // 设置mtr的日志模式
  if (index->table->is_temporary()) {
    mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);  // 临时表不记录redo
  } else {
    mtr_set_log_mode(&mtr, MTR_LOG_ALL);      // 普通表记录redo
  }
  
  // 在B+树中定位插入位置
  btr_cur_search_to_nth_level(
      index, 0, entry, PAGE_CUR_LE,
      BTR_MODIFY_LEAF,  // 叶子节点修改锁
      &pcur.btr_cur, 0,
      &mtr);
  
  // 执行乐观插入（不分裂页面）
  err = btr_cur_optimistic_insert(
      flags, &pcur.btr_cur, &offsets, &heap,
      entry, &insert_rec, &big_rec, n_ext, thr, &mtr);
  
  if (err == DB_SUCCESS) {
    // 插入成功，提交mtr
    mtr_commit(&mtr);  // ★★★ 重要：mtr提交，写入redo log ★★★
    return DB_SUCCESS;
  }
  
  // 如果乐观插入失败，需要分裂页面
  mtr_commit(&mtr);  // 先提交当前mtr
  
  // 重新开始一个mtr，执行悲观插入
  mtr_start(&mtr);
  
  err = btr_cur_pessimistic_insert(
      flags, &pcur.btr_cur, &offsets, &heap,
      entry, &insert_rec, &big_rec, n_ext, thr, &mtr);
  
  mtr_commit(&mtr);  // 提交mtr
  
  return err;
}
```

### 3.4 btr_cur_optimistic_insert() - B+树乐观插入

**文件**: `storage/innobase/btr/btr0cur.cc:2400+`

```cpp
dberr_t btr_cur_optimistic_insert(
    ulint flags,
    btr_cur_t *cursor,
    ulint **offsets,
    mem_heap_t **heap,
    dtuple_t *entry,
    rec_t **rec,
    big_rec_t **big_rec,
    ulint n_ext,
    que_thr_t *thr,
    mtr_t *mtr) {  // ★★★ mtr作为参数传入 ★★★
  
  page_t *page;
  rec_t *insert_rec;
  ulint max_size;
  
  // 获取当前页面
  page = btr_cur_get_page(cursor);
  block = btr_cur_get_block(cursor);
  
  // 检查页面是否有足够空间
  max_size = page_get_max_insert_size_after_reorganize(page, 1);
  
  if (max_size < rec_size) {
    return DB_FAIL;  // 空间不足，需要分裂页面
  }
  
  // 在页面中插入记录
  insert_rec = page_cur_tuple_insert(
      page_cursor, entry, index, offsets, heap, n_ext, mtr);
      // ★★★ mtr传递下去，用于记录页面修改 ★★★
  
  if (insert_rec == nullptr) {
    return DB_FAIL;
  }
  
  // 记录redo log：插入操作
  // 这在page_cur_tuple_insert内部通过mtr记录
  
  return DB_SUCCESS;
}
```

### 3.5 page_cur_tuple_insert() - 页面级别插入

**文件**: `storage/innobase/page/page0cur.cc`

```cpp
rec_t *page_cur_tuple_insert(
    page_cur_t *cursor,
    const dtuple_t *tuple,
    dict_index_t *index,
    ulint **offsets,
    mem_heap_t **heap,
    ulint n_ext,
    mtr_t *mtr) {  // ★★★ mtr继续传递 ★★★
  
  page_t *page = page_cur_get_page(cursor);
  rec_t *rec;
  ulint rec_size;
  
  // 将tuple转换为物理记录格式
  rec = rec_convert_dtuple_to_rec((byte *)mem_heap_alloc(*heap, rec_size),
                                  index, tuple, n_ext);
  
  // 在页面中插入记录
  rec = page_cur_insert_rec_low(cursor, index, rec, offsets, mtr);
  // ★★★ mtr传递给底层函数 ★★★
  
  return rec;
}
```

### 3.6 page_cur_insert_rec_low() - 底层插入和redo log记录

**文件**: `storage/innobase/page/page0cur.cc`

```cpp
rec_t *page_cur_insert_rec_low(
    page_cur_t *cursor,
    dict_index_t *index,
    const rec_t *rec,
    ulint *offsets,
    mtr_t *mtr) {  // ★★★ mtr在此处使用 ★★★
  
  page_t *page = page_cur_get_page(cursor);
  byte *insert_buf;
  rec_t *insert_rec;
  ulint rec_size = rec_offs_size(offsets);
  
  // 在页面中找到插入位置
  insert_rec = page_mem_alloc_heap(page, rec_size, &heap_no);
  
  // 复制记录数据
  memcpy(insert_rec, rec, rec_size);
  
  // ★★★ 记录redo log：页面插入操作 ★★★
  page_cur_insert_rec_write_log(insert_rec, rec_size, cursor, index, mtr);
  
  return insert_rec;
}

// 写入INSERT类型的redo log
void page_cur_insert_rec_write_log(
    rec_t *insert_rec,
    ulint rec_size,
    page_cur_t *cursor,
    dict_index_t *index,
    mtr_t *mtr) {
  
  byte *log_ptr;
  
  // 在mtr的内部buffer中分配空间
  log_ptr = mlog_open_and_write_index(
      mtr, insert_rec, index, MLOG_REC_INSERT, 2 + 5 + 1 + 5);
  
  if (log_ptr == nullptr) {
    return;
  }
  
  // 写入日志内容：
  // - 记录类型: MLOG_REC_INSERT
  // - 页面偏移量
  // - 记录大小
  // - 记录内容
  
  mlog_close(mtr, log_ptr);
  // ★★★ redo log记录已添加到mtr的buffer中 ★★★
}
```

## 四、Mini-Transaction (mtr) 机制

### 4.1 什么是mtr？

**Mini-Transaction（mtr）是InnoDB中的核心概念：**

1. **原子性单位**: 一个mtr包含对一个或多个页面的修改，这些修改要么全部恢复，要么全部不恢复
2. **WAL日志**: mtr在内部buffer中累积redo log记录
3. **生命周期**: 
   - `mtr_start()` - 开始
   - 执行页面修改操作（会自动记录redo log）
   - `mtr_commit()` - 提交，将redo log写入log buffer

**文件**: `storage/innobase/include/mtr0mtr.h`

```cpp
struct mtr_t {
  class Impl {
    mtr_buf_t m_log;      // mtr内部的redo log buffer
    mtr_buf_t m_memo;     // 记录持有的锁和latch
    mtr_log_t m_log_mode; // 日志模式（ALL/NONE等）
  };
  
  Impl m_impl;
  lsn_t m_commit_lsn;     // 提交后的LSN
};
```

### 4.2 mtr_start() - 启动mtr

**文件**: `storage/innobase/mtr/mtr0mtr.cc`

```cpp
void mtr_t::start() {
  m_impl.m_log_mode = MTR_LOG_ALL;  // 默认记录所有日志
  m_impl.m_log.create();            // 创建内部buffer
  m_impl.m_memo.create();           // 创建memo
  m_impl.m_state = MTR_STATE_ACTIVE;
}
```

### 4.3 mtr记录redo log

在页面修改过程中，通过以下函数记录redo log：

```cpp
// 记录单字节修改
void mlog_write_ulint(byte *ptr, ulint val, mlog_id_t type, mtr_t *mtr);

// 记录字符串修改
void mlog_write_string(byte *ptr, const byte *str, ulint len, mtr_t *mtr);

// 记录初始化页面
void mlog_write_initial_log_record(const byte *ptr, mlog_id_t type, mtr_t *mtr);
```

这些函数都会将log记录追加到 `mtr->m_impl.m_log` buffer中。

### 4.4 mtr_commit() - 提交mtr **（核心）**

**文件**: `storage/innobase/mtr/mtr0mtr.cc:842`

```cpp
void mtr_t::Command::execute() {
  ut_ad(m_impl->m_log_mode != MTR_LOG_NONE);
  
  // 步骤1: 准备写入，计算总大小
  ulint len = prepare_write();  [行846]
  
  if (len > 0) {
    // 步骤2: 在全局log buffer中预留空间
    auto handle = log_buffer_reserve(*log_sys, len);  [行853]
    // 返回分配的LSN范围：handle.start_lsn ~ handle.end_lsn
    
    // 步骤3: 将mtr的log buffer写入全局log buffer
    mtr_write_log_t write_log;
    write_log.m_left_to_write = len;
    write_log.m_handle = handle;
    write_log.m_lsn = handle.start_lsn;
    
    m_impl->m_log.for_each_block(write_log);  [行858]
    // 调用log_buffer_write()，将数据复制到log buffer
    
    // 步骤4: 等待log buffer有足够空间
    log_wait_for_space_in_log_recent_closed(*log_sys, handle.start_lsn);
    
    // 步骤5: 将修改的页面加入flush list（标记为脏页）
    add_dirty_blocks_to_flush_list(handle.start_lsn, handle.end_lsn);  [行867]
    
    // 步骤6: 关闭log buffer的这段空间，允许后台线程写入磁盘
    log_buffer_close(*log_sys, handle);  [行869]
    
    // 步骤7: 记录commit_lsn
    m_impl->m_mtr->m_commit_lsn = handle.end_lsn;  [行871]
  }
  
  // 步骤8: 释放所有持有的latch和锁
  release_all();
  release_resources();
}
```

**关键点说明:**

1. **LSN (Log Sequence Number)**: 
   - 全局递增的日志序列号
   - 每个字节的redo log都有唯一的LSN
   - 用于标识redo log的位置

2. **Log Buffer预留**:
   - 多个mtr可以并发预留空间
   - 预留时分配LSN范围
   - 实际写入时可能乱序，但最终按LSN排序

3. **Dirty Page标记**:
   - 修改的页面被标记为脏页
   - 记录页面的最新LSN
   - 刷脏页前必须先刷对应的redo log

## 五、Redo Log Buffer管理

### 5.1 全局Log系统

**文件**: `storage/innobase/include/log0sys.h`

```cpp
struct log_t {
  // Redo log buffer
  aligned_array_pointer<byte, OS_FILE_LOG_BLOCK_SIZE> buf;
  std::atomic<lsn_t> write_lsn;   // 已写入文件的LSN
  std::atomic<lsn_t> flushed_to_disk_lsn;  // 已刷盘的LSN
  
  // LSN相关
  std::atomic<lsn_t> buf_ready_for_write_lsn;  // 可写入的LSN
  std::atomic<lsn_t> buf_dirty_pages_added_up_to_lsn;  // 脏页添加的LSN
  
  // 后台线程
  // log_writer  - 将log buffer写入文件
  // log_flusher - 将文件fsync到磁盘
  // log_write_notifier - 通知写完成
  // log_flush_notifier - 通知刷盘完成
};

extern log_t *log_sys;  // 全局log系统实例
```

### 5.2 log_buffer_reserve() - 预留空间

**文件**: `storage/innobase/log/log0buf.cc`

```cpp
Log_handle log_buffer_reserve(log_t &log, size_t len) {
  lsn_t start_lsn;
  lsn_t end_lsn;
  
  // 原子地递增并获取LSN范围
  start_lsn = log.sn_locked.fetch_add(len);
  end_lsn = start_lsn + len;
  
  // 等待log buffer有足够空间
  while (log_buffer_capacity(log) < len) {
    std::this_thread::sleep_for(std::chrono::microseconds(20));
  }
  
  return {start_lsn, end_lsn};
}
```

### 5.3 log_buffer_write() - 写入log buffer

**文件**: `storage/innobase/log/log0buf.cc`

```cpp
lsn_t log_buffer_write(
    log_t &log,
    const byte *str,
    size_t str_len,
    lsn_t start_lsn) {
  
  lsn_t end_lsn = start_lsn + str_len;
  
  // 将数据写入log buffer的指定LSN位置
  // log buffer是循环缓冲区
  size_t write_offset = start_lsn % log.buf_size;
  
  // 可能需要分两次写（环形缓冲区）
  if (write_offset + str_len > log.buf_size) {
    size_t first_part = log.buf_size - write_offset;
    memcpy(log.buf + write_offset, str, first_part);
    memcpy(log.buf, str + first_part, str_len - first_part);
  } else {
    memcpy(log.buf + write_offset, str, str_len);
  }
  
  return end_lsn;
}
```

### 5.4 add_dirty_blocks_to_flush_list() - 标记脏页

**文件**: `storage/innobase/mtr/mtr0mtr.cc:831`

```cpp
void mtr_t::Command::add_dirty_blocks_to_flush_list(
    lsn_t start_lsn, lsn_t end_lsn) {
  
  Add_dirty_blocks_to_flush_list add_to_flush(start_lsn, end_lsn, ...);
  
  // 遍历mtr持有的所有页面
  m_impl->m_memo.for_each_block_in_reverse([&](buf_block_t *block) {
    // 将页面加入buffer pool的flush list
    // 记录页面的oldest_modification = start_lsn
    // 记录页面的newest_modification = end_lsn
    buf_flush_note_modification(block, start_lsn, end_lsn);
  });
}
```

**关键概念：**
- **oldest_modification**: 页面第一次修改的LSN
- **newest_modification**: 页面最新修改的LSN
- **Flush List**: 按oldest_modification排序的脏页链表
- **刷脏规则**: 刷一个页面前，必须先确保其newest_modification对应的redo log已刷盘

## 六、Redo Log后台线程

### 6.1 四个后台线程

**文件**: `storage/innobase/log/log0write.cc`

InnoDB有四个专门处理redo log的后台线程：

#### 1. log_writer 线程

```cpp
void log_writer(log_t *log_ptr) {
  while (true) {
    // 等待有新数据需要写入
    log_writer_wait_on_new_writer_request(log);
    
    // 将log buffer写入文件（不fsync）
    log_writer_write_buffer(log);
    
    // 更新write_lsn
    log.write_lsn.store(new_write_lsn);
    
    // 通知写入完成
    os_event_set(log.write_notifier_event);
  }
}
```

**功能**: 
- 将log buffer中的数据写入redo log文件
- 使用普通write()，不进行fsync
- 更新`write_lsn`，表示已写入文件的位置

#### 2. log_flusher 线程

```cpp
void log_flusher(log_t *log_ptr) {
  while (true) {
    // 等待需要刷盘的请求
    log_flusher_wait_on_new_request(log);
    
    // fsync redo log文件
    log_flusher_do_fsync(log);
    
    // 更新flushed_to_disk_lsn
    log.flushed_to_disk_lsn.store(new_flush_lsn);
    
    // 通知刷盘完成
    os_event_set(log.flush_notifier_event);
  }
}
```

**功能**:
- 调用fsync()将redo log持久化到磁盘
- 更新`flushed_to_disk_lsn`
- 保证数据持久性

#### 3. log_write_notifier 线程

```cpp
void log_write_notifier(log_t *log_ptr) {
  while (true) {
    os_event_wait(log.write_notifier_event);
    
    // 通知所有等待写入完成的线程
    log_write_notifier_notify(log);
  }
}
```

**功能**: 唤醒等待redo log写入的用户线程

#### 4. log_flush_notifier 线程

```cpp
void log_flush_notifier(log_t *log_ptr) {
  while (true) {
    os_event_wait(log.flush_notifier_event);
    
    // 通知所有等待刷盘完成的线程
    log_flush_notifier_notify(log);
  }
}
```

**功能**: 唤醒等待redo log刷盘的用户线程（如事务提交）

### 6.2 写入和刷盘的触发时机

#### 写入文件的触发条件:
1. Log buffer使用超过50%
2. 每秒定时触发（后台线程）
3. Checkpoint需要推进
4. 用户事务提交请求

#### 刷盘(fsync)的触发条件:
根据参数 `innodb_flush_log_at_trx_commit`:

- **0**: 每秒刷一次（由后台线程）
- **1**: 每次事务提交时刷盘（最安全，性能最低）
- **2**: 每次事务提交时写入文件，每秒刷盘一次

## 七、事务提交与Redo Log刷盘

### 7.1 事务提交流程

**文件**: `storage/innobase/handler/ha_innodb.cc:5710+`

```cpp
static int innobase_commit(handlerton *hton, THD *thd, bool commit_trx) {
  trx_t *trx = check_trx_exists(thd);
  
  if (commit_trx) {
    // 提交整个事务
    
    if (!trx->read_only) {
      // 读写事务需要刷redo log
      // 设置flush_log_later = true，延迟刷盘（用于group commit）
      trx->flush_log_later = true;  [行5873]
    }
    
    // 提交事务
    innobase_commit_low(trx);  [行5881]
    
    if (!trx->read_only) {
      trx->flush_log_later = false;
      
      // 现在执行redo log的写入和刷盘
      if (srv_flush_log_at_trx_commit == 1) {
        // ★★★ 刷盘点 ★★★
        log_buffer_flush_to_disk(true);
      } else if (srv_flush_log_at_trx_commit == 2) {
        // 只写入文件，不fsync
        log_buffer_flush_to_disk(false);
      }
      // 如果是0，则不做任何操作，由后台线程定时刷
    }
  }
  
  return 0;
}
```

### 7.2 innobase_commit_low() - 底层提交

**文件**: `storage/innobase/handler/ha_innodb.cc`

```cpp
void innobase_commit_low(trx_t *trx) {
  // 调用InnoDB的事务提交
  trx_commit_for_mysql(trx);
}
```

**文件**: `storage/innobase/trx/trx0trx.cc`

```cpp
void trx_commit_for_mysql(trx_t *trx) {
  // 提交undo log相关的mtr
  // 这会生成更多的redo log记录
  
  trx_commit(trx);
}

void trx_commit(trx_t *trx) {
  // 如果有undo log需要持久化
  if (trx->rsegs.m_redo.insert_undo != nullptr ||
      trx->rsegs.m_redo.update_undo != nullptr) {
    
    mtr_t mtr;
    mtr_start(&mtr);
    
    // 标记undo log为已提交状态
    trx_write_serialisation_history(trx, &mtr);
    
    mtr_commit(&mtr);  // ★★★ 最后一个mtr，写入COMMIT记录 ★★★
  }
  
  // 释放锁
  trx_release_impl_and_expl_locks(trx, true);
  
  // 标记事务为已提交
  trx->state = TRX_STATE_COMMITTED_IN_MEMORY;
}
```

### 7.3 log_buffer_flush_to_disk() - 刷盘函数

**文件**: `storage/innobase/log/log0write.cc:2700+`

```cpp
void log_buffer_flush_to_disk(bool sync) {
  lsn_t lsn = log_get_lsn(*log_sys);
  
  if (sync) {
    // 等待redo log写入文件并fsync
    log_write_up_to(*log_sys, lsn, true);  // ★★★ 同步刷盘 ★★★
  } else {
    // 只等待redo log写入文件（不fsync）
    log_write_up_to(*log_sys, lsn, false);
  }
}
```

### 7.4 log_write_up_to() - 等待写入/刷盘

**文件**: `storage/innobase/log/log0write.cc`

```cpp
void log_write_up_to(log_t &log, lsn_t lsn, bool flush_to_disk) {
  // 如果已经刷到指定LSN，直接返回
  if (flush_to_disk) {
    if (log.flushed_to_disk_lsn.load() >= lsn) {
      return;
    }
  } else {
    if (log.write_lsn.load() >= lsn) {
      return;
    }
  }
  
  // 唤醒log_writer线程
  os_event_set(log.writer_event);
  
  if (flush_to_disk) {
    // 唤醒log_flusher线程
    os_event_set(log.flusher_event);
    
    // 等待刷盘完成
    while (log.flushed_to_disk_lsn.load() < lsn) {
      // 等待flush_notifier通知
      log_wait_for_flush(log, lsn);
    }
  } else {
    // 等待写入完成
    while (log.write_lsn.load() < lsn) {
      log_wait_for_write(log, lsn);
    }
  }
}
```

## 八、完整的INSERT流程示例

### 示例SQL
```sql
INSERT INTO users (name, age) VALUES ('Alice', 25);
```

### 完整调用栈和redo log流程

```
┌────────────────────────────────────────────────────────────────┐
│ 1. SQL层处理                                                    │
└────────────────────────────────────────────────────────────────┘

mysql_execute_command()
  └─ mysql_insert()
      └─ write_record()
          └─ handler::ha_write_row()
              └─ ha_innobase::write_row()
                  │
                  ├─ trx = thd_to_trx(m_user_thd)  // 获取事务对象
                  ├─ update_auto_increment()       // 处理自增ID
                  └─ row_insert_for_mysql()        // ↓ 进入InnoDB层

┌────────────────────────────────────────────────────────────────┐
│ 2. InnoDB行插入                                                 │
└────────────────────────────────────────────────────────────────┘

row_insert_for_mysql()
  ├─ row_mysql_to_innobase()  // MySQL格式 → InnoDB格式
  ├─ 分配row_id/trx_id
  │
  └─ 遍历所有索引（聚簇索引 + 二级索引）
      │
      ├─ 聚簇索引: row_ins_clust_index_entry()
      │   │
      │   ├─ mtr_start(&mtr);  // ★★★ 开始mini-transaction ★★★
      │   │
      │   ├─ btr_cur_search_to_nth_level()  // 在B+树中定位
      │   │   └─ 从根节点向下搜索到叶子节点
      │   │
      │   ├─ btr_cur_optimistic_insert()    // 乐观插入
      │   │   └─ page_cur_tuple_insert()
      │   │       └─ page_cur_insert_rec_low()
      │   │           │
      │   │           ├─ 在页面中插入记录
      │   │           │
      │   │           └─ page_cur_insert_rec_write_log()  
      │   │               // ★★★ 记录redo log到mtr ★★★
      │   │               └─ mlog_write_index()
      │   │                   // 在mtr->m_impl.m_log中添加：
      │   │                   // [MLOG_REC_INSERT][页面ID][偏移量][记录内容]
      │   │
      │   └─ mtr_commit(&mtr);  // ★★★ 提交mtr ★★★
      │       │
      │       └─ mtr_t::Command::execute()  // [mtr0mtr.cc:842]
      │
      └─ 二级索引: row_ins_sec_index_entry()
          └─ （类似的流程，也使用mtr记录redo log）

┌────────────────────────────────────────────────────────────────┐
│ 3. mtr提交 - Redo Log写入Log Buffer                            │
└────────────────────────────────────────────────────────────────┘

mtr_t::Command::execute()  [mtr0mtr.cc:842]
  │
  ├─ 步骤1: prepare_write()
  │   └─ 计算mtr buffer中的log记录总大小: len
  │
  ├─ 步骤2: log_buffer_reserve(*log_sys, len)  [行853]
  │   └─ 在全局log buffer中分配空间
  │       ├─ start_lsn = log_sys->sn.fetch_add(len)
  │       ├─ end_lsn = start_lsn + len
  │       └─ return {start_lsn, end_lsn}
  │
  ├─ 步骤3: 写入log buffer
  │   └─ m_impl->m_log.for_each_block(write_log)  [行858]
  │       └─ log_buffer_write(*log_sys, block->data, block->size, start_lsn)
  │           └─ memcpy(log_sys->buf + offset, data, size)
  │               // 将mtr的log复制到全局log buffer
  │
  ├─ 步骤4: add_dirty_blocks_to_flush_list(start_lsn, end_lsn)  [行867]
  │   └─ 将修改的页面加入flush list，标记为脏页
  │       └─ buf_flush_note_modification(block, start_lsn, end_lsn)
  │           ├─ block->oldest_modification = start_lsn
  │           ├─ block->newest_modification = end_lsn
  │           └─ 插入buffer_pool->flush_list
  │
  ├─ 步骤5: log_buffer_close(*log_sys, handle)  [行869]
  │   └─ 标记这段LSN空间已完成，可以被后台线程写入磁盘
  │
  └─ m_impl->m_mtr->m_commit_lsn = end_lsn;  [行871]
      // 记录此mtr的commit LSN

┌────────────────────────────────────────────────────────────────┐
│ 4. 后台线程 - 将Log Buffer写入文件                              │
└────────────────────────────────────────────────────────────────┘

log_writer 线程:  [log0write.cc]
  └─ while (true) {
      ├─ 等待新的写入请求或定时唤醒
      ├─ log_writer_write_buffer(log)
      │   └─ write(log_file_fd, log.buf + offset, size)
      │       // 将log buffer写入redo log文件（不fsync）
      │
      ├─ log.write_lsn.store(new_write_lsn)  // 更新write_lsn
      └─ os_event_set(log.write_notifier_event)  // 通知写入完成
    }

log_write_notifier 线程:
  └─ while (true) {
      ├─ os_event_wait(log.write_notifier_event)
      └─ 唤醒所有等待write_lsn的线程
    }

此时状态:
  ✓ Redo log已写入文件（在OS缓存中）
  ✗ 还未持久化到磁盘（需要fsync）

┌────────────────────────────────────────────────────────────────┐
│ 5. 事务提交 - Redo Log刷盘                                      │
└────────────────────────────────────────────────────────────────┘

用户执行: COMMIT;

  └─ innobase_commit()  [ha_innodb.cc:5710]
      │
      ├─ 步骤1: 提交undo log
      │   └─ innobase_commit_low(trx)
      │       └─ trx_commit_for_mysql(trx)
      │           └─ trx_commit(trx)
      │               │
      │               ├─ mtr_start(&mtr)
      │               ├─ trx_write_serialisation_history(trx, &mtr)
      │               │   // 标记undo log为已提交
      │               └─ mtr_commit(&mtr)  // 写入COMMIT记录到log buffer
      │
      ├─ 步骤2: 根据innodb_flush_log_at_trx_commit决定刷盘策略
      │   │
      │   └─ if (srv_flush_log_at_trx_commit == 1) {
      │       // ★★★ 最安全：立即刷盘 ★★★
      │       log_buffer_flush_to_disk(true);
      │         │
      │         └─ log_write_up_to(*log_sys, commit_lsn, flush_to_disk=true)
      │             │
      │             ├─ 唤醒log_writer线程（如果还未写入文件）
      │             ├─ 唤醒log_flusher线程
      │             │
      │             └─ 等待log.flushed_to_disk_lsn >= commit_lsn
      │   }

log_flusher 线程:  [log0write.cc]
  └─ while (true) {
      ├─ 等待刷盘请求
      ├─ log_flusher_do_fsync(log)
      │   └─ fsync(log_file_fd)  // ★★★ 持久化到磁盘 ★★★
      │
      ├─ log.flushed_to_disk_lsn.store(new_flush_lsn)
      └─ os_event_set(log.flush_notifier_event)  // 通知刷盘完成
    }

log_flush_notifier 线程:
  └─ while (true) {
      ├─ os_event_wait(log.flush_notifier_event)
      └─ 唤醒所有等待flushed_to_disk_lsn的线程
    }

用户线程被唤醒:
  └─ log_write_up_to() 返回
      └─ innobase_commit() 返回
          └─ 向客户端返回 "Query OK, 1 row affected"

此时状态:
  ✓ Redo log已持久化到磁盘
  ✓ 事务已提交
  ✓ 即使此时断电，也能从redo log恢复这条INSERT

┌────────────────────────────────────────────────────────────────┐
│ 6. 后续 - 脏页刷新（异步，由后台线程完成）                        │
└────────────────────────────────────────────────────────────────┘

page_cleaner 线程组:
  └─ while (true) {
      ├─ 从flush_list选择脏页
      ├─ 检查页面的newest_modification LSN
      ├─ 等待对应的redo log已刷盘
      │   └─ log.flushed_to_disk_lsn >= page.newest_modification
      │
      ├─ 将页面写入数据文件
      │   └─ write(data_file_fd, page, PAGE_SIZE)
      │
      └─ 标记页面为clean，从flush_list移除
    }
```

## 九、Redo Log的关键配置参数

### 9.1 innodb_flush_log_at_trx_commit

**最重要的参数，控制redo log刷盘策略：**

```sql
-- 查看当前设置
SHOW VARIABLES LIKE 'innodb_flush_log_at_trx_commit';

-- 修改设置
SET GLOBAL innodb_flush_log_at_trx_commit = 1;
```

**取值说明：**

| 值 | 行为 | 持久性 | 性能 | 适用场景 |
|----|------|--------|------|---------|
| **0** | 每秒写入文件并fsync | 最低（可能丢失1秒数据） | 最高 | 可容忍少量数据丢失 |
| **1** | 每次提交都fsync | 最高（不会丢失已提交事务） | 最低 | 生产环境推荐 |
| **2** | 每次提交写入文件，每秒fsync | 中等（OS崩溃不丢失） | 中等 | 折中方案 |

**详细说明：**

- **0 - 每秒刷盘**:
  ```
  事务提交 → 写入log buffer（内存）
  后台线程 → 每秒写入文件 → 每秒fsync
  
  风险：MySQL崩溃或OS崩溃，可能丢失最后1秒的事务
  ```

- **1 - 每次提交刷盘（默认）**:
  ```
  事务提交 → 写入log buffer → 写入文件 → fsync刷盘 → 返回客户端
  
  保证：已提交事务绝不丢失（即使OS崩溃）
  代价：每次提交都有磁盘IO，性能最低
  ```

- **2 - 每次提交写文件**:
  ```
  事务提交 → 写入log buffer → 写入文件（OS缓存）→ 返回客户端
  后台线程 → 每秒fsync
  
  保证：MySQL崩溃不丢失，但OS崩溃可能丢失1秒数据
  性能：比1好，比0差
  ```

### 9.2 innodb_log_buffer_size

```sql
-- Redo log buffer的大小（默认16MB）
SET GLOBAL innodb_log_buffer_size = 16777216;
```

**说明：**
- 太小：频繁触发写入，增加IO
- 太大：浪费内存
- 推荐：16MB ~ 64MB

### 9.3 innodb_log_file_size（8.0.30前）

```sql
-- 单个redo log文件大小（8.0.30之前）
innodb_log_file_size = 512M
```

**MySQL 8.0.30+改进：**
- 新参数：`innodb_redo_log_capacity`（总容量）
- 自动管理redo log文件数量和大小
- 更灵活的空间管理

### 9.4 innodb_log_group_home_dir

```sql
-- Redo log文件存放目录
innodb_log_group_home_dir = /path/to/redo/logs
```

**性能优化：**
- 将redo log放在SSD上
- 与数据文件分离到不同磁盘

### 9.5 innodb_log_write_ahead_size

```sql
-- 预写大小，避免read-modify-write
SET GLOBAL innodb_log_write_ahead_size = 8192;
```

## 十、Redo Log相关数据结构

### 10.1 mtr_t - Mini-Transaction

**文件**: `storage/innobase/include/mtr0mtr.h`

```cpp
struct mtr_t {
  class Impl {
    mtr_buf_t m_log;        // redo log buffer (链表形式)
    mtr_buf_t m_memo;       // 持有的锁和latch
    mtr_log_t m_log_mode;   // MTR_LOG_ALL / MTR_LOG_NONE / MTR_LOG_NO_REDO
    ulint m_n_log_recs;     // log记录数量
  };
  
  Impl m_impl;
  lsn_t m_commit_lsn;       // 提交后的LSN
};
```

### 10.2 log_t - 全局Log系统

**文件**: `storage/innobase/include/log0sys.h`

```cpp
struct log_t {
  // Log Buffer
  aligned_array_pointer<byte, OS_FILE_LOG_BLOCK_SIZE> buf;
  size_t buf_size;
  
  // LSN相关
  std::atomic<lsn_t> sn;                          // 下一个可分配的SN
  std::atomic<lsn_t> write_lsn;                   // 已写入文件的LSN
  std::atomic<lsn_t> flushed_to_disk_lsn;         // 已刷盘的LSN
  std::atomic<lsn_t> buf_ready_for_write_lsn;     // 可写入的LSN
  
  // Checkpoint
  std::atomic<lsn_t> last_checkpoint_lsn;         // 最后checkpoint的LSN
  
  // 后台线程
  std::thread m_writer_thread;                    // log_writer线程
  std::thread m_flusher_thread;                   // log_flusher线程
  std::thread m_write_notifier_thread;            
  std::thread m_flush_notifier_thread;
  
  // 事件同步
  os_event_t writer_event;                        // 唤醒writer
  os_event_t flusher_event;                       // 唤醒flusher
  os_event_t write_notifier_event;                // 写完成通知
  os_event_t flush_notifier_event;                // 刷盘完成通知
  
  // Redo log文件
  Log_files_dict m_files;                         // 文件字典
  Log_files_capacity m_capacity;                  // 容量管理
};
```

### 10.3 Log_handle - LSN范围

```cpp
struct Log_handle {
  lsn_t start_lsn;  // 起始LSN
  lsn_t end_lsn;    // 结束LSN
};
```

### 10.4 Redo Log Record格式

```
+----------------+------------------+------------------+
| Type (1 byte)  | Space ID (varint)| Page No (varint) |
+----------------+------------------+------------------+
| Data Length    |      Data                           |
+----------------+-------------------------------------+
```

**常见类型：**
- `MLOG_REC_INSERT` - 插入记录
- `MLOG_REC_UPDATE` - 更新记录
- `MLOG_REC_DELETE` - 删除记录
- `MLOG_COMP_REC_INSERT` - 压缩格式插入
- `MLOG_PAGE_CREATE` - 创建页面
- `MLOG_UNDO_INSERT` - 插入undo记录

## 十一、性能优化建议

### 11.1 Group Commit（组提交）

**原理**：
多个事务的redo log可以批量刷盘，减少fsync调用次数。

**MySQL 8.0自动实现：**
```cpp
// handler/ha_innodb.cc:5873
trx->flush_log_later = true;  // 延迟刷盘

// 多个事务都设置flush_log_later
// 然后统一调用一次log_buffer_flush_to_disk()
// 一次fsync刷新多个事务的redo log
```

**效果**：
- 高并发下，多个事务共享一次fsync
- 显著提升吞吐量

### 11.2 Binlog与Redo Log的两阶段提交

**保证**：binlog和redo log的一致性

```
事务提交流程:
  1. Prepare阶段 - 写入redo log，但标记为PREPARE
  2. Commit阶段 - 写入binlog，然后写入redo log COMMIT记录
  3. 刷盘
```

**崩溃恢复**：
- 如果只有PREPARE记录，检查binlog：
  - 有binlog → 提交事务
  - 无binlog → 回滚事务

### 11.3 异步IO

**InnoDB使用异步IO写redo log：**
- Linux：使用native AIO (io_submit/io_getevents)
- 其他平台：使用模拟异步IO

**配置：**
```sql
SET GLOBAL innodb_use_native_aio = ON;
```

### 11.4 Redo Log文件放在SSD

```
# my.cnf
innodb_log_group_home_dir = /ssd/mysql/redolog
```

**收益**：
- SSD的随机IO性能远超机械硬盘
- 显著降低fsync延迟

## 十二、故障恢复流程

### 12.1 Redo Log在崩溃恢复中的作用

**文件**: `storage/innobase/log/log0recv.cc`

```
MySQL启动
  ↓
InnoDB初始化
  ↓
读取redo log文件
  ↓
从last_checkpoint_lsn开始扫描
  ↓
解析redo log记录
  ↓
应用到对应的页面
  ↓
Redo log应用完成
  ↓
启动完成，接受连接
```

**关键函数：**
```cpp
void recv_recovery_from_checkpoint_start(log_t &log, lsn_t flush_lsn) {
  // 1. 读取checkpoint信息
  lsn_t checkpoint_lsn = log.last_checkpoint_lsn;
  
  // 2. 从checkpoint_lsn开始扫描redo log
  recv_sys->parse_start_lsn = checkpoint_lsn;
  
  // 3. 解析所有redo log记录
  while (lsn < flush_lsn) {
    recv_parse_log_recs(lsn, ...);
  }
  
  // 4. 应用redo log到页面
  recv_apply_hashed_log_recs(true);
}
```

### 12.2 恢复示例

**场景**: INSERT后崩溃

```
1. INSERT执行:
   - mtr记录MLOG_REC_INSERT到log buffer
   - mtr提交，LSN=1000
   - log buffer写入文件并fsync
   - 页面标记为脏，但还未刷新到磁盘

2. 系统崩溃（此时脏页丢失）

3. 重启恢复:
   - 从checkpoint_lsn=900开始扫描
   - 读到LSN=1000的MLOG_REC_INSERT记录
   - 读取对应的数据页（旧版本，没有这条INSERT）
   - 应用redo log：重新插入这条记录
   - 页面恢复到崩溃前的状态

4. 恢复完成，数据不丢失
```

## 十三、监控和诊断

### 13.1 查看Redo Log状态

```sql
-- 查看当前LSN
SHOW ENGINE INNODB STATUS\G

-- 输出示例：
---
LOG
---
Log sequence number          123456789        -- 当前LSN
Log buffer assigned up to    123456789        -- 已分配的LSN
Log buffer completed up to   123456780        -- 已完成的LSN
Log written up to            123456780        -- 已写入文件的LSN
Log flushed up to            123456780        -- 已刷盘的LSN
Last checkpoint at           123456700        -- 最后checkpoint
```

### 13.2 Performance Schema监控

```sql
-- Redo log写入等待
SELECT * FROM performance_schema.events_waits_summary_global_by_event_name
WHERE EVENT_NAME LIKE '%log%';

-- Redo log写入次数和大小
SELECT * FROM performance_schema.file_summary_by_instance
WHERE FILE_NAME LIKE '%redo%';
```

### 13.3 关键指标

1. **Checkpoint Age**: `当前LSN - 最后checkpoint LSN`
   - 过大表示脏页太多，需要加快刷脏

2. **Log Buffer使用率**: `(write_lsn - flush_lsn) / log_buffer_size`
   - 过高表示后台线程跟不上

3. **Flush频率**: 根据`innodb_flush_log_at_trx_commit`监控fsync调用

## 十四、总结

### 14.1 核心流程回顾

```
INSERT语句执行
  ↓
1. SQL层: write_record() → handler::ha_write_row()
  ↓
2. InnoDB层: ha_innobase::write_row() → row_insert_for_mysql()
  ↓
3. 索引层: row_ins_clust_index_entry() → btr_cur_optimistic_insert()
  ↓
4. 页面层: page_cur_insert_rec_low()
  ↓
5. 日志层: page_cur_insert_rec_write_log() → 记录到mtr buffer
  ↓
6. mtr提交: mtr_commit()
   ├─ log_buffer_reserve() → 分配LSN范围
   ├─ log_buffer_write() → 写入log buffer
   ├─ add_dirty_blocks_to_flush_list() → 标记脏页
   └─ log_buffer_close() → 允许后台线程写入
  ↓
7. 后台线程:
   ├─ log_writer: write() → 写入文件
   └─ log_flusher: fsync() → 持久化
  ↓
8. 事务提交: innobase_commit()
   └─ log_write_up_to() → 等待redo log刷盘
  ↓
9. 返回客户端: "Query OK"
```

### 14.2 关键点总结

1. **WAL (Write-Ahead Logging)**:
   - 先写redo log，后写数据页
   - 保证崩溃可恢复

2. **Mini-Transaction**:
   - 原子性单位
   - 提交时一次性写入log buffer

3. **Log Buffer**:
   - 全局循环缓冲区
   - 多个mtr并发写入

4. **后台线程**:
   - log_writer: 写入文件
   - log_flusher: fsync刷盘
   - 用户线程不直接操作磁盘

5. **Group Commit**:
   - 批量刷盘
   - 提升并发性能

6. **参数调优**:
   - `innodb_flush_log_at_trx_commit`: 持久性与性能平衡
   - `innodb_log_buffer_size`: 内存使用
   - `innodb_log_file_size`: 磁盘空间

### 14.3 性能考虑

**影响INSERT性能的因素：**

1. **Redo Log刷盘**:
   - `innodb_flush_log_at_trx_commit=1`: 最慢但最安全
   - 使用SSD可显著提升

2. **索引数量**:
   - 每个索引都需要插入
   - 每个索引都记录redo log

3. **页面分裂**:
   - 悲观插入比乐观插入慢
   - 需要额外的redo log记录

4. **事务大小**:
   - 批量INSERT比逐条快
   - Group Commit更有效

**优化建议：**
- 批量插入而不是逐条
- 合理设置`innodb_flush_log_at_trx_commit`
- 将redo log放在快速存储上
- 监控checkpoint age，及时调整刷脏策略

---

**文档版本**: 1.0  
**适用版本**: MySQL 8.0+  
**更新日期**: 2024

