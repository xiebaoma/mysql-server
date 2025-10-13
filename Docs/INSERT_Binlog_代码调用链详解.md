# MySQL INSERT 语句 Binlog 代码调用链详解

## 文档说明

本文档是《INSERT_Binlog写入流程分析_BLOB字段.md》的补充文档，提供详细的代码调用链、实际示例和调试方法。

---

## 1. 完整的函数调用链（带代码位置）

### 1.1 从 SQL 解析到存储引擎

```
客户端: INSERT INTO t1 (id, name, data) VALUES (1, 'test', <1MB BLOB>);
    ↓
mysql_execute_command()
  文件: sql/sql_parse.cc
  位置: 处理 SQLCOM_INSERT 命令
    ↓
Sql_cmd_insert::execute()
  文件: sql/sql_insert.h, sql/sql_insert.cc
    ↓
Sql_cmd_insert::execute_inner()
  文件: sql/sql_insert.cc
    ↓
mysql_insert()
  文件: sql/sql_insert.cc
  功能: INSERT 的核心处理函数
    ↓
write_record()
  文件: sql/sql_insert.cc:1749
  功能: 写入单行记录
    ↓
table->file->ha_write_row(table->record[0])
  文件: sql/handler.cc:8004
  参数: buf = table->record[0]（包含要插入的数据）
```

### 1.2 存储引擎写入

```
handler::ha_write_row(uchar *buf)
  文件: sql/handler.cc:8004
    ↓
  [步骤 1] mark_trx_read_write()
    标记事务为读写事务
    ↓
  [步骤 2] write_row(buf)  ← 虚函数，由具体引擎实现
    对于 InnoDB: ha_innobase::write_row()
    文件: storage/innobase/handler/ha_innodb.cc
    功能: 将数据写入 InnoDB 缓冲池
    ↓
  [步骤 3] binlog_log_row(table, nullptr, buf, log_func)
    文件: sql/handler.cc:8029
    功能: 开始记录 binlog
```

### 1.3 Binlog 记录流程

```
binlog_log_row(table, before_record, after_record, log_func)
  文件: sql/handler.cc:7863
  参数: 
    - table: 表对象
    - before_record: nullptr (INSERT 没有旧值)
    - after_record: buf (新插入的数据)
    - log_func: Write_rows_log_event::binlog_row_logging_function
    ↓
(*log_func)(thd, table, is_trans, before_record, after_record)
  实际调用: Write_rows_log_event::binlog_row_logging_function()
  文件: sql/log_event.cc:11971
    ↓
thd->binlog_write_row(table, is_trans, after_record, extra_row_info)
  文件: sql/binlog.cc:11097
```

### 1.4 行数据打包

```
THD::binlog_write_row(TABLE *table, bool is_trans, 
                      uchar const *record,
                      const unsigned char *extra_row_info)
  文件: sql/binlog.cc:11097
    ↓
  [步骤 1] 分配内存
    Row_data_memory memory(table, record);
    文件: sql/binlog.cc:11011
    └─ 对于有 BLOB 的表:
       allocate_memory() 调用 my_malloc()
    ↓
  [步骤 2] 打包行数据（关键！）
    size_t len = pack_row(table, table->write_set, row_data, record,
                          enum_row_image_type::WRITE_AI);
    文件: sql/rpl_record.cc:270
```

### 1.5 pack_row() 详细流程

```
pack_row(TABLE *table, MY_BITMAP *columns_in_image,
         uchar *row_data, const uchar *record, 
         enum_row_image_type row_image_type, ulonglong value_options)
  文件: sql/rpl_record.cc:270
    ↓
  [步骤 1] 初始化指针
    uchar *pack_ptr = row_data;
    ↓
  [步骤 2] 写入 NULL bitmap
    Bit_writer null_bits(pack_ptr);
    pack_ptr += (image_column_count + 7) / 8;
    ↓
  [步骤 3] 遍历所有字段
    for (auto field : fields) {
      if (field->is_null()) {
        null_bits.set(true);
      } else {
        null_bits.set(false);
        pack_field(&pack_ptr, field, ...);  ← 关键调用
      }
    }
    ↓
  [步骤 4] 返回打包后的大小
    return pack_ptr - row_data;
```

### 1.6 BLOB 字段打包

```
pack_field(uchar **pack_ptr, Field *field, ...)
  文件: sql/rpl_record.cc:101
    ↓
  对于 BLOB 字段:
    field->pack(*pack_ptr, field->field_ptr() + rec_offset, UINT_MAX)
    ↓
Field_blob::pack(uchar *to, const uchar *from, size_t max_length)
  文件: sql/field.cc:7355
    ↓
  [步骤 1] 获取 BLOB 长度
    uint32 length = get_length(from);
    └─ 从 record buffer 读取长度字段（1-4 字节）
    ↓
  [步骤 2] 写入长度
    store_blob_length(len_buf, packlength, length);
    memcpy(to, len_buf, packlength);
    ↓
  [步骤 3] 获取 BLOB 数据指针
    uchar *data = get_blob_data(from + packlength);
    └─ 从 record buffer 读取指针（8 字节）
    ↓
  [步骤 4] 复制 BLOB 完整数据（关键！）
    memcpy(to + packlength, data, length);
    ↓
  [步骤 5] 返回新位置
    return to + packlength + length;
```

### 1.7 添加到 Rows Event

```
回到 THD::binlog_write_row()
  ↓
  [步骤 3] 准备或获取 Write_rows_log_event
    Rows_log_event *ev =
        binlog_prepare_pending_rows_event<Write_rows_log_event>(
            table, server_id, len, is_trans, extra_row_info);
    文件: sql/binlog.cc (模板函数)
    ↓
  [步骤 4] 添加行数据到事件
    ev->add_row_data(row_data, len);
    └─ 调用 Rows_log_event::do_add_row_data()
       文件: sql/log_event.cc:8149
```

### 1.8 do_add_row_data() 详细流程

```
Rows_log_event::do_add_row_data(uchar *row_data, size_t length)
  文件: sql/log_event.cc:8149
    ↓
  [步骤 1] 检查缓冲区空间
    if (m_rows_end - m_rows_cur <= length) {
      // 需要扩展缓冲区
      ulong cur_size = m_rows_cur - m_rows_buf;
      const size_t new_alloc = 
          block_size * ((cur_size + length + block_size - 1) / block_size);
      row.resize(new_alloc);  // std::vector::resize
      
      // 更新指针
      m_rows_buf = &row[0];
      m_rows_cur = m_rows_buf + cur_size;
      m_rows_end = m_rows_buf + new_alloc;
    }
    ↓
  [步骤 2] 复制数据（包括完整的 BLOB！）
    memcpy(m_rows_cur, row_data, length);
    m_rows_cur += length;
    m_row_count++;
```

### 1.9 事务提交流程

```
客户端: COMMIT;
    ↓
mysql_execute_command()
  case SQLCOM_COMMIT:
    ↓
trans_commit()
  文件: sql/transaction.cc
    ↓
ha_commit_trans(thd, true)
  文件: sql/handler.cc
    ↓
tc_log->commit(thd, true)
  └─ 对于 binlog: MYSQL_BIN_LOG::commit()
     文件: sql/binlog.cc:8073
    ↓
ordered_commit(thd, all, skip_commit)
  文件: sql/binlog.cc (详见下面的三阶段)
```

### 1.10 三阶段组提交

```
MYSQL_BIN_LOG::ordered_commit(THD *thd, bool all, bool skip_commit)
  文件: sql/binlog.cc
    ↓
+------------------------------------------+
| Stage 1: FLUSH                           |
+------------------------------------------+
change_stage(thd, BINLOG_FLUSH_STAGE, ...)
  ↓
process_flush_stage_queue(&total_bytes, &wait_queue)
  └─ 遍历队列中的所有线程
     └─ flush(thd, &bytes_written, &wrote_xid)
        └─ binlog_cache_data::flush()
           文件: sql/binlog.cc:2392
           └─ write_cache(thd, cache_data, writer)
              文件: sql/binlog.cc:7616
              └─ 将 cache 内容写入 binlog 文件
  ↓
flush_cache_to_file(&flush_end_pos)
  └─ m_binlog_file->flush()
     └─ 写入文件（但不 sync）
    ↓
+------------------------------------------+
| Stage 2: SYNC                            |
+------------------------------------------+
change_stage(thd, SYNC_STAGE, ...)
  ↓
if (sync_binlog == 1)
  m_binlog_file->sync()
  └─ 调用 fsync() 刷盘
    ↓
+------------------------------------------+
| Stage 3: COMMIT                          |
+------------------------------------------+
change_stage(thd, COMMIT_STAGE, ...)
  ↓
process_commit_stage_queue()
  └─ 遍历队列中的所有线程
     └─ ha_commit_low(thd, all)
        └─ 提交存储引擎事务
           └─ 对于 InnoDB: innobase_commit()
              └─ trx_commit_for_mysql()
    ↓
finish_commit(thd)
  └─ 清理并返回
```

---

## 2. 关键数据结构的内存布局

### 2.1 TABLE::record[0] 中的 BLOB 字段

```cpp
// 假设表定义：
// CREATE TABLE t1 (
//   id INT,
//   name VARCHAR(100),
//   data BLOB
// );

// record[0] 的内存布局（简化）
struct record_format {
  // NULL bitmap（假设 1 字节，支持 8 个字段）
  uchar null_bitmap;  // offset 0
  
  // id INT (4 字节)
  int32 id;           // offset 1
  
  // name VARCHAR(100)
  uchar name_len;     // offset 5 (1 字节长度)
  char name[100];     // offset 6
  
  // data BLOB (Field_blob 格式)
  uint32 blob_length; // offset 106 (4 字节，因为是 BLOB)
  uchar *blob_ptr;    // offset 110 (8 字节指针)
  
  // 总大小：118 字节（不包括实际的 BLOB 数据）
};

// 实际 BLOB 数据在堆上
// blob_ptr 指向的内存：
// [byte0][byte1][byte2]...[byteN]  (N = blob_length)
```

### 2.2 打包后的 Binlog 行格式

```cpp
// pack_row() 之后的格式
struct packed_row_format {
  // NULL bitmap（每个列一个 bit）
  // 对于 3 个列：需要 (3 + 7) / 8 = 1 字节
  uchar null_bitmap;  // offset 0
  
  // id INT (4 字节)
  int32 id;           // offset 1
  
  // name VARCHAR(100)
  uchar name_len;     // offset 5
  char name[name_len];// offset 6 (变长)
  
  // data BLOB (关键区别！)
  uint32 blob_length; // offset 6 + name_len (4 字节)
  uchar blob_data[blob_length];  // offset 10 + name_len (完整数据！)
  
  // 总大小：11 + name_len + blob_length 字节
};

// 例如：id=1, name='test'(4字节), data=1MB
// 总大小：11 + 4 + 1048576 = 1048591 字节 ≈ 1MB
```

### 2.3 Rows_log_event 的内存布局

```cpp
class Rows_log_event {
  // ... 其他成员
  
  // 核心：存储所有行数据的缓冲区
  uchar *m_rows_buf;   // 缓冲区起始地址
  uchar *m_rows_cur;   // 当前写入位置
  uchar *m_rows_end;   // 缓冲区结束位置
  ulong m_row_count;   // 已存储的行数
  
  // 实际数据来自父类
  binary_log::Rows_event {
    std::vector<uint8_t> row;  // 实际存储
  };
};

// 内存布局（假设有 3 行）
// row = [row1_packed_data][row2_packed_data][row3_packed_data]
//       ^                 ^                 ^                ^
//       m_rows_buf        (1MB)             (1MB)            m_rows_cur
//                                                            m_rows_end
```

---

## 3. 实际示例分析

### 3.1 示例：插入一行包含 1MB BLOB 的数据

```sql
-- 建表
CREATE TABLE test_blob (
  id INT PRIMARY KEY,
  name VARCHAR(100),
  data BLOB
);

-- 插入数据
INSERT INTO test_blob VALUES (1, 'test', <1MB binary data>);
```

#### 内存分配和数据流动

```
步骤 1: 解析 SQL
  输入: SQL 字符串
  输出: 解析树

步骤 2: 准备 record[0]
  位置: sql/sql_insert.cc
  内存: table->record[0] = 118 字节
        + 堆上的 1MB BLOB 数据

步骤 3: 写入 InnoDB
  调用: ha_innobase::write_row(table->record[0])
  位置: storage/innobase/handler/ha_innodb.cc
  操作: 将数据写入 InnoDB buffer pool

步骤 4: 分配打包缓冲区
  调用: Row_data_memory memory(table, record);
  内存: my_malloc(11 + 4 + 1048576) ≈ 1MB
  
步骤 5: 打包数据
  调用: pack_row(table, table->write_set, row_data, record, ...)
  操作:
    5.1 写入 null_bitmap: 1 字节
    5.2 写入 id: 4 字节
    5.3 写入 name: 1 + 4 = 5 字节
    5.4 写入 BLOB:
        - 写入长度: 4 字节
        - 复制完整数据: memcpy(dest, blob_ptr, 1048576)
  输出: row_data 填充了 1,048,591 字节

步骤 6: 添加到事件
  调用: ev->do_add_row_data(row_data, 1048591)
  内存: m_rows_buf 扩展到 1,048,576 字节（按 1KB 块对齐）
  操作: memcpy(m_rows_cur, row_data, 1048591)

步骤 7: 写入 binlog cache
  调用: binlog_cache_data::write_event()
  位置: sql/binlog.cc
  操作:
    - 写入事件头（约 20-30 字节）
    - 写入列位图（几个字节）
    - 写入行数据（1,048,591 字节）
  
  由于 1MB > binlog_cache_size (32KB)
  → 溢出到临时文件
  
步骤 8: 事务提交
  调用: MYSQL_BIN_LOG::ordered_commit()
  操作:
    8.1 Stage 1 FLUSH:
        - 将临时文件内容写入 binlog 文件
        - write() 系统调用
    8.2 Stage 2 SYNC:
        - 如果 sync_binlog=1: fsync()
    8.3 Stage 3 COMMIT:
        - InnoDB 提交: trx_commit_for_mysql()

步骤 9: 清理
  - 释放 Row_data_memory (1MB)
  - Rows_log_event 的 m_rows_buf 保持到事件销毁
  - Binlog cache 重置
```

#### 内存占用峰值分析

```
时刻 T1: 存储引擎写入
  - table->record[0]: 118 字节
  - BLOB 数据（堆）: 1,048,576 字节
  总计: ≈ 1 MB

时刻 T2: 打包数据
  - table->record[0]: 118 字节
  - BLOB 数据（堆）: 1,048,576 字节
  - Row_data_memory: 1,048,591 字节  ← 新增
  总计: ≈ 2 MB

时刻 T3: 添加到事件
  - table->record[0]: 118 字节
  - BLOB 数据（堆）: 1,048,576 字节
  - Row_data_memory: 1,048,591 字节
  - m_rows_buf: 1,049,600 字节  ← 新增（对齐到 1KB）
  总计: ≈ 3 MB

时刻 T4: 写入 cache
  - table->record[0]: 118 字节
  - BLOB 数据（堆）: 1,048,576 字节
  - m_rows_buf: 1,049,600 字节
  - Binlog cache 临时文件: ~1,048,700 字节  ← 新增（加上事件头）
  总计: ≈ 3 MB（临时文件在磁盘）

时刻 T5: 提交后
  - Binlog file: ~1,048,700 字节（持久化）
  - 其他内存已释放
```

---

## 4. GDB 调试示例

### 4.1 设置断点

```bash
# 启动 MySQL（debug 版本）
gdb --args mysqld --defaults-file=/path/to/my.cnf

# 设置断点
(gdb) break handler::ha_write_row
(gdb) break binlog_log_row
(gdb) break THD::binlog_write_row
(gdb) break pack_row
(gdb) break Field_blob::pack
(gdb) break Rows_log_event::do_add_row_data
(gdb) break MYSQL_BIN_LOG::ordered_commit

(gdb) run
```

### 4.2 执行 INSERT 并观察

```sql
-- 在另一个终端连接 MySQL
mysql> INSERT INTO test_blob VALUES (1, 'test', repeat('A', 1048576));
```

### 4.3 在断点处检查数据

```gdb
# 断点 1: handler::ha_write_row
(gdb) break handler::ha_write_row
(gdb) continue

# 到达断点
Breakpoint 1, handler::ha_write_row (this=0x..., buf=0x...) at sql/handler.cc:8004

# 查看 buf 指向的数据（table->record[0]）
(gdb) x/20xb buf
0x...: 0x00 0x01 0x00 0x00 0x00 0x04 0x74 0x65
0x...: 0x73 0x74 0x00 0x00 0x10 0x00 0x12 0x34
0x...: 0x56 0x78 0x9a 0xbc

# 解释：
# 0x00: null bitmap
# 0x01 0x00 0x00 0x00: id = 1
# 0x04: name length = 4
# 0x74 0x65 0x73 0x74: 'test'
# 0x00 0x00 0x10 0x00: BLOB length = 1048576 (小端序)
# 0x12 0x34 0x56 0x78 0x9a 0xbc...: BLOB 指针

# 查看 BLOB 指针
(gdb) print ((Field_blob*)table->field[2])->get_blob_data()
$1 = (uchar *) 0x12345678...

# 查看 BLOB 数据
(gdb) x/20xb 0x12345678
0x12345678: 0x41 0x41 0x41 0x41 0x41 0x41 0x41 0x41
0x12345680: 0x41 0x41 0x41 0x41 0x41 0x41 0x41 0x41
# 'A' = 0x41

# 继续执行
(gdb) continue

# 断点 2: Field_blob::pack
Breakpoint 2, Field_blob::pack (this=0x..., to=0x..., from=0x..., max_length=...) 
  at sql/field.cc:7355

# 查看参数
(gdb) print length
$2 = 1048576

(gdb) print packlength
$3 = 4

# 查看 BLOB 数据指针
(gdb) print get_blob_data(from + packlength)
$4 = (uchar *) 0x12345678

# 单步执行到 memcpy
(gdb) next
(gdb) next
...
7377        memcpy(to + packlength, get_blob_data(from + packlength), store_length);

# 查看即将复制的数据
(gdb) print store_length
$5 = 1048576

# 执行 memcpy
(gdb) next

# 查看复制后的数据
(gdb) x/20xb to
0x...: 0x00 0x00 0x10 0x00 0x41 0x41 0x41 0x41
0x...: 0x41 0x41 0x41 0x41 0x41 0x41 0x41 0x41
# 前 4 字节是长度，后面是 'A' 的数据

# 继续执行
(gdb) continue

# 断点 3: Rows_log_event::do_add_row_data
Breakpoint 3, Rows_log_event::do_add_row_data (this=0x..., row_data=0x..., length=1048591)
  at sql/log_event.cc:8149

# 查看参数
(gdb) print length
$6 = 1048591  # 注意：这是打包后的总大小

# 查看当前缓冲区状态
(gdb) print m_rows_cur - m_rows_buf
$7 = 0  # 第一行，缓冲区为空

(gdb) print m_rows_end - m_rows_cur
$8 = 0  # 需要扩展

# 单步执行到 resize
(gdb) next
...
8199            if (new_alloc) row.resize(new_alloc);

# 查看 new_alloc
(gdb) print new_alloc
$9 = 1049600  # 1048591 向上对齐到 1024 的倍数

# 执行 resize
(gdb) next

# 查看新的缓冲区状态
(gdb) print m_rows_end - m_rows_buf
$10 = 1049600

# 单步到 memcpy
(gdb) next
...
8216      memcpy(m_rows_cur, row_data, length);

# 执行 memcpy
(gdb) next

# 查看复制后的数据
(gdb) x/100xb m_rows_buf
0x...: 0x00 0x01 0x00 0x00 0x00 0x04 0x74 0x65
0x...: 0x73 0x74 0x00 0x00 0x10 0x00 0x41 0x41
0x...: 0x41 0x41 0x41 0x41 0x41 0x41 0x41 0x41
...
# 可以看到完整的打包数据

(gdb) continue
```

---

## 5. 性能分析工具

### 5.1 使用 perf 分析

```bash
# 记录性能数据
sudo perf record -g -p $(pidof mysqld) -- sleep 60

# 执行大量 INSERT
mysql> INSERT INTO test_blob SELECT ...;

# 分析结果
sudo perf report

# 关键函数的 CPU 占用：
#   - memcpy: ~40%（BLOB 数据复制）
#   - Field_blob::pack: ~15%
#   - Rows_log_event::do_add_row_data: ~10%
#   - InnoDB 相关: ~20%
#   - 其他: ~15%
```

### 5.2 使用 Performance Schema

```sql
-- 启用 performance_schema
UPDATE performance_schema.setup_instruments 
SET ENABLED = 'YES', TIMED = 'YES' 
WHERE NAME LIKE '%binlog%';

-- 执行 INSERT
INSERT INTO test_blob VALUES (1, 'test', <1MB BLOB>);

-- 查看 binlog 相关的性能数据
SELECT EVENT_NAME, COUNT_STAR, SUM_TIMER_WAIT/1000000000 AS sum_ms
FROM performance_schema.events_statements_summary_by_event_name
WHERE EVENT_NAME LIKE '%binlog%'
ORDER BY SUM_TIMER_WAIT DESC;

-- 查看内存使用
SELECT * FROM performance_schema.memory_summary_global_by_event_name
WHERE EVENT_NAME LIKE '%binlog%'
ORDER BY CURRENT_NUMBER_OF_BYTES_USED DESC;
```

---

## 6. 常见问题和解决方法

### 6.1 问题：Binlog cache 溢出

**现象**：
```
ERROR 1197 (HY000): Multi-statement transaction required more than 
'max_binlog_cache_size' bytes of storage
```

**原因**：
- 单个事务包含太多 BLOB 数据
- 默认 `max_binlog_cache_size` 太小

**排查**：
```sql
-- 查看当前设置
SHOW VARIABLES LIKE 'max_binlog_cache_size';

-- 查看使用情况
SHOW GLOBAL STATUS LIKE 'Binlog_cache%';

Binlog_cache_disk_use      | 100     ← 使用临时文件的次数
Binlog_cache_use           | 1000    ← 使用 cache 的次数
Binlog_stmt_cache_disk_use | 0
Binlog_stmt_cache_use      | 0
```

**解决**：
```sql
-- 临时解决
SET GLOBAL max_binlog_cache_size = 4294967296;  -- 4GB

-- 或者分批提交
BEGIN;
INSERT INTO test_blob VALUES (1, ...);
...
INSERT INTO test_blob VALUES (100, ...);  -- 不要太多
COMMIT;

BEGIN;
INSERT INTO test_blob VALUES (101, ...);
...
COMMIT;
```

### 6.2 问题：INSERT 性能差

**排查步骤**：

```sql
-- 1. 查看慢查询日志
SET GLOBAL slow_query_log = 1;
SET GLOBAL long_query_time = 1;

-- 2. 查看 binlog 写入统计
SHOW GLOBAL STATUS LIKE 'Binlog%';

-- 3. 查看 InnoDB 写入统计
SHOW ENGINE INNODB STATUS\G

-- 4. 查看磁盘 I/O
# Linux
iostat -x 1 10
```

**可能原因**：

1. **sync_binlog=1 导致频繁 fsync**
   ```sql
   -- 查看当前设置
   SHOW VARIABLES LIKE 'sync_binlog';
   
   -- 临时调整（注意安全性）
   SET GLOBAL sync_binlog = 1000;  -- 每 1000 个事务 fsync 一次
   ```

2. **大 BLOB 导致频繁内存分配**
   ```sql
   -- 使用 noblob 模式（如果 BLOB 不是主键）
   SET GLOBAL binlog_row_image = 'noblob';
   ```

3. **binlog_cache_size 太小导致频繁写临时文件**
   ```sql
   -- 增加 cache 大小
   SET GLOBAL binlog_cache_size = 1048576;  -- 1MB
   ```

### 6.3 问题：主从复制延迟

**现象**：
```sql
SHOW SLAVE STATUS\G
Seconds_Behind_Master: 3600  -- 1 小时延迟
```

**排查 BLOB 相关的原因**：

```sql
-- 1. 查看 binlog 大小
SHOW BINARY LOGS;
+------------------+-----------+
| Log_name         | File_size |
+------------------+-----------+
| mysql-bin.000001 | 1073741824|  -- 1GB！
| mysql-bin.000002 | 1073741824|
+------------------+-----------+

-- 2. 分析 binlog 内容
# 在主库执行
mysqlbinlog --base64-output=decode-rows -vv mysql-bin.000001 | less

# 查找大事务
mysqlbinlog --base64-output=decode-rows -vv mysql-bin.000001 \
  | grep -A 10 "# at"

-- 3. 查看从库的 relay log 处理速度
SHOW SLAVE STATUS\G
Relay_Log_Space: 10737418240  -- 10GB relay log 积压
```

**解决方法**：

1. **优化应用层**：
   - 减少单个事务的大小
   - 避免在事务中插入大量 BLOB

2. **优化配置**：
   ```sql
   -- 主库：使用 noblob（如适用）
   SET GLOBAL binlog_row_image = 'noblob';
   
   -- 从库：使用并行复制
   SET GLOBAL slave_parallel_type = 'LOGICAL_CLOCK';
   SET GLOBAL slave_parallel_workers = 4;
   ```

3. **使用 binlog 压缩**（MySQL 8.0.20+）：
   ```sql
   SET GLOBAL binlog_transaction_compression = ON;
   SET GLOBAL binlog_transaction_compression_level_zstd = 3;
   ```

---

## 7. 优化建议总结

### 7.1 应用层优化

| 优化项 | 建议 | 影响 |
|--------|------|------|
| BLOB 大小 | 避免存储超大对象（>10MB） | 减少内存和 I/O |
| 事务大小 | 控制在 100MB 以内 | 避免 cache 溢出 |
| 批量插入 | 分批提交，不要一次插入太多 | 减少内存占用 |
| BLOB 位置 | 将大对象存储在文件系统 | 根本解决问题 |

### 7.2 配置优化

| 参数 | 推荐值 | 说明 |
|------|--------|------|
| `binlog_cache_size` | 1MB - 16MB | 根据平均事务大小调整 |
| `max_binlog_cache_size` | 4GB | 防止超大事务失败 |
| `binlog_row_image` | NOBLOB | 如果 BLOB 不是主键 |
| `sync_binlog` | 1 或 1000 | 平衡安全性和性能 |
| `binlog_transaction_compression` | ON | MySQL 8.0.20+ |

### 7.3 硬件优化

1. **使用 SSD**：减少 binlog 和临时文件的 I/O 延迟
2. **增加内存**：减少 binlog cache 溢出到磁盘
3. **网络优化**：主从复制使用高速网络

---

## 8. 参考代码片段

### 8.1 自定义 BLOB 大小检查（示例）

如果你想在代码中添加 BLOB 大小检查，可以在以下位置：

**位置 1**: `Field_blob::pack()` 中

```cpp
// sql/field.cc:7355
uchar *Field_blob::pack(uchar *to, const uchar *from, size_t max_length) const {
  uint32 length = get_length(from);
  
  // 自定义检查：限制单个 BLOB 最大 10MB
  if (length > 10 * 1024 * 1024) {
    my_error(ER_TOO_BIG_FIELDLENGTH, MYF(0), field_name, 
             static_cast<ulong>(10 * 1024 * 1024));
    return nullptr;  // 返回错误
  }
  
  // ... 原有代码
}
```

**位置 2**: `Rows_log_event::do_add_row_data()` 中

```cpp
// sql/log_event.cc:8149
int Rows_log_event::do_add_row_data(uchar *row_data, size_t length) {
  // 自定义检查：限制单行最大 50MB
  if (length > 50 * 1024 * 1024) {
    LogErr(ERROR_LEVEL, ER_BINLOG_ROW_VALUE_OPTION_IGNORED, 
           "Row too large", length);
    return ER_BINLOG_ROW_LOGGING_FAILED;
  }
  
  // ... 原有代码
}
```

### 8.2 监控 BLOB 大小（示例）

```cpp
// 在 pack_row() 中添加监控
// sql/rpl_record.cc:270

size_t pack_row(...) {
  size_t total_size = 0;
  size_t blob_size = 0;
  
  for (auto field : fields) {
    if (field->type() == MYSQL_TYPE_BLOB) {
      Field_blob *blob_field = static_cast<Field_blob*>(field);
      uint32 length = blob_field->get_length();
      blob_size += length;
      
      // 记录到日志
      if (length > 1024 * 1024) {  // > 1MB
        LogErr(WARNING_LEVEL, ER_BINLOG_UNSAFE_SYSTEM_FUNCTION,
               "Large BLOB field", field->field_name, length);
      }
    }
  }
  
  // ... 原有代码
  
  return total_size;
}
```

---

## 9. 总结

本文档详细说明了 MySQL 8.0 中 INSERT 语句写入 binlog 的完整代码调用链，特别关注了 BLOB 大字段的处理。

**关键要点**：

1. **BLOB 数据被完整复制多次**：
   - 存储引擎 → record buffer
   - record buffer → 打包缓冲区（Row_data_memory）
   - 打包缓冲区 → Rows_log_event::m_rows_buf
   - m_rows_buf → Binlog cache
   - Binlog cache → Binlog file

2. **内存管理**：
   - 有 BLOB 时使用 `my_malloc` 动态分配
   - Rows_log_event 使用 `std::vector` 自动扩展
   - Binlog cache 超过大小后溢出到临时文件

3. **性能影响**：
   - 多次 memcpy 消耗 CPU
   - 大内存分配可能导致内存碎片
   - 临时文件导致额外的磁盘 I/O

4. **优化方向**：
   - 应用层：减少 BLOB 使用，分批提交
   - 配置层：调整 cache 大小，使用 noblob 模式
   - 硬件层：使用 SSD，增加内存

希望本文档能帮助你深入理解 MySQL binlog 的内部机制！

