# 临时压缩表拦截功能说明

## 功能概述

本功能是一个**临时性**的拦截机制，用于在InnoDB层面阻止创建压缩表，并返回明确的错误信息。

## 实施背景

在某些场景下，可能需要临时禁用压缩表功能，例如：
- 压缩功能需要维护或升级
- 性能调优期间暂时关闭压缩
- 测试环境中限制压缩表的使用

## 实现细节

### 修改位置

**文件**: `storage/innobase/handler/ha_innodb.cc`  
**函数**: `create_table_info_t::create_options_are_invalid()`  
**行号**: 约第13625-13633行

### 核心代码

```cpp
/* TEMPORARY FEATURE: Block compressed tables
   TODO: Remove this block when compression support is ready */
if (row_format == ROW_TYPE_COMPRESSED || has_key_block_size) {
  my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0), innobase_hton_name,
           row_format == ROW_TYPE_COMPRESSED ? "ROW_FORMAT=COMPRESSED"
                                              : "KEY_BLOCK_SIZE",
           "Compressed tables are temporarily not supported");
  return (row_format == ROW_TYPE_COMPRESSED ? "ROW_FORMAT" : "KEY_BLOCK_SIZE");
}
```

### 拦截逻辑

1. **检查条件**：
   - `row_format == ROW_TYPE_COMPRESSED`：显式指定了 `ROW_FORMAT=COMPRESSED`
   - `has_key_block_size`：指定了 `KEY_BLOCK_SIZE > 0`

2. **执行时机**：
   - 在所有其他CREATE TABLE选项验证之前执行
   - 确保压缩表在最早期就被拦截

3. **错误返回**：
   - 使用现有的 `ER_ILLEGAL_HA_CREATE_OPTION` 错误码
   - 返回明确的错误信息："Compressed tables are temporarily not supported"
   - 指示具体是哪个选项导致的错误（ROW_FORMAT 或 KEY_BLOCK_SIZE）

## 拦截场景

### 场景1: 显式指定 ROW_FORMAT=COMPRESSED

```sql
CREATE TABLE t1 (
  id INT PRIMARY KEY,
  name VARCHAR(100)
) ENGINE=InnoDB ROW_FORMAT=COMPRESSED;
```

**错误信息**:
```
ERROR 1478 (HY000): InnoDB: ROW_FORMAT=COMPRESSED Compressed tables are temporarily not supported
```

### 场景2: 指定 KEY_BLOCK_SIZE

```sql
CREATE TABLE t1 (
  id INT PRIMARY KEY,
  name VARCHAR(100)
) ENGINE=InnoDB KEY_BLOCK_SIZE=8;
```

**错误信息**:
```
ERROR 1478 (HY000): InnoDB: KEY_BLOCK_SIZE Compressed tables are temporarily not supported
```

### 场景3: 同时指定两者

```sql
CREATE TABLE t1 (
  id INT PRIMARY KEY,
  name VARCHAR(100)
) ENGINE=InnoDB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8;
```

**错误信息**:
```
ERROR 1478 (HY000): InnoDB: ROW_FORMAT=COMPRESSED Compressed tables are temporarily not supported
```

### 场景4: 隐式压缩（通过KEY_BLOCK_SIZE推断）

即使没有显式指定 `ROW_FORMAT=COMPRESSED`，只要指定了 `KEY_BLOCK_SIZE`，也会被拦截：

```sql
CREATE TABLE t1 (
  id INT PRIMARY KEY,
  name VARCHAR(100)
) ENGINE=InnoDB KEY_BLOCK_SIZE=4;
```

**说明**: 在InnoDB中，指定非零的 `KEY_BLOCK_SIZE` 会隐式推断为 `ROW_FORMAT=COMPRESSED`

## 不受影响的场景

以下场景**不会**被拦截，可以正常创建：

### 正常的未压缩表

```sql
-- ROW_FORMAT=DYNAMIC (默认)
CREATE TABLE t1 (
  id INT PRIMARY KEY,
  name VARCHAR(100)
) ENGINE=InnoDB;

-- ROW_FORMAT=COMPACT
CREATE TABLE t2 (
  id INT PRIMARY KEY,
  name VARCHAR(100)
) ENGINE=InnoDB ROW_FORMAT=COMPACT;

-- ROW_FORMAT=REDUNDANT
CREATE TABLE t3 (
  id INT PRIMARY KEY,
  name VARCHAR(100)
) ENGINE=InnoDB ROW_FORMAT=REDUNDANT;
```

### 使用其他存储引擎

```sql
-- MyISAM 表不受影响
CREATE TABLE t1 (
  id INT PRIMARY KEY,
  name VARCHAR(100)
) ENGINE=MyISAM;
```

## 设计优势

### 1. 改动最小化
- 仅修改一个函数
- 添加代码行数：8行
- 不涉及其他模块的修改

### 2. 影响范围精确
- 只影响新创建的表
- 不影响已存在的压缩表
- 不影响其他行格式的表

### 3. 易于维护
- 代码位置明确，有清晰的TODO注释
- 删除时只需移除这一个代码块
- 不会留下技术债务

### 4. 错误信息清晰
- 明确告知用户压缩表暂不支持
- 指出具体是哪个选项导致的问题
- 使用标准的MySQL错误机制

### 5. 拦截时机早
- 在参数验证阶段就拦截
- 避免进入更深层的创建逻辑
- 减少资源浪费

## 测试验证

提供了测试脚本 `test_compression_block.sql`，包含以下测试用例：

1. **Test 1**: 创建 `ROW_FORMAT=COMPRESSED` 表 → 应失败
2. **Test 2**: 创建 `KEY_BLOCK_SIZE=8` 表 → 应失败
3. **Test 3**: 创建同时指定两者的表 → 应失败
4. **Test 4**: 创建 `ROW_FORMAT=DYNAMIC` 表 → 应成功
5. **Test 5**: 隐式压缩（只有KEY_BLOCK_SIZE） → 应失败
6. **Test 6**: 创建 `ROW_FORMAT=COMPACT` 表 → 应成功

### 运行测试

```bash
# 在MySQL客户端中运行
mysql> source test_compression_block.sql

# 或通过命令行
mysql -u root -p < test_compression_block.sql
```

## 恢复正常功能

当需要恢复压缩表功能时，只需：

1. 打开文件 `storage/innobase/handler/ha_innodb.cc`
2. 找到第13625-13633行（搜索 "TEMPORARY FEATURE: Block compressed tables"）
3. 删除以下代码块：

```cpp
  /* TEMPORARY FEATURE: Block compressed tables
     TODO: Remove this block when compression support is ready */
  if (row_format == ROW_TYPE_COMPRESSED || has_key_block_size) {
    my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0), innobase_hton_name,
             row_format == ROW_TYPE_COMPRESSED ? "ROW_FORMAT=COMPRESSED"
                                                : "KEY_BLOCK_SIZE",
             "Compressed tables are temporarily not supported");
    return (row_format == ROW_TYPE_COMPRESSED ? "ROW_FORMAT" : "KEY_BLOCK_SIZE");
  }
```

4. 重新编译InnoDB存储引擎

## 技术说明

### 为什么选择这个拦截点？

1. **在验证阶段早期拦截**：避免进入更复杂的创建逻辑
2. **统一的错误处理**：与其他选项验证使用相同的机制
3. **不影响其他功能**：函数专门用于验证CREATE选项
4. **容易回滚**：独立的代码块，删除即可恢复

### 为什么不在SQL层拦截？

- SQL层拦截需要修改语法分析器，改动较大
- Parse Tree层面修改会影响更多代码
- 存储引擎层拦截更加精确，只影响InnoDB

### 为什么不用系统变量控制？

- 这是临时性功能，不需要用户可配置
- 避免引入额外的系统变量维护成本
- 简单直接，满足临时需求

## 已存在压缩表的处理

**重要提示**：此功能只阻止**创建新的**压缩表，对已存在的压缩表：

- ✓ 可以正常读写数据
- ✓ 可以正常查询
- ✓ 可以删除（DROP TABLE）
- ✗ 不能通过 ALTER TABLE 修改为压缩（会被拦截）
- ✗ 不能创建新的压缩表

## 监控建议

如果需要监控压缩表创建的尝试次数，可以：

1. 监控MySQL错误日志中的相关错误信息
2. 统计 `ER_ILLEGAL_HA_CREATE_OPTION` 错误码的出现次数
3. 检查慢查询日志中失败的CREATE TABLE语句

## 总结

这是一个设计良好的临时性功能，具有以下特点：

✅ **改动最小**：只修改一个函数，增加8行代码  
✅ **影响精确**：只影响新建压缩表，不影响其他功能  
✅ **易于移除**：删除代码块即可恢复  
✅ **错误清晰**：明确告知用户压缩表暂不支持  
✅ **拦截完整**：覆盖所有压缩表创建场景  

适用于需要临时禁用压缩表功能的场景，同时保持代码整洁和可维护性。

---

**修改日期**: 2025-10-16  
**影响版本**: Percona Server 8.0  
**修改类型**: 临时性功能  
**回滚难度**: ⭐ 极易（删除单个代码块）

