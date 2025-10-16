# SET GLOBAL innodb_redo_log_capacity 执行流程分析

## 1. 概述

本文档分析以下 SQL 语句的执行流程：

```sql
SET GLOBAL innodb_redo_log_capacity = 34359738368;
```

该语句用于动态修改 InnoDB 的 redo log 容量。本文档重点关注**语法解析过程**，对核心实现不做深入分析。

## 2. 整体执行流程

```
客户端发送 SQL
    ↓
词法分析 (Lexer)
    ↓
语法分析 (Parser - sql_yacc.yy)
    ↓
构建解析树 (Parse Tree)
    ↓
执行 (mysql_execute_command)
    ↓
变量解析、检查、更新 (sql_set_variables)
    ↓
InnoDB 回调函数 (innodb_redo_log_capacity_update)
    ↓
返回结果给客户端
```

## 3. 语法解析详解

### 3.1 词法分析 (Lexical Analysis)

SQL 文本首先被词法分析器（Lexer）分解为 token 序列：

- `SET_SYM` - SET 关键字
- `GLOBAL_SYM` - GLOBAL 关键字
- `ident` - 变量名 "innodb_redo_log_capacity"
- `equal` - 等号 =
- `NUM` - 数值 34359738368

词法分析器定义在 `sql/lex.h` 和 `sql/gen_lex_token.cc` 中。

### 3.2 语法分析 (Syntax Analysis)

语法分析器使用 Bison 生成，定义在 `sql/sql_yacc.yy` 文件中。

#### 3.2.1 SET 语句的语法规则

**位置**: `sql/sql_yacc.yy:16091-16096`

```yacc
set:
    SET_SYM start_option_value_list
    {
        $$= NEW_PTN PT_set(@$, @1, $2);
    }
    ;
```

这是 SET 语句的顶层语法规则，它识别 `SET` 关键字并解析后续的选项列表。

#### 3.2.2 选项列表的起始规则

**位置**: `sql/sql_yacc.yy:16100-16145`

```yacc
start_option_value_list:
    option_value_no_option_type option_value_list_continued
    {
        $$= NEW_PTN PT_start_option_value_list_no_type(@$, $1, @1, $2);
    }
  | TRANSACTION_SYM transaction_characteristics
    {
        $$= NEW_PTN PT_start_option_value_list_transaction(@$, $2, @2);
    }
  | option_type start_option_value_list_following_option_type
    {
        $$= NEW_PTN PT_start_option_value_list_type(@$, $1, $2);
    }
  | PASSWORD equal TEXT_STRING_password ...
  | ...
  ;
```

对于我们的 SQL `SET GLOBAL innodb_redo_log_capacity = ...`，匹配第三个分支：
- `option_type` 匹配 `GLOBAL`
- `start_option_value_list_following_option_type` 匹配后续内容

#### 3.2.3 选项类型 (option_type)

**位置**: `sql/sql_yacc.yy:16263-16269`

```yacc
option_type:
    GLOBAL_SYM  { $$=OPT_GLOBAL; }
  | PERSIST_SYM { $$=OPT_PERSIST; }
  | PERSIST_ONLY_SYM { $$=OPT_PERSIST_ONLY; }
  | LOCAL_SYM   { $$=OPT_SESSION; }
  | SESSION_SYM { $$=OPT_SESSION; }
  ;
```

`GLOBAL_SYM` 被识别为 `OPT_GLOBAL` 类型（定义在 `sql/set_var.h:92-98`）：

```cpp
enum enum_var_type : int {
  OPT_DEFAULT = 0,
  OPT_SESSION,
  OPT_GLOBAL,
  OPT_PERSIST,
  OPT_PERSIST_ONLY
};
```

#### 3.2.4 跟随选项类型后的赋值语句

**位置**: `sql/sql_yacc.yy:16295-16301`

```yacc
option_value_following_option_type:
    lvalue_variable equal set_expr_or_default
    {
        $$ = NEW_PTN PT_set_scoped_system_variable(
            @$, @1, $1.prefix, $1.name, $3);
    }
    ;
```

这里创建了一个 `PT_set_scoped_system_variable` 解析树节点，包含：
- 变量名前缀（对于简单变量为空）
- 变量名（`innodb_redo_log_capacity`）
- 要设置的值（`34359738368`）

#### 3.2.5 左值变量 (lvalue_variable)

**位置**: `sql/sql_yacc.yy:16340-16365`

```yacc
lvalue_variable:
    lvalue_ident
    {
        $$ = Bipartite_name{{}, to_lex_cstring($1)};
    }
  | lvalue_ident '.' ident
    {
        /* 处理带前缀的变量名，如 component_name.variable_name */
        ...
    }
    ;
```

对于 `innodb_redo_log_capacity`，它是一个简单标识符，没有前缀。

### 3.3 解析树构建

语法分析器构建解析树（Parse Tree），主要涉及以下类：

**位置**: `sql/parse_tree_nodes.h` 和 `sql/parse_tree_nodes.cc`

- `PT_set`: 顶层 SET 语句节点
- `PT_start_option_value_list_type`: 带类型的选项列表
- `PT_set_scoped_system_variable`: 作用域系统变量赋值

在解析阶段，解析树节点会调用 `add_system_variable_assignment()` 函数：

**位置**: `sql/parse_tree_nodes.cc:470-497`

```cpp
static bool add_system_variable_assignment(THD *thd, LEX_CSTRING prefix,
                                           LEX_CSTRING suffix,
                                           enum enum_var_type var_type,
                                           Item *val) {
  System_variable_tracker var_tracker = System_variable_tracker::make_tracker(
      to_string_view(prefix), to_string_view(suffix));
  if (var_tracker.access_system_variable(thd)) {
    return true;
  }
  
  // 创建 set_var 对象并添加到 LEX 的变量列表中
  // ...
}
```

这个函数负责：
1. 创建 `System_variable_tracker` 对象来追踪变量
2. 验证系统变量是否存在
3. 创建 `set_var` 对象
4. 将 `set_var` 添加到 `LEX::var_list` 中

### 3.4 SET 命令类型设置

**位置**: `sql/parse_tree_helpers.cc:175` 和 `sql/parse_tree_nodes.cc:4312`

在解析完成后，设置 SQL 命令类型：

```cpp
lex->sql_command = SQLCOM_SET_OPTION;
```

这个枚举值标识当前 SQL 是一条 SET 语句，后续执行时会根据这个值进行分发。

## 4. 执行阶段

### 4.1 命令分发

**位置**: `sql/sql_parse.cc:3994-4022`

```cpp
case SQLCOM_SET_OPTION: {
  List<set_var_base> *lex_var_list = &lex->var_list;

  // 检查表访问权限
  if (check_table_access(thd, SELECT_ACL, all_tables, false, UINT_MAX, false))
    goto error;

  // 打开相关表
  if (open_tables_for_query(thd, all_tables, false)) goto error;
  
  // 执行 SET 变量操作
  if (!(res = sql_set_variables(thd, lex_var_list, true)))
    my_ok(thd);
  else {
    if (!thd->is_error()) my_error(ER_WRONG_ARGUMENTS, MYF(0), "SET");
    goto error;
  }
  break;
}
```

### 4.2 变量设置流程 (sql_set_variables)

**位置**: `sql/set_var.cc:1606-1733`

`sql_set_variables()` 函数是 SET 语句执行的核心，流程如下：

```cpp
int sql_set_variables(THD *thd, List<set_var_base> *var_list, bool opened) {
  int error;
  List_iterator_fast<set_var_base> it(*var_list);

  // 阶段1: 解析所有变量 (resolve)
  while ((var = it++)) {
    if ((error = var->resolve(thd))) goto err;
  }
  
  // 阶段2: 检查所有变量 (check)
  it.rewind();
  while ((var = it++)) {
    if ((error = var->check(thd))) goto err;
  }
  
  // 阶段3: 更新所有变量 (update)
  it.rewind();
  while ((var = it++)) {
    if ((error = var->update(thd))) goto err;
  }
  
  // 阶段4: 持久化变量（如果需要）
  // ...
}
```

#### 4.2.1 变量解析 (resolve)

**位置**: `sql/set_var.cc:1835-1906`

```cpp
int set_var::resolve(THD *thd) {
  auto f = [this, thd](const System_variable_tracker &, sys_var *var) -> int {
    // 1. 发出废弃警告（如果变量已废弃）
    var->do_deprecated_warning(thd);
    
    // 2. 检查是否只读
    if (var->is_readonly()) {
      if (type != OPT_PERSIST_ONLY) {
        my_error(ER_INCORRECT_GLOBAL_LOCAL_VAR, MYF(0), var->name.str, "read only");
        return -1;
      }
    }
    
    // 3. 检查作用域（GLOBAL/SESSION）
    if (!var->check_scope(type)) {
      const int err = (is_global_persist()) ? ER_LOCAL_VARIABLE : ER_GLOBAL_VARIABLE;
      my_error(err, MYF(0), var->name.str);
      return -1;
    }
    
    // 4. 检查权限
    if (type == OPT_GLOBAL || type == OPT_PERSIST) {
      if (check_priv(thd, false)) {
        return -1;
      }
    }
    
    // 5. 修正表达式字段（fix_fields）
    if (value && !value->fixed) {
      if (value->fix_fields(thd, &value)) {
        return -1;
      }
    }
    
    return 0;
  };

  return m_var_tracker
      .access_system_variable<int>(thd, f, Suppress_not_found_error::NO)
      .value_or(-1);
}
```

#### 4.2.2 变量检查 (check)

**位置**: `sql/set_var.cc:1918-1948`

```cpp
int set_var::check(THD *thd) {
  if (value == nullptr) {
    return 0;  // SET ... = DEFAULT
  }

  auto f = [this, thd](const System_variable_tracker &, sys_var *var) -> int {
    // 1. 检查值的类型是否匹配
    if (var->check_update_type(value->result_type())) {
      my_error(ER_WRONG_TYPE_FOR_VAR, MYF(0), var->name.str);
      return -1;
    }
    
    // 2. 调用变量特定的检查函数
    return var->check(thd, this) ? -1 : 0;
  };

  int ret = m_var_tracker
      .access_system_variable<int>(thd, f, Suppress_not_found_error::NO)
      .value_or(-1);

  // 3. 触发审计事件
  if (!ret && (is_global_persist())) {
    ret = mysql_event_tracking_global_variable_notify(
        thd, AUDIT_EVENT(EVENT_TRACKING_GLOBAL_VARIABLE_SET),
        m_var_tracker.get_var_name(), value->item_name.ptr(),
        value->item_name.length());
  }

  return ret;
}
```

#### 4.2.3 变量更新 (update)

**位置**: `sql/set_var.cc:2032-2061`

```cpp
int set_var::update(THD *thd) {
  auto f = [this, thd](const System_variable_tracker &, sys_var *var) -> bool {
    bool ret = false;
    
    // 对于 PERSIST_ONLY，不更新当前会话的值
    if (type != OPT_PERSIST_ONLY) {
      auto saved_var_source = var->get_source();
      var->set_source(enum_variable_source::DYNAMIC);
      
      // 调用变量的更新函数
      if (value)
        ret = var->update(thd, this);
      else
        ret = var->set_default(thd, this);
      
      var->set_source(saved_var_source);
      
      // 更新变量的源信息（用户、主机、时间戳）
      if (!ret) {
        update_source_user_host_timestamp(thd, var);
      }
    }
    return ret;
  };
  
  return m_var_tracker
      .access_system_variable<bool>(thd, f, Suppress_not_found_error::NO)
      .value_or(true) ? 1 : 0;
}
```

### 4.3 系统变量架构

#### 4.3.1 System_variable_tracker

**位置**: `sql/set_var.h:577-953`

`System_variable_tracker` 是一个封装类，用于追踪和访问系统变量。它支持四种类型的变量：

1. **STATIC**: 静态系统变量（编译时定义）
2. **KEYCACHE**: MyISAM 多键缓存变量
3. **PLUGIN**: 插件注册的变量
4. **COMPONENT**: 组件注册的变量

主要方法：
- `make_tracker()`: 静态工厂方法，根据变量名创建 tracker
- `access_system_variable()`: 安全地访问系统变量，处理锁和生命周期

#### 4.3.2 sys_var 基类

**位置**: `sql/set_var.h:107-413`

`sys_var` 是所有系统变量的基类，定义了变量的基本属性和行为：

```cpp
class sys_var {
public:
  LEX_CSTRING name;               // 变量名
  int flags;                      // 标志位（GLOBAL, SESSION, READONLY等）
  SHOW_TYPE show_val_type;        // 显示类型
  my_option option;               // 选项配置（min, max, default）
  ptrdiff_t offset;              // 在 global_system_variables 中的偏移
  on_check_function on_check;     // 检查回调函数
  on_update_function on_update;   // 更新回调函数
  
  // 主要方法
  bool check(THD *thd, set_var *var);              // 检查新值
  bool update(THD *thd, set_var *var);             // 更新变量值
  bool check_scope(enum_var_type query_type);     // 检查作用域
  // ...
};
```

#### 4.3.3 变量标志位

**位置**: `sql/set_var.h:128-154`

```cpp
enum flag_enum {
  GLOBAL = 0x0001,              // 全局变量
  SESSION = 0x0002,             // 会话变量
  ONLY_SESSION = 0x0004,        // 仅会话变量
  SCOPE_MASK = 0x03FF,
  READONLY = 0x0400,            // 只读变量
  ALLOCATED = 0x0800,           // 已分配
  INVISIBLE = 0x1000,           // 不可见
  TRI_LEVEL = 0x2000,           // 三级变量
  NOTPERSIST = 0x4000,          // 不可持久化
  HINT_UPDATEABLE = 0x8000,     // 可通过 hint 更新
  PERSIST_AS_READ_ONLY = 0x10000, // 持久化为只读
  SENSITIVE = 0x20000            // 敏感变量
};
```

### 4.4 InnoDB 变量定义

#### 4.4.1 innodb_redo_log_capacity 变量定义

**位置**: `storage/innobase/handler/ha_innodb.cc:23868-23873`

```cpp
static MYSQL_SYSVAR_ULONGLONG(
    redo_log_capacity, srv_redo_log_capacity,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_PERSIST_AS_READ_ONLY,
    "Limitation for total size of redo log files on disk (expressed in bytes).",
    nullptr, innodb_redo_log_capacity_update, 100 * 1024 * 1024,
    LOG_CAPACITY_MIN, LOG_CAPACITY_MAX, MB);
```

参数说明：
- **变量名**: `redo_log_capacity`（实际使用时带 innodb_ 前缀）
- **C++ 变量**: `srv_redo_log_capacity`
- **标志**: 
  - `PLUGIN_VAR_RQCMDARG`: 需要命令行参数
  - `PLUGIN_VAR_PERSIST_AS_READ_ONLY`: 可持久化为只读
- **描述**: Redo log 文件总大小限制
- **检查函数**: nullptr（无特殊检查）
- **更新函数**: `innodb_redo_log_capacity_update`
- **默认值**: 100 MB
- **最小值**: `LOG_CAPACITY_MIN` (8 MB)
- **最大值**: `LOG_CAPACITY_MAX` (128 TB)
- **块大小**: MB (1048576)

#### 4.4.2 更新回调函数

**位置**: `storage/innobase/handler/ha_innodb.cc:23014-23048`

```cpp
static void innodb_redo_log_capacity_update(THD *thd, SYS_VAR *, void *,
                                            const void *save) {
  const auto new_value = *static_cast<const ulonglong *>(save);

  // 1. 断言检查：确保新值在有效范围内
  ut_a(LOG_CAPACITY_MIN <= new_value);
  ut_a(new_value <= LOG_CAPACITY_MAX);
  ut_a(new_value % MB == 0);

  // 2. 检查只读模式
  if (srv_read_only_mode) {
    my_error(ER_CANT_CHANGE_SYS_VAR_IN_READ_ONLY_MODE, MYF(0),
             "innodb-redo-log-capacity");
    return;
  }

  // 3. 更新全局变量
  srv_redo_log_capacity = new_value;

  // 4. 如果值没有变化，直接返回
  if (new_value == srv_redo_log_capacity_used) {
    return;
  }

  // 5. 更新实际使用的容量
  srv_redo_log_capacity_used = new_value;

  // 6. 记录日志
  ib::info(ER_IB_MSG_LOG_FILES_CAPACITY_CHANGED,
           srv_redo_log_capacity_used / MB);

  // 7. 请求 redo log 文件重新调整大小
  log_files_resize_requested(*log_sys);

  // 8. 检查并发安全裕度，必要时发出警告
  if (!log_sys->concurrency_margin_is_safe.load()) {
    push_warning_printf(thd, Sql_condition::SL_WARNING, ER_WRONG_ARGUMENTS,
                        "Current innodb_redo_log_capacity"
                        " is too small for safety of redo log files."
                        " Consider increasing it or decreasing"
                        " innodb_thread_concurrency.");
  }
}
```

这个回调函数的职责：
1. **验证新值**: 确保值在有效范围内且是 MB 的倍数
2. **检查模式**: 在只读模式下不允许修改
3. **更新变量**: 更新全局配置变量
4. **触发调整**: 调用 `log_files_resize_requested()` 触发 redo log 文件重新调整
5. **安全检查**: 检查并发安全裕度并在需要时发出警告

## 5. 关键数据结构

### 5.1 set_var

**位置**: `sql/set_var.h:992-1038`

```cpp
class set_var : public set_var_base {
public:
  Item *value;                          // 要设置的值（表达式）
  const enum_var_type type;             // 变量类型（GLOBAL/SESSION/PERSIST等）
  
  union {                               // 临时存储检查后的值
    ulonglong ulonglong_value;
    double double_value;
    plugin_ref plugin;
    Time_zone *time_zone;
    LEX_STRING string_value;
    const void *ptr;
  } save_result;

  const System_variable_tracker m_var_tracker;  // 变量追踪器

  // 主要方法
  int resolve(THD *thd) override;       // 解析变量
  int check(THD *thd) override;         // 检查新值
  int update(THD *thd) override;        // 更新变量
};
```

### 5.2 LEX (Lexical Context)

**位置**: `sql/sql_lex.h`

```cpp
struct LEX {
  enum_sql_command sql_command;         // SQL 命令类型
  List<set_var_base> var_list;         // SET 语句的变量列表
  Query_block *query_block;             // 查询块
  // ... 其他字段
};
```

### 5.3 THD (Thread Descriptor)

**位置**: `sql/sql_class.h`

```cpp
class THD {
  LEX *lex;                             // 当前 SQL 的词法上下文
  Security_context *m_security_ctx;     // 安全上下文（权限）
  System_variables variables;           // 会话变量
  // ... 其他字段
};
```

## 6. 执行流程图

```
┌─────────────────────────────────────────────────────────┐
│ 客户端发送 SQL:                                          │
│ SET GLOBAL innodb_redo_log_capacity = 34359738368;      │
└────────────────────┬────────────────────────────────────┘
                     ↓
┌────────────────────────────────────────────────────────┐
│ 1. 词法分析 (Lexer)                                     │
│    - SET_SYM                                            │
│    - GLOBAL_SYM                                         │
│    - ident: "innodb_redo_log_capacity"                 │
│    - equal                                              │
│    - NUM: 34359738368                                   │
└────────────────────┬───────────────────────────────────┘
                     ↓
┌────────────────────────────────────────────────────────┐
│ 2. 语法分析 (Parser - sql_yacc.yy)                     │
│    set → SET_SYM start_option_value_list               │
│         ↓                                               │
│    start_option_value_list                             │
│         → option_type start_option_value_list_...      │
│         ↓                                               │
│    option_type → GLOBAL_SYM (OPT_GLOBAL)               │
│         ↓                                               │
│    option_value_following_option_type                  │
│         → lvalue_variable equal set_expr_or_default    │
│         ↓                                               │
│    lvalue_variable → "innodb_redo_log_capacity"        │
│    set_expr_or_default → 34359738368                   │
└────────────────────┬───────────────────────────────────┘
                     ↓
┌────────────────────────────────────────────────────────┐
│ 3. 构建解析树                                           │
│    PT_set                                               │
│      └─ PT_start_option_value_list_type                │
│           └─ PT_set_scoped_system_variable             │
│                 ├─ var_type: OPT_GLOBAL                │
│                 ├─ var_name: innodb_redo_log_capacity  │
│                 └─ value: 34359738368                  │
└────────────────────┬───────────────────────────────────┘
                     ↓
┌────────────────────────────────────────────────────────┐
│ 4. 添加到变量列表                                       │
│    add_system_variable_assignment()                    │
│      ├─ 创建 System_variable_tracker                   │
│      ├─ 验证变量存在                                    │
│      └─ 创建 set_var 对象 → LEX::var_list             │
│                                                         │
│    设置命令类型: lex->sql_command = SQLCOM_SET_OPTION  │
└────────────────────┬───────────────────────────────────┘
                     ↓
┌────────────────────────────────────────────────────────┐
│ 5. 执行阶段 (mysql_execute_command)                    │
│    case SQLCOM_SET_OPTION:                             │
│      ├─ 检查表访问权限                                  │
│      ├─ 打开相关表                                      │
│      └─ 调用 sql_set_variables()                       │
└────────────────────┬───────────────────────────────────┘
                     ↓
┌────────────────────────────────────────────────────────┐
│ 6. sql_set_variables() 三阶段处理                      │
│                                                         │
│  阶段 1: resolve() - 解析变量                          │
│    ├─ 检查变量是否废弃                                  │
│    ├─ 检查是否只读                                      │
│    ├─ 检查作用域 (GLOBAL/SESSION)                      │
│    ├─ 检查权限 (SYSTEM_VARIABLES_ADMIN)               │
│    └─ 修正表达式字段 (fix_fields)                      │
│                                                         │
│  阶段 2: check() - 检查值                              │
│    ├─ 检查值类型                                        │
│    ├─ 调用变量特定检查函数                              │
│    └─ 触发审计事件                                      │
│                                                         │
│  阶段 3: update() - 更新变量                           │
│    ├─ 调用 var->update()                               │
│    └─ 更新源信息（用户/主机/时间戳）                   │
└────────────────────┬───────────────────────────────────┘
                     ↓
┌────────────────────────────────────────────────────────┐
│ 7. InnoDB 回调: innodb_redo_log_capacity_update()     │
│    ├─ 验证新值范围                                      │
│    ├─ 检查只读模式                                      │
│    ├─ 更新 srv_redo_log_capacity                       │
│    ├─ 更新 srv_redo_log_capacity_used                  │
│    ├─ 记录日志                                          │
│    ├─ 调用 log_files_resize_requested()               │
│    └─ 检查并发安全裕度                                  │
└────────────────────┬───────────────────────────────────┘
                     ↓
┌────────────────────────────────────────────────────────┐
│ 8. 返回结果                                             │
│    my_ok(thd) → 客户端收到 "Query OK"                  │
└────────────────────────────────────────────────────────┘
```

## 7. 关键文件索引

| 文件 | 主要内容 | 行号参考 |
|------|---------|---------|
| `sql/sql_yacc.yy` | 语法规则定义 | 16091-16365 |
| `sql/lex.h` | 词法分析器定义 | - |
| `sql/set_var.h` | 系统变量相关类定义 | 1-1168 |
| `sql/set_var.cc` | SET 语句执行逻辑 | 1606-2061 |
| `sql/sql_parse.cc` | SQL 命令分发 | 3994-4022 |
| `sql/parse_tree_nodes.h` | 解析树节点定义 | - |
| `sql/parse_tree_nodes.cc` | 解析树节点实现 | 470-497, 4312 |
| `sql/parse_tree_helpers.cc` | 解析辅助函数 | 175, 205-268 |
| `storage/innobase/handler/ha_innodb.cc` | InnoDB 变量定义和回调 | 23868-23048 |
| `storage/innobase/include/srv0srv.h` | InnoDB 全局变量声明 | 476-487 |

## 8. 重要概念

### 8.1 变量作用域

- **GLOBAL (OPT_GLOBAL)**: 全局变量，影响所有会话
- **SESSION (OPT_SESSION)**: 会话变量，只影响当前会话
- **PERSIST (OPT_PERSIST)**: 持久化全局变量，重启后保留
- **PERSIST_ONLY (OPT_PERSIST_ONLY)**: 仅持久化，不影响当前运行

### 8.2 三阶段提交模式

SET 语句使用类似事务的三阶段处理：

1. **Resolve**: 解析并验证变量和权限
2. **Check**: 检查新值的有效性
3. **Update**: 应用新值

这种设计确保：
- 要么所有变量都成功更新
- 要么全部失败，不会出现部分成功的情况

### 8.3 权限检查

修改全局变量需要以下权限之一：
- `SUPER_ACL`: 超级管理员权限
- `SYSTEM_VARIABLES_ADMIN`: 系统变量管理权限

权限检查在 `set_var::resolve()` 阶段完成。

### 8.4 插件变量 vs 静态变量

- **静态变量**: 编译时定义，存储在 `static_system_variable_hash` 中
- **插件变量**: 运行时注册，存储在 `dynamic_system_variable_hash` 中

`innodb_redo_log_capacity` 是 InnoDB 插件注册的变量。

## 9. 总结

SET GLOBAL 语句的执行流程可以概括为：

1. **词法分析**: 将 SQL 文本分解为 token 序列
2. **语法分析**: 使用 Bison 生成的解析器识别语法结构
3. **构建解析树**: 创建 PT_* 节点表示 SQL 结构
4. **变量解析**: 创建 set_var 对象并添加到 LEX
5. **命令执行**: 在 mysql_execute_command 中分发到 SQLCOM_SET_OPTION 分支
6. **三阶段处理**: 
   - Resolve: 验证变量和权限
   - Check: 验证值的有效性
   - Update: 应用新值并调用回调函数
7. **InnoDB 回调**: 执行存储引擎特定的逻辑（如调整 redo log 大小）
8. **返回结果**: 向客户端返回成功或错误信息

整个流程体现了 MySQL 的模块化设计：
- SQL 层负责语法解析、权限检查和通用逻辑
- 存储引擎层（InnoDB）通过回调函数实现特定的业务逻辑
- 使用 System_variable_tracker 抽象不同类型的变量访问

---

**文档版本**: 1.0  
**创建日期**: 2025-10-15  
**适用版本**: Percona Server 8.4.3-3

