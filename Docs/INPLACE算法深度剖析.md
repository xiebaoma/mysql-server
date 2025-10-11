# MySQL Online DDL - INPLACE 算法深度剖析

## 目录

1. [INPLACE 算法概述](#一inplace-算法概述)
2. [INPLACE 的两种模式](#二inplace-的两种模式)
3. [判断逻辑详解](#三判断逻辑详解)
4. [No-Rebuild 模式详解](#四no-rebuild-模式详解)
5. [Rebuild 模式详解](#五rebuild-模式详解)
6. [Row Log 深度剖析](#六row-log-深度剖析)
7. [锁与并发](#七锁与并发)
8. [性能分析与优化](#八性能分析与优化)
9. [常见场景分析](#九常见场景分析)
10. [故障诊断](#十故障诊断)

---

## 一、INPLACE 算法概述

### 1.1 什么是 INPLACE？

**INPLACE** 是 MySQL 8.0 中三种 ALTER TABLE 算法之一：

| 算法 | 特点 | 适用场景 |
|------|------|---------|
| **COPY** | 创建临时表，拷贝所有数据，替换原表 | 必须改变存储引擎或不支持 inplace 的操作 |
| **INPLACE** | 在原表上就地修改，支持并发 DML | 添加/删除索引、修改列等大部分操作 |
| **INSTANT** | 仅修改元数据，不触碰数据 | 在表末尾添加列等少数操作 |

**INPLACE 的核心优势**：
- ✅ **并发性**：允许并发读写（根据具体操作）
- ✅ **空间效率**：不需要完整拷贝数据（no-rebuild 情况）
- ✅ **性能**：比 COPY 快得多

### 1.2 INPLACE vs ONLINE

**重要概念区分**：

```sql
-- ALGORITHM 指定算法类型
ALTER TABLE t1 ADD INDEX idx(col), ALGORITHM=INPLACE;

-- LOCK 指定并发级别
ALTER TABLE t1 ADD INDEX idx(col), LOCK=NONE;
```

| 维度 | 含义 |
|------|------|
| **ALGORITHM** | 指定如何执行 DDL（COPY/INPLACE/INSTANT） |
| **LOCK** | 指定并发级别（NONE/SHARED/EXCLUSIVE） |

**关系**：
- `ALGORITHM=INPLACE` + `LOCK=NONE` = 真正的 "Online DDL"
- INPLACE 不一定 ONLINE（可能需要 SHARED 或 EXCLUSIVE 锁）

### 1.3 源码位置

**核心文件**：
- `storage/innobase/handler/handler0alter.cc`（11000+ 行）
- `storage/innobase/ddl/ddl0*.cc`（DDL 模块）
- `storage/innobase/row/row0log.cc`（Row Log）

---

## 二、INPLACE 的两种模式

INPLACE 算法分为两种根本不同的执行模式：

### 2.1 No-Rebuild（不重建表）

**特点**：
- ✅ 保留原聚簇索引（主键）不变
- ✅ 只修改二级索引或元数据
- ✅ 不需要拷贝数据
- ✅ 速度较快，空间占用小

**典型操作**：
- 添加/删除二级索引
- 添加/删除虚拟列
- 重命名列
- 修改索引注释
- 添加/删除外键

**源码标志**：
```cpp
// storage/innobase/handler/handler0alter.cc:162
static const Alter_inplace_info::HA_ALTER_FLAGS INNOBASE_ALTER_NOREBUILD =
    INNOBASE_ONLINE_CREATE |          // 添加索引
    INNOBASE_FOREIGN_OPERATIONS |     // 外键操作
    Alter_inplace_info::DROP_INDEX |  // 删除索引
    Alter_inplace_info::RENAME_INDEX |
    Alter_inplace_info::ALTER_COLUMN_NAME |
    Alter_inplace_info::ADD_VIRTUAL_COLUMN |
    Alter_inplace_info::DROP_VIRTUAL_COLUMN |
    // ...
```

### 2.2 Rebuild（重建表）

**特点**：
- ⚠️ 需要重新创建聚簇索引
- ⚠️ 需要拷贝所有数据
- ⚠️ 占用双倍磁盘空间（临时）
- ⚠️ 耗时较长

**典型操作**：
- 添加/删除主键
- 修改列类型
- 修改列顺序
- 添加/删除普通列（非 INSTANT）
- 修改行格式（ROW_FORMAT）
- 更改字符集

**源码标志**：
```cpp
// storage/innobase/handler/handler0alter.cc:120
static const Alter_inplace_info::HA_ALTER_FLAGS INNOBASE_ALTER_REBUILD =
    Alter_inplace_info::ADD_PK_INDEX |      // 添加主键
    Alter_inplace_info::DROP_PK_INDEX |     // 删除主键
    Alter_inplace_info::ALTER_COLUMN_NULLABLE |
    Alter_inplace_info::ALTER_COLUMN_NOT_NULLABLE |
    Alter_inplace_info::ALTER_STORED_COLUMN_ORDER |
    Alter_inplace_info::DROP_STORED_COLUMN |
    Alter_inplace_info::ADD_STORED_BASE_COLUMN |
    Alter_inplace_info::RECREATE_TABLE;
```

### 2.3 两种模式的对比

| 对比项 | No-Rebuild | Rebuild |
|--------|-----------|---------|
| **聚簇索引** | 保持不变 | 重新创建 |
| **数据拷贝** | 不需要 | 需要全表拷贝 |
| **临时空间** | 仅索引大小 | 整个表大小 |
| **执行时间** | 较快 | 较慢（与表大小成正比） |
| **Row Log** | 针对新索引 | 针对整个表 |
| **并发 DML** | 更容易处理 | 需要记录和重放所有操作 |

---

## 三、判断逻辑详解

### 3.1 判断是否需要 Rebuild

**核心函数**：`innobase_need_rebuild()`  
**文件**：`storage/innobase/handler/handler0alter.cc:920`

```cpp
static bool innobase_need_rebuild(const Alter_inplace_info *ha_alter_info) {
  // 1. INSTANT DDL 永远不需要 rebuild
  if (is_instant(ha_alter_info)) {
    return false;
  }

  // 2. 过滤掉 InnoDB 不关心的操作
  Alter_inplace_info::HA_ALTER_FLAGS alter_inplace_flags =
      ha_alter_info->handler_flags & ~(INNOBASE_INPLACE_IGNORE);

  // 3. 特殊处理 CHANGE_CREATE_OPTION
  if (alter_inplace_flags == Alter_inplace_info::CHANGE_CREATE_OPTION &&
      !(ha_alter_info->create_info->used_fields &
        (HA_CREATE_USED_ROW_FORMAT |      // 修改行格式
         HA_CREATE_USED_KEY_BLOCK_SIZE |  // 修改索引块大小
         HA_CREATE_USED_TABLESPACE))) {   // 修改表空间
    // 其他 CREATE OPTION 的修改不需要 rebuild
    return false;
  }

  // 4. 检查是否包含 REBUILD 标志
  return !!(ha_alter_info->handler_flags & INNOBASE_ALTER_REBUILD);
}
```

**判断流程图**：

```
开始
  ↓
是否为 INSTANT DDL?
  ↓ 否
是否为 INNOBASE_INPLACE_IGNORE 操作?
  ↓ 否
是否仅修改 CREATE OPTION（不含 ROW_FORMAT/KEY_BLOCK_SIZE/TABLESPACE）?
  ↓ 否
是否包含 INNOBASE_ALTER_REBUILD 标志?
  ↓ 是
需要 REBUILD
```

### 3.2 判断是否支持 INPLACE

**核心函数**：`ha_innobase::check_if_supported_inplace_alter()`  
**文件**：`storage/innobase/handler/handler0alter.cc:956`

**返回值类型**：

```cpp
enum enum_alter_inplace_result {
  HA_ALTER_ERROR,                               // 错误
  HA_ALTER_INPLACE_NOT_SUPPORTED,              // 不支持 INPLACE
  HA_ALTER_INPLACE_EXCLUSIVE_LOCK,             // 需要排他锁
  HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE,  // Prepare 后需要共享锁
  HA_ALTER_INPLACE_SHARED_LOCK,                // 需要共享锁
  HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE,      // Prepare 后无需锁
  HA_ALTER_INPLACE_NO_LOCK,                    // 完全不需要锁
  HA_ALTER_INPLACE_INSTANT                     // 支持 INSTANT
};
```

**判断逻辑简化**：

```cpp
enum_alter_inplace_result check_if_supported_inplace_alter(...) {
  // 1. 检查是否可以 INSTANT
  if (can_use_instant()) {
    return HA_ALTER_INPLACE_INSTANT;
  }
  
  // 2. 检查不支持的操作
  if (unsupported_operations) {
    return HA_ALTER_INPLACE_NOT_SUPPORTED;
  }
  
  // 3. 仅元数据修改
  if (only_metadata_changes) {
    return HA_ALTER_INPLACE_NO_LOCK;
  }
  
  // 4. 添加全文索引
  if (adding_fulltext_index) {
    return HA_ALTER_INPLACE_SHARED_LOCK;
  }
  
  // 5. 添加普通索引
  if (adding_regular_index) {
    return HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE;
  }
  
  // 6. 需要重建表
  if (need_rebuild) {
    return HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE;
  }
  
  // 默认
  return HA_ALTER_INPLACE_NO_LOCK;
}
```

### 3.3 实际案例分析

#### 案例 1: 添加二级索引

```sql
ALTER TABLE users ADD INDEX idx_email(email);
```

**分析**：
- `handler_flags`: `ADD_INDEX`
- 匹配 `INNOBASE_ONLINE_CREATE`
- 不匹配 `INNOBASE_ALTER_REBUILD`
- **结论**：No-Rebuild，返回 `HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE`

#### 案例 2: 修改列类型

```sql
ALTER TABLE users MODIFY COLUMN age BIGINT;
```

**分析**：
- `handler_flags`: `ALTER_COLUMN_TYPE`（实际会设置多个标志）
- 匹配 `INNOBASE_ALTER_REBUILD`
- **结论**：Rebuild，返回 `HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE`

#### 案例 3: 重命名列

```sql
ALTER TABLE users CHANGE COLUMN name user_name VARCHAR(100);
```

**分析**：
- `handler_flags`: `ALTER_COLUMN_NAME`
- 匹配 `INNOBASE_ALTER_NOREBUILD`
- **结论**：No-Rebuild，返回 `HA_ALTER_INPLACE_NO_LOCK`

---

## 四、No-Rebuild 模式详解

### 4.1 执行流程

```
1. Prepare 阶段
   ├─ 创建新索引的 dict_index_t 结构（标记为 ONLINE_INDEX_CREATION）
   ├─ 为每个新索引分配 Row Log
   └─ 分配一致性读视图

2. Execute 阶段
   ├─ 扫描聚簇索引
   ├─ 为每条记录提取索引键值
   ├─ 写入临时排序文件
   ├─ 归并排序
   └─ 批量插入 B-tree
   （期间并发 DML 记录到 Row Log）

3. Commit 阶段
   ├─ 升级到排他锁
   ├─ 应用 Row Log（重放并发 DML）
   ├─ 标记索引为 ONLINE_INDEX_COMPLETE
   ├─ 删除旧索引（如果有）
   └─ 更新数据字典
```

### 4.2 索引构建详解

#### 4.2.1 扫描阶段

**类**：`ddl::Cursor`  
**文件**：`storage/innobase/ddl/ddl0ddl.cc`

```cpp
dberr_t Cursor::scan(Builders &builders) {
  // 打开聚簇索引的游标
  open_cursor();
  
  while (has_more_records) {
    // 读取一条记录
    fetch_record();
    
    // 对每个 Builder（对应一个索引）
    for (auto builder : builders) {
      // 提取索引键值
      dtuple_t *entry = extract_index_entry(record, builder->index);
      
      // 添加到 builder
      builder->add_row(entry);
    }
  }
}
```

#### 4.2.2 Builder 构建索引

**类**：`ddl::Builder`  
**文件**：`storage/innobase/ddl/ddl0builder.cc`

```cpp
class Builder {
  // 添加一行
  dberr_t add_row(dtuple_t *entry) {
    // 1. 写入排序缓冲区
    write_to_sort_buffer(entry);
    
    // 2. 如果缓冲区满，刷到磁盘
    if (buffer_full) {
      flush_to_disk();
    }
  }
  
  // 完成构建
  dberr_t finish() {
    // 1. 刷新剩余数据
    flush_remaining();
    
    // 2. 归并排序临时文件
    merge_sort_files();
    
    // 3. 批量插入 B-tree
    bulk_insert_btree();
  }
};
```

**排序文件**：
- 位置：`innodb_temp/temp_*`
- 大小：取决于索引大小和 `innodb_sort_buffer_size`
- 格式：序列化的索引记录

#### 4.2.3 批量插入 B-tree

**优化**：使用 Bottom-Up 构建算法

```
传统插入：
  插入第1条 → 插入第2条 → ... → 插入第N条
  每次插入需要：
  - 查找插入位置
  - 可能的页分裂
  - 大量随机 I/O

批量插入：
  1. 数据已排序
  2. 从叶子节点开始，从左到右依次填充
  3. 页满后创建新页
  4. 最后构建索引的上层节点
  优势：
  - 顺序 I/O
  - 无页分裂
  - 页填充率高（默认 90%）
```

### 4.3 并发 DML 处理

#### 4.3.1 拦截点

在索引构建期间，对该表的 DML 操作会被拦截：

**INSERT 拦截**：
```cpp
// storage/innobase/row/row0ins.cc
dberr_t row_ins_sec_index_entry(...) {
  // 检查索引状态
  if (dict_index_get_online_status(index) == ONLINE_INDEX_CREATION) {
    // 不实际插入索引，而是记录到 row log
    row_log_online_op(index, entry, trx->id);
    return DB_SUCCESS;
  }
  
  // 正常插入
  return row_ins_sec_index_entry_low(...);
}
```

**UPDATE 拦截**（拆分为 DELETE + INSERT）：
```cpp
// storage/innobase/row/row0upd.cc
dberr_t row_upd_sec_index_entry(...) {
  if (dict_index_get_online_status(index) == ONLINE_INDEX_CREATION) {
    // 记录 DELETE（旧值）
    row_log_online_op(index, old_entry, 0);
    
    // 记录 INSERT（新值）
    row_log_online_op(index, new_entry, trx->id);
    return DB_SUCCESS;
  }
  
  // 正常更新
  // ...
}
```

**DELETE 拦截**：
```cpp
// DELETE 在 InnoDB 中实现为 UPDATE（标记删除）
// 因此走 UPDATE 的逻辑
```

#### 4.3.2 Row Log 记录格式

**文件**：`storage/innobase/row/row0log.cc:283`

```cpp
void row_log_online_op(dict_index_t *index, const dtuple_t *tuple, 
                       trx_id_t trx_id) {
  row_log_t *log = index->online_log;
  
  // 获取锁
  mutex_enter(&log->mutex);
  
  // 写入操作类型
  if (trx_id != 0) {
    *b++ = ROW_OP_INSERT;  // 插入操作
    trx_write_trx_id(b, trx_id);
    b += DATA_TRX_ID_LEN;
  } else {
    *b++ = ROW_OP_DELETE;  // 删除操作
  }
  
  // 写入索引记录
  // 格式：[extra_size][extra_data][data]
  if (extra_size < 0x80) {
    *b++ = (byte)extra_size;
  } else {
    *b++ = (byte)(0x80 | (extra_size >> 8));
    *b++ = (byte)extra_size;
  }
  
  rec_serialize_dtuple(b, index, tuple->fields, tuple->n_fields, ...);
  
  // 如果内存缓冲区满，写入磁盘
  if (log->tail.bytes >= srv_sort_buf_size) {
    row_log_block_write(log);
  }
  
  mutex_exit(&log->mutex);
}
```

**Row Log 结构**：

```
内存部分：
┌─────────────────────────────────────┐
│ Row Log Header                      │
│ - max_trx: 最大事务 ID              │
│ - error: 错误状态                   │
│ - mutex: 互斥锁                     │
└─────────────────────────────────────┘
          ↓
┌─────────────────────────────────────┐
│ Tail (循环缓冲区)                   │
│ ┌─────────┬─────────┬─────────┐    │
│ │ Block 1 │ Block 2 │ Block 3 │    │
│ └─────────┴─────────┴─────────┘    │
│ 大小：srv_sort_buf_size            │
└─────────────────────────────────────┘
          ↓ (满了写入磁盘)
┌─────────────────────────────────────┐
│ 磁盘临时文件                        │
│ - 位置：innodb_temp/                │
│ - 文件名：ib_<space_id>_<index_id> │
│ - 格式：序列化的操作日志             │
└─────────────────────────────────────┘
```

### 4.4 Commit 阶段应用 Row Log

**函数**：`row_log_apply_ops()`  
**文件**：`storage/innobase/row/row0log.cc:3500`

```cpp
dberr_t row_log_apply_ops(const trx_t *trx, dict_index_t *index,
                          ddl::Dup *dup, Alter_stage *stage) {
  // 此时已持有索引的 X 锁，没有新的 DML
  
  mem_heap_t *heap = mem_heap_create(UNIV_PAGE_SIZE);
  
  while (true) {
    // 1. 读取下一个日志块
    mrec = read_next_log_block();
    if (mrec == NULL) break;
    
    // 2. 解析操作类型
    op_type = *mrec++;
    
    switch (op_type) {
      case ROW_OP_INSERT: {
        // 读取事务 ID
        trx_id = trx_read_trx_id(mrec);
        mrec += DATA_TRX_ID_LEN;
        
        // 反序列化索引记录
        entry = row_log_read_index_entry(mrec, index, heap);
        
        // 插入索引
        error = row_ins_sec_index_entry_low(
            index, entry, BTR_CREATE_FLAG | BTR_NO_LOCKING_FLAG, ...);
        
        if (error == DB_DUPLICATE_KEY) {
          // 处理重复键错误
          dup->report(trx, index, entry);
        }
        break;
      }
      
      case ROW_OP_DELETE: {
        // 反序列化索引记录
        entry = row_log_read_index_entry(mrec, index, heap);
        
        // 从索引中删除
        // 1. 先搜索记录
        btr_pcur_open(index, entry, ...);
        
        // 2. 标记删除
        if (found) {
          btr_cur_del_mark_set_sec_rec(...);
        }
        break;
      }
    }
    
    // 检查是否被中断
    if (trx_is_interrupted(trx)) {
      return DB_INTERRUPTED;
    }
    
    // 更新进度
    if (stage) {
      stage->inc();
    }
  }
  
  mem_heap_free(heap);
  return DB_SUCCESS;
}
```

**关键点**：
1. **顺序应用**：按照 DML 发生的顺序应用
2. **事务 ID**：用于 MVCC，确保可见性
3. **重复键检测**：INSERT 时可能发现重复键
4. **删除标记**：DELETE 只是标记删除，不实际删除
5. **幂等性**：DELETE 不存在的记录不报错

### 4.5 性能特点

**No-Rebuild 的性能优势**：

| 指标 | 说明 |
|------|------|
| **时间复杂度** | O(N log N)，主要是排序 |
| **空间复杂度** | 仅新索引的大小 + Row Log |
| **并发影响** | 低，仅在 commit 阶段需要短暂排他锁 |
| **磁盘 I/O** | 主要是顺序 I/O（扫描 + 写排序文件） |

**示例**：添加一个索引到 1 亿行的表
- 表大小：100 GB
- 新索引大小：10 GB
- 临时空间：~15 GB（排序文件 + row log）
- 时间：取决于 CPU 和磁盘速度，可能 1-2 小时
- 并发 DML：全程允许，commit 阶段可能几秒钟排他锁

---

## 五、Rebuild 模式详解

### 5.1 执行流程

Rebuild 模式需要重新创建整个表（包括聚簇索引）：

```
1. Prepare 阶段
   ├─ 创建新表结构（ctx->new_table）
   ├─ 创建所有索引结构（聚簇索引 + 所有二级索引）
   ├─ 为聚簇索引分配 Row Log（重要！）
   └─ 建立列映射（col_map: old → new）

2. Execute 阶段
   ├─ 扫描旧表的聚簇索引
   ├─ 转换每一行（应用列映射、默认值等）
   ├─ 插入新表的聚簇索引
   └─ 同时构建所有二级索引
   （期间并发 DML 记录到旧表聚簇索引的 Row Log）

3. Commit 阶段
   ├─ 升级到排他锁
   ├─ 应用 Row Log 到新表
   ├─ 交换新旧表的元数据
   ├─ 删除旧表
   └─ 更新数据字典
```

### 5.2 新表创建

**函数**：`prepare_inplace_alter_table_impl()`  
**文件**：`storage/innobase/handler/handler0alter.cc`

```cpp
// 创建新表
ctx->new_table = dict_mem_table_create(
    ctx->tmp_name,        // 临时表名：#sql-ib<table_id>-<random>
    space,                // 表空间
    n_cols,               // 列数
    n_v_cols,             // 虚拟列数
    flags,                // 表标志
    flags2                // 表标志2
);

// 添加列
for (i = 0; i < n_cols; i++) {
  dict_mem_table_add_col(ctx->new_table, ...);
}

// 创建聚簇索引
dict_index_t *clust_index = dict_mem_index_create(
    ctx->tmp_name,
    "GEN_CLUST_INDEX",    // 如果没有主键
    space,
    DICT_CLUSTERED,
    n_fields
);

// 创建所有二级索引
for (i = 0; i < n_indexes; i++) {
  dict_index_t *index = dict_mem_index_create(...);
  // ...
}
```

### 5.3 列映射（Column Mapping）

**作用**：将旧表的列映射到新表

```cpp
// ctx->col_map 数组：old_col_no → new_col_no
// 
// 示例：
// 旧表：id, name, age, email
// 新表：id, name, phone, age, email (在中间添加了 phone)
//
// col_map:
//   0 → 0  (id → id)
//   1 → 1  (name → name)
//   2 → 3  (age → age)  
//   3 → 4  (email → email)
//   ULINT_UNDEFINED → 2  (phone 是新列)
```

**使用示例**：
```cpp
// 转换一行数据
for (old_col_no = 0; old_col_no < old_n_cols; old_col_no++) {
  new_col_no = ctx->col_map[old_col_no];
  
  if (new_col_no != ULINT_UNDEFINED) {
    // 拷贝列值
    new_row[new_col_no] = old_row[old_col_no];
  }
}

// 填充新列的默认值
for (new_col_no = 0; new_col_no < new_n_cols; new_col_no++) {
  if (is_new_column(new_col_no)) {
    new_row[new_col_no] = get_default_value(new_col_no);
  }
}
```

### 5.4 数据转换与插入

**类**：`ddl::Loader`  
**文件**：`storage/innobase/ddl/ddl0loader.cc`

```cpp
dberr_t Loader::scan_and_build_indexes() {
  // 创建游标
  Cursor *cursor = Cursor::create_cursor(m_ctx);
  
  // 扫描旧表
  while (cursor->fetch_next()) {
    // 读取旧行
    const rec_t *old_rec = cursor->get_rec();
    
    // 转换为新行
    dtuple_t *new_row = convert_row(old_rec, ctx->col_map, ctx->add_cols);
    
    // 插入新表的聚簇索引
    error = insert_into_clustered_index(ctx->new_table, new_row);
    
    // 同时为所有二级索引提取键值
    for (auto builder : m_builders) {
      dtuple_t *index_entry = extract_index_entry(new_row, builder->index);
      builder->add_row(index_entry);
    }
  }
  
  // 完成所有索引的构建
  for (auto builder : m_builders) {
    builder->finish();
  }
}
```

**转换函数**：`row_build_w_add_vcol()`  
**文件**：`storage/innobase/row/row0row.cc`

```cpp
dtuple_t* row_build_w_add_vcol(
    row_type_t type,
    const dict_index_t *index,
    const rec_t *rec,
    const ulint *offsets,
    const dict_table_t *table,
    const dtuple_t *add_cols,      // 新增列的默认值
    const dict_add_v_col_t *add_v, // 新增虚拟列
    const ulint *col_map,          // 列映射
    mem_heap_t *heap
) {
  // 1. 创建新行
  dtuple_t *row = dtuple_create(heap, table->n_cols);
  
  // 2. 拷贝旧列
  for (i = 0; i < n_old_cols; i++) {
    new_col_no = col_map[i];
    if (new_col_no != ULINT_UNDEFINED) {
      dfield_t *dfield = dtuple_get_nth_field(row, new_col_no);
      // 从旧记录中读取
      read_field_from_record(rec, offsets, i, dfield);
    }
  }
  
  // 3. 填充新增列的默认值
  if (add_cols) {
    for (i = 0; i < add_cols->n_fields; i++) {
      dfield_t *dfield = dtuple_get_nth_field(row, add_col_no);
      dfield_copy(dfield, dtuple_get_nth_field(add_cols, i));
    }
  }
  
  // 4. 计算虚拟列
  if (add_v) {
    for (i = 0; i < add_v->n_v_col; i++) {
      compute_virtual_column(row, add_v->v_col[i]);
    }
  }
  
  return row;
}
```

### 5.5 并发 DML 处理（Rebuild）

**关键区别**：
- **No-Rebuild**：DML 记录到新索引的 row log
- **Rebuild**：DML 记录到旧表聚簇索引的 row log

**原因**：
- 新表还在构建中，DML 不能直接操作新表
- DML 必须在旧表上执行（保证数据一致性）
- Row Log 记录这些 DML，稍后应用到新表

**拦截点**：

```cpp
// storage/innobase/row/row0ins.cc
dberr_t row_ins_clust_index_entry(...) {
  // 检查聚簇索引是否正在重建
  if (dict_index_get_online_status(index) == ONLINE_INDEX_CREATION) {
    // 记录 INSERT 到 row log
    row_log_online_op(index, entry, trx->id);
    
    // 仍然要插入旧表（重要！）
    // 因为旧表还在对外服务
  }
  
  // 正常插入旧表
  return row_ins_clust_index_entry_low(...);
}
```

**Row Log 格式（Rebuild）**：

```
每条日志记录：
┌──────────────────────────────────────┐
│ Operation Type (1 byte)              │
│ - ROW_OP_INSERT: 插入                │
│ - ROW_OP_DELETE: 删除                │
├──────────────────────────────────────┤
│ Transaction ID (6 bytes, 如果INSERT) │
├──────────────────────────────────────┤
│ Record Data (变长)                   │
│ - Extra size                         │
│ - Extra data (NULL bitmap, etc.)     │
│ - Column data                        │
└──────────────────────────────────────┘
```

### 5.6 应用 Row Log（Rebuild）

**函数**：`row_log_table_apply_ops()`  
**文件**：`storage/innobase/row/row0log.cc:2726`

```cpp
dberr_t row_log_table_apply_ops(que_thr_t *thr, ddl::Dup *dup,
                                Alter_stage *stage) {
  dict_index_t *clust_index = old_table->first_index();
  dict_table_t *new_table = clust_index->online_log->table;
  
  while (true) {
    // 1. 读取日志记录
    mrec = read_next_log_record();
    if (mrec == NULL) break;
    
    op_type = *mrec++;
    
    switch (op_type) {
      case ROW_OP_INSERT: {
        // 读取事务 ID
        trx_id = trx_read_trx_id(mrec);
        
        // 反序列化行
        old_row = row_log_read_row(mrec, old_table, heap);
        
        // 转换为新行
        new_row = convert_row_to_new_table(
            old_row, new_table, col_map, add_cols);
        
        // 插入新表
        error = row_ins_clust_index_entry_low(
            new_table->first_index(), new_row, ...);
        
        // 同时插入所有二级索引
        for (index = new_table->first_index()->next(); 
             index != NULL; 
             index = index->next()) {
          entry = row_build_index_entry(new_row, index, heap);
          row_ins_sec_index_entry_low(index, entry, ...);
        }
        break;
      }
      
      case ROW_OP_DELETE: {
        // 读取行（用于定位）
        old_row = row_log_read_row(mrec, old_table, heap);
        
        // 转换为新行（用主键定位）
        new_row = convert_row_to_new_table(old_row, ...);
        
        // 从新表中删除
        // 1. 根据主键搜索
        btr_pcur_open_on_user_rec(new_table->first_index(), 
                                   new_row, ...);
        
        // 2. 标记删除
        if (found) {
          rec_set_deleted_flag(rec, TRUE);
          
          // 同时从二级索引删除
          for (index = new_table->first_index()->next(); 
               index != NULL; 
               index = index->next()) {
            row_upd_del_mark_sec_rec(index, entry, ...);
          }
        }
        break;
      }
    }
  }
}
```

**关键点**：
1. **转换**：旧表的行 → 新表的行（应用列映射）
2. **完整操作**：插入聚簇索引 + 所有二级索引
3. **事务语义**：保持原事务的可见性
4. **错误处理**：重复键、空间不足等

### 5.7 表交换

**函数**：`commit_try_rebuild()`  
**文件**：`storage/innobase/handler/handler0alter.cc:6906`

```cpp
bool commit_try_rebuild(...) {
  dict_table_t *old_table = ctx->old_table;
  dict_table_t *new_table = ctx->new_table;
  
  // 1. 应用 row log
  row_log_table_apply(old_table, new_table, stage);
  
  // 2. 准备重命名
  //    旧表：t1      → #sql-old-t1
  //    新表：#sql-new → t1
  
  // 3. 更新数据字典（在 commit_cache_rebuild 中）
  commit_cache_rebuild(ctx);
  
  // 4. 交换表的内部结构
  //    将 new_table 的内容复制到 old_table
  dict_table_t temp = *old_table;
  *old_table = *new_table;
  *new_table = temp;
  
  // 5. 旧表会被后续清理
}
```

**交换过程**：

```
Before:
  old_table (name="t1")
    ├─ dict_table_t 结构
    ├─ space_id = 10
    └─ indexes...
  
  new_table (name="#sql-ib123-456")
    ├─ dict_table_t 结构
    ├─ space_id = 20
    └─ indexes...

After:
  old_table (name="t1")  ← 指向新数据
    ├─ dict_table_t 结构 ← 从 new_table 复制
    ├─ space_id = 20     ← 新表的表空间
    └─ indexes...        ← 新表的索引
  
  new_table (name="#sql-ib123-456")  ← 指向旧数据（待删除）
    ├─ space_id = 10
    └─ ...
```

### 5.8 性能特点

**Rebuild 的性能代价**：

| 指标 | 说明 |
|------|------|
| **时间复杂度** | O(N)，需要扫描全表 |
| **空间复杂度** | 2倍表大小（旧表 + 新表） |
| **并发影响** | 中等，commit 阶段排他锁时间取决于 row log 大小 |
| **磁盘 I/O** | 大量顺序读写 |

**示例**：修改列类型（1 亿行的表）
- 表大小：100 GB
- 临时空间：~120 GB（新表 100 GB + row log 20 GB）
- 时间：可能 3-6 小时（取决于硬件）
- 并发 DML：
  - 扫描阶段：完全允许
  - Commit 阶段：几分钟排他锁（应用 row log）

---

## 六、Row Log 深度剖析

### 6.1 Row Log 的生命周期

```
1. 分配（Prepare 阶段）
   row_log_allocate()
   ├─ 分配 row_log_t 结构
   ├─ 分配内存缓冲区
   └─ 关联到索引（index->online_log）

2. 记录（Execute 阶段）
   row_log_online_op()
   ├─ 序列化操作
   ├─ 写入内存缓冲区
   └─ 必要时刷到磁盘

3. 应用（Commit 阶段）
   row_log_apply() / row_log_table_apply()
   ├─ 读取日志记录
   ├─ 解析并执行
   └─ 处理冲突

4. 清理
   row_log_free()
   ├─ 删除临时文件
   ├─ 释放内存缓冲区
   └─ 清空 index->online_log
```

### 6.2 Row Log 的内存管理

**数据结构**：`row_log_t`  
**文件**：`storage/innobase/include/row0log.h`

```cpp
struct row_log_t {
  // 内存缓冲区（循环缓冲）
  struct {
    byte *block;          // 当前活跃块
    byte buf[...];        // 临时缓冲区
    ulint bytes;          // 已使用字节数
    os_offset_t blocks;   // 已写入磁盘的块数
  } tail;
  
  // 磁盘文件
  pfs_os_file_t fd;       // 文件描述符
  os_offset_t head_offset;// 读取位置
  ib_uint64_t head_size;  // 已读取大小
  
  // 控制信息
  mutex_t mutex;          // 保护结构的锁
  dict_table_t *table;    // 关联的表（rebuild 时）
  dict_index_t *index;    // 关联的索引
  trx_id_t max_trx;       // 最大事务 ID
  dberr_t error;          // 错误状态
  
  // 限制
  ulonglong max_size;     // 最大大小限制
};
```

**缓冲区管理**：

```cpp
// 写入时
row_log_online_op() {
  mutex_enter(&log->mutex);
  
  // 检查空间
  if (log->tail.bytes + size > srv_sort_buf_size) {
    // 刷新到磁盘
    row_log_block_write(log);
    log->tail.bytes = 0;
  }
  
  // 写入缓冲区
  memcpy(log->tail.block + log->tail.bytes, data, size);
  log->tail.bytes += size;
  
  // 检查总大小限制
  if (log->head_size + log->tail.bytes > log->max_size) {
    log->error = DB_ONLINE_LOG_TOO_BIG;
  }
  
  mutex_exit(&log->mutex);
}
```

### 6.3 Row Log 的磁盘存储

**文件位置**：`innodb_temp/ib_<space_id>_<index_id>.row_log`

**文件格式**：

```
文件头：
┌────────────────────────────────────┐
│ Magic Number (4 bytes)             │
│ Version (4 bytes)                  │
│ Index ID (8 bytes)                 │
└────────────────────────────────────┘

数据块（重复）：
┌────────────────────────────────────┐
│ Block Header                       │
│ - Block size (4 bytes)             │
│ - Checksum (4 bytes)               │
├────────────────────────────────────┤
│ Log Records (多条)                 │
│ ┌────────────────────────────────┐│
│ │ Record 1                       ││
│ ├────────────────────────────────┤│
│ │ Record 2                       ││
│ ├────────────────────────────────┤│
│ │ ...                            ││
│ └────────────────────────────────┘│
└────────────────────────────────────┘
```

**写入流程**：

```cpp
void row_log_block_write(row_log_t *log) {
  // 1. 计算校验和
  ulint checksum = ut_crc32(log->tail.block, log->tail.bytes);
  
  // 2. 写入块头
  mach_write_to_4(block_header, log->tail.bytes);
  mach_write_to_4(block_header + 4, checksum);
  
  // 3. 写入磁盘
  os_file_write(log->fd, block_header, offset, 8);
  os_file_write(log->fd, log->tail.block, offset + 8, log->tail.bytes);
  
  // 4. 刷新到磁盘（可选）
  if (srv_file_flush_method == SRV_FSYNC) {
    os_file_flush(log->fd);
  }
  
  // 5. 更新统计
  log->tail.blocks++;
}
```

### 6.4 Row Log 大小限制

**参数**：`innodb_online_alter_log_max_size`  
**默认值**：128 MB  
**作用**：限制 row log 的最大大小

**为什么需要限制？**

1. **磁盘空间**：防止占用过多临时空间
2. **Commit 时间**：row log 越大，应用时间越长（持有排他锁）
3. **失败恢复**：row log 太大，失败后浪费的工作更多

**超限处理**：

```cpp
// 在 row_log_online_op() 中
if (log->head_size + log->tail.bytes > log->max_size) {
  // 设置错误
  log->error = DB_ONLINE_LOG_TOO_BIG;
  
  // DDL 会失败并回滚
  // 用户需要：
  // 1. 增大 innodb_online_alter_log_max_size，或
  // 2. 在 DML 较少的时间执行 DDL
}
```

### 6.5 Row Log vs Redo Log

很多人会混淆这两个概念，它们完全不同：

| 特性 | Row Log | Redo Log |
|------|---------|----------|
| **用途** | Online DDL 期间记录并发 DML | 崩溃恢复 |
| **作用域** | 单个 DDL 操作 | 整个实例 |
| **生命周期** | DDL 开始到结束 | 循环使用，持久化 |
| **位置** | `innodb_temp/` | `ib_logfile*` 或 `#innodb_redo/` |
| **格式** | 完整的行或索引记录 | 页面修改的物理日志 |
| **大小** | 取决于并发 DML，有上限 | 固定大小，循环写入 |
| **应用时机** | DDL Commit 阶段 | 崩溃恢复时 |

---

## 七、锁与并发

### 7.1 MDL 锁的演变

Online DDL 期间 MDL 锁的变化：

```
Timeline:
┌─────────────────────────────────────────────────────────────────┐
│                                                                 │
│ Phase 1: Initialization                                         │
│ ├─ 获取 MDL_SHARED_UPGRADABLE                                  │
│ │  允许：SELECT, INSERT, UPDATE, DELETE                        │
│ │  阻止：其他 DDL                                              │
│ └─────────────────────────────────────────────────────────────│
│                                                                 │
│ Phase 2-1: Prepare (如果需要 EXCLUSIVE)                        │
│ ├─ 升级到 MDL_EXCLUSIVE                                        │
│ │  允许：无                                                    │
│ │  阻止：一切访问                                              │
│ │  持续时间：很短（创建结构、分配 row log）                   │
│ └─────────────────────────────────────────────────────────────│
│                                                                 │
│ Phase 2-2: Execute                                              │
│ ├─ 降级到 MDL_SHARED_NO_WRITE 或 MDL_SHARED_UPGRADABLE        │
│ │  SHARED_NO_WRITE:                                            │
│ │    允许：SELECT                                              │
│ │    阻止：INSERT, UPDATE, DELETE                              │
│ │  SHARED_UPGRADABLE (NO_LOCK):                                │
│ │    允许：SELECT, INSERT, UPDATE, DELETE                      │
│ │    阻止：其他 DDL                                            │
│ │  持续时间：最长（扫描表、构建索引）                          │
│ └─────────────────────────────────────────────────────────────│
│                                                                 │
│ Phase 2-3: Commit                                               │
│ ├─ 升级到 MDL_EXCLUSIVE                                        │
│ │  允许：无                                                    │
│ │  阻止：一切访问                                              │
│ │  持续时间：较短（应用 row log、交换元数据）                 │
│ └─────────────────────────────────────────────────────────────│
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 7.2 不同操作的锁需求

| 操作 | Prepare 锁 | Execute 锁 | Commit 锁 | 说明 |
|------|-----------|-----------|-----------|------|
| 添加二级索引 | EXCLUSIVE（短） | SHARED_UPGRADABLE | EXCLUSIVE（短） | 允许并发 DML |
| 删除二级索引 | EXCLUSIVE（短） | SHARED_UPGRADABLE | EXCLUSIVE（短） | 允许并发 DML |
| 添加列（末尾） | SHARED_UPGRADABLE | - | EXCLUSIVE（短） | INSTANT，很快 |
| 修改列类型 | EXCLUSIVE（短） | SHARED_NO_WRITE | EXCLUSIVE（较长） | 需要 rebuild |
| 添加主键 | EXCLUSIVE（短） | SHARED_NO_WRITE | EXCLUSIVE（较长） | 需要 rebuild |
| 删除主键 | EXCLUSIVE（短） | SHARED_NO_WRITE | EXCLUSIVE（较长） | 需要 rebuild |
| 重命名列 | SHARED_UPGRADABLE | SHARED_UPGRADABLE | EXCLUSIVE（短） | 仅元数据 |

### 7.3 等待锁的问题

**常见问题**：DDL 一直等待，无法获取锁

**原因**：
1. 有长事务持有表的锁
2. 有大量活跃的短事务

**诊断**：
```sql
-- 查看锁等待
SELECT * FROM performance_schema.metadata_locks
WHERE OBJECT_NAME = 'your_table';

-- 查看活跃事务
SELECT * FROM information_schema.innodb_trx
WHERE trx_state = 'RUNNING';

-- 查看长事务
SELECT *
FROM information_schema.innodb_trx
WHERE TIMESTAMPDIFF(SECOND, trx_started, NOW()) > 60;
```

**解决**：
```sql
-- 等待长事务结束，或
-- 杀掉长事务
KILL <trx_mysql_thread_id>;
```

### 7.4 死锁风险

**场景 1**：两个 DDL 同时执行

```sql
-- Session 1
ALTER TABLE t1 ADD INDEX idx1(col1);

-- Session 2 (同时)
ALTER TABLE t1 ADD INDEX idx2(col2);

-- 结果：第二个会等待第一个完成
```

**场景 2**：DDL 和 DML 死锁

```sql
-- Session 1
BEGIN;
UPDATE t1 SET col1 = 1 WHERE id = 1;
-- (未提交)

-- Session 2
ALTER TABLE t1 ADD INDEX idx(col2);
-- 等待 Session 1 释放锁

-- Session 1
UPDATE t1 SET col1 = 2 WHERE id = 2;
-- 可能死锁：取决于锁的获取顺序
```

**避免**：
- 在低峰期执行 DDL
- 执行 DDL 前确保没有长事务
- 使用 `LOCK=NONE` 明确要求 online 执行

---

## 八、性能分析与优化

### 8.1 性能瓶颈分析

**1. I/O 瓶颈**

```
症状：
- DDL 执行很慢
- 磁盘 I/O 利用率高
- CPU 利用率低

原因：
- 扫描大表需要大量读 I/O
- 写排序文件需要大量写 I/O
- 磁盘速度慢（HDD vs SSD）

优化：
- 使用 SSD
- 增大 innodb_buffer_pool_size（减少物理 I/O）
- 调整 innodb_io_capacity
```

**2. CPU 瓶颈**

```
症状：
- CPU 利用率高
- 排序阶段很慢

原因：
- 排序需要大量 CPU
- 数据压缩/解压缩
- 索引键值比较

优化：
- 增加 innodb_ddl_threads（并行）
- 使用更快的 CPU
```

**3. 内存瓶颈**

```
症状：
- 大量 swap
- OOM killer 触发

原因：
- innodb_sort_buffer_size 过大
- innodb_ddl_threads 过多
- 系统内存不足

优化：
- 减小 innodb_sort_buffer_size
- 减少 innodb_ddl_threads
- 增加物理内存
```

### 8.2 关键参数调优

#### 8.2.1 innodb_sort_buffer_size

**默认值**：1 MB  
**推荐值**：根据内存调整

```sql
-- 查看当前值
SHOW VARIABLES LIKE 'innodb_sort_buffer_size';

-- 设置（全局）
SET GLOBAL innodb_sort_buffer_size = 64 * 1024 * 1024;  -- 64 MB
```

**影响**：
- 更大 → 更少的磁盘临时文件，更快的排序
- 但会占用更多内存（每个 DDL 线程一个）

**经验值**：
- 小表（< 1 GB）：1-4 MB
- 中表（1-10 GB）：4-16 MB
- 大表（> 10 GB）：16-64 MB

#### 8.2.2 innodb_ddl_threads

**默认值**：4  
**推荐值**：根据 CPU 核数调整

```sql
-- 查看当前值
SHOW VARIABLES LIKE 'innodb_ddl_threads';

-- 设置（全局）
SET GLOBAL innodb_ddl_threads = 8;
```

**影响**：
- 更多线程 → 更快的扫描和排序
- 但会占用更多 CPU 和内存

**经验值**：
- 小表：2-4
- 大表：8-16（不超过 CPU 核数）

#### 8.2.3 innodb_online_alter_log_max_size

**默认值**：128 MB  
**推荐值**：根据 DML 频率调整

```sql
-- 查看当前值
SHOW VARIABLES LIKE 'innodb_online_alter_log_max_size';

-- 设置（全局）
SET GLOBAL innodb_online_alter_log_max_size = 1024 * 1024 * 1024;  -- 1 GB
```

**影响**：
- 更大 → DDL 期间可以容纳更多 DML
- 但 commit 阶段应用 row log 时间更长

**经验值**：
- 低 DML：128-256 MB
- 中 DML：256-512 MB
- 高 DML：512 MB - 1 GB

### 8.3 监控 DDL 进度

**Performance Schema**：

```sql
-- 查看当前阶段
SELECT 
    EVENT_NAME,
    WORK_COMPLETED,
    WORK_ESTIMATED,
    ROUND(100 * WORK_COMPLETED / WORK_ESTIMATED, 2) AS PCT_COMPLETED
FROM performance_schema.events_stages_current
WHERE EVENT_NAME LIKE 'stage/innodb/alter%';

-- 查看详细信息
SELECT * FROM performance_schema.events_stages_current
WHERE EVENT_NAME LIKE 'stage/innodb/alter%'\G
```

**SHOW ENGINE INNODB STATUS**：

```sql
SHOW ENGINE INNODB STATUS\G

-- 查找 ONLINE DDL 部分：
------------
ONLINE DDL
------------
online_ddl_num_pending: 1
online_ddl_pct_complete: 45.67
```

### 8.4 案例：优化 1TB 表的索引添加

**场景**：
- 表大小：1 TB（10 亿行）
- 操作：添加一个二级索引
- 目标：最小化对业务的影响

**步骤 1：评估**

```sql
-- 评估表大小
SELECT 
    table_name,
    ROUND(data_length / 1024 / 1024 / 1024, 2) AS data_gb,
    ROUND(index_length / 1024 / 1024 / 1024, 2) AS index_gb,
    table_rows
FROM information_schema.tables
WHERE table_schema = 'your_db' AND table_name = 'your_table';

-- 评估新索引大小（估算）
-- 假设索引列大小 = 100 bytes
-- 索引大小约 = rows * (100 + 页开销) ≈ 10^9 * 120 ≈ 120 GB
```

**步骤 2：调优参数**

```sql
-- 增大排序缓冲区
SET GLOBAL innodb_sort_buffer_size = 64 * 1024 * 1024;  -- 64 MB

-- 增加并行度
SET GLOBAL innodb_ddl_threads = 16;

-- 增大 row log 限制
SET GLOBAL innodb_online_alter_log_max_size = 2 * 1024 * 1024 * 1024;  -- 2 GB
```

**步骤 3：选择时机**

```
最佳时间：
- 业务低峰期（如凌晨）
- DML 较少的时间段
- 确保没有长事务
```

**步骤 4：执行**

```sql
-- 使用 ALGORITHM=INPLACE, LOCK=NONE 确保 online
ALTER TABLE your_table 
ADD INDEX idx_col(col),
ALGORITHM=INPLACE,
LOCK=NONE;
```

**步骤 5：监控**

```bash
# 在另一个 session 中
while true; do
    mysql -e "
        SELECT 
            EVENT_NAME,
            ROUND(100 * WORK_COMPLETED / WORK_ESTIMATED, 2) AS PCT
        FROM performance_schema.events_stages_current
        WHERE EVENT_NAME LIKE 'stage/innodb/alter%';
    "
    sleep 60
done
```

**预期时间**：
- 扫描：6-12 小时（取决于磁盘速度）
- 排序：2-4 小时
- 插入 B-tree：2-4 小时
- 总计：10-20 小时

**临时空间需求**：
- 排序文件：~150 GB
- Row log：< 2 GB
- 总计：~152 GB

---

## 九、常见场景分析

### 9.1 添加索引

```sql
ALTER TABLE users ADD INDEX idx_email(email);
```

**分析**：
- **算法**：INPLACE (No-Rebuild)
- **锁**：LOCK=NONE（允许并发 DML）
- **流程**：
  1. 扫描聚簇索引
  2. 提取 email 列的值
  3. 排序
  4. 构建 B-tree
  5. 应用 row log
- **时间**：O(N log N)
- **空间**：新索引大小 + row log

### 9.2 删除索引

```sql
ALTER TABLE users DROP INDEX idx_email;
```

**分析**：
- **算法**：INPLACE (No-Rebuild)
- **锁**：LOCK=NONE
- **流程**：
  1. 标记索引为待删除
  2. 更新数据字典
  3. 后台清理索引页面
- **时间**：很快（几秒）
- **空间**：释放索引占用的空间（异步）

### 9.3 修改列类型（扩大）

```sql
ALTER TABLE users MODIFY COLUMN name VARCHAR(200);  -- 原来是 VARCHAR(100)
```

**分析**：
- **算法**：INPLACE (No-Rebuild)
- **锁**：LOCK=NONE
- **流程**：仅修改元数据
- **时间**：很快（几秒）
- **注意**：仅当新类型更大时才是 no-rebuild

### 9.4 修改列类型（缩小或改变类型）

```sql
ALTER TABLE users MODIFY COLUMN age TINYINT;  -- 原来是 INT
```

**分析**：
- **算法**：INPLACE (Rebuild)
- **锁**：LOCK=SHARED（不允许 DML）
- **流程**：
  1. 创建新表
  2. 转换并拷贝所有数据
  3. 应用 row log
  4. 交换表
- **时间**：O(N)
- **空间**：2倍表大小

### 9.5 添加列（末尾）

```sql
ALTER TABLE users ADD COLUMN phone VARCHAR(20);
```

**分析**：
- **算法**：INSTANT（8.0.12+）
- **锁**：LOCK=NONE
- **流程**：仅修改元数据
- **时间**：瞬间（< 1秒）
- **注意**：列添加在表末尾才是 INSTANT

### 9.6 添加列（中间）

```sql
ALTER TABLE users ADD COLUMN phone VARCHAR(20) AFTER name;
```

**分析**：
- **算法**：INPLACE (Rebuild)（8.0.29 之前）或 INSTANT（8.0.29+）
- **锁**：取决于算法
- **流程**：
  - REBUILD：需要重组所有数据
  - INSTANT（8.0.29+）：修改元数据 + 记录列顺序
- **时间**：
  - REBUILD：O(N)
  - INSTANT：瞬间

### 9.7 添加主键

```sql
ALTER TABLE logs ADD PRIMARY KEY (id);  -- 原来没有主键
```

**分析**：
- **算法**：INPLACE (Rebuild)
- **锁**：LOCK=SHARED
- **流程**：
  1. 创建新表（以 id 为聚簇索引）
  2. 重新组织所有数据
  3. 构建所有二级索引
  4. 交换表
- **时间**：O(N)
- **空间**：2倍表大小
- **注意**：这是最重的操作之一

### 9.8 删除主键

```sql
ALTER TABLE logs DROP PRIMARY KEY;
```

**分析**：
- **算法**：INPLACE (Rebuild)
- **锁**：LOCK=SHARED
- **流程**：
  1. 创建新表（GEN_CLUST_INDEX 作为隐藏聚簇索引）
  2. 拷贝所有数据
  3. 交换表
- **时间**：O(N)
- **空间**：2倍表大小

### 9.9 重命名列

```sql
ALTER TABLE users CHANGE COLUMN name user_name VARCHAR(100);
```

**分析**：
- **算法**：INPLACE (No-Rebuild)
- **锁**：LOCK=NONE
- **流程**：仅修改数据字典
- **时间**：很快
- **注意**：如果同时修改类型，可能需要 rebuild

### 9.10 修改字符集

```sql
ALTER TABLE users CONVERT TO CHARACTER SET utf8mb4;
```

**分析**：
- **算法**：INPLACE (Rebuild)
- **锁**：LOCK=SHARED
- **流程**：
  1. 创建新表（新字符集）
  2. 转换并拷贝所有数据
  3. 交换表
- **时间**：O(N)
- **空间**：2倍表大小
- **注意**：字符集转换需要重新编码数据

---

## 十、故障诊断

### 10.1 DDL 失败：Row Log 过大

**错误**：
```
ERROR 1799 (HY000): Creating index 'idx' required more than 
'innodb_online_alter_log_max_size' bytes of modification log. 
Please try again.
```

**原因**：DDL 期间的并发 DML 太多，row log 超过限制

**解决方案**：

**方案 1：增大限制**
```sql
-- 增大到 1 GB
SET GLOBAL innodb_online_alter_log_max_size = 1024 * 1024 * 1024;

-- 重试 DDL
ALTER TABLE users ADD INDEX idx_email(email);
```

**方案 2：减少并发 DML**
```sql
-- 在业务低峰期执行
-- 或者使用 pt-online-schema-change 工具
```

**方案 3：分批处理**
```sql
-- 如果可能，分批添加索引
-- 例如，先删除部分数据，再添加索引
```

### 10.2 DDL 失败：磁盘空间不足

**错误**：
```
ERROR 1114 (HY000): The table is full
```

**原因**：
- 临时文件占用大量空间
- 磁盘剩余空间不足

**诊断**：
```bash
# 查看磁盘使用
df -h

# 查看临时目录
du -sh /path/to/innodb_temp/

# 查看 InnoDB 表空间
SELECT 
    file_name, 
    ROUND(allocated_size / 1024 / 1024 / 1024, 2) AS size_gb
FROM information_schema.files
WHERE tablespace_name LIKE '%temp%';
```

**解决方案**：

**方案 1：清理空间**
```bash
# 删除不需要的文件
# 清理日志文件
# 清理临时文件
```

**方案 2：更改临时目录**
```sql
-- 更改 tmpdir 到更大的磁盘
SET GLOBAL innodb_tmpdir = '/path/to/larger/disk';
```

**方案 3：使用 COPY 算法**
```sql
-- COPY 算法可以更精细地控制临时表的位置
ALTER TABLE users ADD INDEX idx_email(email), ALGORITHM=COPY;
```

### 10.3 DDL 失败：重复键

**错误**：
```
ERROR 1062 (23000): Duplicate entry 'xxx' for key 'idx_email'
```

**原因**：
- 表中存在重复数据
- 尝试创建唯一索引或主键

**诊断**：
```sql
-- 查找重复数据
SELECT email, COUNT(*)
FROM users
GROUP BY email
HAVING COUNT(*) > 1;
```

**解决方案**：

**方案 1：清理重复数据**
```sql
-- 删除重复数据（保留一条）
DELETE t1 FROM users t1
INNER JOIN users t2 
WHERE t1.id > t2.id AND t1.email = t2.email;

-- 重试 DDL
ALTER TABLE users ADD UNIQUE INDEX idx_email(email);
```

**方案 2：改为非唯一索引**
```sql
-- 如果不需要唯一性
ALTER TABLE users ADD INDEX idx_email(email);  -- 非 UNIQUE
```

### 10.4 DDL 长时间等待锁

**症状**：DDL 一直处于 "Waiting for table metadata lock" 状态

**诊断**：
```sql
-- 查看锁等待
SELECT 
    r.trx_id AS waiting_trx_id,
    r.trx_mysql_thread_id AS waiting_thread,
    r.trx_query AS waiting_query,
    b.trx_id AS blocking_trx_id,
    b.trx_mysql_thread_id AS blocking_thread,
    b.trx_query AS blocking_query,
    b.trx_state AS blocking_state
FROM information_schema.innodb_lock_waits w
JOIN information_schema.innodb_trx r ON w.requesting_trx_id = r.trx_id
JOIN information_schema.innodb_trx b ON w.blocking_trx_id = b.trx_id;

-- 查看长事务
SELECT 
    trx_id,
    trx_state,
    trx_started,
    TIMESTAMPDIFF(SECOND, trx_started, NOW()) AS duration_sec,
    trx_mysql_thread_id,
    trx_query
FROM information_schema.innodb_trx
WHERE trx_state = 'RUNNING'
ORDER BY trx_started;
```

**解决方案**：

**方案 1：等待事务完成**
```sql
-- 如果是合理的业务事务，等待其完成
```

**方案 2：杀掉阻塞的事务**
```sql
-- 如果是长期未提交的事务
KILL <blocking_thread_id>;

-- 重试 DDL
```

**方案 3：设置锁等待超时**
```sql
-- 设置 DDL 的锁等待超时
SET SESSION lock_wait_timeout = 60;  -- 60 秒

-- 执行 DDL
ALTER TABLE users ADD INDEX idx_email(email);
```

### 10.5 DDL 失败后的清理

**问题**：DDL 失败后，可能留下临时文件或中间状态

**检查**：
```sql
-- 查看临时表
SELECT * FROM information_schema.tables
WHERE table_name LIKE '%#sql%';

-- 查看临时文件
SHOW ENGINE INNODB STATUS\G  -- 查找 "TEMP" 相关信息
```

**清理**：
```sql
-- 如果有临时表残留
DROP TABLE IF EXISTS `#sql-ib123-456`;

-- 清理临时文件（重启 MySQL 会自动清理）
-- 或手动删除 innodb_temp/ 下的文件（需要停机）
```

---

## 十一、总结与最佳实践

### 11.1 INPLACE 算法的优势

✅ **并发性**：支持并发 DML（取决于具体操作）  
✅ **性能**：比 COPY 快得多  
✅ **空间效率**：No-Rebuild 模式不需要双倍空间  
✅ **灵活性**：支持大部分 DDL 操作

### 11.2 最佳实践

**1. 规划 DDL**
- 在业务低峰期执行
- 评估所需时间和空间
- 准备回滚方案

**2. 调优参数**
- `innodb_sort_buffer_size`：根据表大小调整
- `innodb_ddl_threads`：根据 CPU 核数调整
- `innodb_online_alter_log_max_size`：根据 DML 频率调整

**3. 明确指定算法和锁**
```sql
-- 明确要求 INPLACE + NONE 锁
ALTER TABLE t ADD INDEX idx(col), 
ALGORITHM=INPLACE, 
LOCK=NONE;
```

**4. 监控进度**
- 使用 Performance Schema
- 监控磁盘空间
- 监控 row log 大小

**5. 处理失败**
- 检查错误日志
- 清理临时文件
- 必要时重试

**6. 大表优化**
- 考虑使用 `pt-online-schema-change`
- 分批处理（如果可能）
- 提前测试

### 11.3 避免的陷阱

❌ 在高峰期执行大表 DDL  
❌ 忽略磁盘空间检查  
❌ 不监控 DDL 进度  
❌ 不测试直接在生产环境执行  
❌ 不了解操作是否需要 rebuild

### 11.4 进一步学习

- MySQL 官方文档：Online DDL Operations
- InnoDB 源码：`storage/innobase/handler/handler0alter.cc`
- Performance Schema：DDL 监控
- 相关工具：`pt-online-schema-change`, `gh-ost`

---

**文档版本**：1.0  
**创建日期**：2025-10-11  
**基于源码版本**：MySQL 8.0

---

## 附录：快速参考

### A.1 判断是否 Rebuild 的快速参考

| 操作 | Rebuild? | 原因 |
|------|---------|------|
| 添加二级索引 | ❌ | 只需构建新索引 |
| 删除二级索引 | ❌ | 只需删除索引 |
| 添加主键 | ✅ | 需要重新组织数据 |
| 删除主键 | ✅ | 需要重新组织数据 |
| 添加列（末尾） | ❌ | INSTANT（8.0.12+） |
| 添加列（中间） | ✅/❌ | 8.0.29+ 可能 INSTANT |
| 删除列 | ✅ | 需要重新组织数据 |
| 修改列类型（扩大） | ❌ | 仅元数据 |
| 修改列类型（缩小/改变） | ✅ | 需要转换数据 |
| 修改列顺序 | ✅ | 需要重新组织数据 |
| 重命名列 | ❌ | 仅元数据 |
| 修改字符集 | ✅ | 需要重新编码数据 |
| 修改行格式 | ✅ | 需要重新组织数据 |

### A.2 锁级别快速参考

| LOCK 参数 | 允许的操作 | 适用场景 |
|-----------|----------|---------|
| NONE | SELECT, INSERT, UPDATE, DELETE | 最大并发，首选 |
| SHARED | SELECT | 需要阻止写入 |
| EXCLUSIVE | 无 | 快速元数据修改 |

### A.3 关键参数快速参考

```sql
-- 排序缓冲区（影响临时文件大小）
innodb_sort_buffer_size = 64M

-- DDL 并行线程数（影响构建速度）
innodb_ddl_threads = 8

-- Row Log 最大大小（影响 DDL 失败概率）
innodb_online_alter_log_max_size = 512M

-- 临时目录（影响临时文件位置）
innodb_tmpdir = /path/to/temp

-- 锁等待超时（影响 DDL 等待时间）
lock_wait_timeout = 60
```

