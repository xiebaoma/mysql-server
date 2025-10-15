# SET GLOBAL innodb_redo_log_capacity 执行流程分析

## 概述

本文档详细分析了 `SET GLOBAL innodb_redo_log_capacity = 2G` 这条SQL语句在MySQL源码中的完整执行流程，从SQL解析到最终的物理文件调整。

## 执行流程概览

```
SQL解析层 → SET变量处理 → 系统变量框架 → InnoDB回调 → Governor线程 → 容量管理 → 文件IO操作
```

---

## 详细执行流程

### 1. SQL解析与分发（SQL Layer）

#### 1.1 SQL命令解析

**文件位置**: `sql/sql_parse.cc`

**入口函数**: `mysql_execute_command()`

```cpp
// sql/sql_parse.cc: 3721-3761
case SQLCOM_SET_OPTION: {
    List<set_var_base> *lex_var_list = &lex->var_list;
    
    // 权限检查
    if (check_table_access(thd, SELECT_ACL, all_tables, false, UINT_MAX, false))
        goto error;
    
    // 打开必要的表
    if (open_tables_for_query(thd, all_tables, false)) 
        goto error;
    
    // 执行SET变量操作
    if (!(res = sql_set_variables(thd, lex_var_list, true)))
        my_ok(thd);
    else {
        if (!thd->is_error()) 
            my_error(ER_WRONG_ARGUMENTS, MYF(0), "SET");
        goto error;
    }
    break;
}
```

**关键点**:
- 命令类型识别: `SQLCOM_SET_OPTION`
- 调用 `sql_set_variables()` 处理变量列表

---

### 2. SET变量处理流程

#### 2.1 变量更新主流程

**文件位置**: `sql/set_var.cc`

**函数**: `sql_set_variables()`

```cpp
// sql/set_var.cc: 1413-1450
int sql_set_variables(THD *thd, List<set_var_base> *var_list, bool opened) {
    int error;
    List_iterator_fast<set_var_base> it(*var_list);
    set_var_base *var;
    
    // 阶段1: 解析所有变量
    while ((var = it++)) {
        if ((error = var->resolve(thd))) 
            goto err;
    }
    
    // 阶段2: 检查所有变量的值
    it.rewind();
    while ((var = it++)) {
        if ((error = var->check(thd))) 
            goto err;
    }
    
    // 阶段3: 更新所有变量
    it.rewind();
    while ((var = it++)) {
        if ((error = var->update(thd)))
            goto err;
    }
    
    return 0;
}
```

**三阶段提交模式**:
1. **Resolve**: 解析变量名称和表达式
2. **Check**: 检查值的有效性（范围、类型等）
3. **Update**: 真正更新变量值

#### 2.2 变量解析（Resolve）

```cpp
// sql/set_var.cc: 1578-1648
int set_var::resolve(THD *thd) {
    auto f = [this, thd](const System_variable_tracker &, sys_var *var) -> int {
        // 检查是否是只读变量
        if (var->is_readonly() && type != OPT_PERSIST_ONLY) {
            my_error(ER_INCORRECT_GLOBAL_LOCAL_VAR, MYF(0), var->name.str, "read only");
            return -1;
        }
        
        // 检查作用域（GLOBAL/SESSION）
        if (!var->check_scope(type)) {
            int err = (is_global_persist()) ? ER_LOCAL_VARIABLE : ER_GLOBAL_VARIABLE;
            my_error(err, MYF(0), var->name.str);
            return -1;
        }
        
        // 检查权限
        if (type == OPT_GLOBAL || type == OPT_PERSIST) {
            if (check_priv(thd, false)) 
                return -1;
        }
        
        // 修正表达式的类型
        if (value && value->fix_fields(thd, &value))
            return -1;
        
        return 0;
    };
    
    return m_var_tracker.access_system_variable<int>(thd, f).value_or(-1);
}
```

#### 2.3 变量检查（Check）

```cpp
// sql/set_var.cc: 1660-1702
int set_var::check(THD *thd) {
    auto f = [this, thd](const System_variable_tracker &, sys_var *var) -> int {
        // 检查数据类型
        if (var->check_update_type(value->result_type())) {
            my_error(ER_WRONG_TYPE_FOR_VAR, MYF(0), var->name.str);
            return -1;
        }
        
        // 调用变量特定的check函数
        return (type != OPT_PERSIST_ONLY && var->check(thd, this)) ? -1 : 0;
    };
    
    int ret = m_var_tracker.access_system_variable<int>(thd, f).value_or(-1);
    
    // 审计日志
    if (!ret && (is_global_persist())) {
        ret = mysql_audit_notify(thd, AUDIT_EVENT(MYSQL_AUDIT_GLOBAL_VARIABLE_SET),
                                 m_var_tracker.get_var_name(),
                                 value->item_name.ptr(), value->item_name.length());
    }
    
    return ret;
}
```

#### 2.4 变量更新（Update）

```cpp
// sql/set_var.cc: 1785-1811
int set_var::update(THD *thd) {
    auto f = [this, thd](const System_variable_tracker &, sys_var *var) -> bool {
        bool ret = false;
        
        // 对于PERSIST_ONLY，不更新当前值
        if (type != OPT_PERSIST_ONLY) {
            if (value)
                ret = var->update(thd, this);  // ← 调用sys_var的update
            else
                ret = var->set_default(thd, this);
            
            if (!ret) {
                update_source_user_host_timestamp(thd, var);
            }
        }
        return ret;
    };
    
    return m_var_tracker.access_system_variable<bool>(thd, f).value_or(true) ? 1 : 0;
}
```

---

### 3. 系统变量框架层

#### 3.1 sys_var基类的update方法

**文件位置**: `sql/set_var.cc`

```cpp
// sql/set_var.cc: 337-386
bool sys_var::update(THD *thd, set_var *var) {
    // 预更新钩子（可选）
    if (pre_update && pre_update(this, thd, var)) 
        return true;
    
    enum_var_type type = var->type;
    
    if (type == OPT_GLOBAL || type == OPT_PERSIST || scope() == GLOBAL) {
        // GLOBAL变量更新路径
        AutoWLock lock1(&PLock_global_system_variables);
        AutoWLock lock2(guard);
        
        // 调用子类的global_update + on_update回调
        return global_update(thd, var) ||
               (on_update && on_update(this, thd, OPT_GLOBAL));
    } else {
        // SESSION变量更新路径
        mysql_mutex_lock(&thd->LOCK_thd_sysvar);
        
        bool ret = session_update(thd, var) ||
                   (on_update && on_update(this, thd, OPT_SESSION));
        
        mysql_mutex_unlock(&thd->LOCK_thd_sysvar);
        
        // Session tracker记录变化
        if (!ret && (var->type == OPT_SESSION || !is_trilevel())) {
            // 更新session跟踪器...
        }
        
        return ret;
    }
}
```

**关键点**:
- **锁机制**: GLOBAL变量需要获取两把写锁
- **回调机制**: `global_update()` + `on_update()`
- **Session跟踪**: 记录会话级别的变量变化

#### 3.2 Sys_var_ulonglong的global_update

**文件位置**: `sql/sys_vars.h`

```cpp
// sql/sys_vars.h: 303-306
bool global_update(THD *, set_var *var) override {
    global_var(T) = static_cast<T>(var->save_result.ulonglong_value);
    return false;
}
```

这个方法非常简单，只是将检查通过的值赋给全局变量。真正的逻辑在 `on_update` 回调中。

---

### 4. InnoDB存储引擎层

#### 4.1 系统变量定义

**文件位置**: `storage/innobase/handler/ha_innodb.cc`

```cpp
// storage/innobase/handler/ha_innodb.cc: 22585-22590
static MYSQL_SYSVAR_ULONGLONG(
    redo_log_capacity,                              // 变量名
    srv_redo_log_capacity,                          // 底层变量
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_PERSIST_AS_READ_ONLY,  // 标志
    "Limitation for total size of redo log files on disk (expressed in bytes).",
    nullptr,                                        // check函数
    innodb_redo_log_capacity_update,               // update回调 ← 关键！
    100 * 1024 * 1024,                             // 默认值: 100MB
    LOG_CAPACITY_MIN,                              // 最小值
    LOG_CAPACITY_MAX,                              // 最大值
    MB                                             // 块大小: 1MB
);
```

**变量属性**:
- 类型: ULONGLONG (8字节无符号整数)
- 作用域: GLOBAL
- 可持久化: PERSIST_AS_READ_ONLY
- 范围: `[LOG_CAPACITY_MIN, LOG_CAPACITY_MAX]`，步长1MB

#### 4.2 Update回调函数

```cpp
// storage/innobase/handler/ha_innodb.cc: 21875-21909
static void innodb_redo_log_capacity_update(
    THD *thd,
    SYS_VAR *,
    void *,
    const void *save) {
    
    const auto new_value = *static_cast<const ulonglong *>(save);
    
    // 值有效性断言
    ut_a(LOG_CAPACITY_MIN <= new_value);
    ut_a(new_value <= LOG_CAPACITY_MAX);
    ut_a(new_value % MB == 0);
    
    // 只读模式检查
    if (srv_read_only_mode) {
        my_error(ER_CANT_CHANGE_SYS_VAR_IN_READ_ONLY_MODE, MYF(0),
                 "innodb-redo-log-capacity");
        return;
    }
    
    // 更新全局变量
    srv_redo_log_capacity = new_value;
    
    // 如果值没变，直接返回
    if (new_value == srv_redo_log_capacity_used) {
        return;
    }
    
    // 更新使用中的容量值
    srv_redo_log_capacity_used = new_value;
    
    // 记录信息日志
    ib::info(ER_IB_MSG_LOG_FILES_CAPACITY_CHANGED,
             srv_redo_log_capacity_used / MB);
    
    // ★ 关键调用：通知governor线程进行resize ★
    log_files_resize_requested(*log_sys);
    
    // 检查并发安全裕度
    if (!log_sys->concurrency_margin_is_safe.load()) {
        push_warning_printf(thd, Sql_condition::SL_WARNING, ER_WRONG_ARGUMENTS,
                            "Current innodb_redo_log_capacity"
                            " is too small for safety of redo log files."
                            " Consider increasing it or decreasing"
                            " innodb_thread_concurrency.");
    }
}
```

**执行步骤**:
1. **参数验证**: 检查值的范围和对齐（必须是1MB的倍数）
2. **只读检查**: 只读模式下不允许修改
3. **更新变量**: 更新 `srv_redo_log_capacity` 和 `srv_redo_log_capacity_used`
4. **通知Governor**: 调用 `log_files_resize_requested()` 触发异步resize
5. **安全检查**: 验证并发安全裕度

---

### 5. Redo Log Governor线程

#### 5.1 唤醒Governor线程

**文件位置**: `storage/innobase/log/log0files_governor.cc`

```cpp
// storage/innobase/log/log0files_governor.cc: 2034-2042
static void log_files_wait_until_next_governor_iteration(log_t &log) {
    // 重置事件计数器
    const auto sig_count = os_event_reset(log.m_files_governor_iteration_event);
    
    // 唤醒governor线程
    os_event_set(log.m_files_governor_event);
    
    // 等待governor完成一次迭代
    os_event_wait_low(log.m_files_governor_iteration_event, sig_count);
}

void log_files_resize_requested(log_t &log) {
    log_files_wait_until_next_governor_iteration(log);
}
```

**工作机制**:
- **异步通知**: 通过事件机制唤醒后台线程
- **同步等待**: 等待Governor完成一次处理迭代
- **非阻塞设计**: SET命令不会阻塞在文件IO上

#### 5.2 Governor线程主循环

```cpp
// storage/innobase/log/log0files_governor.cc: 1360-1397
void log_files_governor(log_t *log_ptr) {
    ut_a(log_ptr != nullptr);
    log_t &log = *log_ptr;
    
    // 初始化线程上下文
    ut_d(log.m_files_governor_thd = create_internal_thd());
    
    Log_files_stats tmp{};
    log.m_files_stats = tmp;
    
    // ★ 主事件循环 ★
    while (true) {
        // 记录事件信号计数
        const auto sig_count = os_event_reset(log.m_files_governor_event);
        
        // 检查writer线程状态
        if (!log_writer_is_active()) {
            break;  // shutdown
        }
        
        // ★ 执行一次governor迭代 ★
        log_files_governor_iteration(log);
        
        // 等待下一次唤醒或超时（10ms）
        os_event_wait_time_low(log.m_files_governor_event,
                               std::chrono::milliseconds{10}, sig_count);
    }
    
    // 清理工作
    {
        IB_mutex_guard writer_latch{&(log.writer_mutex), UT_LOCATION_HERE};
        IB_mutex_guard files_latch{&(log.m_files_mutex), UT_LOCATION_HERE};
        log_files_update_capacity_limits(log);
        log_files_mark_consumed_files(log);
        log_files_process_consumed_files(log);
    }
    
    ut_d(destroy_internal_thd(log.m_files_governor_thd));
}
```

**Governor职责**:
1. **容量管理**: 监控和调整redo log总容量
2. **文件消费**: 删除或回收旧的redo log文件
3. **Resize协调**: 协调redo log的扩容/缩容操作
4. **Dummy记录**: 在需要时生成填充记录以完成文件
5. **Consumer协调**: 找出最慢的consumer并督促其完成工作

#### 5.3 Governor迭代逻辑

```cpp
// storage/innobase/log/log0files_governor.cc: 579-591
static Log_files_governor_iteration_result 
log_files_governor_iteration_low(log_t &log, bool has_writer_mutex);

static void log_files_governor_iteration(log_t &log) {
    // 第一次尝试: 不持有writer_mutex
    auto result = log_files_governor_iteration_low(log, false);
    
    // 如果需要writer_mutex，获取后重试
    if (result == Iteration_result::RETRY_WITH_WRITER_MUTEX) {
        IB_mutex_guard writer_latch{&(log.writer_mutex), UT_LOCATION_HERE};
        result = log_files_governor_iteration_low(log, true);
        ut_a(result != Iteration_result::RETRY_WITH_WRITER_MUTEX);
    }
    
    // 如果需要更多intake，生成dummy记录
    if (result == Iteration_result::COMPLETED_BUT_NEED_MORE_INTAKE &&
        !log_free_check_is_required(log)) {
        log_files_generate_dummy_records(log, LOG_FILES_DUMMY_INTAKE_SIZE);
    }
}
```

**迭代返回值**:
- `RETRY_WITH_WRITER_MUTEX`: 需要重新获取writer_mutex
- `COMPLETED_BUT_NEED_MORE_INTAKE`: 需要生成更多redo记录
- `COMPLETED`: 正常完成

---

### 6. Redo Log容量管理

#### 6.1 更新目标容量

**文件位置**: `storage/innobase/log/log0files_capacity.cc`

```cpp
// storage/innobase/log/log0files_capacity.cc: 226-271
void Log_files_capacity::update_target() {
    const os_offset_t target_physical_capacity = srv_redo_log_capacity_used;
    
    // 如果目标未变，直接返回
    if (m_target_physical_capacity == target_physical_capacity) {
        return;
    }
    
    // 目标为0表示不需要resize（工具模式）
    if (target_physical_capacity == 0) {
        return;
    }
    
    // ★ 取消当前进行中的resize操作 ★
    cancel_resize();
    
    // 确认没有resize在进行
    ut_a(m_resize_mode == Log_resize_mode::NONE);
    ut_a(m_current_physical_capacity == m_target_physical_capacity);
    
    // 如果新目标和当前容量不同，开始新的resize
    if (target_physical_capacity != m_current_physical_capacity) {
        m_target_physical_capacity = target_physical_capacity;
        
        // 记录resize请求
        ib::info(ER_IB_MSG_LOG_FILES_RESIZE_REQUESTED,
                 ulonglong{m_current_physical_capacity} / (1024 * 1024UL),
                 ulonglong{m_target_physical_capacity} / (1024 * 1024UL));
        
        if (m_target_physical_capacity < m_current_physical_capacity) {
            // ★ 缩容: 设置RESIZING_DOWN模式 ★
            m_resize_mode = Log_resize_mode::RESIZING_DOWN;
        } else {
            // ★ 扩容: 直接更新当前容量 ★
            ut_a(m_resize_mode == Log_resize_mode::NONE);
            m_current_physical_capacity = m_target_physical_capacity;
            
            ib::info(ER_IB_MSG_LOG_FILES_RESIZE_FINISHED,
                     ulonglong{m_current_physical_capacity} / (1024 * 1024UL));
        }
    }
    
    ut_a(m_target_physical_capacity <= m_current_physical_capacity);
}
```

**Resize模式**:
- `NONE`: 无resize操作
- `RESIZING_DOWN`: 缩容进行中
- `RESIZING_UP`: 扩容进行中（实际上扩容是瞬时完成的）

**扩容vs缩容**:
- **扩容**: 立即完成，只需更新内存中的容量值
- **缩容**: 需要等待旧文件被消费后才能删除，是渐进式的

#### 6.2 取消resize操作

```cpp
// storage/innobase/log/log0files_capacity.cc: 195-224
void Log_files_capacity::cancel_resize() {
    if (m_resize_mode == Log_resize_mode::NONE) {
        return;  // 没有resize在进行
    }
    
    // 取消并重置状态
    m_current_physical_capacity = m_target_physical_capacity;
    m_resize_mode = Log_resize_mode::NONE;
    
    ib::info(ER_IB_MSG_LOG_FILES_RESIZE_CANCELLED);
}
```

---

### 7. 文件物理操作

#### 7.1 判断是否需要文件消费

**文件位置**: `storage/innobase/log/log0files_governor.cc`

```cpp
// storage/innobase/log/log0files_governor.cc: 912-924
bool is_consumption_needed(const log_t &log) {
    const auto current_size = physical_size(log, log.m_capacity.next_file_size());
    const auto target_capacity = log.m_capacity.target_physical_capacity();
    const auto current_capacity = log.m_capacity.current_physical_capacity();
    
    ut_a(current_size <= current_capacity);
    
    return /* case 1. */ log.m_requested_files_consumption ||
           /* case 2. */ log.m_unused_files_count == 0 ||
           /* case 3. */ target_capacity < current_capacity ||
           /* case 4. */ current_size < current_capacity;
}
```

**需要消费文件的情况**:
1. 显式请求了文件消费
2. 没有预留的unused文件
3. 正在缩容（目标容量 < 当前容量）
4. 当前物理大小 < 当前容量（说明有文件可以回收）

#### 7.2 删除已消费的文件

```cpp
// storage/innobase/log/log0files_governor.cc: 928-949
static dberr_t log_files_remove_file(log_t &log, Log_file_id file_id) {
    log_files_write_allowed_validate(log);
    
    ut_a(!log.m_files.empty());
    const auto file = log.m_files.file(file_id);
    ut_a(file != log.m_files.end());
    ut_a(file->m_id == file_id);
    
    // ★ 物理删除文件 ★
    const dberr_t remove_err = log_remove_file(log.m_files_ctx, file_id);
    if (remove_err != DB_SUCCESS) {
        return remove_err;
    }
    
    // 从内存结构中移除
    log.m_files.erase(file_id);
    
    // 通知等待删除的线程
    os_event_set(log.m_file_removed_event);
    
    return DB_SUCCESS;
}

static bool log_files_remove_consumed_file(log_t &log, Log_file_id file_id) {
    ut_a(log.m_files.begin()->m_id == file_id);
    ut_a(log.m_files.begin()->m_consumed);
    return log_files_remove_file(log, file_id) == DB_SUCCESS;
}
```

#### 7.3 回收文件

```cpp
// storage/innobase/log/log0files_governor.cc: 951-xxx
static bool log_files_recycle_file(log_t &log, Log_file_id file_id,
                                   os_offset_t unused_file_size) {
    log_files_write_allowed_validate(log);
    
    // 第一步: 重命名为unused文件
    const auto old_path = log_file_path(log.m_files_ctx, file_id);
    const auto new_path = log_file_path_for_unused_file(log.m_files_ctx, file_id);
    
    if (!log_rename_file(old_path, new_path)) {
        return false;
    }
    
    // 第二步: 如果需要，调整文件大小
    if (unused_file_size != log.m_files.begin()->m_size_in_bytes) {
        const dberr_t err = log_resize_unused_file(
            log.m_files_ctx, file_id, unused_file_size);
        if (err != DB_SUCCESS) {
            // resize失败，删除文件
            log_remove_unused_file(log.m_files_ctx, file_id);
            return false;
        }
    }
    
    // 第三步: 从内存结构中移除
    log.m_files.erase(file_id);
    log.m_unused_files_count++;
    
    return true;
}
```

**文件回收流程**:
1. 重命名: `ib_logfileN` → `#ib_redo_N_tmp`
2. Resize: 调整到目标大小（如果需要）
3. 更新元数据: 增加unused文件计数

#### 7.4 文件Resize操作

**文件位置**: `storage/innobase/log/log0files_io.cc`

```cpp
// storage/innobase/log/log0files_io.cc: 1035-1103
static dberr_t log_resize_file_low(
    const std::string &file_path,
    os_offset_t size_in_bytes,
    int err_msg_id) {
    
    // 获取文件状态
    os_file_stat_t stat_info;
    const dberr_t err = os_file_get_status(
        file_path.c_str(), &stat_info, false, false);
    
    if (err != DB_SUCCESS) {
        ib::error(err_msg_id, file_path.c_str(),
                  ulonglong{size_in_bytes / (1024 * 1024UL)},
                  err == DB_NOT_FOUND ? "Failed to find the file"
                                      : "Failed to retrieve status of the file");
        return err == DB_NOT_FOUND ? DB_NOT_FOUND : DB_ERROR;
    }
    
    // 如果大小已经匹配，直接返回
    if (size_in_bytes == stat_info.size) {
        return DB_SUCCESS;
    }
    
    // 打开文件
    bool ret;
    auto file = os_file_create(
        innodb_log_file_key, file_path.c_str(),
        OS_FILE_OPEN | OS_FILE_ON_ERROR_NO_EXIT,
        OS_LOG_FILE_RESIZING, false, &ret);
    
    if (!ret) {
        ib::error(err_msg_id, file_path.c_str(),
                  ulonglong{size_in_bytes / (1024 * 1024UL)},
                  "Failed to open the file");
        return DB_ERROR;
    }
    
    // 扩展或截断文件
    if (size_in_bytes > stat_info.size) {
        // ★ 扩展文件 ★
        ret = os_file_set_size_fast(
            file_path.c_str(), file, 0, size_in_bytes, true);
    } else if (size_in_bytes < stat_info.size) {
        // ★ 截断文件 ★
        ret = os_file_truncate(file_path.c_str(), file, size_in_bytes);
        os_file_flush(file);
    } else {
        ret = true;
    }
    
    // 关闭文件
    const bool close_ret = os_file_close(file);
    ut_a(close_ret);
    
    // 错误处理
    if (!ret) {
        if (os_has_said_disk_full) {
            ib::error(err_msg_id, file_path.c_str(),
                      ulonglong{size_in_bytes / (1024 * 1024UL)},
                      "Missing space on disk");
            return DB_OUT_OF_DISK_SPACE;
        }
        ib::error(err_msg_id, file_path.c_str(),
                  ulonglong{size_in_bytes / (1024 * 1024UL)},
                  "Failed to resize the file");
        return DB_ERROR;
    }
    
    return DB_SUCCESS;
}

dberr_t log_resize_file(const Log_files_context &ctx, 
                        Log_file_id file_id,
                        os_offset_t size_in_bytes) {
    return log_resize_file_low(log_file_path(ctx, file_id), 
                               size_in_bytes,
                               ER_IB_MSG_LOG_FILE_RESIZE_FAILED);
}

dberr_t log_resize_unused_file(const Log_files_context &ctx,
                               Log_file_id file_id, 
                               os_offset_t size_in_bytes) {
    return log_resize_file_low(log_file_path_for_unused_file(ctx, file_id),
                               size_in_bytes,
                               ER_IB_MSG_LOG_FILE_UNUSED_RESIZE_FAILED);
}
```

**Resize操作**:
- **扩展**: 使用 `os_file_set_size_fast()` 快速扩展
- **截断**: 使用 `os_file_truncate()` + `os_file_flush()`
- **错误处理**: 检测磁盘空间不足等错误

---

## 完整调用链

```
1. mysql_execute_command()                    [sql/sql_parse.cc]
   └─> sql_set_variables()                    [sql/set_var.cc]
       │
       ├─> set_var::resolve()                 [sql/set_var.cc]
       │   └─> 检查权限、作用域、数据类型
       │
       ├─> set_var::check()                   [sql/set_var.cc]
       │   └─> sys_var::check()
       │       └─> Sys_var_ulonglong::do_check()
       │           └─> 检查范围、对齐等
       │
       └─> set_var::update()                  [sql/set_var.cc]
           └─> sys_var::update()              [sql/set_var.cc]
               └─> Sys_var_ulonglong::global_update()  [sql/sys_vars.h]
                   │   更新 srv_redo_log_capacity
                   │
                   └─> on_update回调: innodb_redo_log_capacity_update()
                       [storage/innobase/handler/ha_innodb.cc]
                       │
                       ├─> 更新 srv_redo_log_capacity_used
                       │
                       └─> log_files_resize_requested()
                           [storage/innobase/log/log0files_governor.cc]
                           │
                           └─> log_files_wait_until_next_governor_iteration()
                               │   唤醒governor线程并等待完成一次迭代
                               │
                               ▼
                           [Governor线程异步执行]
                           │
                           └─> log_files_governor_iteration()
                               │
                               ├─> Log_files_capacity::update_target()
                               │   [storage/innobase/log/log0files_capacity.cc]
                               │   │
                               │   ├─> 取消旧的resize操作
                               │   │
                               │   └─> 开始新的resize
                               │       ├─> 扩容: 立即完成
                               │       └─> 缩容: 设置RESIZING_DOWN标志
                               │
                               ├─> is_consumption_needed()
                               │   判断是否需要消费文件
                               │
                               ├─> log_files_mark_consumed_files()
                               │   标记可以消费的文件
                               │
                               └─> log_files_process_consumed_files()
                                   │
                                   ├─> log_files_remove_consumed_file()
                                   │   [删除文件]
                                   │
                                   └─> log_files_recycle_file()
                                       [回收文件]
                                       │
                                       ├─> log_rename_file()
                                       │   重命名为unused文件
                                       │
                                       └─> log_resize_unused_file()
                                           [storage/innobase/log/log0files_io.cc]
                                           │
                                           └─> log_resize_file_low()
                                               │
                                               ├─> os_file_set_size_fast()  [扩展]
                                               └─> os_file_truncate()       [截断]
```

---

## 关键数据结构

### 1. log_t结构（核心redo log管理）

**文件位置**: `storage/innobase/include/log0types.h`

```cpp
struct alignas(ut::INNODB_CACHE_LINE_SIZE) log_t {
    // Capacity管理
    Log_files_capacity m_capacity;
    
    // 文件管理
    Log_files m_files;
    Log_files_context m_files_ctx;
    
    // Governor线程同步
    os_event_t m_files_governor_event;
    os_event_t m_files_governor_iteration_event;
    
    // 统计信息
    Log_files_stats m_files_stats;
    
    // 并发安全标志
    std::atomic<bool> concurrency_margin_is_safe;
    
    // Unused文件计数
    size_t m_unused_files_count;
    
    // 其他...
};
```

### 2. Log_files_capacity（容量管理）

**文件位置**: `storage/innobase/include/log0files_capacity.h`

```cpp
class Log_files_capacity {
private:
    // 当前物理容量
    os_offset_t m_current_physical_capacity;
    
    // 目标物理容量（来自srv_redo_log_capacity_used）
    os_offset_t m_target_physical_capacity;
    
    // Resize模式
    Log_resize_mode m_resize_mode;
    
public:
    void initialize(...);
    void update_target();          // 更新目标容量
    void cancel_resize();          // 取消resize
    
    os_offset_t current_physical_capacity() const;
    os_offset_t target_physical_capacity() const;
};

enum class Log_resize_mode {
    NONE,           // 无resize
    RESIZING_DOWN,  // 缩容中
};
```

### 3. Log_files（文件集合）

**文件位置**: `storage/innobase/include/log0files_capacity.h`

```cpp
class Log_files {
private:
    // 文件列表（按file_id排序）
    std::vector<Log_file> m_files;
    
public:
    iterator begin();
    iterator end();
    
    void add(Log_file file);
    void erase(Log_file_id file_id);
    
    const Log_file* file(Log_file_id file_id) const;
};

struct Log_file {
    Log_file_id m_id;               // 文件ID
    os_offset_t m_size_in_bytes;    // 文件大小
    lsn_t m_start_lsn;              // 起始LSN
    lsn_t m_end_lsn;                // 结束LSN
    bool m_consumed;                // 是否已消费
    bool m_full;                    // 是否已满
};
```

---

## 关键全局变量

### InnoDB全局变量

**文件位置**: `storage/innobase/include/srv0srv.h`

```cpp
// 用户设置的redo log容量（来自SET GLOBAL）
extern ulonglong srv_redo_log_capacity;

// 实际使用的redo log容量
extern ulonglong srv_redo_log_capacity_used;

// 容量限制常量
constexpr os_offset_t LOG_CAPACITY_MIN = 8 * 1024 * 1024;        // 8MB
constexpr os_offset_t LOG_CAPACITY_MAX = 128ULL * 1024 * 1024 * 1024;  // 128GB

// 其他相关变量
extern ulonglong srv_log_file_size;     // 已废弃
extern ulong srv_log_n_files;           // 已废弃
extern bool srv_read_only_mode;
extern bool srv_dedicated_server;
```

---

## 执行时序图

```
用户线程                          Governor线程                   文件系统
   |                                    |                            |
   | SET GLOBAL                         |                            |
   | innodb_redo_log_capacity=2G        |                            |
   |-------------------------------->   |                            |
   |                                    |                            |
   | [SQL解析]                          |                            |
   | [权限检查]                          |                            |
   | [值检查: 2G在范围内]                |                            |
   |                                    |                            |
   | 调用innodb_redo_log_capacity_update()|                          |
   |   - 更新srv_redo_log_capacity=2G   |                            |
   |   - 更新srv_redo_log_capacity_used=2G|                          |
   |   - 唤醒governor                    |                            |
   |-------------------------------->   |                            |
   |                                    |                            |
   |  [等待governor完成一次迭代]         |                            |
   |                                    | [被唤醒]                   |
   |                                    | update_target()            |
   |                                    |   - 设置m_target=2G        |
   |                                    |   - 判断扩容/缩容          |
   |                                    |                            |
   |                                    | [如果是扩容]               |
   |                                    |   m_current=2G (立即完成)  |
   |                                    |                            |
   |                                    | [如果是缩容]               |
   |                                    |   m_resize_mode=RESIZING_DOWN|
   |                                    |   is_consumption_needed()  |
   |                                    |     -> true                |
   |                                    |   mark_consumed_files()    |
   |                                    |   process_consumed_files() |
   |                                    |     - remove_file()        |
   |                                    |       或 recycle_file()    |
   |                                    |                            |
   |                                    | rename file                |
   |                                    |------------------------->  |
   |                                    |                     ib_logfile3|
   |                                    |                     -> #ib_redo_3_tmp|
   |                                    |                            |
   |                                    | resize file                |
   |                                    |------------------------->  |
   |                                    |              truncate/extend|
   |                                    |<-------------------------- |
   |                                    |            (成功)          |
   |                                    |                            |
   |<---------------------------------- |                            |
   | [迭代完成]                          |                            |
   |                                    |                            |
   | 返回成功给客户端                    |                            |
   |                                    |                            |
   |                                    | [继续监控]                 |
   |                                    | 每10ms检查一次             |
   |                                    | 或被事件唤醒               |
```

---

## 重要配置参数

### 1. innodb_redo_log_capacity

- **类型**: ULONGLONG
- **作用域**: GLOBAL
- **动态**: 是
- **默认值**: 100MB
- **范围**: [8MB, 128GB]
- **单位**: 字节，必须是1MB的倍数
- **持久化**: PERSIST_AS_READ_ONLY（可以persist但只读模式下不能修改）

### 2. 已废弃参数（8.0.30之前）

- `innodb_log_file_size`: 单个redo log文件大小
- `innodb_log_files_in_group`: redo log文件数量

这两个参数在8.0.30+版本中已废弃，被 `innodb_redo_log_capacity` 替代。

---

## 扩容与缩容的差异

### 扩容（Resize Up）

```
当前容量: 1GB
目标容量: 2GB

执行流程:
1. update_target()
2. m_current_physical_capacity = 2GB  ← 立即完成！
3. 记录日志: "Redo log resize finished"
4. 后续可以使用更大的空间

特点: 瞬时完成，无需等待
```

**为什么扩容可以瞬时完成？**
- Redo log文件是按需创建的
- 增加容量只需更新内存中的限制值
- Governor线程会在需要时创建新文件

### 缩容（Resize Down）

```
当前容量: 2GB
目标容量: 1GB

执行流程:
1. update_target()
2. m_resize_mode = RESIZING_DOWN
3. 等待旧文件被完全消费
4. 逐步删除或回收超出容量的文件
5. 当 current_capacity == target_capacity 时完成

特点: 渐进式完成，可能需要较长时间
```

**为什么缩容需要时间？**
- 必须等待checkpoint推进，标记文件为consumed
- 不能删除仍包含有效redo记录的文件
- 需要等待所有consumer（如checkpoint、clone等）完成读取

---

## 安全性保证

### 1. 事务性保证

虽然resize操作本身不是事务性的，但有以下保证：

- **原子性**: 单个文件的删除/重命名是原子操作
- **一致性**: crash recovery可以识别并处理不完整的文件
- **持久性**: 文件操作完成后会fsync

### 2. 并发安全

- **Mutex保护**: 
  - `PLock_global_system_variables`: 保护全局变量
  - `log.m_files_mutex`: 保护文件列表
  - `log.writer_mutex`: 保护写入操作

- **Atomic标志**: 
  - `log.concurrency_margin_is_safe`: 检查并发安全裕度

### 3. Crash恢复

- **临时文件**: Unused文件使用 `#ib_redo_N_tmp` 命名
- **恢复时清理**: 启动时会清理所有 `#ib_redo_*_tmp` 文件
- **Checkpoint机制**: 确保不会丢失已提交的事务

---

## 性能考虑

### 1. 为什么使用后台线程？

- **避免阻塞**: SET命令不会阻塞在文件IO上
- **批量处理**: Governor可以批量处理多个文件
- **优先级控制**: 后台线程可以让步给用户事务

### 2. Governor线程的唤醒机制

- **事件驱动**: 通过 `os_event` 机制唤醒
- **定时检查**: 默认10ms检查一次
- **按需唤醒**: 有resize请求时立即唤醒

### 3. 文件操作优化

- **预分配**: Unused文件提前创建和调整大小
- **快速扩展**: 使用 `os_file_set_size_fast()` 避免写零
- **回收机制**: 优先回收而不是删除+创建

---

## 错误处理

### 1. 用户输入错误

```cpp
// 值不是1MB的倍数
SET GLOBAL innodb_redo_log_capacity = 100000000;
ERROR: Value must be a multiple of 1048576

// 值超出范围
SET GLOBAL innodb_redo_log_capacity = 1024;
ERROR: Value must be at least 8388608

// 只读模式
SET GLOBAL innodb_redo_log_capacity = 2G;
ERROR: Cannot modify innodb-redo-log-capacity in read-only mode
```

### 2. 文件系统错误

```cpp
// 磁盘空间不足
if (os_has_said_disk_full) {
    return DB_OUT_OF_DISK_SPACE;
}

// 文件操作失败
if (!os_file_create(...)) {
    ib::error("Failed to create redo log file");
    return DB_ERROR;
}
```

### 3. 并发冲突

```cpp
// 并发margin不安全
if (!log_sys->concurrency_margin_is_safe.load()) {
    push_warning_printf(thd, 
        "Current innodb_redo_log_capacity is too small for safety"
        " of redo log files.");
}
```

---

## 监控与诊断

### 1. 查看当前状态

```sql
-- 查看redo log容量设置
SHOW VARIABLES LIKE 'innodb_redo_log_capacity';

-- 查看redo log使用情况
SELECT * FROM performance_schema.innodb_redo_log_files;

-- 查看resize进度（error log）
-- 日志示例:
-- [Note] InnoDB: Redo log resize requested from 1024 MB to 2048 MB
-- [Note] InnoDB: Redo log resize finished: 2048 MB
```

### 2. Performance Schema表

```sql
-- Redo log文件信息
SELECT 
    FILE_ID,
    START_LSN,
    END_LSN,
    SIZE_IN_BYTES,
    IS_FULL,
    CONSUMER_LEVEL
FROM performance_schema.innodb_redo_log_files;
```

### 3. Error Log关键消息

```
[Note] InnoDB: Redo log resize requested from X MB to Y MB
[Note] InnoDB: Redo log resize finished: Y MB
[Note] InnoDB: Redo log resize cancelled
[Warning] InnoDB: Current innodb_redo_log_capacity is too small for safety
```

---

## 最佳实践

### 1. 容量规划

```sql
-- 根据工作负载选择合适的容量
-- 高并发写入: 建议 >= 1GB
-- 批量加载: 建议 >= 8GB
-- 一般OLTP: 100MB-1GB

-- 示例
SET GLOBAL innodb_redo_log_capacity = 1073741824;  -- 1GB
```

### 2. 调整时机

- **扩容**: 任何时候都可以，瞬时完成
- **缩容**: 建议在低峰期进行，可能需要等待

### 3. 监控指标

- Redo log写入速率
- Checkpoint age
- Unused文件数量
- 文件消费速度

---

## 相关源码文件列表

### SQL层
- `sql/sql_parse.cc`: SQL命令解析和分发
- `sql/set_var.cc`: SET语句处理
- `sql/set_var.h`: 系统变量框架定义
- `sql/sys_vars.h`: 系统变量类型模板

### InnoDB Handler层
- `storage/innobase/handler/ha_innodb.cc`: InnoDB handler和系统变量定义

### Redo Log核心
- `storage/innobase/log/log0files_governor.cc`: Governor线程实现
- `storage/innobase/log/log0files_capacity.cc`: 容量管理
- `storage/innobase/log/log0files_io.cc`: 文件IO操作
- `storage/innobase/log/log0log.cc`: Redo log核心逻辑

### 头文件
- `storage/innobase/include/log0types.h`: Redo log类型定义
- `storage/innobase/include/log0files_governor.h`: Governor接口
- `storage/innobase/include/log0files_capacity.h`: 容量管理接口
- `storage/innobase/include/srv0srv.h`: 全局变量声明

---

## 总结

`SET GLOBAL innodb_redo_log_capacity` 的执行涉及MySQL的多个层次：

1. **SQL层**: 解析、权限检查、事务性更新
2. **系统变量框架**: 统一的check/update机制
3. **InnoDB存储引擎**: 回调函数处理实际逻辑
4. **后台线程**: Governor异步处理文件操作
5. **文件系统**: 物理文件的创建、删除、resize

整个过程体现了MySQL的几个设计原则：
- **分层架构**: 清晰的职责分离
- **异步处理**: 避免阻塞用户线程
- **渐进式操作**: 缩容不会影响正在进行的事务
- **可靠性**: 完善的错误处理和恢复机制

---

## 参考资料

- MySQL 8.0 官方文档: InnoDB Redo Log
- WL#13781: InnoDB Redo Log Capacity Automatic Configuration
- InnoDB源码注释

---

**文档版本**: 1.0
**生成时间**: 2025-10-15
**适用版本**: MySQL 8.0.30+

