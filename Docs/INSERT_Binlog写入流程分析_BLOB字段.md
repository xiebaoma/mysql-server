# MySQL INSERT 语句 Binlog 写入流程分析（关注 BLOB 大字段）

## 文档概述

本文档详细分析 MySQL 8.0 中执行 INSERT 语句时如何将数据写入 binlog（二进制日志），**重点关注 BLOB/TEXT 等大字段的处理机制**。

## 目录

1. [整体流程概览](#1-整体流程概览)
2. [SQL 层处理](#2-sql-层处理)
3. [Handler 层处理](#3-handler-层处理)
4. [Binlog 记录流程](#4-binlog-记录流程)
5. [BLOB 字段的打包机制](#5-blob-字段的打包机制)
6. [Rows Event 的数据管理](#6-rows-event-的数据管理)
7. [Binlog Cache 机制](#7-binlog-cache-机制)
8. [事务提交与刷盘](#8-事务提交与刷盘)
9. [内存管理与优化](#9-内存管理与优化)
10. [相关配置参数](#10-相关配置参数)

---

## 1. 整体流程概览

```
客户端发送 INSERT 语句
  ↓
【SQL 层】解析和执行
  ↓
write_record() - sql/sql_insert.cc
  ↓
【Handler 层】ha_write_row() - sql/handler.cc:8004
  ↓
存储引擎写入数据（如 InnoDB::write_row()）
  ↓
【Binlog 层】binlog_log_row() - sql/handler.cc:8029
  ↓
THD::binlog_write_row() - sql/binlog.cc:11097
  ↓
【打包层】pack_row() - sql/rpl_record.cc:270
  ├─ 处理 NULL bits
  ├─ 处理普通字段
  └─ 处理 BLOB 字段 → Field_blob::pack() - sql/field.cc:7355
      ├─ 存储 BLOB 长度（1-4 字节）
      └─ 复制 BLOB 完整数据
  ↓
【事件层】Rows_log_event::do_add_row_data() - sql/log_event.cc:8149
  ├─ 将打包数据添加到 m_rows_buf
  └─ 自动扩展缓冲区（如需要）
  ↓
【缓存层】数据写入 Binlog Cache (IO_CACHE)
  ├─ 先写入内存缓冲区
  └─ 超过大小后写入临时文件
  ↓
【提交层】事务提交时 ordered_commit()
  ├─ Stage 1: FLUSH - 将 cache 写入 binlog 文件
  ├─ Stage 2: SYNC - fsync 刷盘（根据 sync_binlog）
  └─ Stage 3: COMMIT - 提交存储引擎事务
  ↓
完成
```

---

## 2. SQL 层处理

### 2.1 INSERT 语句入口

**文件**: `sql/sql_parse.cc`

```cpp
mysql_execute_command(THD *thd, bool first_level)
  ↓
  case SQLCOM_INSERT:
    Sql_cmd_insert::execute(thd);
```

**文件**: `sql/sql_insert.cc`

```cpp
Sql_cmd_insert::execute_inner(THD *thd)
  ↓
  mysql_insert(thd, table_list)
    ↓
    // 循环处理每一行
    write_record(thd, table, &info, &update)
```

### 2.2 write_record() 函数

**文件**: `sql/sql_insert.cc:1749`

这是实际写入记录的核心函数，它会：
1. 调用 `table->file->ha_write_row()` 写入存储引擎
2. 同时触发 binlog 记录

---

## 3. Handler 层处理

### 3.1 ha_write_row() 函数

**文件**: `sql/handler.cc:8004`

```cpp
int handler::ha_write_row(uchar *buf) {
  int error;
  Log_func *log_func = Write_rows_log_event::binlog_row_logging_function;
  
  // 标记事务为读写事务
  mark_trx_read_write();
  
  // 1. 调用存储引擎的 write_row() 写入数据
  MYSQL_TABLE_IO_WAIT(PSI_TABLE_WRITE_ROW, MAX_KEY, error,
                      { error = write_row(buf); })
  
  if (unlikely(error)) return error;
  
  // 2. 记录 binlog（用于主从复制）
  if (unlikely((error = binlog_log_row(table, nullptr, buf, log_func))))
    return error;
  
  return 0;
}
```

**关键点**：
- 先写存储引擎（如 InnoDB），再记录 binlog
- `buf` 指向 `table->record[0]`，包含要写入的完整记录
- 对于 BLOB 字段，`record[0]` 中存储的是：长度（1-4字节）+ 指针（8字节）

### 3.2 binlog_log_row() 函数

**文件**: `sql/handler.cc:7863`

```cpp
static bool binlog_log_row(
    TABLE *table,
    const uchar *before_record,
    const uchar *after_record,
    Log_func *log_func) {
  
  THD *const thd = table->in_use;
  
  if (check_table_binlog_row_based(thd, table)) {
    // 调用具体的 log 函数（Write/Update/Delete）
    return (*log_func)(thd, table, table->file->has_transactions(),
                      before_record, after_record);
  }
  return false;
}
```

---

## 4. Binlog 记录流程

### 4.1 THD::binlog_write_row()

**文件**: `sql/binlog.cc:11097`

```cpp
int THD::binlog_write_row(TABLE *table, bool is_trans, 
                          uchar const *record,
                          const unsigned char *extra_row_info) {
  assert(is_current_stmt_binlog_format_row() && mysql_bin_log.is_open());
  
  // 1. 分配内存用于打包行数据
  Row_data_memory memory(table, record);
  if (!memory.has_memory()) return HA_ERR_OUT_OF_MEM;
  
  uchar *row_data = memory.slot(0);
  
  // 2. 将行数据打包成二进制格式（关键步骤！）
  size_t const len = pack_row(table, table->write_set, row_data, record,
                              enum_row_image_type::WRITE_AI);
  
  // 3. 准备或获取 Write_rows_log_event
  Rows_log_event *const ev =
      binlog_prepare_pending_rows_event<Write_rows_log_event>(
          table, server_id, len, is_trans, extra_row_info);
  
  if (unlikely(ev == nullptr)) return HA_ERR_OUT_OF_MEM;
  
  // 4. 将打包后的行数据添加到事件中
  return ev->add_row_data(row_data, len);
}
```

**关键点**：
- `Row_data_memory` 负责分配足够的内存来存储打包后的行数据
- 对于包含 BLOB 的表，会使用 `my_malloc` 动态分配内存
- `pack_row()` 是核心打包函数

### 4.2 Row_data_memory 类

**文件**: `sql/binlog.cc:11011`

```cpp
class Row_data_memory {
  void allocate_memory(TABLE *const table, const size_t total_length) {
    if (table->s->blob_fields == 0) {
      // 无 BLOB 字段：使用表的预分配内存
      size_t const maxlen = table->s->reclength + 2 * table->s->fields;
      if (table->write_row_record == nullptr)
        table->write_row_record = (uchar *)table->mem_root.Alloc(2 * maxlen);
      m_memory = table->write_row_record;
      m_release_memory_on_destruction = false;
    } else {
      // 有 BLOB 字段：动态分配内存（关键！）
      m_memory = (uchar *)my_malloc(key_memory_Row_data_memory_memory,
                                    total_length, MYF(MY_WME));
      m_release_memory_on_destruction = true;
    }
  }
};
```

**重要说明**：
- **无 BLOB 字段**：使用表的内存池，避免频繁分配
- **有 BLOB 字段**：必须动态分配，因为大小不固定，可能非常大

---

## 5. BLOB 字段的打包机制

### 5.1 pack_row() 函数

**文件**: `sql/rpl_record.cc:270`

```cpp
size_t pack_row(TABLE *table, MY_BITMAP const *columns_in_image,
                uchar *row_data, const uchar *record,
                enum_row_image_type row_image_type, ulonglong value_options) {
  
  // 计算要写入的列数
  uint image_column_count = bitmap_bits_set(&table->pack_row_tmp_set);
  
  uchar *pack_ptr = row_data;
  
  // 1. 写入 partial_bits（用于 JSON 增量更新）
  Bit_writer partial_bits;
  // ... 处理 JSON 字段
  
  // 2. 写入 null_bits（每个列一个 bit）
  Bit_writer null_bits(pack_ptr);
  pack_ptr += (image_column_count + 7) / 8;  // 跳过 null bitmap
  
  // 3. 遍历所有字段，逐个打包
  for (auto field : fields) {
    if (bitmap_is_set(&table->pack_row_tmp_set, field->field_index())) {
      if (field->is_null(rec_offset)) {
        null_bits.set(true);
      } else {
        null_bits.set(false);
        // 调用字段的 pack 方法（关键！）
        pack_field(&pack_ptr, field, rec_offset, row_image_type, 
                   value_options, &is_partial_json);
      }
    }
  }
  
  return static_cast<size_t>(pack_ptr - row_data);
}
```

**Binlog 行格式**：
```
+-----------+----------+----------+-----+----------+
| null_bits | column_1 | column_2 | ... | column_N |
+-----------+----------+----------+-----+----------+
```

### 5.2 pack_field() 函数

**文件**: `sql/rpl_record.cc:101`

```cpp
static void pack_field(uchar **pack_ptr, Field *field, size_t rec_offset,
                       enum_row_image_type row_image_type,
                       ulonglong value_options, bool *is_partial_format) {
  
  // 对于大部分字段类型，调用 Field::pack()
  // 对于 BLOB，会调用 Field_blob::pack()
  *pack_ptr = field->pack(*pack_ptr, field->field_ptr() + rec_offset, 
                          UINT_MAX);
}
```

### 5.3 Field_blob::pack() - **BLOB 字段打包的核心**

**文件**: `sql/field.cc:7355`

```cpp
uchar *Field_blob::pack(uchar *to, const uchar *from, size_t max_length) const {
  // 1. 获取 BLOB 的实际长度
  uint32 length = get_length(from);
  
  // 2. 先存储长度（占用 1-4 字节，取决于 BLOB 类型）
  uchar len_buf[4];
  store_blob_length(len_buf, packlength, length);
  
  if (packlength >= max_length) {
    memcpy(to, len_buf, max_length);
    return to + max_length;
  }
  
  memcpy(to, len_buf, packlength);
  
  // 3. 复制 BLOB 的完整数据内容（关键！）
  size_t store_length = min<size_t>(length, max_length - packlength);
  if (store_length > 0) {
    memcpy(to + packlength, get_blob_data(from + packlength), store_length);
  }
  
  return to + packlength + store_length;
}
```

**BLOB 打包格式**：
```
+------------------+----------------------+
| 长度 (1-4 bytes) | BLOB 完整数据内容     |
+------------------+----------------------+
```

**重要说明**：
- **TINYBLOB**: `packlength = 1`，最大 255 字节
- **BLOB**: `packlength = 2`，最大 64KB
- **MEDIUMBLOB**: `packlength = 3`，最大 16MB
- **LONGBLOB**: `packlength = 4`，最大 4GB

### 5.4 get_blob_data() - 获取 BLOB 数据指针

**文件**: `sql/field.h:3783`

```cpp
static uchar *get_blob_data(const uchar *position) {
  uchar *data;
  // 从 record buffer 中读取指针
  memcpy(&data, position, sizeof(data));
  return data;
}
```

**原理**：
- 在 `table->record[0]` 中，BLOB 字段的格式是：
  ```
  [ 长度(1-4 bytes) ][ 指针(8 bytes) ]
  ```
- 指针指向实际存储 BLOB 数据的内存地址
- `pack()` 方法会解引用指针，将完整的 BLOB 数据复制到打包缓冲区

### 5.5 BLOB 字段在不同阶段的存储形式

```
阶段 1: TABLE->record[0]（存储引擎记录格式）
+--------+----------+
| 长度   | 指针     |  ← BLOB 字段
+--------+----------+
  1-4B      8B          指针指向堆上的实际数据

阶段 2: Row_data_memory（打包后的临时缓冲区）
+-----------+--------+------------------+
| null_bits | 长度   | BLOB 完整数据     |
+-----------+--------+------------------+
                1-4B    实际数据大小

阶段 3: Rows_log_event::m_rows_buf（事件缓冲区）
多行数据连续存储，每行格式同阶段 2

阶段 4: Binlog Cache（事务缓冲）
包含完整的事件头 + 事件数据

阶段 5: Binlog File（磁盘文件）
持久化的最终形式
```

---

## 6. Rows Event 的数据管理

### 6.1 Rows_log_event 类

**文件**: `sql/log_event.h` 和 `sql/log_event.cc:7766`

```cpp
class Rows_log_event : public Log_event {
protected:
  // 存储所有行数据的缓冲区（关键成员）
  uchar *m_rows_buf;    // 缓冲区起始地址
  uchar *m_rows_cur;    // 当前写入位置
  uchar *m_rows_end;    // 缓冲区结束位置
  ulong m_row_count;    // 行数
};
```

**继承关系**：
```
Rows_log_event (MySQL Server 特有实现)
  ├─ Write_rows_log_event
  ├─ Update_rows_log_event
  └─ Delete_rows_log_event

binary_log::Rows_event (通用 binlog 库)
  └─ std::vector<uint8_t> row;  // 实际存储行数据的容器
```

### 6.2 do_add_row_data() - 添加行数据

**文件**: `sql/log_event.cc:8149`

```cpp
int Rows_log_event::do_add_row_data(uchar *row_data, size_t length) {
  DBUG_TRACE;
  
  if (length == 0) {
    m_row_count++;
    return 0;
  }
  
  // 1. 检查缓冲区空间是否足够
  if (static_cast<size_t>(m_rows_end - m_rows_cur) <= length) {
    ulong cur_size = m_rows_cur - m_rows_buf;
    
    // 2. 检查是否会超过最大限制（UINT_MAX32）
    ulong remaining_space = UINT_MAX32 - cur_size;
    if (length > remaining_space || 
        ((length + block_size) > remaining_space)) {
      LogErr(ERROR_LEVEL, ER_ROW_DATA_TOO_BIG_TO_WRITE_IN_BINLOG);
      return ER_BINLOG_ROW_LOGGING_FAILED;
    }
    
    // 3. 按 block_size (1024 字节) 扩展缓冲区
    const size_t new_alloc =
        block_size * ((cur_size + length + block_size - 1) / block_size);
    
    if (new_alloc) 
      row.resize(new_alloc);  // row 是 std::vector<uint8_t>
    
    // 4. 更新指针（如果内存地址改变）
    if (new_alloc && &row[0] != m_rows_buf) {
      m_rows_buf = &row[0];
      m_rows_cur = m_rows_buf + cur_size;
    }
    
    m_rows_end = m_rows_buf + new_alloc;
  }
  
  // 5. 复制行数据到缓冲区（包括 BLOB 完整数据！）
  assert(m_rows_cur + length <= m_rows_end);
  memcpy(m_rows_cur, row_data, length);
  m_rows_cur += length;
  m_row_count++;
  
  return 0;
}
```

**关键点**：
- **自动扩展**：以 1024 字节为单位扩展缓冲区
- **大小限制**：单个事件最大不能超过 4GB（`UINT_MAX32`）
- **完整复制**：BLOB 数据在这里被完整复制到事件缓冲区

### 6.3 binlog_row_image 参数的影响

**文件**: `sql/table.cc:5748`

```cpp
void TABLE::mark_columns_per_binlog_row_image(THD *thd) {
  switch (thd->variables.binlog_row_image) {
    case BINLOG_ROW_IMAGE_FULL:
      // 记录所有列（包括 BLOB）
      bitmap_set_all(read_set);
      bitmap_set_all(write_set);
      break;
      
    case BINLOG_ROW_IMAGE_NOBLOB:
      // 跳过 BLOB 字段（如果不是主键）
      for (Field **ptr = field; *ptr; ptr++) {
        Field *my_field = *ptr;
        if ((s->primary_key < MAX_KEY) &&
            (my_field->is_flag_set(PRI_KEY_FLAG) ||
             (my_field->type() != MYSQL_TYPE_BLOB)))
          bitmap_set_bit(read_set, my_field->field_index());
        
        if (my_field->type() != MYSQL_TYPE_BLOB)
          bitmap_set_bit(write_set, my_field->field_index());
      }
      break;
      
    case BINLOG_ROW_IMAGE_MINIMAL:
      // 只记录主键列
      if (s->primary_key < MAX_KEY)
        mark_columns_used_by_index_no_reset(s->primary_key, read_set);
      break;
  }
}
```

**优化建议**：
- 如果 BLOB 字段不是主键，可以设置 `binlog_row_image=noblob` 来减少 binlog 大小
- 但这会影响基于行的复制（如无主键表）

---

## 7. Binlog Cache 机制

### 7.1 binlog_cache_mngr 类

**文件**: `sql/binlog.cc:1192`

```cpp
class binlog_cache_mngr {
  binlog_cache_data stmt_cache;  // 非事务性语句缓存
  binlog_cache_data trx_cache;   // 事务性语句缓存
  
  bool init() {
    return stmt_cache.open(binlog_stmt_cache_size,
                           max_binlog_stmt_cache_size) ||
           trx_cache.open(binlog_cache_size, max_binlog_cache_size);
  }
};
```

**缓存类型**：
- **stmt_cache**: 用于非事务性表（如 MyISAM）
- **trx_cache**: 用于事务性表（如 InnoDB）

### 7.2 Binlog_cache_storage 类

**文件**: `sql/binlog_ostream.h:174` 和 `sql/binlog_ostream.cc:45`

```cpp
class Binlog_cache_storage : public Basic_ostream {
  IO_CACHE_binlog_cache_storage m_file;  // 底层 IO_CACHE
  
  bool write(const unsigned char *buffer, my_off_t length) override {
    return m_pipeline_head->write(buffer, length);
  }
};
```

### 7.3 IO_CACHE_binlog_cache_storage

```cpp
bool IO_CACHE_binlog_cache_storage::write(
    const unsigned char *buffer, my_off_t length) {
  // 1. 先写入内存缓冲区
  // 2. 如果超过 cache_size，自动写入临时文件
  return my_b_safe_write(&m_io_cache, buffer, length);
}
```

**缓存行为**：
```
数据大小 <= binlog_cache_size
  ↓
存储在内存缓冲区

数据大小 > binlog_cache_size
  ↓
溢出到磁盘临时文件
（路径：tmpdir/ML_xxxxx）
```

**参数**：
- `binlog_cache_size`: 默认 32KB，事务缓存初始大小
- `max_binlog_cache_size`: 默认 18EB，最大缓存大小

---

## 8. 事务提交与刷盘

### 8.1 ordered_commit() - 组提交机制

**文件**: `sql/binlog.cc:8073` 和 `sql/binlog.h:600`

```cpp
int MYSQL_BIN_LOG::ordered_commit(THD *thd, bool all, bool skip_commit) {
  // MySQL 使用三阶段组提交来优化性能
}
```

**组提交的三个阶段**：

#### Stage 1: FLUSH 阶段

```cpp
// 1. 将所有线程的 binlog cache 刷入 binlog 文件
int process_flush_stage_queue(my_off_t *total_bytes_var, THD **out_queue_var);

// 2. 写入文件（但不 sync）
int flush_cache_to_file(&flush_end_pos);
```

**关键代码**：`sql/binlog.cc:8890`

```cpp
if (change_stage(thd, Commit_stage_manager::BINLOG_FLUSH_STAGE, 
                 thd, nullptr, &LOCK_log)) {
  return finish_commit(thd);
}

flush_error = process_flush_stage_queue(&total_bytes, &wait_queue);

if (flush_error == 0 && total_bytes > 0)
  flush_error = flush_cache_to_file(&flush_end_pos);
```

#### Stage 2: SYNC 阶段

```cpp
// 根据 sync_binlog 参数决定是否调用 fsync
if (change_stage(thd, Commit_stage_manager::SYNC_STAGE, 
                 wait_queue, &LOCK_log, &LOCK_sync)) {
  return finish_commit(thd);
}

if (opt_sync_binlog_period == 1)
  m_binlog_file->sync();  // 立即刷盘
```

**sync_binlog 参数**：
- `sync_binlog=0`: 不主动 fsync，由操作系统决定
- `sync_binlog=1`: 每个事务都 fsync（最安全，性能较低）
- `sync_binlog=N`: 每 N 个事务组提交后 fsync

#### Stage 3: COMMIT 阶段

```cpp
// 提交存储引擎事务
if (change_stage(thd, Commit_stage_manager::COMMIT_STAGE, 
                 final_queue, leave_mutex_before_commit_stage, 
                 &LOCK_commit)) {
  return finish_commit(thd);
}

// 调用存储引擎的 commit
ha_commit_low(thd, all);
```

### 8.2 write_cache() - 将 cache 写入 binlog 文件

**文件**: `sql/binlog.cc:7616`

```cpp
bool MYSQL_BIN_LOG::write_cache(THD *thd, binlog_cache_data *cache_data,
                                Binlog_event_writer *writer) {
  Binlog_cache_storage *const cache = cache_data->get_cache();
  
  // 从 cache 读取数据并写入 binlog 文件
  const unsigned char *buffer;
  my_off_t length;
  
  while (cache->begin(&buffer, &length) == false) {
    if (length > 0) {
      if (wrapper_my_b_safe_write(writer, buffer, length))
        return true;
    }
    if (cache->next(&buffer, &length))
      return true;
  }
  
  return false;
}
```

### 8.3 完整的提交流程时序图

```
事务开始
  ↓
执行 INSERT（多次）
  ├─ 写入存储引擎
  └─ 数据写入 Binlog Cache (内存)
  ↓
COMMIT 命令
  ↓
+---------------------+
| Stage 0: 排序       |  （如果是从库应用线程）
+---------------------+
  ↓
+---------------------+
| Stage 1: FLUSH      |
+---------------------+
  ├─ 生成 GTID
  ├─ 将 Cache 写入 Binlog 文件（磁盘 page cache）
  └─ 增加 prepared XID 计数
  ↓
+---------------------+
| Stage 2: SYNC       |
+---------------------+
  ├─ 调用 fsync (根据 sync_binlog)
  └─ 通知 dump 线程可以读取
  ↓
+---------------------+
| Stage 3: COMMIT     |
+---------------------+
  ├─ 调用 after_sync hook
  ├─ 提交存储引擎事务 (ha_commit_low)
  ├─ 调用 after_commit hook
  ├─ 更新 GTID
  └─ 减少 prepared XID 计数
  ↓
返回客户端成功
```

---

## 9. 内存管理与优化

### 9.1 BLOB 字段导致的内存分配

**问题**：包含 BLOB 的 INSERT 会导致多次内存复制和分配

**内存占用分析**（以 1MB BLOB 为例）：

1. **TABLE->record[0]**: 
   - BLOB 字段：12 字节（4字节长度 + 8字节指针）
   - 实际数据：1MB（在堆上）
   
2. **Row_data_memory**:
   - 临时缓冲区：1MB + 其他字段大小
   - 使用 `my_malloc` 分配（有 BLOB 时）
   
3. **Rows_log_event::m_rows_buf**:
   - 事件缓冲区：1MB + 其他字段大小
   - 使用 `std::vector<uint8_t>`，自动扩展
   - **这是主要的内存占用**
   
4. **Binlog Cache**:
   - 如果 <= 32KB：内存缓冲区
   - 如果 > 32KB：溢出到临时文件
   
5. **Binlog File**:
   - 持久化到磁盘

**总内存占用** ≈ 3MB（原始数据 + 临时缓冲 + 事件缓冲）

### 9.2 优化建议

#### 对于应用层

1. **避免在 BLOB 字段上建主键**
   - 设置 `binlog_row_image=noblob` 可跳过非主键 BLOB

2. **批量插入时注意事务大小**
   ```sql
   -- 不好：单个事务插入大量 BLOB
   BEGIN;
   INSERT INTO t VALUES (huge_blob_1);
   INSERT INTO t VALUES (huge_blob_2);
   ...
   INSERT INTO t VALUES (huge_blob_1000);
   COMMIT;  -- Binlog cache 可能超过 GB 级别
   
   -- 较好：分批提交
   BEGIN;
   INSERT INTO t VALUES (huge_blob_1);
   ...
   INSERT INTO t VALUES (huge_blob_100);
   COMMIT;
   
   BEGIN;
   INSERT INTO t VALUES (huge_blob_101);
   ...
   COMMIT;
   ```

3. **考虑将大对象存储在文件系统**
   - 数据库只存储路径
   - 避免 binlog 膨胀

#### 对于数据库配置

1. **调整 binlog cache 大小**
   ```sql
   SET GLOBAL binlog_cache_size = 1048576;  -- 1MB
   SET GLOBAL max_binlog_cache_size = 4294967296;  -- 4GB
   ```

2. **调整 binlog_row_image**
   ```sql
   SET GLOBAL binlog_row_image = 'NOBLOB';  -- 跳过非主键 BLOB
   ```

3. **监控 binlog cache 使用情况**
   ```sql
   SHOW GLOBAL STATUS LIKE 'Binlog_cache%';
   
   Binlog_cache_disk_use  -- 使用临时文件的次数
   Binlog_cache_use       -- 使用 cache 的次数
   ```

---

## 10. 相关配置参数

### 10.1 Binlog 基础参数

| 参数名 | 默认值 | 说明 |
|--------|--------|------|
| `log_bin` | OFF | 是否启用 binlog |
| `binlog_format` | ROW | binlog 格式：STATEMENT/ROW/MIXED |
| `binlog_row_image` | FULL | 行镜像：FULL/MINIMAL/NOBLOB |
| `max_binlog_size` | 1GB | 单个 binlog 文件最大大小 |
| `sync_binlog` | 1 | 多少次提交后 fsync |

### 10.2 Binlog Cache 参数

| 参数名 | 默认值 | 说明 |
|--------|--------|------|
| `binlog_cache_size` | 32KB | 事务缓存大小（InnoDB 等） |
| `max_binlog_cache_size` | 18EB | 事务缓存最大值 |
| `binlog_stmt_cache_size` | 32KB | 非事务缓存大小（MyISAM 等） |
| `max_binlog_stmt_cache_size` | 18EB | 非事务缓存最大值 |

### 10.3 性能相关参数

| 参数名 | 默认值 | 说明 |
|--------|--------|------|
| `binlog_group_commit_sync_delay` | 0 | 组提交延迟（微秒） |
| `binlog_group_commit_sync_no_delay_count` | 0 | 多少事务后立即提交 |
| `binlog_order_commits` | ON | 是否保证提交顺序 |

### 10.4 与 BLOB 相关的参数

| 参数名 | 默认值 | 说明 |
|--------|--------|------|
| `max_allowed_packet` | 64MB | 单个包（或 binlog event）最大大小 |
| `binlog_row_value_options` | '' | 行值选项（PARTIAL_JSON） |

---

## 11. 关键数据结构总结

### 11.1 BLOB 字段的内存表示

```cpp
// 在 TABLE->record[0] 中
struct blob_field_in_record {
  uint32 length;     // 1-4 bytes (根据 BLOB 类型)
  uchar *data_ptr;   // 8 bytes (指向实际数据)
};

// 在 Binlog 中
struct blob_field_in_binlog {
  uint32 length;     // 1-4 bytes
  uchar data[];      // length bytes (完整数据)
};
```

### 11.2 核心类的关系

```
THD (线程/会话)
  └─ binlog_cache_mngr (Binlog 缓存管理器)
      ├─ stmt_cache (非事务缓存)
      └─ trx_cache (事务缓存)
          └─ Binlog_cache_storage (存储层)
              └─ IO_CACHE_binlog_cache_storage (IO 缓存)
                  └─ IO_CACHE (底层缓存)
                      ├─ 内存缓冲区
                      └─ 临时文件（如溢出）

Rows_log_event (行事件基类)
  ├─ Write_rows_log_event (INSERT)
  ├─ Update_rows_log_event (UPDATE)
  └─ Delete_rows_log_event (DELETE)
      └─ m_rows_buf (std::vector<uint8_t>)
          └─ [row1_data][row2_data]...[rowN_data]
```

---

## 12. 调试和排查

### 12.1 查看 Binlog 内容

```bash
# 查看 binlog 文件列表
mysql> SHOW BINARY LOGS;

# 查看 binlog 事件
mysqlbinlog --base64-output=decode-rows -v mysql-bin.000001

# 只看 BLOB 字段的处理
mysqlbinlog --base64-output=decode-rows -vv mysql-bin.000001
```

### 12.2 监控 Binlog 大小

```sql
-- 查看 binlog 大小
SELECT 
  file_name, 
  file_size/1024/1024 AS size_mb 
FROM performance_schema.file_summary_by_instance 
WHERE file_name LIKE '%binlog%';

-- 查看 cache 使用情况
SHOW GLOBAL STATUS LIKE 'Binlog_cache%';
```

### 12.3 问题排查

**问题 1**: Binlog cache 溢出错误

```
ERROR 1197 (HY000): Multi-statement transaction required more than 
'max_binlog_cache_size' bytes of storage
```

**解决**：
```sql
SET GLOBAL max_binlog_cache_size = 4294967296;  -- 增加到 4GB
```

**问题 2**: INSERT 性能差

**排查步骤**：
1. 检查是否有大 BLOB 字段
2. 查看 `Binlog_cache_disk_use` 是否频繁增加
3. 考虑调整 `binlog_cache_size` 或 `binlog_row_image`

---

## 13. 源代码文件索引

### 核心文件列表

| 文件路径 | 功能说明 |
|---------|---------|
| `sql/sql_parse.cc` | SQL 命令解析入口 |
| `sql/sql_insert.cc` | INSERT 语句执行 |
| `sql/handler.cc` | 存储引擎接口层 |
| `sql/handler.h` | Handler 类定义 |
| `sql/binlog.cc` | Binlog 核心实现 |
| `sql/binlog.h` | Binlog 类定义 |
| `sql/log_event.cc` | Binlog 事件实现 |
| `sql/log_event.h` | Binlog 事件定义 |
| `sql/rpl_record.cc` | 行记录打包/解包 |
| `sql/rpl_record.h` | 行记录相关定义 |
| `sql/field.cc` | 字段类型实现（包含 Field_blob） |
| `sql/field.h` | 字段类型定义 |
| `sql/binlog_ostream.cc` | Binlog 输出流 |
| `sql/binlog_ostream.h` | Binlog 缓存存储 |
| `sql/table.cc` | 表结构管理 |

### 关键函数索引

| 函数名 | 文件:行号 | 功能 |
|--------|----------|------|
| `mysql_execute_command()` | sql/sql_parse.cc | SQL 命令执行入口 |
| `mysql_insert()` | sql/sql_insert.cc | INSERT 语句主函数 |
| `write_record()` | sql/sql_insert.cc:1749 | 写入单条记录 |
| `handler::ha_write_row()` | sql/handler.cc:8004 | Handler 层写入接口 |
| `binlog_log_row()` | sql/handler.cc:7863 | 记录 binlog 入口 |
| `THD::binlog_write_row()` | sql/binlog.cc:11097 | Binlog 写入行数据 |
| `pack_row()` | sql/rpl_record.cc:270 | 打包行数据 |
| `pack_field()` | sql/rpl_record.cc:101 | 打包单个字段 |
| `Field_blob::pack()` | sql/field.cc:7355 | BLOB 字段打包 |
| `Rows_log_event::do_add_row_data()` | sql/log_event.cc:8149 | 添加行到事件 |
| `MYSQL_BIN_LOG::ordered_commit()` | sql/binlog.cc:8073 | 组提交主函数 |
| `MYSQL_BIN_LOG::write_cache()` | sql/binlog.cc:7616 | 写入 cache 到文件 |

---

## 14. 总结

### 14.1 BLOB 字段的特殊处理

1. **内存分配**：有 BLOB 字段时，使用 `my_malloc` 动态分配，而不是表内存池
2. **完整复制**：BLOB 数据会被完整复制到 binlog，不是引用
3. **多次复制**：
   - 第 1 次：从存储引擎读取到 `table->record[0]`（指针）
   - 第 2 次：`pack()` 时复制到临时缓冲区
   - 第 3 次：`do_add_row_data()` 时复制到事件缓冲区
   - 第 4 次：写入 binlog cache
   - 第 5 次：从 cache 写入 binlog 文件

### 14.2 性能影响

- **CPU**: BLOB 数据的多次 `memcpy`
- **内存**: 峰值内存 ≈ BLOB 大小 × 3
- **磁盘 I/O**: Binlog 文件增长快速
- **网络**: 主从复制传输大量数据

### 14.3 最佳实践

1. **设计层面**：
   - 避免在数据库存储超大 BLOB（如视频、大图片）
   - 考虑使用对象存储 + 数据库存路径
   
2. **配置层面**：
   - 根据业务调整 `binlog_cache_size`
   - 考虑使用 `binlog_row_image=noblob`
   - 合理设置 `sync_binlog`
   
3. **应用层面**：
   - 批量操作时控制事务大小
   - 监控 binlog 大小和缓存使用情况

---

## 参考资料

1. MySQL 8.0 官方文档：https://dev.mysql.com/doc/refman/8.0/en/
2. MySQL 源码：https://github.com/mysql/mysql-server
3. Binary Log 格式：https://dev.mysql.com/doc/refman/8.0/en/binary-log.html
4. Replication：https://dev.mysql.com/doc/refman/8.0/en/replication.html

---

**文档版本**: 1.0  
**创建日期**: 2025-01-13  
**适用版本**: MySQL 8.0  
**作者**: AI Assistant

