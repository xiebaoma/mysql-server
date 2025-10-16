# CREATE TABLE 压缩表执行流程分析

## 1. 概述

本文档分析以下CREATE TABLE SQL语句的执行流程，重点关注语法分析部分，特别是压缩表相关的选项处理：

```sql
CREATE TABLE t1 (
  id INT PRIMARY KEY,
  name VARCHAR(100)
) ENGINE=InnoDB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8;
```

该语句涉及的关键选项：
- `ENGINE=InnoDB`：指定存储引擎
- `ROW_FORMAT=COMPRESSED`：指定行格式为压缩格式
- `KEY_BLOCK_SIZE=8`：指定压缩页大小为8KB

## 2. 整体流程概览

```
SQL文本
  ↓
词法分析器(Lexer) → MYSQLlex()
  ↓
语法分析器(Parser) → MYSQLparse() [Bison]
  ↓
抽象语法树(AST) → PT_create_table_stmt
  ↓
上下文化(Contextualize) → HA_CREATE_INFO
  ↓
SQL命令对象 → Sql_cmd_create_table
  ↓
执行阶段 → mysql_create_table()
  ↓
存储引擎层 → ha_innodb::create()
```

## 3. 词法分析阶段（Lexical Analysis）

### 3.1 关键Token定义

在 `sql/sql_yacc.yy` 中定义了压缩表相关的Token：

```
%token<lexer.keyword> KEY_BLOCK_SIZE 517
%token<lexer.keyword> ROW_FORMAT_SYM 738
```

词法分析器会识别以下关键字：
- `ROW_FORMAT`
- `KEY_BLOCK_SIZE`
- `COMPRESSED`（作为 `COMPRESSED_SYM` token）
- `ENGINE`
- 其他行格式：`DEFAULT`, `FIXED`, `DYNAMIC`, `REDUNDANT`, `COMPACT`

### 3.2 词法分析流程

1. 词法扫描器 `MYSQLlex()` 扫描SQL文本
2. 识别关键字并转换为相应的Token
3. 将Token和位置信息传递给语法分析器

## 4. 语法分析阶段（Syntax Analysis）

### 4.1 CREATE TABLE语法规则

在 `sql/sql_yacc.yy` 第3279-3310行定义了CREATE TABLE的语法规则：

```yacc
create_table_stmt:
    CREATE opt_temporary TABLE_SYM opt_if_not_exists table_ident
    '(' table_element_list ')' opt_create_table_options_etc
    {
        $$= NEW_PTN PT_create_table_stmt(@$, YYMEM_ROOT, $1, $2, $4, $5,
                                         $7,
                                         $9.opt_create_table_options,
                                         $9.opt_partitioning,
                                         $9.on_duplicate,
                                         $9.opt_query_expression);
    }
```

### 4.2 表选项解析规则

#### 4.2.1 表选项容器（第6601-6613行）

```yacc
create_table_options:
    create_table_option
    {
        $$= NEW_PTN Mem_root_array<PT_create_table_option *>(YYMEM_ROOT);
        if ($$ == nullptr || $$->push_back($1))
            MYSQL_YYABORT; // OOM
    }
    | create_table_options opt_comma create_table_option
    {
        $$= $1;
        if ($$->push_back($3))
            MYSQL_YYABORT; // OOM
    }
```

这个规则允许多个表选项，用逗号分隔或空格分隔。

#### 4.2.2 ROW_FORMAT选项（第6721-6724行）

```yacc
| ROW_FORMAT_SYM opt_equal row_types
  {
      $$= NEW_PTN PT_create_row_format_option(@$, $3);
  }
```

#### 4.2.3 行格式类型（第6827-6834行）

```yacc
row_types:
    DEFAULT_SYM    { $$= ROW_TYPE_DEFAULT; }
  | FIXED_SYM      { $$= ROW_TYPE_FIXED; }
  | DYNAMIC_SYM    { $$= ROW_TYPE_DYNAMIC; }
  | COMPRESSED_SYM { $$= ROW_TYPE_COMPRESSED; }
  | REDUNDANT_SYM  { $$= ROW_TYPE_REDUNDANT; }
  | COMPACT_SYM    { $$= ROW_TYPE_COMPACT; }
  ;
```

`ROW_FORMAT=COMPRESSED` 会被解析为 `ROW_TYPE_COMPRESSED` 枚举值。

#### 4.2.4 KEY_BLOCK_SIZE选项（第6765-6778行）

```yacc
| KEY_BLOCK_SIZE opt_equal ulonglong_num
  {
      // The frm-format only allocated 2 bytes for key_block_size,
      // even if it is represented as std::uint32_t in HA_CREATE_INFO and
      // elsewhere.
      if ($3 > std::numeric_limits<std::uint16_t>::max()) {
          YYTHD->syntax_error_at(@3,
          "The valid range for key_block_size is [0,65535]. Error");
          MYSQL_YYABORT;
      }

      $$= NEW_PTN
      PT_create_key_block_size_option(@$, static_cast<std::uint32_t>($3));
  }
```

关键点：
- `KEY_BLOCK_SIZE` 接受一个无符号长整数
- 值的有效范围是 [0, 65535]
- 超出范围会触发语法错误

### 4.3 Parse Tree节点类

#### 4.3.1 PT_create_table_stmt

主要的CREATE TABLE Parse Tree节点，定义在 `sql/parse_tree_nodes.h` 中。

文件：`sql/parse_tree_nodes.cc` 第2340-2380行

```cpp
Sql_cmd *PT_create_table_stmt::make_cmd(THD *thd) {
    LEX *const lex = thd->lex;
    
    lex->sql_command = SQLCOM_CREATE_TABLE;
    
    Parse_context pc(thd, lex->current_query_block());
    
    // ... 添加表到table list
    
    lex->create_info = &m_create_info;
    Table_ddl_parse_context pc2(thd, pc.select, &m_alter_info);
    
    pc2.create_info->options = 0;
    if (is_temporary) pc2.create_info->options |= HA_LEX_CREATE_TMP_TABLE;
    if (only_if_not_exists)
        pc2.create_info->options |= HA_LEX_CREATE_IF_NOT_EXISTS;
    
    // ... 处理各种选项
}
```

#### 4.3.2 PT_create_row_format_option

ROW_FORMAT选项的Parse Tree节点，定义在 `sql/parse_tree_nodes.h` 第2694-2696行：

```cpp
typedef PT_traceable_create_table_option<TYPE_AND_REF(HA_CREATE_INFO::row_type),
                                         HA_CREATE_USED_ROW_FORMAT>
    PT_create_row_format_option;
```

该节点使用模板类 `PT_traceable_create_table_option` 实现。

#### 4.3.3 PT_create_key_block_size_option

KEY_BLOCK_SIZE选项的Parse Tree节点，定义在 `sql/parse_tree_nodes.h` 第2719-2721行：

```cpp
typedef PT_traceable_create_table_option<
    TYPE_AND_REF(HA_CREATE_INFO::key_block_size), HA_CREATE_USED_KEY_BLOCK_SIZE>
    PT_create_key_block_size_option;
```

#### 4.3.4 PT_traceable_create_table_option模板

定义在 `sql/parse_tree_nodes.h` 第2603-2618行：

```cpp
template <typename Option_type, Option_type HA_CREATE_INFO::*Property,
          uint64_t Property_flag>
class PT_traceable_create_table_option : public PT_create_table_option {
    typedef PT_create_table_option super;
    
    const Option_type value;

public:
    explicit PT_traceable_create_table_option(const POS &pos, Option_type value)
        : super(pos), value(value) {}

    bool do_contextualize(Table_ddl_parse_context *pc) override {
        if (super::do_contextualize(pc)) return true;
        pc->create_info->*Property = value;
        pc->create_info->used_fields |= Property_flag;
        return false;
    }
};
```

该模板类的工作机制：
1. 在构造时存储选项值
2. 在 `do_contextualize()` 时将值设置到 `HA_CREATE_INFO` 结构体
3. 同时设置 `used_fields` 标志位，标记该字段已被显式指定

## 5. 上下文化阶段（Contextualization）

### 5.1 HA_CREATE_INFO结构体

定义在 `sql/handler.h` 第3432-3544行，关键字段包括：

```cpp
struct HA_CREATE_INFO {
    const CHARSET_INFO *table_charset{nullptr};
    const CHARSET_INFO *default_table_charset{nullptr};
    LEX_STRING connect_string{nullptr, 0};
    LEX_STRING compress{nullptr, 0};        // 压缩算法
    LEX_STRING comment{nullptr, 0};
    
    const char *data_file_name{nullptr};
    const char *index_file_name{nullptr};
    ulonglong max_rows{0};
    ulonglong min_rows{0};
    ulonglong auto_increment_value{0};
    
    ulong table_options{0};                 // 表选项标志位
    ulong avg_row_length{0};
    uint64_t used_fields{0};                // 标记哪些字段被显式设置
    
    std::uint32_t key_block_size{0};        // KEY_BLOCK_SIZE值（1,2,4,8,16）
    uint stats_sample_pages{0};
    enum_stats_auto_recalc stats_auto_recalc{HA_STATS_AUTO_RECALC_DEFAULT};
    
    handlerton *db_type{nullptr};           // 存储引擎
    enum row_type row_type = ROW_TYPE_DEFAULT;  // 行格式类型
    
    // ... 其他字段
};
```

### 5.2 关键字段说明

#### 5.2.1 row_type字段

表示用户显式指定的行格式，可能的值：
- `ROW_TYPE_DEFAULT`：未指定（默认值）
- `ROW_TYPE_FIXED`：固定长度行
- `ROW_TYPE_DYNAMIC`：动态长度行
- `ROW_TYPE_COMPRESSED`：压缩行（我们的示例）
- `ROW_TYPE_REDUNDANT`：冗余格式（InnoDB早期格式）
- `ROW_TYPE_COMPACT`：紧凑格式

#### 5.2.2 key_block_size字段

压缩块大小，以KB为单位。有效值：
- `0`：未指定或禁用
- `1, 2, 4, 8, 16`：有效的压缩块大小（KB）

#### 5.2.3 used_fields字段

位掩码，标记哪些字段被显式设置：
- `HA_CREATE_USED_ROW_FORMAT`：row_type字段被设置
- `HA_CREATE_USED_KEY_BLOCK_SIZE`：key_block_size字段被设置
- 其他字段的标志位...

### 5.3 上下文化过程

当Parse Tree的 `contextualize()` 方法被调用时：

1. 遍历所有 `create_table_option` 节点
2. 每个选项节点调用 `do_contextualize()`
3. `PT_create_row_format_option::do_contextualize()` 设置：
   - `create_info->row_type = ROW_TYPE_COMPRESSED`
   - `create_info->used_fields |= HA_CREATE_USED_ROW_FORMAT`
4. `PT_create_key_block_size_option::do_contextualize()` 设置：
   - `create_info->key_block_size = 8`
   - `create_info->used_fields |= HA_CREATE_USED_KEY_BLOCK_SIZE`

## 6. 存储引擎特定处理（InnoDB）

### 6.1 row_type的验证和调整

文件：`storage/innobase/handler/ha_innodb.cc` 第14203-14276行

```cpp
switch (row_type) {
    case ROW_TYPE_REDUNDANT:
        innodb_row_format = REC_FORMAT_REDUNDANT;
        break;
    case ROW_TYPE_COMPACT:
        innodb_row_format = REC_FORMAT_COMPACT;
        break;

    case ROW_TYPE_COMPRESSED:
        if (is_temp) {
            // 临时表不支持压缩，警告并转为DYNAMIC
            push_warning_printf(m_thd, Sql_condition::SL_WARNING,
                                ER_ILLEGAL_HA_CREATE_OPTION,
                                "InnoDB: %s is ignored for TEMPORARY TABLE.",
                                get_row_format_name(row_type));
            row_type = ROW_TYPE_DYNAMIC;
            innodb_row_format = REC_FORMAT_DYNAMIC;
            break;
        }

        // ROW_FORMAT=COMPRESSED requires file_per_table unless
        // there is a target tablespace.
        if (!m_allow_file_per_table && !m_use_shared_space) {
            push_warning_printf(m_thd, Sql_condition::SL_WARNING,
                                ER_ILLEGAL_HA_CREATE_OPTION,
                                "InnoDB: %s requires innodb_file_per_table.",
                                get_row_format_name(row_type));
        } else {
            // 可以使用压缩行格式
            innodb_row_format = REC_FORMAT_COMPRESSED;
            break;
        }
        zip_allowed = false;
        [[fallthrough]];
        
    case ROW_TYPE_DYNAMIC:
        innodb_row_format = REC_FORMAT_DYNAMIC;
        break;
        
    // ... 其他情况
}
```

### 6.2 KEY_BLOCK_SIZE的处理

文件：`storage/innobase/handler/ha_innodb.cc` 第14191-14200行

```cpp
} else {
    /* zip_ssize == 0 means no KEY_BLOCK_SIZE. */
    if (row_type == ROW_TYPE_COMPRESSED && zip_allowed) {
        /* ROW_FORMAT=COMPRESSED without KEY_BLOCK_SIZE
        implies half the maximum KEY_BLOCK_SIZE(*1k) or
        UNIV_PAGE_SIZE, whichever is less. */
        zip_ssize = zip_ssize_max - 1;
    }
}
```

#### 6.2.1 KEY_BLOCK_SIZE的转换

InnoDB将 `KEY_BLOCK_SIZE` 转换为 `zip_ssize`（压缩页大小的log2值减1）：

- `KEY_BLOCK_SIZE=1` → `zip_ssize=0`（实际大小：1KB）
- `KEY_BLOCK_SIZE=2` → `zip_ssize=1`（实际大小：2KB）
- `KEY_BLOCK_SIZE=4` → `zip_ssize=2`（实际大小：4KB）
- `KEY_BLOCK_SIZE=8` → `zip_ssize=3`（实际大小：8KB）
- `KEY_BLOCK_SIZE=16` → `zip_ssize=4`（实际大小：16KB）

#### 6.2.2 压缩限制检查

文件：`storage/innobase/handler/ha_innodb.cc` 第14278-14285行

```cpp
/* Don't support compressed table when page size > 16k. */
if (zip_allowed && zip_ssize && UNIV_PAGE_SIZE > UNIV_PAGE_SIZE_DEF) {
    push_warning(m_thd, Sql_condition::SL_WARNING, ER_ILLEGAL_HA_CREATE_OPTION,
                 "InnoDB: Cannot create a COMPRESSED table"
                 " when innodb_page_size > 16k."
                 " Assuming ROW_FORMAT=DYNAMIC.");
    zip_allowed = false;
}
```

### 6.3 get_real_row_type()方法

文件：`storage/innobase/handler/ha_innodb.cc` 第7042-7087行

该方法返回存储引擎实际使用的行格式，可能与用户指定的不同：

```cpp
enum row_type ha_innobase::get_real_row_type(
    const HA_CREATE_INFO *create_info) const {
    const bool is_temp = create_info->options & HA_LEX_CREATE_TMP_TABLE;
    row_type rt = create_info->row_type;

    // 如果ROW_TYPE=DEFAULT但指定了KEY_BLOCK_SIZE，自动推断为COMPRESSED
    if (rt == ROW_TYPE_DEFAULT && create_info->key_block_size &&
        get_zip_shift_size(create_info->key_block_size) && !is_temp &&
        (srv_file_per_table || tablespace_is_shared_space(create_info))) {
        rt = ROW_TYPE_COMPRESSED;
    }

    switch (rt) {
        case ROW_TYPE_REDUNDANT:
        case ROW_TYPE_DYNAMIC:
        case ROW_TYPE_COMPACT:
            return (rt);
        case ROW_TYPE_COMPRESSED:
            if (!is_temp &&
                (srv_file_per_table || tablespace_is_shared_space(create_info))) {
                return (rt);
            } else {
                return (ROW_TYPE_DYNAMIC);
            }
        case ROW_TYPE_DEFAULT:
        default:
            // 返回系统默认行格式
            switch (innodb_default_row_format) {
                case DEFAULT_ROW_FORMAT_REDUNDANT:
                    return (ROW_TYPE_REDUNDANT);
                case DEFAULT_ROW_FORMAT_COMPACT:
                    return (ROW_TYPE_COMPACT);
                case DEFAULT_ROW_FORMAT_DYNAMIC:
                    return (ROW_TYPE_DYNAMIC);
                default:
                    return (ROW_TYPE_DYNAMIC);
            }
    }
}
```

## 7. 数据字典更新

### 7.1 表选项存储

文件：`sql/dd/dd_table.cc` 第2107-2121行

```cpp
// ROW_FORMAT which was explicitly specified by user (if any).
if (create_info->row_type != ROW_TYPE_DEFAULT)
    table_options->set("row_type",
                       dd_get_new_row_format(create_info->row_type));

// ROW_FORMAT which is really used for the table by SE (perhaps implicitly).
tab_obj->set_row_format(
    dd_get_new_row_format(file->get_real_row_type(create_info)));

table_options->set("stats_sample_pages",
                   create_info->stats_sample_pages & 0xffff);

table_options->set("stats_auto_recalc", create_info->stats_auto_recalc);

table_options->set("key_block_size", create_info->key_block_size);
```

关键点：
1. **用户指定的row_type**：如果不是DEFAULT，存储在 `table_options` 中
2. **实际使用的row_format**：存储引擎实际使用的格式，存储在 `dd::Table` 对象中
3. **key_block_size**：直接存储用户指定的值

### 7.2 TABLE_SHARE加载

文件：`sql/dd_table_share.cc` 第684-712行

从数据字典读取表定义时：

```cpp
// Row type. First one really used by the storage engine.
share->real_row_type = dd_get_old_row_format(tab_obj->row_format());

// Then one which was explicitly specified by user for this table.
if (table_options.exists("row_type")) {
    table_options.get("row_type", &option_value);
    share->row_type =
        dd_get_old_row_format((dd::Table::enum_row_format)option_value);
} else
    share->row_type = ROW_TYPE_DEFAULT;

// key block size
table_options.get("key_block_size", &share->key_block_size);
```

## 8. 压缩表选项的兼容性规则

### 8.1 ROW_FORMAT和KEY_BLOCK_SIZE的交互

参考测试文件：`mysql-test/suite/innodb_zip/t/create_options.test` 第1-36行

规则总结：

1. **CREATE选项累加**：ALTER语句的选项会添加到之前CREATE或ALTER的选项上
2. **KEY_BLOCK_SIZE=0视为未指定**：如果显式设置了ROW_FORMAT=COMPRESSED，InnoDB会使用默认值8
3. **ROW_FORMAT=DEFAULT允许InnoDB选择默认格式**（通常是COMPACT）
4. **不兼容选项处理**：
   - 在非严格模式下，ROW_FORMAT优先
   - 在严格模式下，会报错
5. **自动转换为COMPRESSED**：
   - 如果指定了有效的非零KEY_BLOCK_SIZE
   - 且ROW_FORMAT=DEFAULT或未使用过
6. **InnoDB严格模式**：
   - 有效的ROW_FORMAT：COMPRESSED, COMPACT, DEFAULT, DYNAMIC, REDUNDANT
   - 有效的KEY_BLOCK_SIZE：0,1,2,4,8,16
   - KEY_BLOCK_SIZE=1,2,4,8,16与COMPACT、DYNAMIC不兼容

### 8.2 限制条件

1. **file-per-table要求**：
   - `ROW_FORMAT=COMPRESSED` 需要 `innodb_file_per_table=ON`
   - 或者指定了共享表空间

2. **页大小限制**：
   - 当 `innodb_page_size > 16KB` 时不支持压缩表
   - 会自动转换为 `ROW_FORMAT=DYNAMIC`

3. **临时表限制**：
   - 临时表不支持压缩
   - 会自动转换为 `ROW_FORMAT=DYNAMIC`

4. **KEY_BLOCK_SIZE约束**：
   - 必须 ≤ innodb_page_size
   - 有效值：1, 2, 4, 8, 16（KB）

## 9. 示例SQL的完整处理流程

对于我们的示例SQL：
```sql
CREATE TABLE t1 (
  id INT PRIMARY KEY,
  name VARCHAR(100)
) ENGINE=InnoDB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8;
```

### 9.1 词法分析结果

```
CREATE → CREATE
TABLE → TABLE_SYM
t1 → IDENT
( → (
id → IDENT
INT → INT_SYM
PRIMARY → PRIMARY_SYM
KEY → KEY_SYM
, → ,
name → IDENT
VARCHAR → VARCHAR
( → (
100 → NUM
) → )
) → )
ENGINE → ENGINE_SYM
= → =
InnoDB → IDENT
ROW_FORMAT → ROW_FORMAT_SYM
= → =
COMPRESSED → COMPRESSED_SYM
KEY_BLOCK_SIZE → KEY_BLOCK_SIZE
= → =
8 → NUM
; → ;
```

### 9.2 语法分析结果（Parse Tree）

```
PT_create_table_stmt
├── table_name: "t1"
├── table_element_list:
│   ├── PT_column_def("id", INT, PRIMARY KEY)
│   └── PT_column_def("name", VARCHAR(100))
└── create_table_options:
    ├── PT_create_table_engine_option("InnoDB")
    ├── PT_create_row_format_option(ROW_TYPE_COMPRESSED)
    └── PT_create_key_block_size_option(8)
```

### 9.3 上下文化结果（HA_CREATE_INFO）

```cpp
HA_CREATE_INFO {
    db_type = innodb_hton;              // InnoDB handlerton指针
    row_type = ROW_TYPE_COMPRESSED;     // 压缩行格式
    key_block_size = 8;                 // 8KB压缩块
    used_fields = HA_CREATE_USED_ENGINE |
                  HA_CREATE_USED_ROW_FORMAT |
                  HA_CREATE_USED_KEY_BLOCK_SIZE;
    // ... 其他字段为默认值
}
```

### 9.4 InnoDB处理

1. **验证压缩选项**：
   - 检查 `innodb_file_per_table=ON` 或有共享表空间
   - 检查 `innodb_page_size ≤ 16KB`
   - 验证 `KEY_BLOCK_SIZE=8` 是有效值

2. **计算zip_ssize**：
   - `KEY_BLOCK_SIZE=8` → `zip_ssize=3`

3. **设置InnoDB标志**：
   - `flags |= (zip_ssize << DICT_TF_ZSSIZE_SHIFT)`
   - `row_format = REC_FORMAT_COMPRESSED`

4. **创建表**：
   - 使用压缩页格式创建.ibd文件
   - 页大小为8KB（压缩）+ 16KB（原始页）

### 9.5 数据字典存储

```
dd::Table {
    name = "t1"
    schema_id = <db_id>
    engine = "InnoDB"
    row_format = COMPRESSED           // 实际使用的格式
    options = {
        "row_type": COMPRESSED,       // 用户指定的格式
        "key_block_size": 8
    }
    columns = [...]
    indexes = [...]
}
```

## 10. 关键代码文件清单

### 10.1 语法分析相关
- `sql/sql_yacc.yy`：Bison语法定义文件
  - 第881行：KEY_BLOCK_SIZE token定义
  - 第1104行：ROW_FORMAT_SYM token定义
  - 第3279-3310行：create_table_stmt规则
  - 第6601-6613行：create_table_options规则
  - 第6721-6724行：ROW_FORMAT选项规则
  - 第6765-6778行：KEY_BLOCK_SIZE选项规则
  - 第6827-6834行：row_types规则

### 10.2 Parse Tree节点
- `sql/parse_tree_nodes.h`：Parse Tree节点定义
  - 第2603-2618行：PT_traceable_create_table_option模板类
  - 第2694-2696行：PT_create_row_format_option
  - 第2719-2721行：PT_create_key_block_size_option
  
- `sql/parse_tree_nodes.cc`：Parse Tree节点实现
  - 第2340-2380行：PT_create_table_stmt::make_cmd()

### 10.3 数据结构定义
- `sql/handler.h`：
  - 第3432-3544行：HA_CREATE_INFO结构体定义
  
- `sql/table.h`：
  - 第804-852行：TABLE_SHARE结构体（包含row_type和key_block_size）

### 10.4 InnoDB存储引擎
- `storage/innobase/handler/ha_innodb.cc`：
  - 第7042-7087行：get_real_row_type()方法
  - 第14191-14297行：压缩选项验证和处理

- `storage/innobase/dict/dict0dd.cc`：
  - 第1497-1563行：行格式转换和验证
  - 第2786-2850行：设置表选项

### 10.5 数据字典
- `sql/dd/dd_table.cc`：
  - 第2107-2121行：表选项存储到数据字典

- `sql/dd_table_share.cc`：
  - 第684-712行：从数据字典加载表选项到TABLE_SHARE

### 10.6 测试文件
- `mysql-test/suite/innodb_zip/t/create_options.test`：
  - 压缩表选项的各种组合测试

## 11. 总结

CREATE TABLE压缩表的执行流程涉及多个层次：

1. **词法和语法分析**：将SQL文本转换为结构化的Parse Tree
2. **上下文化**：将Parse Tree转换为执行所需的数据结构（HA_CREATE_INFO）
3. **存储引擎验证**：InnoDB验证压缩选项的有效性和兼容性
4. **表创建**：使用压缩格式创建物理表文件
5. **数据字典更新**：持久化表定义信息

关键点：
- `ROW_FORMAT=COMPRESSED` 和 `KEY_BLOCK_SIZE` 是相互关联的选项
- InnoDB会根据配置和限制条件调整实际使用的行格式
- 数据字典同时存储用户指定的格式和实际使用的格式
- 严格模式会强制检查选项的兼容性

这种分层设计使得MySQL能够灵活处理各种存储引擎的特定选项，同时保持SQL层的统一性。

