# MySQL 8.0 Online DDL (Inplace) 执行流程详解

## 一、概述

Online DDL (Inplace) 是 MySQL 提供的一种高级表结构变更机制，允许在 DDL 操作执行过程中继续接受并发的 DML 操作（INSERT、UPDATE、DELETE），大大减少了表变更对业务的影响。本文档详细分析 MySQL 8.0 源码中 Online DDL 的实现原理。

### 1.1 核心思想

Online DDL 的核心思想是：
1. **并发执行**：在 DDL 执行期间，允许其他会话并发读写表
2. **日志记录**：通过 Row Log 记录 DDL 期间的并发 DML 操作
3. **最终合并**：DDL 主要操作完成后，在短暂的排他锁期间应用这些日志

### 1.2 支持的算法

MySQL 8.0 支持三种 ALTER TABLE 算法：
- **COPY**：传统的表拷贝方式，需要完全重建表
- **INPLACE**：在原表上就地修改，支持并发 DML
- **INSTANT**：即时完成，只修改元数据

本文档主要关注 **INPLACE** 算法的实现。

---

## 二、执行阶段概览

根据 `sql/handler.h` 中的注释（第 5999-6116 行），Online DDL (Inplace) 分为三个主要阶段：

### Phase 1: Initialization (初始化阶段)
- 确定使用的算法和并发级别
- 编译新旧表之间的差异（HA_ALTER_FLAGS）
- 检查存储引擎是否支持 inplace 操作

### Phase 2: Execution (执行阶段)
- 准备阶段：`prepare_inplace_alter_table()`
- 执行阶段：`inplace_alter_table()`
- 提交阶段：`commit_inplace_alter_table()`

### Phase 3: Final (收尾阶段)
- 更新数据字典
- 通知存储引擎
- 清理临时对象

---

## 三、关键数据结构

### 3.1 Alter_inplace_info

**文件位置**：`sql/handler.h` (第 3196-3600 行)

```cpp
class Alter_inplace_info {
  // 核心字段
  HA_CREATE_INFO *create_info;        // 建表信息
  Alter_info *alter_info;             // ALTER 操作信息
  KEY *key_info_buffer;               // 新的索引定义
  uint key_count;                     // 索引数量
  
  // 索引操作缓冲区
  KEY **index_drop_buffer;            // 要删除的索引
  uint *index_add_buffer;             // 要添加的索引
  uint index_drop_count;
  uint index_add_count;
  
  // 存储引擎上下文
  inplace_alter_handler_ctx *handler_ctx;
  
  // 操作标志位
  HA_ALTER_FLAGS handler_flags;       // 详细的操作类型
  bool online;                        // 是否为 online 操作
};
```

**HA_ALTER_FLAGS** 定义了各种 ALTER 操作类型：
- `ADD_INDEX`：添加普通索引
- `DROP_INDEX`：删除索引
- `ADD_UNIQUE_INDEX`：添加唯一索引
- `ADD_PK_INDEX`：添加主键
- `ADD_COLUMN`：添加列
- `DROP_COLUMN`：删除列
- 等等...

### 3.2 ha_innobase_inplace_ctx

**文件位置**：`storage/innobase/handler/handler0alter.cc` (第 173-250 行)

InnoDB 的 inplace 上下文，继承自 `inplace_alter_handler_ctx`：

```cpp
struct ha_innobase_inplace_ctx : public inplace_alter_handler_ctx {
  que_thr_t *thr;                   // 查询线程
  row_prebuilt_t *prebuilt;         // 预构建结构
  
  // 索引操作相关
  dict_index_t **add_index;         // 要创建的索引数组
  ulint num_to_add_index;           // 创建索引数量
  dict_index_t **drop_index;        // 要删除的索引数组
  ulint num_to_drop_index;          // 删除索引数量
  
  // 表重建相关
  dict_table_t *old_table;          // 原表
  dict_table_t *new_table;          // 新表（如果需要重建）
  const ulint *col_map;             // 列映射
  const dtuple_t *add_cols;         // 添加的列
  
  // Online 相关
  bool online;                      // 是否 online 操作
  trx_t *trx;                       // 事务
  mem_heap_t *heap;                 // 内存堆
};
```

---

## 四、详细执行流程

### 4.1 入口函数

**函数**：`mysql_alter_table()`  
**文件**：`sql/sql_table.cc`

这是 ALTER TABLE 的主入口函数，它会：
1. 解析 ALTER 语句
2. 确定使用哪种算法（COPY/INPLACE/INSTANT）
3. 调用相应的执行函数

### 4.2 Phase 1: 初始化阶段

#### 4.2.1 填充 Alter_inplace_info

**函数**：`fill_alter_inplace_info()`  
**文件**：`sql/sql_table.cc` (第 11930-12800 行)

**主要职责**：
1. 比较新旧表定义
2. 设置 `HA_ALTER_FLAGS` 标志位
3. 填充索引添加/删除缓冲区

**核心逻辑**：
```cpp
// 检测列的变化
for (field in old_table->fields) {
  if (column_is_renamed()) {
    ha_alter_info->handler_flags |= ALTER_COLUMN_NAME;
  }
  if (column_is_dropped()) {
    ha_alter_info->handler_flags |= DROP_COLUMN;
  }
  // ... 其他检测
}

// 检测索引的变化
for (key in new_key_info) {
  if (key_is_new()) {
    // 添加到 index_add_buffer
  }
}
for (key in old_key_info) {
  if (key_should_be_dropped()) {
    // 添加到 index_drop_buffer
  }
}
```

#### 4.2.2 检查存储引擎支持

**函数**：`handler::check_if_supported_inplace_alter()`  
**文件**：`sql/handler.h` (第 6159 行声明)  
**InnoDB 实现**：`storage/innobase/handler/handler0alter.cc`

**返回值**：
- `HA_ALTER_INPLACE_NOT_SUPPORTED`：不支持，必须使用 COPY
- `HA_ALTER_INPLACE_EXCLUSIVE_LOCK`：支持，但需要排他锁
- `HA_ALTER_INPLACE_SHARED_LOCK`：支持，需要共享写锁（SNW）
- `HA_ALTER_INPLACE_NO_LOCK`：支持，允许并发读写
- `HA_ALTER_INPLACE_INSTANT`：支持即时算法

**InnoDB 的判断逻辑**：
```cpp
// 需要重建表的操作
if (handler_flags & INNOBASE_ALTER_REBUILD) {
  return check_if_need_rebuild();
}

// 仅创建索引的操作
if (handler_flags & INNOBASE_ONLINE_CREATE) {
  // 可以 online 创建
  return HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE;
}
```

### 4.3 Phase 2: 执行阶段

#### 4.3.1 准备阶段 - prepare_inplace_alter_table()

**函数**：`ha_innobase::prepare_inplace_alter_table()`  
**文件**：`storage/innobase/handler/handler0alter.cc`

**主要职责**：
1. 创建 `ha_innobase_inplace_ctx` 上下文
2. 为新索引分配数据结构
3. 为 online 操作分配 Row Log

**关键步骤**：

##### 1) 创建新索引结构
```cpp
// 为每个新索引创建 dict_index_t
for (i = 0; i < n_add; i++) {
  dict_index_t *index = dict_mem_index_create(...);
  // 添加索引字段
  for (field in index_fields) {
    index->add_field(...);
  }
}
```

##### 2) 分配 Row Log（关键！）

**Row Log** 是 Online DDL 的核心机制，用于记录 DDL 期间的并发 DML 操作。

**函数**：`row_log_allocate()`  
**文件**：`storage/innobase/row/row0log.cc`

```cpp
// 为二级索引分配 row log
if (ctx->online && is_secondary_index) {
  rw_lock_x_lock(&index->lock);
  row_log_allocate(index, NULL, true, NULL, NULL, path);
  rw_lock_x_unlock(&index->lock);
}

// 为聚簇索引（表重建）分配 row log
if (ctx->need_rebuild() && ctx->online) {
  dict_index_t *clust_index = table->first_index();
  rw_lock_x_lock(&clust_index->lock);
  row_log_allocate(clust_index, ctx->new_table, 
                   true, ctx->add_cols, ctx->col_map, path);
  rw_lock_x_unlock(&clust_index->lock);
}
```

**Row Log 结构**：
- 每个正在创建的索引都有一个 row log
- Row log 记录该索引上的 INSERT/UPDATE/DELETE 操作
- 使用循环缓冲区，必要时写入磁盘临时文件

##### 3) 分配一致性读视图
```cpp
if (ctx->online) {
  // 为索引构建扫描分配一致性读视图
  trx_assign_read_view(ctx->prebuilt->trx);
}
```

#### 4.3.2 执行阶段 - inplace_alter_table()

**函数**：`ha_innobase::inplace_alter_table()`  
**文件**：`storage/innobase/handler/handler0alter.cc` (第 6117-6400 行)

**主要职责**：
1. 扫描原表（聚簇索引）
2. 构建新索引
3. 在此期间，并发 DML 被记录到 Row Log

**核心流程**：

##### 1) 创建 DDL Context
```cpp
ddl::Context ddl_ctx(trx, old_table, new_table, 
                     online, indexes, col_map, ...);
```

##### 2) 扫描并构建索引

**类**：`ddl::Loader`  
**文件**：`storage/innobase/ddl/ddl0loader.cc`

```cpp
// Loader::scan_and_build_indexes()
dberr_t Loader::scan_and_build_indexes() {
  // 创建游标扫描聚簇索引
  auto cursor = Cursor::create_cursor(m_ctx);
  
  // 扫描聚簇索引，为每个索引构建器提供数据
  err = cursor->scan(m_builders);
  
  // 加载（排序和构建索引）
  if (err == DB_SUCCESS) {
    err = load();
  }
}
```

**Builder 机制**：
- 为每个新索引创建一个 `ddl::Builder`
- Builder 从游标读取记录，提取索引键值
- 将键值写入临时排序文件
- 最后进行归并排序并批量插入 B-tree

**并发 DML 的处理**：
- 在扫描期间，其他会话可以执行 DML
- DML 操作被拦截并记录到 Row Log

**拦截点**：`row_ins_sec_index_entry()`, `row_upd_sec_index_entry()`  
**文件**：`storage/innobase/row/row0ins.cc`, `storage/innobase/row/row0upd.cc`

```cpp
// 在插入/更新/删除二级索引时
if (dict_index_get_online_status(index) == ONLINE_INDEX_CREATION) {
  // 记录操作到 row log
  row_log_online_op(index, entry, trx_id);
  // 不实际修改索引
  return;
}
```

##### 3) 索引构建的并行化

**文件**：`storage/innobase/ddl/ddl0par-scan.cc`

InnoDB 支持并行扫描和构建索引：
```cpp
// 使用多个线程并行扫描
for (i = 0; i < n_threads; i++) {
  // 每个线程扫描聚簇索引的一部分
  thread_ctx[i]->scan_range(...);
}
```

#### 4.3.3 提交阶段 - commit_inplace_alter_table()

**函数**：`ha_innobase::commit_inplace_alter_table()`  
**文件**：`storage/innobase/handler/handler0alter.cc` (第 1587-1700 行)

**主要职责**：
1. 应用 Row Log 中记录的并发 DML
2. 交换新旧索引/表定义
3. 更新数据字典

**核心步骤**：

##### 1) 应用 Row Log（最关键！）

**函数**：`row_log_apply()`  
**文件**：`storage/innobase/row/row0log.cc`

对于**二级索引**：
```cpp
// row_log_apply_ops()
dberr_t row_log_apply_ops(trx_t *trx, dict_index_t *index, 
                          ddl::Dup *dup, Alter_stage *stage) {
  // 读取 row log 中的每一条记录
  while (has_more_log_records) {
    mrec = read_next_log_record();
    
    switch (mrec->op_type) {
      case ROW_OP_INSERT:
        // 插入索引记录
        row_ins_sec_index_entry_low(index, entry, ...);
        break;
      case ROW_OP_DELETE:
        // 删除索引记录
        row_search_index_entry(index, entry, ...);
        btr_cur_del_mark_set_sec_rec(...);
        break;
    }
  }
}
```

对于**表重建**（聚簇索引）：
```cpp
// row_log_table_apply()
dberr_t row_log_table_apply_ops(que_thr_t *thr, ddl::Dup *dup, 
                                Alter_stage *stage) {
  // 应用聚簇索引的 DML 日志
  while (has_more_log_records) {
    mrec = read_next_log_record();
    
    switch (mrec->op_type) {
      case ROW_OP_INSERT:
        // 插入到新表
        row_ins_clust_index_entry_low(new_index, entry, ...);
        break;
      case ROW_OP_DELETE:
        // 从新表删除
        row_search_on_row_ref(entry, ...);
        row_update_for_mysql(...);
        break;
    }
  }
}
```

**注意**：
- 此时必须持有排他锁（EXCLUSIVE），确保没有新的 DML
- Row Log 可能非常大，需要分批应用
- 如果发现重复键，返回错误并回滚

##### 2) 交换索引定义
```cpp
// 删除旧索引
for (i = 0; i < ctx->num_to_drop_index; i++) {
  dict_index_remove_from_cache(table, ctx->drop_index[i]);
}

// 提交新索引
for (i = 0; i < ctx->num_to_add_index; i++) {
  dict_index_set_online_status(ctx->add_index[i], ONLINE_INDEX_COMPLETE);
  ctx->add_index[i]->set_committed(true);
}
```

##### 3) 更新数据字典
```cpp
// 对于支持 Atomic DDL 的引擎（InnoDB）
dd_commit_inplace_alter_table(ctx, old_table_def, new_table_def);
```

### 4.4 Phase 3: 收尾阶段

**函数**：`mysql_inplace_alter_table()` 的后续部分  
**文件**：`sql/sql_table.cc` (第 13620-13800 行)

**主要步骤**：

1. **更新 SQL 层数据字典**：
```cpp
// 删除旧表定义
thd->dd_client()->drop(table_def);

// 存储/更新新表定义
if (db_type->flags & HTON_SUPPORTS_ATOMIC_DDL) {
  thd->dd_client()->store(altered_table_def);
} else {
  thd->dd_client()->update(altered_table_def);
}
```

2. **提交事务**：
```cpp
if (db_type->flags & HTON_SUPPORTS_ATOMIC_DDL) {
  // Atomic DDL：最后统一提交
  trans_commit_stmt(thd);
  trans_commit_implicit(thd);
  
  // 调用 post_ddl hook
  db_type->post_ddl(thd);
} else {
  // 非 Atomic DDL：立即提交
  trans_commit_implicit(thd);
}
```

3. **通知存储引擎**：
```cpp
table->file->ha_notify_table_changed(ha_alter_info);
```

4. **清理资源**：
```cpp
// 销毁 context
destroy(ha_alter_info->handler_ctx);
```

---

## 五、Row Log 机制详解

Row Log 是 Online DDL 的核心机制，值得单独详细说明。

### 5.1 Row Log 的作用

在 Online DDL 执行期间：
- 允许并发 DML（INSERT/UPDATE/DELETE）
- 这些 DML 操作不能直接修改正在构建的索引（因为索引还不完整）
- Row Log 记录下这些操作，等索引构建完成后再应用

### 5.2 Row Log 的结构

**文件**：`storage/innobase/include/row0log.h`

```cpp
struct row_log_t {
  // 内存缓冲区
  struct {
    byte *buf;              // 缓冲区
    ulint bytes;            // 已使用字节数
    os_offset_t blocks;     // 已写入磁盘的块数
  } tail;
  
  // 磁盘临时文件
  pfs_os_file_t fd;         // 文件描述符
  os_offset_t head_offset;  // 读取偏移
  
  // 其他
  mutex_t mutex;            // 保护 row log 的互斥锁
  trx_id_t max_trx;         // 最大事务 ID
  dict_table_t *table;      // 关联的表
  dberr_t error;            // 错误状态
};
```

### 5.3 记录 DML 到 Row Log

**函数**：`row_log_online_op()`  
**文件**：`storage/innobase/row/row0log.cc` (第 283-400 行)

```cpp
void row_log_online_op(dict_index_t *index, const dtuple_t *tuple, 
                       trx_id_t trx_id) {
  // trx_id != 0: INSERT
  // trx_id == 0: DELETE
  
  row_log_t *log = index->online_log;
  
  // 获取 log 的互斥锁
  mutex_enter(&log->mutex);
  
  // 序列化操作记录
  if (trx_id != 0) {
    *b++ = ROW_OP_INSERT;
    trx_write_trx_id(b, trx_id);
  } else {
    *b++ = ROW_OP_DELETE;
  }
  
  // 写入索引记录
  rec_serialize_dtuple(b, index, tuple->fields, tuple->n_fields, ...);
  
  // 如果内存缓冲区满了，写入磁盘
  if (log->tail.bytes >= srv_sort_buf_size) {
    row_log_block_write(log);
  }
  
  mutex_exit(&log->mutex);
}
```

### 5.4 UPDATE 操作的特殊处理

UPDATE 操作被分解为 DELETE + INSERT：
```cpp
// 在 row_upd_sec_index_entry() 中
if (dict_index_get_online_status(index) == ONLINE_INDEX_CREATION) {
  // 1. 记录 DELETE（旧值）
  row_log_online_op(index, old_entry, 0);
  
  // 2. 记录 INSERT（新值）
  row_log_online_op(index, new_entry, trx->id);
}
```

### 5.5 应用 Row Log

**函数**：`row_log_apply()`  
**调用时机**：在 `commit_inplace_alter_table()` 中

**流程**：
1. 升级到排他锁，阻止新的 DML
2. 从 row log 读取记录
3. 应用每条记录（INSERT/DELETE）
4. 处理可能的重复键冲突
5. 释放锁

**伪代码**：
```cpp
row_log_apply() {
  // 1. 获取排他锁
  rw_lock_x_lock(index->lock);
  
  // 2. 批量应用日志
  while (has_more_log) {
    // 读取一批日志记录
    records = read_log_batch();
    
    for (rec in records) {
      if (rec.op == INSERT) {
        // 插入索引记录
        insert_into_index(index, rec.data);
      } else if (rec.op == DELETE) {
        // 删除索引记录
        delete_from_index(index, rec.data);
      }
    }
    
    // 检查是否被中断
    if (trx_is_interrupted(trx)) {
      return DB_INTERRUPTED;
    }
  }
  
  // 3. 释放锁
  rw_lock_x_unlock(index->lock);
}
```

---

## 六、锁机制与并发控制

### 6.1 MDL 锁的使用

MySQL 使用 Metadata Lock (MDL) 来保护表结构：

**锁级别**：
- `MDL_SHARED_UPGRADABLE`：允许读写，但可以升级
- `MDL_SHARED_NO_WRITE`：允许读，不允许写
- `MDL_EXCLUSIVE`：排他锁

**Online DDL 的锁策略**：
```cpp
// 初始：MDL_SHARED_UPGRADABLE
// 允许并发读写

// prepare 阶段（如果需要）：升级到 MDL_EXCLUSIVE
// 创建索引结构、分配 row log

// 主要执行阶段：降级到 MDL_SHARED_UPGRADABLE 或 MDL_SHARED_NO_WRITE
// 扫描表、构建索引、记录并发 DML

// commit 阶段：升级到 MDL_EXCLUSIVE
// 应用 row log、交换索引、更新数据字典
```

### 6.2 InnoDB 内部锁

**文件**：`storage/innobase/include/dict0dict.h`

```cpp
// 索引锁（rw_lock）
rw_lock_t index->lock;

// 使用场景：
// - S-lock: 读取索引、记录 row log
// - X-lock: 应用 row log、删除索引
```

---

## 七、关键函数调用链

### 7.1 完整调用链

```
mysql_alter_table()                                 [sql/sql_table.cc]
  └─ mysql_inplace_alter_table()                    [sql/sql_table.cc:13329]
      ├─ handler::check_if_supported_inplace_alter() [sql/handler.h:6159]
      │   └─ ha_innobase::check_if_supported_inplace_alter() [handler0alter.cc]
      │
      ├─ handler::ha_prepare_inplace_alter_table()   [sql/handler.cc]
      │   └─ ha_innobase::prepare_inplace_alter_table() [handler0alter.cc]
      │       ├─ prepare_inplace_add_virtual()
      │       ├─ prepare_inplace_drop_virtual()
      │       ├─ row_log_allocate()                  [row/row0log.cc]
      │       └─ trx_assign_read_view()
      │
      ├─ handler::ha_inplace_alter_table()           [sql/handler.h:6175]
      │   └─ ha_innobase::inplace_alter_table()      [handler0alter.cc:6117]
      │       └─ ddl::Loader::build_all()            [ddl/ddl0loader.cc:477]
      │           ├─ prepare()                       [ddl/ddl0loader.cc:366]
      │           └─ scan_and_build_indexes()        [ddl/ddl0loader.cc:398]
      │               ├─ Cursor::scan()              [ddl/ddl0ddl.cc]
      │               │   └─ Builder::add_row()      [ddl/ddl0builder.cc]
      │               │       └─ 写入排序文件
      │               └─ load()
      │                   └─ Builder::finish()
      │                       └─ 归并排序并批量插入 B-tree
      │
      └─ handler::ha_commit_inplace_alter_table()    [sql/handler.cc]
          └─ ha_innobase::commit_inplace_alter_table() [handler0alter.cc:1587]
              ├─ row_log_table_apply()               [row/row0log.cc] (如果重建表)
              ├─ row_log_apply()                     [row/row0log.cc] (应用索引 log)
              ├─ commit_cache_rebuild() 或 commit_cache_norebuild()
              └─ dd_commit_inplace_alter_table()
```

### 7.2 并发 DML 的拦截调用链

```
// INSERT 操作
ha_innobase::write_row()
  └─ row_insert_for_mysql()
      └─ row_ins_step()
          └─ row_ins_sec_index_entry()              [row/row0ins.cc]
              └─ if (online_ddl) row_log_online_op() [row/row0log.cc:283]

// UPDATE 操作
ha_innobase::update_row()
  └─ row_update_for_mysql()
      └─ row_upd_step()
          └─ row_upd_sec_index_entry()              [row/row0upd.cc]
              └─ if (online_ddl) row_log_online_op() * 2 (DELETE + INSERT)

// DELETE 操作
ha_innobase::delete_row()
  └─ row_update_for_mysql() // DELETE 实现为 UPDATE (delete-mark)
      └─ row_upd_step()
          └─ row_upd_sec_index_entry()
              └─ if (online_ddl) row_log_online_op() [row/row0log.cc]
```

---

## 八、重要源码文件索引

### 8.1 SQL 层

| 文件 | 主要内容 |
|------|---------|
| `sql/handler.h` | Handler 接口定义、Alter_inplace_info 结构 |
| `sql/handler.cc` | Handler 通用实现 |
| `sql/sql_table.cc` | ALTER TABLE 主要实现，包括 mysql_alter_table() |
| `sql/sql_alter.h` | Alter_info 结构定义 |

### 8.2 InnoDB Handler 层

| 文件 | 主要内容 |
|------|---------|
| `storage/innobase/handler/handler0alter.cc` | InnoDB Online DDL 主要实现 (11000+ 行) |
| `storage/innobase/handler/handler0alter.h` | 相关头文件 |
| `storage/innobase/handler/ha_innodb.h` | ha_innobase 类定义 |

### 8.3 InnoDB DDL 模块

| 文件 | 主要内容 |
|------|---------|
| `storage/innobase/ddl/ddl0ddl.cc` | DDL 通用功能、索引创建 |
| `storage/innobase/ddl/ddl0loader.cc` | Loader 类：扫描和构建索引的主控制器 |
| `storage/innobase/ddl/ddl0builder.cc` | Builder 类：单个索引的构建器 |
| `storage/innobase/ddl/ddl0ctx.cc` | DDL Context 上下文管理 |
| `storage/innobase/ddl/ddl0par-scan.cc` | 并行扫描实现 |
| `storage/innobase/ddl/ddl0merge.cc` | 归并排序实现 |
| `storage/innobase/include/ddl0ddl.h` | DDL 模块头文件 |

### 8.4 InnoDB Row Log

| 文件 | 主要内容 |
|------|---------|
| `storage/innobase/row/row0log.cc` | Row Log 核心实现 (4000+ 行) |
| `storage/innobase/include/row0log.h` | Row Log 数据结构定义 |

### 8.5 InnoDB 其他相关

| 文件 | 主要内容 |
|------|---------|
| `storage/innobase/row/row0ins.cc` | 插入操作，包含 row log 拦截点 |
| `storage/innobase/row/row0upd.cc` | 更新操作，包含 row log 拦截点 |
| `storage/innobase/dict/dict0dict.cc` | 数据字典操作 |
| `storage/innobase/dict/dict0inst.cc` | Instant DDL 实现 |

---

## 九、性能优化要点

### 9.1 减少 Row Log 大小

**问题**：如果 DDL 执行时间很长，row log 会非常大，导致：
- 占用大量磁盘空间
- commit 阶段应用 log 时间很长（持有排他锁）

**优化建议**：
1. 在业务低峰期执行 DDL
2. 使用 `innodb_online_alter_log_max_size` 限制 row log 大小
3. 如果 row log 超限，DDL 会失败回滚

### 9.2 并行构建索引

InnoDB 8.0 支持并行扫描和构建索引：
- `innodb_ddl_threads`：控制并行线程数
- `innodb_ddl_buffer_size`：每个线程的缓冲区大小

### 9.3 排序缓冲区

- `innodb_sort_buffer_size`：控制排序缓冲区大小
- 更大的缓冲区可以减少磁盘 I/O

---

## 十、常见问题与注意事项

### 10.1 哪些操作支持 Online？

**支持 Online 且无锁（NO_LOCK）**：
- 添加/删除二级索引（非主键）
- 添加虚拟列
- 更改列默认值
- 重命名列
- 修改 AUTO_INCREMENT 值

**支持 Online 但需要锁（SHARED_LOCK）**：
- 添加/删除全文索引
- 添加空间索引

**不支持 Online（需要 EXCLUSIVE_LOCK 或 COPY）**：
- 修改列类型
- 添加/删除主键（取决于情况）
- 更改表的存储引擎
- 更改行格式

### 10.2 Instant DDL vs Inplace DDL

**Instant DDL**（8.0.12+ 支持）：
- 只修改元数据，不触碰数据
- 几乎瞬间完成
- 支持的操作有限（如在表末尾添加列）

**Inplace DDL**：
- 需要扫描和重建索引/表
- 耗时取决于表大小
- 支持更多操作

### 10.3 失败和回滚

如果 Online DDL 失败：
1. **prepare 阶段失败**：不会有任何修改
2. **inplace 阶段失败**：
   - 已创建的临时数据会被清理
   - 回滚到原始状态
3. **commit 阶段失败**：
   - 对于 Atomic DDL（InnoDB）：完全回滚
   - 对于非 Atomic DDL：可能处于不一致状态

### 10.4 监控 Online DDL

**Performance Schema**：
```sql
-- 查看 DDL 进度
SELECT * FROM performance_schema.events_stages_current
WHERE EVENT_NAME LIKE 'stage/innodb/alter%';

-- 查看 row log 大小
SHOW ENGINE INNODB STATUS;  -- 查看 "ONLINE DDL" 部分
```

**重要指标**：
- `Innodb_online_ddl_pending`：正在执行的 online DDL 数量
- `Innodb_online_ddl_*`：各类 online DDL 统计

---

## 十一、总结

MySQL 8.0 的 Online DDL (Inplace) 实现是一个复杂而精巧的系统：

1. **分层设计**：SQL 层定义接口，存储引擎层实现具体逻辑
2. **Row Log 机制**：核心创新，使得在索引构建期间可以并发执行 DML
3. **三阶段提交**：Prepare - Execute - Commit，确保原子性
4. **并发控制**：通过 MDL 锁和 InnoDB 内部锁实现精细的并发控制
5. **性能优化**：并行扫描、外部排序、批量插入等优化技术

理解这些原理对于：
- 优化 DDL 执行性能
- 避免 DDL 对业务的影响
- 排查 DDL 相关问题

都非常有帮助。

---

## 十二、参考资料

1. MySQL 8.0 源码：`sql/handler.h` 中的详细注释（第 5999-6116 行）
2. InnoDB Online DDL 官方文档
3. `handler0alter.cc` 文件头部注释
4. WL#5534: Online ALTER, phase 1
5. WL#6555: Online ALTER, phase 2

---

**文档版本**：1.0  
**创建日期**：2025-10-11  
**基于源码版本**：MySQL 8.0

