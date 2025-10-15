# SET GLOBAL SQL语法解析流程详解

## 概述

本文档详细分析 MySQL 8.0 中 `SET GLOBAL innodb_redo_log_capacity = 34359738368;` 这条SQL语句的**语法解析流程**，重点关注词法分析和语法分析阶段。

## 解析流程概览

```
SQL字符串 → 词法分析(Lexer) → Token流 → 语法分析(Parser) → 抽象语法树(AST) → 语义分析 → 执行
```

本文档重点关注：**词法分析** 和 **语法分析** 两个阶段。

---

## 一、词法分析（Lexical Analysis）

### 1.1 词法分析器入口

**文件位置**: `sql/sql_lex.cc`

**核心函数**: `MYSQLlex()`

```cpp
// sql/sql_lex.cc: 1302-1369
int MYSQLlex(YYSTYPE *yacc_yylval, YYLTYPE *yylloc, THD *thd) {
  auto *yylval = reinterpret_cast<Lexer_yystype *>(yacc_yylval);
  Lex_input_stream *lip = &thd->m_parser_state->m_lip;
  int token;

  // 检查是否有预读的token
  if (lip->lookahead_token >= 0) {
    token = lip->lookahead_token;
    lip->lookahead_token = -1;
    *yylval = *(lip->lookahead_yylval);
    // ... 返回预读token
    return token;
  }

  // 解析下一个token
  token = lex_one_token(yylval, thd);
  
  // 记录位置信息
  yylloc->cpp.start = lip->get_cpp_tok_start();
  yylloc->raw.start = lip->get_tok_start();
  yylloc->cpp.end = lip->get_cpp_ptr();
  yylloc->raw.end = lip->get_ptr();
  
  // 添加到digest（用于性能统计）
  if (!lip->skip_digest) 
    lip->add_digest_token(token, yylval);
    
  return token;
}
```

**说明**:
- `MYSQLlex()` 是 Bison 解析器调用的词法分析器接口
- 每次调用返回一个 token（词法单元）
- 返回值是 token 的类型编号（如 SET_SYM, GLOBAL_SYM 等）

### 1.2 单个Token的解析

**函数**: `lex_one_token()`

```cpp
// sql/sql_lex.cc: 1371-1404
static int lex_one_token(Lexer_yystype *yylval, THD *thd) {
  uchar c = 0;
  uint length;
  enum my_lex_states state;
  Lex_input_stream *lip = &thd->m_parser_state->m_lip;
  const CHARSET_INFO *cs = thd->charset();
  const my_lex_states *state_map = cs->state_maps->main_map;
  const uchar *ident_map = cs->ident_map;

  lip->start_token();
  state = lip->next_state;
  lip->next_state = MY_LEX_START;
  
  for (;;) {
    switch (state) {
      case MY_LEX_START:
        // 跳过空白字符
        while (state_map[c = lip->yyPeek()] == MY_LEX_SKIP) {
          if (c == '\n') lip->yylineno++;
          lip->yySkip();
        }
        
        // 开始真正的token
        lip->restart_token();
        c = lip->yyGet();
        state = state_map[c];
        break;
        
      case MY_LEX_IDENT:
      case MY_LEX_IDENT_OR_KEYWORD:
        // 识别标识符或关键字
        // ...
        break;
        
      // ... 其他状态
    }
  }
}
```

### 1.3 关键字识别

**文件位置**: `sql/lex.h`, `sql/sql_lex.cc`

**关键字表**: `symbols[]` 数组

```cpp
// sql/lex.h: 61-900+
static const SYMBOL symbols[] = {
    // ...
    {SYM("GLOBAL", GLOBAL_SYM)},      // 行297
    // ...
    {SYM("INNODB", IDENT)},           // 实际上innodb不是关键字，是标识符
    // ...
    {SYM("SET", SET_SYM)},            // 行642
    // ...
};
```

**关键字查找函数**:

```cpp
// sql/sql_lex.cc: 905-936
static int find_keyword(Lex_input_stream *lip, uint len, bool function) {
  const char *tok = lip->get_tok_start();

  // 从哈希表中查找关键字
  const SYMBOL *symbol =
      function ? Lex_hash::sql_keywords_and_funcs.get_hash_symbol(tok, len)
               : Lex_hash::sql_keywords.get_hash_symbol(tok, len);

  if (symbol) {
    lip->yylval->keyword.symbol = symbol;
    lip->yylval->keyword.str = const_cast<char *>(tok);
    lip->yylval->keyword.length = len;
    
    // 返回token类型
    return symbol->tok;
  }
  return 0;  // 不是关键字
}
```

### 1.4 我们的SQL语句的Token流

对于 `SET GLOBAL innodb_redo_log_capacity = 34359738368;`

词法分析器会产生以下 token 序列：

```
Token 1: SET_SYM          ("SET")
Token 2: GLOBAL_SYM       ("GLOBAL")  
Token 3: IDENT            ("innodb_redo_log_capacity")
Token 4: EQ               ("=")
Token 5: NUM              ("34359738368")
Token 6: ';'              (';')
```

**说明**:
- `SET` 和 `GLOBAL` 是保留关键字，被识别为 `SET_SYM` 和 `GLOBAL_SYM`
- `innodb_redo_log_capacity` 不是关键字，被识别为 `IDENT`（标识符）
- `=` 是操作符，token类型为 `EQ`
- `34359738368` 是数字字面量，token类型为 `NUM`
- `;` 是语句结束符

---

## 二、语法分析（Syntax Analysis）

### 2.1 语法分析器概述

MySQL 使用 **Bison** (GNU版本的Yacc) 作为语法分析器生成工具。

**语法定义文件**: `sql/sql_yacc.yy`（约18000行代码）

**生成的解析器**: 在编译时，Bison根据 `.yy` 文件生成 C++ 代码：
- `sql_yacc.h` - Token定义和类型声明
- `sql_yacc.cc` - 解析器实现（LALR(1)状态机）

### 2.2 SET语句的语法规则

#### 2.2.1 SET语句入口规则

```yacc
// sql/sql_yacc.yy: 15746-15751
set:
    SET_SYM start_option_value_list
    {
        $$= NEW_PTN PT_set(@1, $2);
    }
    ;
```

**说明**:
- `set:` 是语法规则的名称
- `SET_SYM` 匹配词法分析器返回的 SET_SYM token
- `start_option_value_list` 是非终结符，表示选项值列表
- `{...}` 中是语义动作，创建解析树节点 `PT_set`

#### 2.2.2 选项值列表的起始规则

```yacc
// sql/sql_yacc.yy: 15755-15767
start_option_value_list:
    option_value_no_option_type option_value_list_continued
    {
        $$= NEW_PTN PT_start_option_value_list_no_type($1, @1, $2);
    }
  | TRANSACTION_SYM transaction_characteristics
    {
        $$= NEW_PTN PT_start_option_value_list_transaction($2, @2);
    }
  | option_type start_option_value_list_following_option_type
    {
        $$= NEW_PTN PT_start_option_value_list_type($1, $2);
    }
  | PASSWORD equal TEXT_STRING_password ...
  | ...
  ;
```

**对于我们的SQL**: 匹配第三个分支：
```
option_type start_option_value_list_following_option_type
```

因为语句以 `GLOBAL` 开头（option_type）。

#### 2.2.3 选项类型（option_type）

```yacc
// sql/sql_yacc.yy: 15918-15924
option_type:
    GLOBAL_SYM  { $$=OPT_GLOBAL; }
  | PERSIST_SYM { $$=OPT_PERSIST; }
  | PERSIST_ONLY_SYM { $$=OPT_PERSIST_ONLY; }
  | LOCAL_SYM   { $$=OPT_SESSION; }
  | SESSION_SYM { $$=OPT_SESSION; }
  ;
```

**对于我们的SQL**: 匹配 `GLOBAL_SYM`，返回 `OPT_GLOBAL` 枚举值。

**枚举定义**（在 `sql/set_var.h`）:

```cpp
// sql/set_var.h: 90-96
enum enum_var_type : int {
  OPT_DEFAULT = 0,
  OPT_SESSION,
  OPT_GLOBAL,
  OPT_PERSIST,
  OPT_PERSIST_ONLY
};
```

#### 2.2.4 跟随option_type的选项值列表

```yacc
// sql/sql_yacc.yy: 15875-15889
start_option_value_list_following_option_type:
    option_value_following_option_type option_value_list_continued
    {
        $$= NEW_PTN PT_start_option_value_list_following_option_type_eq(
                     $1, @1, $2);
    }
  | TRANSACTION_SYM transaction_characteristics
    {
        $$= NEW_PTN PT_start_option_value_list_following_option_type_transaction(
                     $2, @2);
    }
  ;
```

**对于我们的SQL**: 匹配第一个分支。

#### 2.2.5 带option_type前缀的选项值

```yacc
// sql/sql_yacc.yy: 15950-15956
option_value_following_option_type:
    lvalue_variable equal set_expr_or_default
    {
        $$ = NEW_PTN PT_set_scoped_system_variable(
                @1, $1.prefix, $1.name, $3);
    }
    ;
```

**说明**:
- `lvalue_variable` - 左值变量（被赋值的变量名）
- `equal` - 等号
- `set_expr_or_default` - 赋值表达式或DEFAULT

#### 2.2.6 左值变量（lvalue_variable）

```yacc
// sql/sql_yacc.yy: 15995-16017
lvalue_variable:
    lvalue_ident
    {
        $$ = Bipartite_name{{}, to_lex_cstring($1)};
    }
  | lvalue_ident '.' ident
    {
        // 拒绝像 GLOBAL.GLOBAL.foo 这样的名称
        if (check_reserved_words($1.str)) {
            YYTHD->syntax_error_at(@1);
            MYSQL_YYABORT;
        }
        $$ = Bipartite_name{to_lex_cstring($1), to_lex_cstring($3)};
    }
  | DEFAULT_SYM '.' ident
    {
        $$ = Bipartite_name{{STRING_WITH_LEN("default")}, 
                           to_lex_cstring($3)};
    }
  ;
```

**对于我们的SQL**: 
- 变量名 `innodb_redo_log_capacity` 是单个标识符
- 匹配第一个分支
- 返回一个 `Bipartite_name` 结构：`{prefix="", name="innodb_redo_log_capacity"}`

**Bipartite_name 结构说明**:
```cpp
// 用于表示可能带前缀的名称，如：
//   简单形式: "variable_name" -> {prefix="", name="variable_name"}
//   点分形式: "cache.key_buffer_size" -> {prefix="cache", name="key_buffer_size"}
struct Bipartite_name {
    LEX_CSTRING prefix;  // 前缀（可能为空）
    LEX_CSTRING name;    // 名称
};
```

#### 2.2.7 SET表达式

```yacc
// sql/sql_yacc.yy (set_expr_or_default的定义)
set_expr_or_default:
    expr
  | DEFAULT_SYM
  | ON_SYM
  | ALL
  | BINARY_SYM
  ;
```

**对于我们的SQL**: 
- 值 `34359738368` 是一个表达式
- 匹配 `expr` 分支
- 表达式会被解析成一个 `Item` 对象（具体是 `Item_int` - 整数字面量）

### 2.3 解析树节点

解析过程中会创建一系列解析树节点，这些节点定义在 `sql/parse_tree_nodes.h`:

```cpp
// PT_set - SET语句的根节点
class PT_set : public Parse_tree_root {
  // ...
};

// PT_start_option_value_list_type - 带类型前缀的选项列表
class PT_start_option_value_list_type : public PT_start_option_value_list {
  enum_var_type type;  // OPT_GLOBAL, OPT_SESSION等
  // ...
};

// PT_set_scoped_system_variable - 设置有作用域的系统变量
class PT_set_scoped_system_variable : public PT_option_value_no_option_type {
  LEX_CSTRING prefix;  // 变量名前缀（如果有）
  LEX_CSTRING name;    // 变量名
  Item *value;         // 赋值的值
  // ...
};
```

### 2.4 完整的语法分析树

对于 `SET GLOBAL innodb_redo_log_capacity = 34359738368;`

解析器构建的语法树结构：

```
PT_set
  └── PT_start_option_value_list_type (type=OPT_GLOBAL)
        └── PT_start_option_value_list_following_option_type_eq
              └── PT_set_scoped_system_variable
                    ├── prefix: ""
                    ├── name: "innodb_redo_log_capacity"
                    └── value: Item_int(34359738368)
```

---

## 三、语法树到执行对象的转换

### 3.1 语义分析阶段

解析树构建完成后，会调用各节点的 `contextualize()` 方法进行语义分析：

```cpp
// sql/parse_tree_nodes.cc
bool PT_set::contextualize(Parse_context *pc) {
  if (super::contextualize(pc)) return true;
  
  THD *thd = pc->thd;
  LEX *lex = thd->lex;
  lex->sql_command = SQLCOM_SET_OPTION;  // 设置SQL命令类型
  
  // 将选项值列表添加到LEX的var_list中
  return m_option_value_list->contextualize(pc);
}
```

### 3.2 创建set_var对象

在语义分析过程中，解析树节点会被转换为执行对象：

```cpp
// PT_set_scoped_system_variable::contextualize() 中会创建：

set_var *var = new (thd->mem_root) set_var(
    OPT_GLOBAL,                              // type
    System_variable_tracker::make_tracker(   // 变量追踪器
        "",                                   // prefix
        "innodb_redo_log_capacity"           // name
    ),
    value_item                               // Item_int(34359738368)
);

// 添加到LEX的变量列表
lex->var_list.push_back(var);
```

**set_var类定义**（在 `sql/set_var.h`）:

```cpp
// sql/set_var.h: 971-1017
class set_var : public set_var_base {
 public:
  Item *value;                           // 赋值表达式
  const enum_var_type type;              // 变量类型（GLOBAL/SESSION等）
  
  union {                                // 临时存储检查后的值
    ulonglong ulonglong_value;
    double double_value;
    plugin_ref plugin;
    Time_zone *time_zone;
    LEX_STRING string_value;
    const void *ptr;
  } save_result;
  
  const System_variable_tracker m_var_tracker;  // 变量追踪器
  
 public:
  set_var(enum_var_type type_arg, 
          const System_variable_tracker &var_arg,
          Item *value_arg)
      : value{value_arg}, 
        type{type_arg}, 
        m_var_tracker(var_arg) {}

  int resolve(THD *thd) override;  // 解析变量名
  int check(THD *thd) override;    // 检查值的有效性
  int update(THD *thd) override;   // 更新变量值
};
```

### 3.3 System_variable_tracker

这是一个重要的辅助类，用于统一处理不同类型的系统变量：

```cpp
// sql/set_var.h: 574-932
class System_variable_tracker final {
 public:
  enum Lifetime {
    STATIC,     // 静态系统变量（编译时定义）
    KEYCACHE,   // MyISAM多键缓存变量
    PLUGIN,     // 插件注册的变量
    COMPONENT,  // 组件注册的变量
  };

  // 静态工厂方法
  static System_variable_tracker make_tracker(
      std::string_view prefix,   // 前缀（如"default"用于keycache）
      std::string_view suffix    // 变量名
  );
  
  // 访问系统变量的核心方法
  template <typename T>
  std::optional<T> access_system_variable(
      THD *thd,
      std::function<T(const System_variable_tracker &, sys_var *)> function,
      // ... 其他参数
  ) const;
};
```

**对于我们的SQL**:
- `innodb_redo_log_capacity` 是一个静态系统变量（STATIC）
- 不需要加锁就能访问
- 变量定义在 `storage/innobase/handler/ha_innodb.cc` 中

---

## 四、完整的解析流程示意图

```
SQL字符串: "SET GLOBAL innodb_redo_log_capacity = 34359738368;"
    |
    v
[词法分析 - MYSQLlex()]
    |
    +---> Token流:
    |     SET_SYM | GLOBAL_SYM | IDENT | EQ | NUM | ';'
    |
    v
[语法分析 - Bison解析器]
    |
    +---> 匹配语法规则:
    |     set: SET_SYM start_option_value_list
    |         start_option_value_list: option_type start_option_value_list_following_option_type
    |             option_type: GLOBAL_SYM -> OPT_GLOBAL
    |             start_option_value_list_following_option_type:
    |                 option_value_following_option_type
    |                     lvalue_variable: innodb_redo_log_capacity
    |                     equal: =
    |                     set_expr_or_default: expr -> Item_int(34359738368)
    |
    v
[构建解析树]
    |
    +---> PT_set
    |       └── PT_start_option_value_list_type (OPT_GLOBAL)
    |             └── PT_set_scoped_system_variable
    |                   ├── name: "innodb_redo_log_capacity"
    |                   └── value: Item_int(34359738368)
    |
    v
[语义分析 - contextualize()]
    |
    +---> 创建执行对象:
    |     set_var {
    |         type = OPT_GLOBAL
    |         m_var_tracker = System_variable_tracker("innodb_redo_log_capacity")
    |         value = Item_int(34359738368)
    |     }
    |
    +---> 添加到 LEX::var_list
    |
    +---> 设置 LEX::sql_command = SQLCOM_SET_OPTION
    |
    v
[准备执行]
    |
    +---> 解析完成，等待执行
    |
    v
[执行阶段 - mysql_execute_command()]
    |
    +---> case SQLCOM_SET_OPTION:
    |       sql_set_variables(thd, &lex->var_list, true)
    |           ├── var->resolve(thd)  // 解析变量，检查权限和作用域
    |           ├── var->check(thd)    // 检查值的有效性（范围、类型）
    |           └── var->update(thd)   // 更新变量值
    |
    v
[实际更新系统变量]
```

---

## 五、关键数据结构总结

### 5.1 LEX（词法/语法分析器状态）

```cpp
// sql/sql_lex.h
class LEX {
  enum_sql_command sql_command;     // SQL命令类型：SQLCOM_SET_OPTION
  List<set_var_base> var_list;     // SET语句中的变量列表
  // ... 大量其他字段
};
```

### 5.2 Token类型（部分）

```cpp
// 在 sql_yacc.h 中由Bison生成
#define SET_SYM 642
#define GLOBAL_SYM 297
#define IDENT 具体值取决于编译
#define EQ 70
#define NUM 具体值取决于编译
```

### 5.3 解析树节点基类

```cpp
// sql/parse_tree_node_base.h
class Parse_tree_node_tmpl {
 public:
  virtual bool contextualize(Context *pc) { return false; }
  // ...
};
```

---

## 六、关键代码文件索引

### 6.1 词法分析相关

| 文件 | 说明 |
|-----|------|
| `sql/sql_lex.cc` | 词法分析器实现，包含 MYSQLlex() 和 lex_one_token() |
| `sql/sql_lex.h` | 词法分析器类定义 (Lex_input_stream) |
| `sql/lex.h` | 关键字表定义 (symbols数组) |
| `sql/lex_symbol.h` | SYMBOL结构定义 |
| `sql/sql_lex_hash.cc` | 关键字哈希表实现 |

### 6.2 语法分析相关

| 文件 | 说明 |
|-----|------|
| `sql/sql_yacc.yy` | Bison语法定义文件（核心） |
| `sql/parse_tree_nodes.h` | 解析树节点类定义 |
| `sql/parse_tree_nodes.cc` | 解析树节点实现 |
| `sql/parse_tree_node_base.h` | 解析树节点基类 |

### 6.3 SET语句执行相关

| 文件 | 说明 |
|-----|------|
| `sql/set_var.h` | set_var类和System_variable_tracker定义 |
| `sql/set_var.cc` | set_var实现，包含resolve/check/update |
| `sql/sql_parse.cc` | SQL命令分发，包含SET语句的执行入口 |

### 6.4 系统变量定义

| 文件 | 说明 |
|-----|------|
| `sql/sys_vars.cc` | 服务器层系统变量定义 |
| `storage/innobase/handler/ha_innodb.cc` | InnoDB系统变量定义（包含innodb_redo_log_capacity） |

---

## 七、调试技巧

### 7.1 打印Token流

可以在 `MYSQLlex()` 中添加调试代码：

```cpp
int MYSQLlex(YYSTYPE *yacc_yylval, YYLTYPE *yylloc, THD *thd) {
  // ...
  int token = lex_one_token(yylval, thd);
  
  // 调试：打印token
  fprintf(stderr, "Token: %d, Text: %.*s\n", 
          token, 
          (int)(yylloc->raw.end - yylloc->raw.start),
          yylloc->raw.start);
  
  return token;
}
```

### 7.2 查看解析树

可以在 `PT_set::contextualize()` 中添加断点，查看解析树结构。

### 7.3 查看Bison状态机

```bash
# 生成Bison调试输出
bison -v sql/sql_yacc.yy
# 会生成 sql_yacc.output 文件，包含状态机详细信息
```

---

## 八、扩展阅读

### 8.1 关于LALR(1)解析

MySQL使用LALR(1)（Look-Ahead LR）语法分析算法：
- **L**: 从左到右扫描输入（Left-to-right）
- **A**: 最右推导（rightmost derivation in reverse, A for "Automated"）
- **1**: 向前看1个token（1-token lookahead）

LALR(1)是LR(1)的简化版本，状态数更少，但解析能力略弱。

### 8.2 关于Bison

Bison是GNU版本的Yacc（Yet Another Compiler Compiler）：
- 输入: `.y` 或 `.yy` 文件（语法规则）
- 输出: `.c` 或 `.cc` 文件（解析器代码）
- 算法: LALR(1)状态机

### 8.3 MySQL语法的复杂性

MySQL的语法文件 `sql_yacc.yy` 有约18000行：
- 定义了数百个语法规则
- 处理SQL-92、SQL-99、SQL-2003标准
- 包含大量MySQL特有的扩展语法
- 需要处理多种歧义和冲突（shift/reduce conflicts）

当前期望的冲突数:
```yacc
%expect 63  // 表示允许63个shift/reduce冲突
```

---

## 九、总结

### 9.1 解析流程回顾

1. **词法分析**: SQL字符串 → Token流
   - `MYSQLlex()` 逐个返回 token
   - 关键字通过哈希表查找
   - 标识符和字面量被识别

2. **语法分析**: Token流 → 解析树
   - Bison根据 `.yy` 文件中的规则进行匹配
   - 自底向上的LR解析
   - 构建 `PT_*` 类型的解析树节点

3. **语义分析**: 解析树 → 执行对象
   - 调用 `contextualize()` 方法
   - 创建 `set_var` 对象
   - 添加到 `LEX::var_list`

4. **执行准备**: 设置执行环境
   - 设置 `sql_command = SQLCOM_SET_OPTION`
   - 准备进入执行阶段

### 9.2 关键点

- **词法分析是无状态的**: 每次调用返回一个token，不需要全局状态
- **语法分析是状态机驱动的**: Bison生成的LALR(1)状态机
- **解析树是临时的**: 转换为执行对象后即可丢弃
- **类型安全**: 使用强类型的解析树节点，而不是简单的树结构

### 9.3 性能考虑

- **哈希表查找**: 关键字查找使用完美哈希，O(1)复杂度
- **内存分配**: 使用线程本地的MEM_ROOT，避免频繁的malloc/free
- **预编译**: Bison状态机在编译时生成，运行时只需查表

---

## 附录：相关SQL MODE

某些SQL MODE会影响词法分析：

```cpp
// sql/sql_lex.cc
if (thd->variables.sql_mode & MODE_IGNORE_SPACE) {
  // 忽略函数名和括号之间的空格
}

if (thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES) {
  // 反斜杠不作为转义字符
}

if (thd->variables.sql_mode & MODE_HIGH_NOT_PRECEDENCE) {
  // NOT运算符使用高优先级
  return NOT2_SYM;  // 而不是 NOT_SYM
}
```

---

**文档版本**: 1.0  
**MySQL版本**: 8.0  
**创建日期**: 2025-10-15  
**作者**: MySQL源码分析

