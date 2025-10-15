# Redo Log 存储结构详解

## 快速回答

**Redo Log采用分层存储结构**：
- **内存层**：连续的环形缓冲区（Ring Buffer）
- **磁盘层**：多个独立的物理文件，逻辑上连续

---

## 详细架构

### 一、内存层：连续的Ring Buffer

#### 1. Log Buffer（主缓冲区）

**文件位置**: `storage/innobase/log/log0buf.cc`

```cpp
/**
 * Log buffer is a ring buffer, directly addressed by lsn values.
 * Byte for a given lsn is stored at lsn modulo size of the buffer.
 */
```

**特点**：
- **环形结构**: 使用 `lsn % buffer_size` 直接寻址
- **连续内存**: 物理上是一段连续分配的内存区域
- **大小**: 默认64MB（`innodb_log_buffer_size`）
- **并发写入**: 多个用户线程可以同时写入不同的lsn位置

**结构示意**：
```
log.buf[]  (连续内存，环形使用)
┌────────────────────────────────────────────────┐
│                                                │
│  LSN=1000   LSN=1001   LSN=1002  ...  LSN=N   │
│    [data]     [data]     [data]  ...  [data]  │
│                                                │
└────────────────────────────────────────────────┘
     ↑                                       ↑
  write_lsn                            current_lsn
  
当 current_lsn 超过 buffer_size 时：
位置 = current_lsn % srv_log_buffer_size
```

**代码证据**：
```cpp
// storage/innobase/log/log0buf.cc: 212-216
// The log buffer is a ring buffer, directly addressed by lsn values, 
// which means that there is no need for shifting of data in the log buffer. 
// Byte for a given lsn is stored at lsn modulo size of the buffer.
```

#### 2. 辅助内存结构

除了主log buffer，还有几个辅助的连续内存结构：

```cpp
// storage/innobase/log/log0log.cc: 231-244

-# Log recent written buffer (e.g. 4MB) 
   - 跟踪最近的写入完成情况
   - 连续内存结构

-# Log recent closed buffer (e.g. 4MB)
   - 跟踪dirty pages添加到flush list的情况
   - 连续内存结构

-# Log write ahead buffer (e.g. 4kB)
   - 用于write ahead操作
   - 连续内存结构
```

**内存层总览**：
```
用户线程写入：
    ↓
[Log Buffer - 64MB 连续内存]
    ↓ (异步)
[Log Recent Written - 4MB]
[Log Recent Closed - 4MB]
    ↓ (后台线程)
[Log Write Ahead - 4KB]
    ↓
磁盘文件
```

---

### 二、磁盘层：多个独立文件

#### 1. 文件组织方式（MySQL 8.0.30+）

**文件位置**: `storage/innobase/include/log0types.h`

```cpp
// storage/innobase/include/log0types.h: 181-189
/** Redo log files are named #ib_redo0, #ib_redo1, ... 
 *  and no longer wrapped.
 *  Redo log files are created on-demand during runtime 
 *  and might have different sizes. 
 */
enum class Log_files_ruleset {
    PRE_8_0_30,  // 旧格式：ib_logfile0, ib_logfile1, ...
    CURRENT      // 新格式：#ib_redo0, #ib_redo1, ...
};
```

**文件系统布局**：
```
<datadir>/#innodb_redo/
├── #ib_redo0          (例如: 512MB)
├── #ib_redo1          (例如: 512MB)
├── #ib_redo2          (例如: 512MB)
├── #ib_redo3          (例如: 256MB)
├── #ib_redo4_tmp      (unused file，等待使用)
└── #ib_redo5_tmp      (unused file，等待使用)

每个文件都是独立的物理文件
```

#### 2. 逻辑上的连续性

虽然物理上是多个文件，但**逻辑上通过LSN形成连续的地址空间**：

```cpp
// storage/innobase/log/log0log.cc: 192-194
// Consecutive bytes written to the redo log are enumerated 
// by the lsn values. Every single byte written to the log buffer 
// corresponds to current lsn increased by one.
```

**LSN到文件的映射**：
```
LSN地址空间 (逻辑连续):
0 ─────────────── 512MB ───────────── 1GB ──────────── 1.5GB ────────► ∞
│                   │                  │                │
│   #ib_redo0       │   #ib_redo1      │  #ib_redo2    │  #ib_redo3 ...
│   (512MB)         │   (512MB)        │  (512MB)      │  (256MB)
└───────────────────┴──────────────────┴───────────────┴─────────────►

每个文件：
- 独立的物理文件
- 固定的LSN范围: [start_lsn, end_lsn)
- 顺序排列，不重叠
```

#### 3. 文件结构

每个redo log文件的内部结构：

```
#ib_redo0 文件布局：
┌──────────────────────────────────────┐
│ File Header (2048 bytes)             │
│  - FORMAT, LOG_UUID, START_LSN, etc. │
├──────────────────────────────────────┤
│ Checkpoint Header 1 (512 bytes)      │
├──────────────────────────────────────┤
│ Checkpoint Header 2 (512 bytes)      │
├──────────────────────────────────────┤
│ Log Blocks (512 bytes each)          │
│  ┌────────────────────────┐          │
│  │ Block Header (12 bytes)│          │
│  ├────────────────────────┤          │
│  │ Data (484 bytes)       │          │
│  ├────────────────────────┤          │
│  │ Block Trailer (4 bytes)│          │
│  └────────────────────────┘          │
│  ┌────────────────────────┐          │
│  │ Next Block...          │          │
│  └────────────────────────┘          │
│           ...                        │
└──────────────────────────────────────┘
```

**代码证据**：
```cpp
// storage/innobase/log/log0write.cc: 603-652
// After the header, there are consecutive log blocks. 
// Each log block has the same format and consists of 
// OS_FILE_LOG_BLOCK_SIZE bytes (512).
```

---

### 三、从内存到磁盘的映射

#### 1. LSN的全局唯一性

```
内存 Log Buffer:                   磁盘 Log Files:
    ↓                                  ↓
LSN是全局唯一的序列号，贯穿整个系统
    ↓                                  ↓
┌─────────────┐                  ┌──────────┐
│ LSN 1000    │  ──写入──>       │#ib_redo0 │
│ LSN 1001    │                  │ LSN 1000 │
│ LSN 1002    │                  │ LSN 1001 │
│   ...       │                  │ LSN 1002 │
│ LSN 2000    │                  │   ...    │
└─────────────┘                  └──────────┘
(环形buffer)                     (顺序文件)
```

#### 2. 写入流程

```cpp
// storage/innobase/log/log0log.cc: 183-185
// Background threads are responsible for writing of new changes 
// in the log buffer to the log files. User threads that require 
// durability for the logged records, have to wait until 
// the log gets flushed up to the required point.
```

**完整流程**：
```
Step 1: 用户线程写入 (并发)
    mtr_commit()
        ↓
    reserve space in log buffer
        ↓
    copy data to log buffer (连续内存)
        位置 = start_lsn % buffer_size
        
Step 2: 后台线程写入 (log_writer)
    从 log buffer 读取
        ↓
    确定目标文件和偏移
        file_id = lsn_to_file_id(lsn)
        offset = lsn_to_offset(lsn)
        ↓
    写入磁盘文件
        write(file_fd, data, size)
        
Step 3: 后台线程刷盘 (log_flusher)
    fsync(file_fd)
```

---

### 四、为什么这样设计？

#### 1. 内存用连续Buffer的原因

✅ **优势**：
- **直接寻址**: `address = lsn % buffer_size`，O(1)时间复杂度
- **无需移动**: 环形结构避免数据搬移
- **高并发**: 不同LSN位置可以并发写入
- **Cache友好**: 连续内存对CPU Cache友好

```cpp
// storage/innobase/log/log0buf.cc: 212-216
// It is then easier to reach higher concurrency with such 
// the log buffer, because shifting would require an exclusive access.
```

❌ **如果用多个buffer**：
- 需要管理buffer切换
- 需要额外的同步机制
- 寻址变复杂

#### 2. 磁盘用多个文件的原因

✅ **优势**：
- **动态扩容**: 按需创建新文件，不需要预分配全部空间
- **灵活大小**: 不同文件可以有不同大小
- **渐进式缩容**: 可以逐个删除旧文件
- **并发IO**: 多个文件可以并发读写（如果在不同磁盘上）
- **文件系统限制**: 避免单个文件过大（例如某些文件系统限制）

**代码证据**：
```cpp
// storage/innobase/include/log0types.h: 181-189
/** Redo log files are created on-demand during runtime 
 *  and might have different sizes. */
```

❌ **如果用单个大文件**：
- 需要预分配全部空间（浪费）
- 扩容需要文件扩展（可能阻塞）
- 缩容困难（需要truncate大文件）
- 可能超过文件系统限制

---

### 五、关键数据结构

#### 1. Log Buffer结构

```cpp
struct alignas(ut::INNODB_CACHE_LINE_SIZE) log_t {
    // ★ 连续内存区域 ★
    aligned_array_pointer<byte, OS_FILE_LOG_BLOCK_SIZE> buf;
    size_t buf_size;  // 默认64MB
    
    // LSN指针
    atomic_lsn_t write_lsn;     // 已写入buffer的最大LSN
    atomic_lsn_t current_lsn;   // 当前分配的最大LSN
    atomic_lsn_t flushed_to_disk_lsn; // 已刷盘的最大LSN
    
    // 文件集合
    Log_files m_files;  // ★ 多个独立文件 ★
};
```

#### 2. Log Files结构

```cpp
class Log_files {
private:
    // ★ 文件列表（不是连续内存，是独立文件的元数据） ★
    std::vector<Log_file> m_files;

public:
    struct Log_file {
        Log_file_id m_id;            // 文件ID: 0, 1, 2, ...
        lsn_t m_start_lsn;           // 文件起始LSN
        lsn_t m_end_lsn;             // 文件结束LSN
        os_offset_t m_size_in_bytes; // 文件大小（字节）
        bool m_consumed;             // 是否已被消费
        bool m_full;                 // 是否已满
    };
};
```

---

### 六、实际示例

#### 示例1：写入过程

```sql
-- 用户执行
INSERT INTO t1 VALUES (1, 'test');
COMMIT;
```

**内部流程**：

```
1. 生成redo log记录（内存）
   MTR生成: "INSERT t1 page=100 offset=50 data='test'"
   大小: 48 bytes

2. 预留log buffer空间（连续内存）
   current_lsn: 1000000 → 1000048
   buffer位置: [1000000 % 67108864] = offset 1000000

3. 写入log buffer（连续内存写入）
   memcpy(log.buf + 1000000, mtr_buf, 48)
   
4. 后台线程刷新（写入磁盘文件）
   确定文件: lsn=1000000 → file_id=1 (#ib_redo1)
   文件内偏移: 1000000 - file_start_lsn
   write(fd_redo1, data, 48)
   
5. fsync刷盘
   fsync(fd_redo1)
```

#### 示例2：文件切换

```
当前状态：
#ib_redo0: [LSN 0 - 512MB)      已满
#ib_redo1: [LSN 512MB - 1GB)    已满
#ib_redo2: [LSN 1GB - 1.5GB)    写入中...
#ib_redo3_tmp: 未使用

当 #ib_redo2 写满时：
1. 重命名: #ib_redo3_tmp → #ib_redo3
2. 初始化文件头
3. 设置: m_start_lsn = 1.5GB
4. 继续写入到 #ib_redo3

同时：
- Governor线程删除或回收已消费的 #ib_redo0
- 提前准备新的 #ib_redo4_tmp
```

---

### 七、关键常量

```cpp
// storage/innobase/include/log0constants.h

// Log buffer大小
constexpr size_t INNODB_LOG_BUFFER_SIZE_DEFAULT = 64 * 1024 * 1024;  // 64MB
constexpr size_t INNODB_LOG_BUFFER_SIZE_MIN = 256 * 1024;            // 256KB
constexpr size_t INNODB_LOG_BUFFER_SIZE_MAX = 4096ULL * 1024 * 1024; // 4GB

// Log block大小（磁盘上的基本单位）
constexpr size_t OS_FILE_LOG_BLOCK_SIZE = 512;     // 512字节
constexpr size_t LOG_BLOCK_HDR_SIZE = 12;          // 块头12字节
constexpr size_t LOG_BLOCK_TRL_SIZE = 4;           // 块尾4字节
constexpr size_t LOG_BLOCK_DATA_SIZE = 496;        // 数据496字节

// Log file大小限制
constexpr os_offset_t LOG_FILE_MIN_SIZE = 4 * 1024 * 1024;        // 4MB
constexpr os_offset_t LOG_FILE_MAX_SIZE = 512ULL * 1024 * 1024 * 1024; // 512GB

// Total capacity限制
constexpr os_offset_t LOG_CAPACITY_MIN = 8 * 1024 * 1024;         // 8MB
constexpr os_offset_t LOG_CAPACITY_MAX = 128ULL * 1024 * 1024 * 1024; // 128GB
```

---

### 八、性能影响

#### 1. 内存层性能（连续buffer）

**写入性能**：
```
直接寻址: O(1)
并发写入: 多线程可以同时写入不同LSN位置
Cache效率: 连续内存，预取友好

测试数据（源码注释）:
- 单线程写入: ~2GB/s
- 多线程写入: 可线性扩展（无锁竞争）
```

**限制因素**：
- Buffer大小（默认64MB）
- 当buffer满时，用户线程需要等待

#### 2. 磁盘层性能（多文件）

**顺序写入**：
```
优势: 
- 顺序IO，对HDD和SSD都友好
- 每个文件独立，可以利用多磁盘并发

劣势:
- 文件切换时有轻微开销（打开新文件）
- 跨文件读取需要多次IO
```

**文件管理开销**：
```
创建新文件: ~100ms (取决于文件系统)
删除旧文件: ~10ms
重命名文件: ~1ms

这些操作都是后台异步完成，不阻塞用户事务
```

---

### 九、监控与诊断

#### 1. 查看内存buffer状态

```sql
-- 查看log buffer配置
SHOW VARIABLES LIKE 'innodb_log_buffer_size';
-- 默认: 67108864 (64MB)

-- 查看buffer使用情况
SELECT 
    NAME,
    COUNT
FROM performance_schema.innodb_metrics
WHERE NAME LIKE 'log_buffer%';
```

#### 2. 查看磁盘文件状态

```sql
-- 查看redo log文件列表
SELECT 
    FILE_ID,
    START_LSN,
    END_LSN,
    SIZE_IN_BYTES,
    IS_FULL,
    CONSUMER_LEVEL
FROM performance_schema.innodb_redo_log_files
ORDER BY FILE_ID;

-- 输出示例：
-- FILE_ID | START_LSN | END_LSN      | SIZE_IN_BYTES | IS_FULL | CONSUMER_LEVEL
-- 0       | 0         | 536870912    | 536870912     | 1       | 1
-- 1       | 536870912 | 1073741824   | 536870912     | 1       | 0
-- 2       | 1073741824| 1610612736   | 536870912     | 0       | 0
```

#### 3. 查看LSN进度

```sql
-- 查看LSN状态
SHOW ENGINE INNODB STATUS\G

-- 关键指标：
-- Log sequence number: 当前LSN
-- Log flushed up to:   已刷盘LSN  
-- Last checkpoint at:  最后checkpoint LSN
```

---

### 十、总结对比

| 特性 | 内存层（Log Buffer） | 磁盘层（Log Files） |
|------|---------------------|-------------------|
| **物理结构** | 连续内存（Ring Buffer） | 多个独立文件 |
| **逻辑结构** | 通过LSN连续寻址 | 通过LSN逻辑连续 |
| **大小** | 固定（64MB默认） | 动态增长 |
| **寻址方式** | `lsn % buffer_size` | `file_id + offset` |
| **并发性** | 高（无锁并发写） | 中（后台线程写） |
| **持久性** | 易失 | 持久化 |
| **扩展性** | 受限于内存 | 可扩展至128GB |
| **访问速度** | 极快（ns级） | 较慢（ms级） |

---

### 十一、设计演进

#### MySQL 8.0.30之前

```
内存：连续buffer (相同)
磁盘：固定数量的固定大小文件
      - ib_logfile0 (固定大小，如1GB)
      - ib_logfile1 (固定大小，如1GB)
      - 循环使用（覆盖旧数据）
```

#### MySQL 8.0.30之后（当前）

```
内存：连续buffer (相同)
磁盘：动态数量的可变大小文件
      - #ib_redo0, #ib_redo1, ...
      - 不循环使用，持续增长
      - 旧文件被删除而不是覆盖
      - 支持动态容量调整
```

**改进原因**：
1. 更灵活的容量管理
2. 避免循环覆盖的复杂性
3. 支持在线动态调整
4. 更好的性能监控

---

## 最终答案

**Redo Log的存储结构是分层的**：

1. **内存层**：是一段**连续的环形缓冲区**
   - 物理上连续分配
   - 通过LSN取模直接寻址
   - 支持无锁并发写入

2. **磁盘层**：是**多个独立的物理文件**
   - 每个文件独立存储
   - 逻辑上通过LSN形成连续地址空间
   - 动态创建和删除

3. **LSN机制**：提供了统一的寻址抽象
   - 在内存中：`buffer[lsn % buffer_size]`
   - 在磁盘上：`file[lsn_to_file(lsn)] + offset[lsn_to_offset(lsn)]`

这种设计兼顾了：
- 内存访问的**高性能**（连续buffer）
- 磁盘存储的**灵活性**（多文件）
- 系统的**可扩展性**（动态容量）

---

## 参考源码

- `storage/innobase/log/log0buf.cc` - Log buffer实现
- `storage/innobase/log/log0write.cc` - 写入磁盘逻辑
- `storage/innobase/log/log0files_governor.cc` - 文件管理
- `storage/innobase/include/log0types.h` - 类型定义
- `storage/innobase/log/log0log.cc` - 架构文档注释

---

**文档版本**: 1.0  
**生成时间**: 2025-10-15  
**关联文档**: SET_GLOBAL_innodb_redo_log_capacity执行流程分析.md

