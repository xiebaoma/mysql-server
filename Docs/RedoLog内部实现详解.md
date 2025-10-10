# InnoDB Redo Log 内部实现详解

## 一、Redo Log架构概览

### 1.1 整体架构图

```
┌──────────────────────────────────────────────────────────────────┐
│                      用户线程 (执行INSERT/UPDATE等)                 │
└────────┬─────────────────────────────────────────────────────────┘
         │
         ├─ mtr_start()    开始mini-transaction
         ├─ 修改页面...    (自动记录redo log到mtr buffer)
         └─ mtr_commit()   提交mtr
               │
               ├─ Step 1: 预留LSN空间
               │    └─ log_buffer_reserve()
               │         └─ sn.fetch_add(len) → 获取LSN范围
               │
               ├─ Step 2: 写入Log Buffer
               │    └─ log_buffer_write()
               │         └─ memcpy(log_sys->buf + offset, data, len)
               │
               ├─ Step 3: 标记脏页
               │    └─ add_dirty_blocks_to_flush_list()
               │
               └─ Step 4: 关闭预留空间
                    └─ log_buffer_close()
                         │
                         └─ 唤醒后台线程
                               │
       ┌───────────────────────┴───────────────────────┐
       │                                               │
   ┌───▼───┐                                      ┌───▼────┐
   │Writer │ log_writer线程                        │Flusher │ log_flusher线程
   └───┬───┘                                      └───┬────┘
       │                                              │
       ├─ write(fd, buf, size)                       ├─ fsync(fd)
       │  写入OS缓存                                  │  持久化到磁盘
       │                                              │
       ├─ update write_lsn                           ├─ update flushed_to_disk_lsn
       │                                              │
       └─ notify_write_complete                      └─ notify_flush_complete
               │                                              │
               └───────────┬──────────────────────────────────┘
                           │
                   唤醒等待的用户线程
                   (如COMMIT时等待刷盘)
```

### 1.2 LSN (Log Sequence Number) 详解

**LSN是什么？**

- 全局递增的64位整数
- 代表redo log中的字节偏移量
- 从0开始，永不回绕

**多个LSN的含义：**

```
Timeline: ─────────────────────────────────────────────────▶
LSN:      0    100   200   300   400   500   600   700   800

标记点:
  checkpoint_lsn: 100           最后checkpoint的位置
    └─ 之前的redo log可以删除，因为对应的脏页已刷盘
  
  write_lsn: 500                已写入文件的位置
    └─ 500之前的log在文件中（但可能在OS缓存）
  
  flushed_to_disk_lsn: 400      已fsync到磁盘的位置
    └─ 400之前的log真正持久化
  
  current_lsn: 700              当前最新LSN
    └─ 700之前的log在log buffer中
  
  sn: 800                       下一个可分配的SN
    └─ 新mtr会从800开始分配
```

## 二、Log Buffer实现细节

### 2.1 Log Buffer结构

**文件**: `storage/innobase/include/log0sys.h`

```cpp
struct log_t {
  // 主要的log buffer
  aligned_array_pointer<byte, OS_FILE_LOG_BLOCK_SIZE> buf;
  size_t buf_size;  // 默认16MB
  
  // 最近关闭的区间（用于并发控制）
  Link_buf<lsn_t> recent_closed;
  
  // 最近写入的区间
  Link_buf<lsn_t> recent_written;
  
  // SN和LSN的转换
  std::atomic<lsn_t> sn;           // 下一个可用的SN (Sequential Number)
  lsn_t sn_locked;                 // 加锁保护的SN（用于分配）
  
  // 状态LSN
  std::atomic<lsn_t> write_lsn;    // 已写入文件
  std::atomic<lsn_t> flushed_to_disk_lsn;  // 已刷盘
  std::atomic<lsn_t> buf_ready_for_write_lsn;  // 可写入文件
  
  // ... 其他成员
};
```

### 2.2 SN vs LSN

**SN (Sequential Number)**：

- 只计算实际数据字节
- 不包括log block的header和trailer

**LSN (Log Sequence Number)**：

- 包括所有字节（数据 + header + trailer）
- 实际文件中的字节偏移

**转换关系**：

```
每512字节的log block结构:
  [12字节header] + [496字节数据] + [4字节trailer]

SN计算:   只计数496字节数据
LSN计算:  计数整个512字节

示例:
  SN=1000  →  LSN=1024  (约等于 SN * 512/496)
```

### 2.3 Log Buffer分配算法

**文件**: `storage/innobase/log/log0buf.cc`

```cpp
Log_handle log_buffer_reserve(log_t &log, size_t len) {
  lsn_t start_sn;
  lsn_t end_sn;
  
  // 原子地分配SN范围
  start_sn = log.sn.fetch_add(len, std::memory_order_relaxed);
  end_sn = start_sn + len;
  
  // 转换为LSN
  lsn_t start_lsn = log_translate_sn_to_lsn(start_sn);
  lsn_t end_lsn = log_translate_sn_to_lsn(end_sn);
  
  // 等待log buffer有足够空间
  // (确保 end_lsn - flushed_to_disk_lsn < buf_size)
  while (!log_buffer_has_space(log, end_lsn)) {
    log_buffer_wait_for_space(log, end_lsn);
  }
  
  // 记录到recent_closed（用于追踪哪些区间已完成写入）
  log.recent_closed.add_link(start_lsn, end_lsn);
  
  return {start_lsn, end_lsn};
}
```

### 2.4 并发写入Log Buffer

**问题**: 多个mtr同时提交，如何保证写入顺序？

**解决方案**: Link Buffer机制

```cpp
// Link_buf是一个链表结构，追踪LSN区间
template <typename T>
class Link_buf {
  struct Slot {
    std::atomic<T> value;   // 区间的结束位置
    std::atomic<T> next;    // 下一个区间的结束位置
  };
  
  std::vector<Slot> m_slots;
  std::atomic<T> m_tail;    // 当前已完成的最大LSN
};

// 添加一个区间
void add_link(lsn_t start, lsn_t end) {
  size_t slot_index = start % m_slots.size();
  
  // 写入这个区间的结束位置
  m_slots[slot_index].value.store(end);
  
  // 尝试推进m_tail
  advance_tail();
}

// 推进m_tail到连续完成的位置
void advance_tail() {
  while (true) {
    lsn_t current_tail = m_tail.load();
    size_t slot_index = current_tail % m_slots.size();
    lsn_t slot_value = m_slots[slot_index].value.load();
    
    if (slot_value == current_tail) {
      // 这个slot还未完成
      break;
    }
    
    // 推进tail
    if (m_tail.compare_exchange_strong(current_tail, slot_value)) {
      // 成功推进
      continue;
    }
  }
}
```

**示例**：

```
时间  mtr1          mtr2          mtr3          m_tail
t0    预留[0,100]   -             -             0
t1    写入中...     预留[100,150] -             0
t2    -             写入中...     预留[150,200] 0
t3    -             完成          -             0  (mtr1未完成)
t4    完成          -             写入中...     150 (推进到150)
t5    -             -             完成          200 (推进到200)
```

## 三、Redo Log文件格式

### 3.1 文件布局

```
redo log文件(ib_logfile0):

┌────────────────────────────────────────────────────────────┐
│  Log File Header (4 blocks = 2KB)                          │
│  - LOG_HEADER_FORMAT (format version)                      │
│  - LOG_HEADER_START_LSN (文件起始LSN)                       │
│  - LOG_HEADER_CREATOR (创建者信息)                          │
│  - Checksum                                                 │
├────────────────────────────────────────────────────────────┤
│                                                             │
│  Log Blocks (512 bytes each)                               │
│                                                             │
│  每个block结构:                                              │
│  ┌──────────────────────────────┐                          │
│  │ Block Header (12 bytes)      │                          │
│  │  - Block Number (4 bytes)    │                          │
│  │  - Data Length (2 bytes)     │                          │
│  │  - First Record Offset       │                          │
│  │  - Checkpoint Number         │                          │
│  ├──────────────────────────────┤                          │
│  │ Log Records (496 bytes)      │                          │
│  │  - Type                       │                          │
│  │  - Space ID                   │                          │
│  │  - Page Number                │                          │
│  │  - Data                       │                          │
│  ├──────────────────────────────┤                          │
│  │ Block Trailer (4 bytes)      │                          │
│  │  - Checksum                   │                          │
│  └──────────────────────────────┘                          │
│                                                             │
│  ... more blocks ...                                        │
│                                                             │
└────────────────────────────────────────────────────────────┘
```

### 3.2 Log Record格式

**通用格式**：

```
┌─────┬──────────┬───────────┬────────┬──────────┐
│Type │ Space ID │ Page No   │ Length │ Data     │
│1byte│ varint   │ varint    │ varint │ variable │
└─────┴──────────┴───────────┴────────┴──────────┘
```

**常见类型的详细格式**：

#### MLOG_REC_INSERT (插入记录)
```
Type: MLOG_REC_INSERT (9)
Space ID: 表空间ID
Page No: 页号
Data:
  ├─ Offset in page (2 bytes)    记录在页面中的偏移
  ├─ Record size (2 bytes)        记录大小
  └─ Record data (variable)       记录内容
```

#### MLOG_REC_UPDATE_IN_PLACE (原地更新)
```
Type: MLOG_REC_UPDATE_IN_PLACE (13)
Space ID: 表空间ID
Page No: 页号
Data:
  ├─ Offset in page (2 bytes)
  ├─ Update vector:
  │   ├─ Field number (2 bytes)
  │   ├─ Field length (2 bytes)
  │   └─ New field value (variable)
  └─ ... more fields
```

#### MLOG_REC_DELETE (删除记录)
```
Type: MLOG_REC_DELETE (14)
Space ID: 表空间ID
Page No: 页号
Data:
  └─ Offset in page (2 bytes)    要删除的记录偏移
```

### 3.3 varint编码

为了节省空间，Space ID、Page No等使用varint编码：

```cpp
// 写入varint
ulint mlog_write_varint(byte *ptr, ulint val) {
  if (val < 0x80) {
    // 1字节: 0xxxxxxx
    *ptr = (byte)val;
    return 1;
  } else if (val < 0x4000) {
    // 2字节: 10xxxxxx xxxxxxxx
    *ptr++ = (byte)(0x80 | (val >> 8));
    *ptr = (byte)(val & 0xFF);
    return 2;
  } else if (val < 0x200000) {
    // 3字节: 110xxxxx xxxxxxxx xxxxxxxx
    *ptr++ = (byte)(0xC0 | (val >> 16));
    *ptr++ = (byte)((val >> 8) & 0xFF);
    *ptr = (byte)(val & 0xFF);
    return 3;
  }
  // ... 更长的编码
}
```

## 四、关键函数实现分析

### 4.1 mtr_commit() 完整实现

**文件**: `storage/innobase/mtr/mtr0mtr.cc:842`

```cpp
void mtr_t::Command::execute() {
  ut_ad(m_impl->m_log_mode != MTR_LOG_NONE);
  
  // ========== 步骤1: 准备写入 ==========
  ulint len = prepare_write();
  // prepare_write()做什么？
  // 1. 遍历m_impl->m_log的所有block
  // 2. 为每个log record添加space_id和page_no
  // 3. 计算总长度
  
  if (len > 0) {
    // ========== 步骤2: 预留LSN空间 ==========
    auto handle = log_buffer_reserve(*log_sys, len);
    // 返回 {start_lsn, end_lsn}
    
    // ========== 步骤3: 写入Log Buffer ==========
    mtr_write_log_t write_log;
    write_log.m_left_to_write = len;
    write_log.m_handle = handle;
    write_log.m_lsn = handle.start_lsn;
    
    // 遍历mtr的log buffer，逐block写入
    m_impl->m_log.for_each_block(write_log);
    // write_log是一个functor:
    // bool operator()(const mtr_buf_t::block_t *block) {
    //   log_buffer_write(*log_sys, block->begin(), 
    //                    block->used(), m_lsn);
    //   m_lsn += block->used();
    //   return true;
    // }
    
    ut_ad(write_log.m_left_to_write == 0);
    ut_ad(write_log.m_lsn == handle.end_lsn);
    
    // ========== 步骤4: 等待log buffer空间 ==========
    log_wait_for_space_in_log_recent_closed(*log_sys, handle.start_lsn);
    // 确保log buffer不会溢出
    
    // ========== 步骤5: 标记脏页 ==========
    add_dirty_blocks_to_flush_list(handle.start_lsn, handle.end_lsn);
    // 将mtr修改的所有页面加入flush_list
    // 设置页面的oldest_modification和newest_modification
    
    // ========== 步骤6: 关闭预留空间 ==========
    log_buffer_close(*log_sys, handle);
    // 在recent_closed中标记这个区间已完成
    // 推进buf_ready_for_write_lsn
    // 唤醒log_writer线程
    
    // ========== 步骤7: 记录commit LSN ==========
    m_impl->m_mtr->m_commit_lsn = handle.end_lsn;
    
  } else {
    // 没有log记录（如只读操作）
    add_dirty_blocks_to_flush_list(0, 0);
  }
  
  // ========== 步骤8: 释放资源 ==========
  release_all();       // 释放所有latch
  release_resources(); // 释放内存
}
```

### 4.2 log_writer线程实现

**文件**: `storage/innobase/log/log0write.cc`

```cpp
void log_writer(log_t *log_ptr) {
  log_t &log = *log_ptr;
  
  while (!log.should_stop_threads.load()) {
    // ========== 等待写入请求 ==========
    // 条件：buf_ready_for_write_lsn > write_lsn
    log_writer_wait_on_new_writer_request(log);
    
    if (log.should_stop_threads.load()) {
      break;
    }
    
    // ========== 计算要写入的范围 ==========
    lsn_t ready_lsn = log.buf_ready_for_write_lsn.load();
    lsn_t write_lsn = log.write_lsn.load();
    
    if (ready_lsn <= write_lsn) {
      continue;  // 没有新数据
    }
    
    // ========== 执行写入 ==========
    lsn_t write_from = write_lsn;
    lsn_t write_to = std::min(ready_lsn, write_from + LOG_WRITE_AHEAD_SIZE);
    
    // 计算log buffer中的位置（循环缓冲区）
    size_t buf_offset = write_from % log.buf_size;
    size_t write_size = write_to - write_from;
    
    // 可能需要分两次写（环形缓冲区）
    if (buf_offset + write_size > log.buf_size) {
      size_t first_part = log.buf_size - buf_offset;
      
      // 写入第一部分
      ssize_t written = pwrite(log.m_current_file_fd,
                               log.buf + buf_offset,
                               first_part,
                               write_from);
      
      // 写入第二部分（回绕）
      written += pwrite(log.m_current_file_fd,
                        log.buf,
                        write_size - first_part,
                        write_from + first_part);
    } else {
      // 一次写入
      ssize_t written = pwrite(log.m_current_file_fd,
                               log.buf + buf_offset,
                               write_size,
                               write_from);
    }
    
    // ========== 更新write_lsn ==========
    log.write_lsn.store(write_to, std::memory_order_release);
    
    // ========== 通知写入完成 ==========
    os_event_set(log.write_notifier_event);
    
    // ========== 记录到recent_written ==========
    log.recent_written.add_link(write_from, write_to);
  }
}
```

### 4.3 log_flusher线程实现

```cpp
void log_flusher(log_t *log_ptr) {
  log_t &log = *log_ptr;
  
  while (!log.should_stop_threads.load()) {
    // ========== 等待刷盘请求 ==========
    // 条件：有用户请求刷盘 OR 定时刷盘
    log_flusher_wait_on_new_request(log);
    
    if (log.should_stop_threads.load()) {
      break;
    }
    
    // ========== 计算要刷盘的LSN ==========
    lsn_t write_lsn = log.write_lsn.load();
    lsn_t flushed_lsn = log.flushed_to_disk_lsn.load();
    lsn_t requested_lsn = log.flush_requested_lsn.load();
    
    lsn_t flush_up_to = std::min(write_lsn, requested_lsn);
    
    if (flush_up_to <= flushed_lsn) {
      continue;  // 已经刷过了
    }
    
    // ========== 执行fsync ==========
    int ret = fsync(log.m_current_file_fd);
    
    if (ret != 0) {
      ib::fatal() << "Failed to fsync redo log file";
    }
    
    // ========== 更新flushed_to_disk_lsn ==========
    log.flushed_to_disk_lsn.store(flush_up_to, std::memory_order_release);
    
    // ========== 通知刷盘完成 ==========
    os_event_set(log.flush_notifier_event);
  }
}
```

## 五、Page结构与Redo Log的关系

### 5.1 Page Header中的LSN信息

**文件**: `storage/innobase/include/page0page.h`

```cpp
// Page Header结构 (38 bytes)
#define FIL_PAGE_LSN          16  // 8 bytes: 页面最后修改的LSN

struct buf_block_t {
  page_t *frame;                    // 页面数据(16KB)
  
  // LSN信息
  lsn_t oldest_modification;        // 第一次变脏的LSN
  lsn_t newest_modification;        // 最新修改的LSN
  
  // Flush控制
  ib_uint32_t flush_type;           // 刷盘类型
  
  // 链表节点
  UT_LIST_NODE_T(buf_block_t) flush_list;  // 在flush_list中的节点
};
```

**页面修改流程**：
```
1. mtr修改页面
   └─ 页面内容变化

2. mtr记录redo log
   └─ log record: [Type][Space][Page][修改内容]

3. mtr_commit()
   └─ 分配LSN: [100, 200]
   
4. add_dirty_blocks_to_flush_list()
   └─ 设置页面LSN:
       ├─ oldest_modification = 100 (如果是第一次变脏)
       └─ newest_modification = 200
   
5. 插入flush_list（按oldest_modification排序）
   └─ flush_list: [page1(50), page2(100), page3(150), ...]

6. 刷脏页时的检查:
   └─ 确保 flushed_to_disk_lsn >= page.newest_modification
       // 确保redo log已持久化，才能刷脏页
```

### 5.2 Flush List管理

**文件**: `storage/innobase/buf/buf0flu.cc`

```cpp
void buf_flush_note_modification(
    buf_block_t *block,
    lsn_t start_lsn,
    lsn_t end_lsn) {
  
  // 获取buffer pool的mutex
  buf_pool_t *buf_pool = buf_pool_from_block(block);
  
  buf_pool_mutex_enter(buf_pool);
  
  if (block->oldest_modification == 0) {
    // 第一次变脏
    block->oldest_modification = start_lsn;
    
    // 插入flush_list（按oldest_modification排序）
    UT_LIST_ADD_FIRST(buf_pool->flush_list, block);
    
    // 增加脏页计数
    buf_pool->stat.n_flush_list_pages++;
  }
  
  // 更新newest_modification
  block->newest_modification = end_lsn;
  
  // 更新页面头的LSN
  page_set_lsn(block->frame, end_lsn);
  
  buf_pool_mutex_exit(buf_pool);
}
```

## 六、Checkpoint机制

### 6.1 Checkpoint的作用

**为什么需要Checkpoint？**

1. **限制恢复时间**: 只需从最后checkpoint开始恢复
2. **删除旧log**: checkpoint之前的log可以覆盖
3. **推进oldest_lsn**: 控制flush_list的增长

### 6.2 Checkpoint写入

**文件**: `storage/innobase/log/log0chkp.cc`

```cpp
void log_checkpoint(log_t &log, bool sync) {
  // ========== 步骤1: 确定checkpoint LSN ==========
  // 从flush_list找到oldest_modification最小的脏页
  lsn_t oldest_lsn = buf_pool_get_oldest_modification();
  
  if (oldest_lsn == 0) {
    // 没有脏页，可以checkpoint到当前LSN
    oldest_lsn = log_get_lsn(log);
  }
  
  // checkpoint LSN不能超过oldest_lsn
  // （因为之后的脏页还未刷盘）
  lsn_t checkpoint_lsn = oldest_lsn;
  
  // ========== 步骤2: 写入checkpoint记录 ==========
  byte buf[LOG_CHECKPOINT_SIZE];
  
  // 填充checkpoint信息
  mach_write_to_8(buf + LOG_CHECKPOINT_LSN, checkpoint_lsn);
  mach_write_to_8(buf + LOG_CHECKPOINT_OFFSET, 
                  log_files_real_offset_for_lsn(log, checkpoint_lsn));
  mach_write_to_8(buf + LOG_CHECKPOINT_LOG_BUF_SIZE, log.buf_size);
  
  // 计算checksum
  ulint checksum = log_checksum_calculate(buf, LOG_CHECKPOINT_SIZE - 4);
  mach_write_to_4(buf + LOG_CHECKPOINT_CHECKSUM, checksum);
  
  // ========== 步骤3: 写入文件 ==========
  // Checkpoint记录写在log file header的第一个block
  pwrite(log.m_current_file_fd, buf, LOG_CHECKPOINT_SIZE, 
         LOG_CHECKPOINT_1);  // offset 512
  
  if (sync) {
    fsync(log.m_current_file_fd);
  }
  
  // ========== 步骤4: 更新内存状态 ==========
  log.last_checkpoint_lsn.store(checkpoint_lsn);
  
  // ========== 步骤5: 删除旧log文件（如果可能）==========
  log_files_remove_consumed_files(log);
}
```

### 6.3 Checkpoint触发时机

```cpp
// 定时checkpoint（每秒检查）
void log_checkpointer(log_t *log_ptr) {
  log_t &log = *log_ptr;
  
  while (!log.should_stop_threads.load()) {
    // 等待1秒或被唤醒
    os_event_wait_time(log.checkpointer_event, 1000000);
    
    // 计算checkpoint age
    lsn_t current_lsn = log_get_lsn(log);
    lsn_t checkpoint_lsn = log.last_checkpoint_lsn.load();
    lsn_t checkpoint_age = current_lsn - checkpoint_lsn;
    
    // 如果age超过阈值，强制checkpoint
    if (checkpoint_age > log.max_checkpoint_age) {
      log_checkpoint(log, true);
    } else if (checkpoint_age > log.max_checkpoint_age * 0.8) {
      // 异步checkpoint
      log_checkpoint(log, false);
    }
  }
}
```

## 七、崩溃恢复流程详解

### 7.1 恢复的三个阶段

```
MySQL启动
  ↓
┌─────────────────────────────────────┐
│ 阶段1: Redo Log解析 (Parse Phase)   │
└─────────────────────────────────────┘
  从checkpoint_lsn开始扫描redo log
  解析所有log record
  构建 (space_id, page_no) → [log records] 的映射
  
  ↓
┌─────────────────────────────────────┐
│ 阶段2: Redo Log应用 (Apply Phase)   │
└─────────────────────────────────────┘
  对每个修改过的页面:
    1. 从磁盘读取页面
    2. 检查页面LSN < log record LSN？
    3. 如果是，应用log record
    4. 更新页面LSN
  
  ↓
┌─────────────────────────────────────┐
│ 阶段3: Undo Log处理                 │
└─────────────────────────────────────┘
  对于未提交的事务:
    1. 找到对应的undo log
    2. 执行回滚操作
    3. 删除事务记录
```

### 7.2 Parse Phase实现

**文件**: `storage/innobase/log/log0recv.cc`

```cpp
void recv_recovery_from_checkpoint_start(log_t &log, lsn_t flush_lsn) {
  // ========== 步骤1: 读取checkpoint ==========
  lsn_t checkpoint_lsn = log.last_checkpoint_lsn.load();
  
  recv_sys->parse_start_lsn = checkpoint_lsn;
  recv_sys->scanned_lsn = checkpoint_lsn;
  
  // ========== 步骤2: 扫描redo log ==========
  byte *buf = ut_malloc(LOG_BUFFER_SIZE);
  lsn_t start_lsn = checkpoint_lsn;
  
  while (start_lsn < flush_lsn) {
    // 读取一段log
    log_files_read(log, buf, start_lsn, LOG_BUFFER_SIZE);
    
    // 解析这段log中的所有record
    recv_parse_log_recs(buf, LOG_BUFFER_SIZE, start_lsn);
    
    start_lsn += LOG_BUFFER_SIZE;
  }
  
  ut_free(buf);
  
  // ========== 步骤3: 准备apply ==========
  recv_sys->apply_log_recs = true;
}

void recv_parse_log_recs(byte *buf, size_t len, lsn_t start_lsn) {
  byte *ptr = buf;
  byte *end_ptr = buf + len;
  lsn_t lsn = start_lsn;
  
  while (ptr < end_ptr) {
    // 读取record header
    mlog_id_t type = (mlog_id_t)*ptr++;
    
    if (type == MLOG_MULTI_REC_END || type == MLOG_DUMMY_RECORD) {
      continue;
    }
    
    // 读取space_id和page_no (varint编码)
    ulint space_id = mach_parse_compressed(&ptr, end_ptr);
    ulint page_no = mach_parse_compressed(&ptr, end_ptr);
    
    // 读取record body
    byte *body_ptr = ptr;
    ulint body_len = 0;
    
    switch (type) {
      case MLOG_REC_INSERT:
        body_len = mach_read_from_2(ptr);
        break;
      // ... 其他类型
    }
    
    ptr += body_len;
    
    // ========== 添加到recv_sys的hash表 ==========
    page_id_t page_id(space_id, page_no);
    recv_addr_t *recv_addr = recv_get_rec(page_id);
    
    if (recv_addr == nullptr) {
      // 创建新entry
      recv_addr = recv_add_rec(page_id);
    }
    
    // 添加log record到这个page的列表
    recv_data_t *recv_data = ut_malloc(sizeof(recv_data_t) + body_len);
    recv_data->type = type;
    recv_data->len = body_len;
    recv_data->lsn = lsn;
    memcpy(recv_data + 1, body_ptr, body_len);
    
    UT_LIST_ADD_LAST(recv_addr->rec_list, recv_data);
    
    lsn += (ptr - buf);
  }
}
```

### 7.3 Apply Phase实现

```cpp
void recv_apply_hashed_log_recs(bool allow_ibuf) {
  // 遍历recv_sys的hash表
  for (auto &entry : recv_sys->addr_hash) {
    page_id_t page_id = entry.first;
    recv_addr_t *recv_addr = entry.second;
    
    // ========== 读取页面 ==========
    buf_block_t *block = buf_page_get(page_id);
    page_t *page = block->frame;
    
    // 读取页面当前的LSN
    lsn_t page_lsn = mach_read_from_8(page + FIL_PAGE_LSN);
    
    // ========== 应用log records ==========
    for (recv_data_t *recv_data : recv_addr->rec_list) {
      if (recv_data->lsn > page_lsn) {
        // 需要应用这个log record
        recv_recover_page(block, recv_data);
        
        // 更新页面LSN
        mach_write_to_8(page + FIL_PAGE_LSN, recv_data->lsn);
      }
    }
    
    // 标记为脏页
    buf_flush_note_modification(block, recv_addr->start_lsn, 
                                recv_addr->end_lsn);
    
    // 释放页面
    buf_page_release(block);
  }
}

void recv_recover_page(buf_block_t *block, recv_data_t *recv_data) {
  page_t *page = block->frame;
  byte *rec_data = (byte *)(recv_data + 1);
  
  switch (recv_data->type) {
    case MLOG_REC_INSERT: {
      // 重新插入记录
      ulint offset = mach_read_from_2(rec_data);
      ulint rec_len = mach_read_from_2(rec_data + 2);
      byte *rec = rec_data + 4;
      
      // 在页面的指定位置插入记录
      page_cur_t page_cur;
      page_cur_set_before_first(block, &page_cur);
      page_cur_move_to_nth_rec(&page_cur, offset);
      
      page_cur_insert_rec_low(&page_cur, rec, nullptr, nullptr);
      break;
    }
    
    case MLOG_REC_DELETE: {
      // 重新删除记录
      ulint offset = mach_read_from_2(rec_data);
      
      page_cur_t page_cur;
      page_cur_set_before_first(block, &page_cur);
      page_cur_move_to_nth_rec(&page_cur, offset);
      
      page_cur_delete_rec(&page_cur, nullptr, nullptr);
      break;
    }
    
    // ... 其他类型
  }
}
```

## 八、性能分析和调优

### 8.1 关键性能指标

**Redo Log相关的性能瓶颈：**

1. **fsync延迟**
   ```
   原因: 磁盘写入速度慢
   影响: 事务提交延迟高
   解决: 
     - 使用SSD
     - 调整innodb_flush_log_at_trx_commit
     - 启用Group Commit
   ```

2. **Log Buffer溢出**
   ```
   原因: log_buffer_size太小 或 后台线程跟不上
   影响: mtr_commit()等待
   解决:
     - 增大innodb_log_buffer_size
     - 检查磁盘IO性能
   ```

3. **Checkpoint Age过大**
   ```
   原因: 脏页刷新太慢
   影响: 
     - 恢复时间长
     - 可能阻塞新的mtr提交
   解决:
     - 增加page_cleaner线程数
     - 调整刷脏策略
   ```

### 8.2 监控SQL

```sql
-- 查看redo log状态
SHOW ENGINE INNODB STATUS\G

-- 关键指标：
-- Log sequence number: 当前LSN
-- Log flushed up to:   已刷盘的LSN
-- Last checkpoint at:  最后checkpoint的LSN

-- Checkpoint Age = Log sequence number - Last checkpoint at
-- 如果过大（如 > 80%的redo log总容量），需要优化

-- 查看redo log写入统计
SELECT * FROM performance_schema.file_summary_by_instance
WHERE FILE_NAME LIKE '%redo%'\G

-- COUNT_WRITE: 写入次数
-- SUM_NUMBER_OF_BYTES_WRITE: 写入字节数
-- SUM_TIMER_WRITE: 总写入时间
```

### 8.3 性能调优建议

#### 调优1: 降低刷盘频率
```sql
-- 生产环境（安全）
SET GLOBAL innodb_flush_log_at_trx_commit = 1;

-- 测试环境（高性能）
SET GLOBAL innodb_flush_log_at_trx_commit = 2;

-- 可容忍数据丢失（最高性能）
SET GLOBAL innodb_flush_log_at_trx_commit = 0;
```

#### 调优2: 增大Log Buffer
```sql
-- 默认16MB，高并发可增大到64MB
SET GLOBAL innodb_log_buffer_size = 67108864;  -- 64MB
```

#### 调优3: 使用批量操作
```sql
-- 低效：逐条INSERT
INSERT INTO t VALUES (1);
INSERT INTO t VALUES (2);
-- 每次都fsync

-- 高效：批量INSERT
START TRANSACTION;
INSERT INTO t VALUES (1);
INSERT INTO t VALUES (2);
-- ... 多条
COMMIT;  -- 只fsync一次
```

#### 调优4: 分离Redo Log到高速存储
```
[mysqld]
innodb_log_group_home_dir = /ssd/mysql/redolog
datadir = /hdd/mysql/data

# Redo log在SSD，数据文件在HDD
# 因为redo log是顺序写，即使HDD也可以
# 但SSD的fsync延迟更低
```

## 九、常见问题排查

### 问题1: 事务提交很慢

**排查步骤：**

```sql
-- 1. 查看redo log刷盘设置
SHOW VARIABLES LIKE 'innodb_flush_log_at_trx_commit';

-- 2. 查看慢查询日志
-- 如果看到大量简单INSERT也慢，可能是fsync慢

-- 3. 检查磁盘IO
-- iostat -x 1
-- 查看redo log文件所在磁盘的await (平均等待时间)

-- 4. 检查checkpoint age
SHOW ENGINE INNODB STATUS\G
-- 如果checkpoint age过大，可能阻塞提交
```

### 问题2: 恢复时间很长

**原因分析：**
```
恢复时间 = (当前LSN - checkpoint LSN) / 应用速度

如果checkpoint age很大，恢复会很慢
```

**解决方案：**
```sql
-- 增加page cleaner线程
SET GLOBAL innodb_page_cleaners = 4;

-- 调整刷脏策略
SET GLOBAL innodb_max_dirty_pages_pct = 75;

-- 更频繁的checkpoint
-- (由InnoDB自动管理，无需手动调整)
```

### 问题3: Redo Log用完

**错误信息：**
```
ERROR 1534: Cannot purge redo logs
ERROR: log file is full
```

**原因：**
- Checkpoint无法推进（脏页刷新太慢）
- Redo log文件太小

**解决：**
```sql
-- 1. 立即刷新所有脏页
SET GLOBAL innodb_fast_shutdown = 0;
SHUTDOWN;
-- 重启

-- 2. 增大redo log容量（MySQL 8.0.30+）
SET GLOBAL innodb_redo_log_capacity = 8589934592;  -- 8GB
```

## 十、总结

### 核心要点回顾

1. **Redo Log是WAL的实现**
   - 先写日志，后写数据
   - 保证持久性

2. **Mini-Transaction是原子单位**
   - 一组页面修改的原子操作
   - 提交时一次性写入log buffer

3. **四个后台线程**
   - log_writer: 写入文件
   - log_flusher: 刷盘
   - log_write_notifier: 通知写完成
   - log_flush_notifier: 通知刷盘完成

4. **LSN是核心概念**
   - 全局递增
   - 标识log的位置
   - 用于崩溃恢复

5. **Group Commit优化**
   - 批量刷盘
   - 提升并发性能

6. **Checkpoint控制恢复时间**
   - 定期推进
   - 删除旧log

### 性能权衡

```
持久性 ← ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ → 性能

innodb_flush_log_at_trx_commit=1     最安全，最慢
innodb_flush_log_at_trx_commit=2     折中
innodb_flush_log_at_trx_commit=0     最快，可能丢失数据
```

**生产环境建议：**
- 关键业务：使用1，配合SSD
- 一般业务：使用2
- 测试环境：使用0

---

**参考文档：**
- [InnoDB Redo Log官方文档](https://dev.mysql.com/doc/refman/8.0/en/innodb-redo-log.html)
- `storage/innobase/log/log0log.cc` 源码注释
- MySQL Internals Manual

**版本**: 1.0  
**适用于**: MySQL 8.0+

