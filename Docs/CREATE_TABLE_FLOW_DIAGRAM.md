# CREATE TABLE 压缩表流程图

## 整体流程图

```
┌─────────────────────────────────────────────────────────────────┐
│ 1. SQL输入                                                       │
│ CREATE TABLE t1 (id INT PRIMARY KEY, name VARCHAR(100))        │
│ ENGINE=InnoDB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8;          │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 2. 词法分析 (Lexical Analysis)                                  │
│ 文件: sql/lex.cc, sql_yacc.yy                                   │
├─────────────────────────────────────────────────────────────────┤
│ MYSQLlex() 扫描SQL文本                                          │
│                                                                 │
│ Token序列：                                                      │
│ CREATE → TABLE_SYM → IDENT("t1") → "(" →                      │
│ IDENT("id") → INT_SYM → PRIMARY_SYM → KEY_SYM → "," →         │
│ IDENT("name") → VARCHAR → "(" → NUM(100) → ")" → ")" →        │
│ ENGINE_SYM → "=" → IDENT("InnoDB") →                          │
│ ROW_FORMAT_SYM → "=" → COMPRESSED_SYM →                       │
│ KEY_BLOCK_SIZE → "=" → NUM(8) → ";"                           │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 3. 语法分析 (Syntax Analysis)                                   │
│ 文件: sql/sql_yacc.yy (Bison Parser)                            │
├─────────────────────────────────────────────────────────────────┤
│ MYSQLparse() 构建Parse Tree                                     │
│                                                                 │
│ 语法规则匹配：                                                   │
│ • create_table_stmt (第3279行)                                 │
│   ├─ table_element_list                                        │
│   │   ├─ column_def: id INT PRIMARY KEY                        │
│   │   └─ column_def: name VARCHAR(100)                         │
│   └─ opt_create_table_options_etc (第6154行)                   │
│       └─ create_table_options (第6601行)                       │
│           ├─ ENGINE option                                     │
│           ├─ ROW_FORMAT option (第6721行)                      │
│           │   └─ row_types: COMPRESSED_SYM (第6827行)         │
│           └─ KEY_BLOCK_SIZE option (第6765行)                  │
│               └─ value: 8 (范围检查: [0, 65535])              │
│                                                                 │
│ Parse Tree节点创建：                                            │
│ • PT_create_table_stmt                                         │
│ • PT_create_table_engine_option("InnoDB")                      │
│ • PT_create_row_format_option(ROW_TYPE_COMPRESSED)             │
│ • PT_create_key_block_size_option(8)                           │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 4. 上下文化 (Contextualization)                                 │
│ 文件: sql/parse_tree_nodes.cc                                   │
├─────────────────────────────────────────────────────────────────┤
│ PT_create_table_stmt::make_cmd() (第2340行)                    │
│                                                                 │
│ 构建HA_CREATE_INFO结构:                                         │
│ ┌─────────────────────────────────────────────────────────┐   │
│ │ HA_CREATE_INFO (sql/handler.h 第3432行)                 │   │
│ │ ┌─────────────────────────────────────────────────────┐ │   │
│ │ │ db_type = innodb_hton                              │ │   │
│ │ │ row_type = ROW_TYPE_COMPRESSED                     │ │   │
│ │ │ key_block_size = 8                                 │ │   │
│ │ │ used_fields = HA_CREATE_USED_ENGINE |              │ │   │
│ │ │               HA_CREATE_USED_ROW_FORMAT |          │ │   │
│ │ │               HA_CREATE_USED_KEY_BLOCK_SIZE        │ │   │
│ │ └─────────────────────────────────────────────────────┘ │   │
│ └─────────────────────────────────────────────────────────┘   │
│                                                                 │
│ 每个选项节点调用do_contextualize():                             │
│ • PT_create_row_format_option::do_contextualize()              │
│   设置: create_info->row_type = ROW_TYPE_COMPRESSED            │
│         create_info->used_fields |= HA_CREATE_USED_ROW_FORMAT  │
│                                                                 │
│ • PT_create_key_block_size_option::do_contextualize()          │
│   设置: create_info->key_block_size = 8                        │
│         create_info->used_fields |= HA_CREATE_USED_KEY_BLOCK_SIZE │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 5. SQL命令对象创建                                              │
│ 文件: sql/sql_cmd_ddl_table.h/cc                                │
├─────────────────────────────────────────────────────────────────┤
│ 创建 Sql_cmd_create_table 对象                                  │
│ • 包含 HA_CREATE_INFO                                           │
│ • 包含 Alter_info (列和索引定义)                                │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 6. 执行阶段                                                      │
│ 文件: sql/sql_table.cc                                          │
├─────────────────────────────────────────────────────────────────┤
│ Sql_cmd_create_table::execute()                                │
│         ↓                                                       │
│ mysql_create_table()                                            │
│         ↓                                                       │
│ mysql_create_table_no_lock()                                    │
│         ↓                                                       │
│ create_table_impl()                                             │
│         ↓                                                       │
│ rea_create_base_table()                                         │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 7. InnoDB存储引擎处理                                           │
│ 文件: storage/innobase/handler/ha_innodb.cc                     │
├─────────────────────────────────────────────────────────────────┤
│ 步骤1: get_real_row_type() (第7042行)                          │
│ ┌─────────────────────────────────────────────────────────┐   │
│ │ 输入: row_type = ROW_TYPE_COMPRESSED                    │   │
│ │       key_block_size = 8                                │   │
│ │                                                          │   │
│ │ 检查:                                                    │   │
│ │ • 不是临时表? ✓                                         │   │
│ │ • innodb_file_per_table=ON 或有共享表空间? ✓           │   │
│ │                                                          │   │
│ │ 输出: ROW_TYPE_COMPRESSED (确认可以使用压缩)            │   │
│ └─────────────────────────────────────────────────────────┘   │
│                                                                 │
│ 步骤2: 验证和转换 (第14203-14297行)                            │
│ ┌─────────────────────────────────────────────────────────┐   │
│ │ KEY_BLOCK_SIZE转换:                                     │   │
│ │   8 KB → zip_ssize = 3                                  │   │
│ │   (公式: zip_ssize = log2(key_block_size))             │   │
│ │                                                          │   │
│ │ ROW_FORMAT验证:                                         │   │
│ │   case ROW_TYPE_COMPRESSED:                             │   │
│ │     ✓ 不是临时表                                        │   │
│ │     ✓ file_per_table 或共享表空间                       │   │
│ │     → innodb_row_format = REC_FORMAT_COMPRESSED         │   │
│ │                                                          │   │
│ │ 页大小检查:                                              │   │
│ │   if (UNIV_PAGE_SIZE > 16KB)                            │   │
│ │     → 不支持压缩，转为DYNAMIC                           │   │
│ │   else                                                  │   │
│ │     → 继续使用压缩 ✓                                    │   │
│ │                                                          │   │
│ │ InnoDB标志设置:                                         │   │
│ │   flags |= (3 << DICT_TF_ZSSIZE_SHIFT)  // zip_ssize    │   │
│ │   flags |= DICT_TF_COMPACT              // 紧凑格式基础 │   │
│ └─────────────────────────────────────────────────────────┘   │
│                                                                 │
│ 步骤3: 创建表文件                                               │
│ ┌─────────────────────────────────────────────────────────┐   │
│ │ ha_innodb::create()                                     │   │
│ │   ↓                                                      │   │
│ │ create_table_info_t::create_table()                     │   │
│ │   ↓                                                      │   │
│ │ row_create_table_for_mysql()                            │   │
│ │   ↓                                                      │   │
│ │ 创建 .ibd 文件                                          │   │
│ │   • 压缩页大小: 8 KB                                    │   │
│ │   • 原始页大小: 16 KB (用于解压)                        │   │
│ │   • 文件格式: Barracuda (支持压缩)                      │   │
│ └─────────────────────────────────────────────────────────┘   │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 8. 数据字典更新                                                  │
│ 文件: sql/dd/dd_table.cc, sql/dd_table_share.cc                 │
├─────────────────────────────────────────────────────────────────┤
│ 步骤1: 存储到数据字典 (dd_table.cc 第2107行)                   │
│ ┌─────────────────────────────────────────────────────────┐   │
│ │ dd::Table 对象:                                         │   │
│ │   name = "t1"                                           │   │
│ │   schema_id = <database_id>                             │   │
│ │   engine = "InnoDB"                                     │   │
│ │   row_format = dd::Table::RF_COMPRESSED  // 实际格式   │   │
│ │                                                          │   │
│ │   se_private_data: {                                    │   │
│ │     "tablespace_id": <space_id>,                        │   │
│ │     "flags": <table_flags>                              │   │
│ │   }                                                      │   │
│ │                                                          │   │
│ │   options: {                                            │   │
│ │     "row_type": RF_COMPRESSED,     // 用户指定        │   │
│ │     "key_block_size": 8                                 │   │
│ │   }                                                      │   │
│ │                                                          │   │
│ │   columns: [...]                                        │   │
│ │   indexes: [...]                                        │   │
│ └─────────────────────────────────────────────────────────┘   │
│                                                                 │
│ 步骤2: 加载到TABLE_SHARE (dd_table_share.cc 第684行)           │
│ ┌─────────────────────────────────────────────────────────┐   │
│ │ TABLE_SHARE 结构:                                       │   │
│ │   real_row_type = ROW_TYPE_COMPRESSED  // 实际格式     │   │
│ │   row_type = ROW_TYPE_COMPRESSED       // 用户指定     │   │
│ │   key_block_size = 8                                    │   │
│ └─────────────────────────────────────────────────────────┘   │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│ 9. 完成                                                          │
├─────────────────────────────────────────────────────────────────┤
│ 表创建成功:                                                      │
│ • 物理文件: database/t1.ibd (压缩表空间)                        │
│ • 数据字典: mysql.tables, mysql.columns, mysql.indexes 等      │
│ • 缓存: TABLE_SHARE 对象缓存在 table_def_cache                 │
└─────────────────────────────────────────────────────────────────┘
```

## 关键数据结构转换流程

```
┌─────────────────────────────────────────────────────────────────┐
│                    ROW_FORMAT 数据流转                           │
└─────────────────────────────────────────────────────────────────┘

SQL文本: "ROW_FORMAT=COMPRESSED"
    ↓
Lexer: ROW_FORMAT_SYM, COMPRESSED_SYM
    ↓
Parser: row_types → ROW_TYPE_COMPRESSED (enum)
    ↓
Parse Tree: PT_create_row_format_option(ROW_TYPE_COMPRESSED)
    ↓
Contextualize: HA_CREATE_INFO::row_type = ROW_TYPE_COMPRESSED
    ↓
InnoDB: innodb_row_format = REC_FORMAT_COMPRESSED
    ↓
Data Dictionary: dd::Table::RF_COMPRESSED
    ↓
TABLE_SHARE: real_row_type = ROW_TYPE_COMPRESSED

┌─────────────────────────────────────────────────────────────────┐
│                  KEY_BLOCK_SIZE 数据流转                         │
└─────────────────────────────────────────────────────────────────┘

SQL文本: "KEY_BLOCK_SIZE=8"
    ↓
Lexer: KEY_BLOCK_SIZE, NUM(8)
    ↓
Parser: ulonglong_num → 8
         ↓ (范围检查: 0-65535)
         ✓ 通过
    ↓
Parse Tree: PT_create_key_block_size_option(8)
    ↓
Contextualize: HA_CREATE_INFO::key_block_size = 8
    ↓
InnoDB: zip_ssize = 3 (log2(8) = 3)
        flags |= (3 << DICT_TF_ZSSIZE_SHIFT)
    ↓
Physical: 8KB压缩页 + 16KB原始页
    ↓
Data Dictionary: options["key_block_size"] = 8
    ↓
TABLE_SHARE: key_block_size = 8
```

## 压缩选项兼容性检查流程

```
┌────────────────────────────────────────────────────────────────┐
│         InnoDB压缩选项验证流程图                                │
└────────────────────────────────────────────────────────────────┘

开始
 │
 ├─→ row_type == ROW_TYPE_COMPRESSED?
 │   ├─ YES ─→ 是临时表?
 │   │         ├─ YES ─→ ❌ 警告: 临时表不支持压缩
 │   │         │         └─→ 转换为 ROW_TYPE_DYNAMIC
 │   │         │
 │   │         └─ NO ──→ innodb_file_per_table=ON 或有共享表空间?
 │   │                   ├─ YES ─→ ✓ 允许使用压缩
 │   │                   │         └─→ innodb_row_format = REC_FORMAT_COMPRESSED
 │   │                   │
 │   │                   └─ NO ──→ ❌ 警告: 需要file_per_table
 │   │                             └─→ 转换为 ROW_TYPE_DYNAMIC
 │   │
 │   └─ NO ──→ key_block_size > 0 且 row_type == DEFAULT?
 │             ├─ YES ─→ 自动推断为 ROW_TYPE_COMPRESSED
 │             └─ NO ──→ 使用其他行格式
 │
 ├─→ key_block_size > 0?
 │   ├─ YES ─→ 值是否有效 (1,2,4,8,16)?
 │   │         ├─ YES ─→ 计算 zip_ssize = log2(key_block_size)
 │   │         │         │
 │   │         │         └─→ key_block_size ≤ innodb_page_size?
 │   │         │             ├─ YES ─→ ✓ 有效的压缩块大小
 │   │         │             └─ NO ──→ ❌ 错误: 超过页大小
 │   │         │
 │   │         └─ NO ──→ ❌ 错误: 无效的KEY_BLOCK_SIZE
 │   │
 │   └─ NO ──→ 如果 row_type=COMPRESSED，使用默认值8KB
 │
 ├─→ innodb_page_size > 16KB 且要使用压缩?
 │   ├─ YES ─→ ❌ 警告: 页大小>16KB不支持压缩
 │   │         └─→ 转换为 ROW_TYPE_DYNAMIC
 │   │
 │   └─ NO ──→ ✓ 可以使用压缩
 │
 └─→ 设置最终的InnoDB标志
     ├─ DICT_TF_COMPACT (基础格式)
     ├─ (zip_ssize << DICT_TF_ZSSIZE_SHIFT)
     └─ 其他标志...
```

## 严格模式vs非严格模式

```
┌────────────────────────────────────────────────────────────────┐
│                   innodb_strict_mode 影响                       │
└────────────────────────────────────────────────────────────────┘

场景: ROW_FORMAT=COMPACT + KEY_BLOCK_SIZE=8 (不兼容)

┌─────────────────────────┐    ┌─────────────────────────┐
│  innodb_strict_mode=OFF │    │  innodb_strict_mode=ON  │
└─────────────────────────┘    └─────────────────────────┘
           │                              │
           ▼                              ▼
    发出警告 (Warning)              报错 (Error)
           │                              │
           ▼                              ▼
    忽略KEY_BLOCK_SIZE                 拒绝执行
    使用ROW_FORMAT=COMPACT              返回错误
           │
           ▼
    表创建成功 (非压缩)


场景: 临时表 + ROW_FORMAT=COMPRESSED

┌─────────────────────────┐    ┌─────────────────────────┐
│  innodb_strict_mode=OFF │    │  innodb_strict_mode=ON  │
└─────────────────────────┘    └─────────────────────────┘
           │                              │
           ▼                              ▼
    发出警告 (Warning)              发出警告 + 错误标志
           │                              │
           ▼                              ▼
    自动转为DYNAMIC                   自动转为DYNAMIC
           │                         (但设置invalid=true)
           ▼
    表创建成功
```

## 数据字典Schema

```
┌────────────────────────────────────────────────────────────────┐
│                   数据字典存储结构                              │
└────────────────────────────────────────────────────────────────┘

mysql.tables (DD)
┌─────────────────────────────────────────────────────────────┐
│ id | schema_id | name | engine | row_format | options | ... │
├─────────────────────────────────────────────────────────────┤
│ 10 | 5         | t1   | InnoDB | 5(COMPRESSED) | {...} | ... │
└─────────────────────────────────────────────────────────────┘
                                      │
                                      │
                    ┌─────────────────┴────────────────┐
                    │                                  │
                    ▼                                  ▼
      ┌──────────────────────┐        ┌─────────────────────────┐
      │ row_format (实际)    │        │ options (用户指定)       │
      ├──────────────────────┤        ├─────────────────────────┤
      │ 5 = RF_COMPRESSED    │        │ {                       │
      │                      │        │   "row_type": 5,        │
      │ 由存储引擎实际使用   │        │   "key_block_size": 8,  │
      │ (get_real_row_type)  │        │   ...                   │
      └──────────────────────┘        │ }                       │
                                      └─────────────────────────┘

行格式枚举值映射:
┌──────────────────────────────────────┐
│ MySQL层         │ DD层    │ InnoDB层 │
├──────────────────────────────────────┤
│ ROW_TYPE_DEFAULT│ 0       │ -        │
│ ROW_TYPE_FIXED  │ 1       │ -        │
│ ROW_TYPE_DYNAMIC│ 2       │ DYNAMIC  │
│ ROW_TYPE_COMPRESSED│ 3    │ COMPRESSED│
│ ROW_TYPE_REDUNDANT│ 4     │ REDUNDANT│
│ ROW_TYPE_COMPACT│ 5       │ COMPACT  │
└──────────────────────────────────────┘
```

## 文件系统布局

```
┌────────────────────────────────────────────────────────────────┐
│                  压缩表文件系统结构                             │
└────────────────────────────────────────────────────────────────┘

MySQL数据目录/
└── database_name/
    ├── t1.ibd          ← 表空间文件 (压缩)
    │   │
    │   ├─ 页类型: 压缩页
    │   ├─ 压缩页大小: 8 KB
    │   ├─ 原始页大小: 16 KB
    │   │
    │   ├─ 页结构:
    │   │   ┌──────────────────────────────┐
    │   │   │ Page Header (38 bytes)       │
    │   │   ├──────────────────────────────┤
    │   │   │ Compressed Data (变长)       │
    │   │   │ • zlib/lz4压缩               │
    │   │   │ • 最大约8KB                  │
    │   │   ├──────────────────────────────┤
    │   │   │ Modification Log (变长)      │
    │   │   │ • 记录页修改                 │
    │   │   ├──────────────────────────────┤
    │   │   │ Page Trailer (8 bytes)       │
    │   │   └──────────────────────────────┘
    │   │
    │   └─ Buffer Pool中的双重存储:
    │       ┌─ 压缩页 (8KB) - 用于磁盘I/O
    │       └─ 解压页 (16KB) - 用于数据操作
    │
    └── (frm文件已废弃，所有元数据在DD中)

InnoDB系统表空间: ibdata1
├── 数据字典表 (DD tables)
├── Undo日志
└── ...
```

## 性能特性

```
┌────────────────────────────────────────────────────────────────┐
│                  压缩表性能考虑                                 │
└────────────────────────────────────────────────────────────────┘

优点:
┌──────────────────────────────┐
│ ✓ 节省磁盘空间 (50-70%)      │
│ ✓ 减少I/O (读写更少的数据)   │
│ ✓ 可能提高I/O密集型性能      │
└──────────────────────────────┘

缺点:
┌──────────────────────────────┐
│ ✗ 增加CPU开销 (压缩/解压)    │
│ ✗ Buffer Pool需要更多内存    │
│   (同时存储压缩和解压页)     │
│ ✗ 可能影响CPU密集型性能      │
└──────────────────────────────┘

KEY_BLOCK_SIZE选择指南:
┌────────────────────────────────────────┐
│ 值  │ 适用场景                          │
├────────────────────────────────────────┤
│ 16KB│ 压缩率低的数据，接近页大小        │
│ 8KB │ 大多数场景的好选择 (默认推荐)     │
│ 4KB │ 数据压缩率高，行较小              │
│ 2KB │ 极小的行，高压缩率                │
│ 1KB │ 很少使用，极端场景                │
└────────────────────────────────────────┘
```

## 故障排查流程图

```
┌────────────────────────────────────────────────────────────────┐
│            压缩表创建失败诊断                                   │
└────────────────────────────────────────────────────────────────┘

创建失败
    │
    ├─→ 错误: "requires innodb_file_per_table"
    │   └─→ 检查: SHOW VARIABLES LIKE 'innodb_file_per_table';
    │       └─→ 设置: SET GLOBAL innodb_file_per_table=ON;
    │
    ├─→ 错误: "Cannot create COMPRESSED table when innodb_page_size > 16k"
    │   └─→ 检查: SHOW VARIABLES LIKE 'innodb_page_size';
    │       └─→ 解决: 
    │           • 重新初始化实例 (innodb_page_size=16k)
    │           • 或使用 ROW_FORMAT=DYNAMIC
    │
    ├─→ 错误: "KEY_BLOCK_SIZE out of range"
    │   └─→ 使用有效值: 1, 2, 4, 8, 16
    │
    ├─→ 警告: "ignored for TEMPORARY TABLE"
    │   └─→ 临时表不支持压缩，自动使用DYNAMIC
    │
    └─→ 错误: "Illegal HA_CREATE_OPTION" (strict mode)
        └─→ 检查ROW_FORMAT和KEY_BLOCK_SIZE兼容性:
            • COMPRESSED + KEY_BLOCK_SIZE: ✓
            • DYNAMIC + KEY_BLOCK_SIZE: ✗ (strict mode)
            • COMPACT + KEY_BLOCK_SIZE: ✗ (strict mode)
```

## 总结

这个流程图文档展示了CREATE TABLE压缩表语句从SQL文本到物理文件创建的完整流程，包括：

1. **词法和语法分析**: SQL → Tokens → Parse Tree
2. **语义分析**: Parse Tree → HA_CREATE_INFO
3. **存储引擎处理**: 验证、转换、创建物理文件
4. **数据字典存储**: 持久化元数据
5. **运行时加载**: DD → TABLE_SHARE → TABLE

关键交互点：
- Parser和InnoDB之间通过HA_CREATE_INFO传递信息
- InnoDB根据配置和限制调整实际使用的格式
- 数据字典同时保存用户指定值和实际使用值

