# 临时拦截 SET GLOBAL innodb_redo_log_capacity 方案

## 1. 需求说明

临时拦截 `SET GLOBAL innodb_redo_log_capacity` 语句，使其返回语法错误，而不是实际执行。

**拦截效果示例**：
```sql
mysql> SET GLOBAL innodb_redo_log_capacity = 34359738368;
ERROR 1235 (42000): This version of MySQL doesn't yet support 'SET GLOBAL innodb_redo_log_capacity'
```

## 2. 解决方案

### 2.1 修改位置

**文件**: `sql/set_var.cc`  
**函数**: `set_var::resolve(THD *thd)`  
**行号**: 1835-1850

### 2.2 修改内容

在 `set_var::resolve()` 函数开始处添加拦截逻辑：

```cpp
int set_var::resolve(THD *thd) {
  DBUG_TRACE;

  // ===== TEMPORARY: Block SET GLOBAL innodb_redo_log_capacity =====
  if (type == OPT_GLOBAL || type == OPT_PERSIST || type == OPT_PERSIST_ONLY) {
    const char *var_name = m_var_tracker.get_var_name();
    if (var_name && 
        (strcasecmp(var_name, "innodb_redo_log_capacity") == 0 ||
         strcasecmp(var_name, "redo_log_capacity") == 0)) {
      my_error(ER_PARSE_ERROR, MYF(0), 
               "You have an error in your SQL syntax",
               "near 'innodb_redo_log_capacity'", 1);
      return -1;
    }
  }
  // ===== END TEMPORARY BLOCK =====

  auto f = [this, thd](const System_variable_tracker &, sys_var *var) -> int {
    // ... 原有代码继续
```

### 2.3 实现原理

1. **拦截时机**: 在变量解析（resolve）阶段，这是三阶段处理的第一阶段
2. **检查条件**: 
   - 变量类型必须是 `GLOBAL`/`PERSIST`/`PERSIST_ONLY`
   - 变量名是 `innodb_redo_log_capacity`（不区分大小写）
3. **错误处理**: 使用 `ER_NOT_SUPPORTED_YET` 返回"不支持"错误（ERROR 1235）
4. **返回值**: `-1` 表示失败，终止后续处理

### 2.4 拦截范围

此方案会拦截以下所有形式：

```sql
SET GLOBAL innodb_redo_log_capacity = 值;
SET @@GLOBAL.innodb_redo_log_capacity = 值;
SET PERSIST innodb_redo_log_capacity = 值;
SET PERSIST_ONLY innodb_redo_log_capacity = 值;
```

**不会拦截**：
```sql
-- 读取变量值（SHOW 和 SELECT 仍然可用）
SHOW VARIABLES LIKE 'innodb_redo_log_capacity';
SELECT @@GLOBAL.innodb_redo_log_capacity;

-- SESSION 级别（虽然这个变量不支持 SESSION）
SET SESSION innodb_redo_log_capacity = 值;  -- 会报其他错误
```

## 3. 编译和部署

### 3.1 编译

由于只修改了一个 `.cc` 文件，增量编译很快：

```bash
cd /Users/baoma.xie/Desktop/percona-server

# 如果已经配置过构建目录
cd build  # 或你的构建目录

# 增量编译（只编译修改的文件）
make -j$(nproc)

# 或者只编译受影响的目标
make sql_main
```

### 3.2 重启服务

```bash
# 停止 MySQL 服务
sudo systemctl stop mysql
# 或
mysqladmin -u root -p shutdown

# 启动 MySQL 服务
sudo systemctl start mysql
# 或直接启动二进制文件
```

### 3.3 无需重启的部署方式

如果不想重启 MySQL：
1. 在开发/测试环境验证
2. 计划维护窗口进行部署
3. 或使用热备份/主从切换方式

## 4. 测试方案

### 4.1 功能测试

```sql
-- 测试1: 应该被拦截（返回不支持错误）
SET GLOBAL innodb_redo_log_capacity = 34359738368;
-- 预期: ERROR 1235 (42000): This version of MySQL doesn't yet support...

-- 测试2: 应该被拦截（不同的写法）
SET @@GLOBAL.innodb_redo_log_capacity = 16777216;
-- 预期: ERROR 1235 (42000): This version of MySQL doesn't yet support...

-- 测试3: PERSIST 应该被拦截
SET PERSIST innodb_redo_log_capacity = 20971520;
-- 预期: ERROR 1235 (42000): This version of MySQL doesn't yet support...

-- 测试4: 大小写不敏感
SET GLOBAL INNODB_REDO_LOG_CAPACITY = 34359738368;
-- 预期: ERROR 1235 (42000): This version of MySQL doesn't yet support...

-- 测试5: 查看变量仍然可用
SHOW VARIABLES LIKE 'innodb_redo_log_capacity';
-- 预期: 正常显示

-- 测试6: SELECT 仍然可用
SELECT @@GLOBAL.innodb_redo_log_capacity;
-- 预期: 正常显示当前值

-- 测试7: 其他 InnoDB 变量不受影响
SET GLOBAL innodb_buffer_pool_size = 134217728;
-- 预期: 正常执行

-- 测试8: 其他 GLOBAL 变量不受影响
SET GLOBAL max_connections = 200;
-- 预期: 正常执行
```

### 4.2 测试脚本

创建测试文件 `test_redo_log_block.sql`:

```sql
-- 测试拦截功能
SELECT '=== Test 1: Direct SET GLOBAL ===' AS test;
SET GLOBAL innodb_redo_log_capacity = 34359738368;

SELECT '=== Test 2: SET @@GLOBAL ===' AS test;
SET @@GLOBAL.innodb_redo_log_capacity = 16777216;

SELECT '=== Test 3: SET PERSIST ===' AS test;
SET PERSIST innodb_redo_log_capacity = 20971520;

SELECT '=== Test 4: Uppercase ===' AS test;
SET GLOBAL INNODB_REDO_LOG_CAPACITY = 34359738368;

SELECT '=== Test 5: SHOW VARIABLES (should work) ===' AS test;
SHOW VARIABLES LIKE 'innodb_redo_log_capacity';

SELECT '=== Test 6: SELECT (should work) ===' AS test;
SELECT @@GLOBAL.innodb_redo_log_capacity;

SELECT '=== Test 7: Other InnoDB variables (should work) ===' AS test;
SHOW VARIABLES LIKE 'innodb_buffer_pool_size';

SELECT '=== Test 8: SET other variable (should work) ===' AS test;
SET GLOBAL max_connections = @@GLOBAL.max_connections;

SELECT '=== All tests completed ===' AS test;
```

运行测试：

```bash
mysql -u root -p < test_redo_log_block.sql
```

## 5. 回滚方案

如果需要移除拦截，只需删除添加的代码块：

### 5.1 回滚修改

在 `sql/set_var.cc` 文件的 `set_var::resolve()` 函数中，删除以下代码块：

```cpp
  // ===== TEMPORARY: Block SET GLOBAL innodb_redo_log_capacity =====
  if (type == OPT_GLOBAL || type == OPT_PERSIST || type == OPT_PERSIST_ONLY) {
    const char *var_name = m_var_tracker.get_var_name();
    if (var_name && (strcasecmp(var_name, "innodb_redo_log_capacity") == 0)) {
      my_error(ER_NOT_SUPPORTED_YET, MYF(0), 
               "SET GLOBAL innodb_redo_log_capacity");
      return -1;
    }
  }
  // ===== END TEMPORARY BLOCK =====
```

### 5.2 重新编译和部署

```bash
cd build
make -j$(nproc)
# 重启 MySQL 服务
```

### 5.3 使用 Git 回滚

如果使用了版本控制：

```bash
cd /Users/baoma.xie/Desktop/percona-server
git checkout sql/set_var.cc
# 重新编译和部署
```

## 6. 优点和限制

### 6.1 优点

✅ **改动最小**: 只修改一个文件，添加约 12 行代码  
✅ **易于回滚**: 删除代码块即可恢复  
✅ **编译快速**: 增量编译只需重新编译一个 .cc 文件  
✅ **精确拦截**: 只拦截目标变量，不影响其他功能  
✅ **返回标准错误**: 使用 MySQL 标准的语法错误格式  
✅ **不影响读取**: SHOW 和 SELECT 操作仍然可用  

### 6.2 限制

❌ **需要重启**: 修改需要重新编译和重启 MySQL  
❌ **所有用户受影响**: 包括 root 用户也无法设置  
❌ **错误信息固定**: 无法自定义更详细的错误提示  

## 7. 替代方案（可选）

### 方案二：使用 ER_PARSE_ERROR（语法错误）

如果想返回语法错误（ERROR 1064），可以使用：

```cpp
if (type == OPT_GLOBAL || type == OPT_PERSIST || type == OPT_PERSIST_ONLY) {
  const char *var_name = m_var_tracker.get_var_name();
  if (var_name && (strcasecmp(var_name, "innodb_redo_log_capacity") == 0)) {
    my_error(ER_PARSE_ERROR, MYF(0), 
             "You have an error in your SQL syntax",
             "innodb_redo_log_capacity",  // 注意：不要包含 "near"
             1);
    return -1;
  }
}
```

这样会返回：
```
ERROR 1064 (42000): You have an error in your SQL syntax near 'innodb_redo_log_capacity' at line 1
```

**注意**: `ER_PARSE_ERROR` 的格式是 `"%s near '%-.80s' at line %d"`，会自动添加 "near"，所以第二个参数不要包含 "near"。详见 `错误码对比说明.md`。

### 方案三：基于权限的拦截

如果只想拦截特定用户：

```cpp
if (type == OPT_GLOBAL || type == OPT_PERSIST || type == OPT_PERSIST_ONLY) {
  const char *var_name = m_var_tracker.get_var_name();
  if (var_name && 
      (strcasecmp(var_name, "innodb_redo_log_capacity") == 0 ||
       strcasecmp(var_name, "redo_log_capacity") == 0)) {
    // 只允许特定用户（例如 root@localhost）
    Security_context *sctx = thd->security_context();
    LEX_CSTRING user = sctx->user();
    LEX_CSTRING host = sctx->host();
    
    // 允许 root@localhost，拦截其他用户
    if (strcmp(user.str, "root") != 0 || strcmp(host.str, "localhost") != 0) {
      my_error(ER_PARSE_ERROR, MYF(0), 
               "You have an error in your SQL syntax",
               "near 'innodb_redo_log_capacity'", 1);
      return -1;
    }
  }
}
```

## 8. 注意事项

⚠️ **重要提醒**：

1. **备份配置**: 修改前确保有配置文件备份
2. **测试环境验证**: 先在测试环境验证，再部署到生产环境
3. **监控日志**: 部署后监控错误日志，确认拦截正常工作
4. **文档记录**: 记录修改时间和原因，方便后续追溯
5. **临时性说明**: 代码注释中已标明 "TEMPORARY"，提醒这是临时修改
6. **影响范围**: 所有连接到该 MySQL 实例的应用都会受影响

## 9. 快速参考

### 修改的文件

```
sql/set_var.cc (行 1838-1850)
```

### 编译命令

```bash
cd build && make -j$(nproc)
```

### 测试命令

```bash
mysql -u root -p -e "SET GLOBAL innodb_redo_log_capacity = 34359738368;"
```

### 预期输出

```
ERROR 1235 (42000): This version of MySQL doesn't yet support 'SET GLOBAL innodb_redo_log_capacity'
```

---

**文档版本**: 1.0  
**创建日期**: 2025-10-15  
**修改人**: [您的名字]  
**状态**: 临时拦截 - 待后续移除

