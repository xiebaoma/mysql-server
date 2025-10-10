# 调研功能

要求用户写入的数据生成的 Binlog row event size 不能有大字段，字段的 size 不允许超过某个固定配置的参数值。这个参数值的大小当前配置成 64M，超过这个大小，TRX 的提交则直接报错回滚。

---

## Mysql binlog 事件类之间的继承关系

Binlog 是按照事件 event 划分的，主要包括三类：

- `Write_rows_log_event`
- `Update_rows_log_event`
- `Delete_rows_log_event`

三种事件都是 row_based logging，其数据布局非常相似，主要区别在于 row 字段存储内容的不同：

- `Write_rows_log_event` 存储插入的数据
- `Update_rows_log_event` 存储修改前和修改后的数据
- `Delete_rows_log_event` 存储删除前的数据

这是 mysql 8.0 中事件类（`Write_rows_log_event`）的继承关系：

```
                         Binary_log_event
                                  ^
                                  |
                Log_event   B_l:Rows_event
                     ^            /\
                     |           /  \
                     |   <<vir>>/    \ <<vir>>
                     |         /      \
                     |        /        \
                  Rows_log_event    B_l:W_R_E
                             \          /
                              \        /
                               \      /
                                \    /
                                 \  /
                                  \/
                        Write_rows_log_event
```

- B_l: Namespace Binary_log
- W_R_E: class Write_rows_event

下文都将以 `Write_rows_log_event` 为例。

---

## Write_rows_log_event 内存分析

### 类的继承层次

- `Binary_log_event` (libbinlogevents, ~96字节)
- `Rows_event` (libbinlogevents, ~130字节)
- `Log_event` (server, ~200字节)
- `Rows_log_event` (server, ~300字节)
- `Write_rows_log_event`（最终类，无额外成员）

虚表指针和对齐: ~30字节

**固定部分总计:** ~756字节

### 动态部分（关键）

`Write_rows_log_event` 没有新增成员变量，所有数据都存在其继承的类中。

---

## 大字段存储位置

用一个实际的 insert 操作流程说明：

### 步骤一：打包行数据

执行 insert 操作，会调用 `pack_row()` 函数。

对于 BLOB/TEXT 大字段，`Field_blob::pack()` 会：

```cpp
uchar *Field_blob::pack(uchar *to, const uchar *from, size_t max_length) {
  uint32 length = get_length(from);  // 获取BLOB长度
  // 1. 先存储长度（1-4字节，取决于BLOB类型）
  store_blob_length(to, packlength, length);
  // 2. 然后复制完整的BLOB数据
  memcpy(to + packlength, get_blob_data(from + packlength), length);
  return to + packlength + length;
}
```

关键：BLOB/TEXT 字段的内容会复制到 row_data 缓冲区。

### 步骤二：添加到事件

打包后的行数据会被添加到 `Rows_log_event::m_rows_buf`：

```cpp
int Rows_log_event::do_add_row_data(uchar *row_data, size_t length) {
  // 如果空间不足，自动扩展 m_rows_buf
  if (static_cast<size_t>(m_rows_end - m_rows_cur) <= length) {
    // 按1024字节的块扩展
    const size_t new_alloc = block_size * ((cur_size + length + block_size - 1) / block_size);
    row.resize(new_alloc);  // row 是 std::vector<uint8_t>
    m_rows_buf = &row[0];
    m_rows_end = m_rows_buf + new_alloc;
  }
  // 复制行数据（包括大字段）到 m_rows_buf
  memcpy(m_rows_cur, row_data, length);
  m_rows_cur += length;
  m_row_count++;
}
```

### 步骤三：写入 binlog

```cpp
bool Rows_log_event::write_data_body(Basic_ostream *ostream) {
  ptrdiff_t const data_size = m_rows_cur - m_rows_buf;
  // 写入列数
  wrapper_my_b_safe_write(ostream, sbuf, ...);
  // 写入列位图
  wrapper_my_b_safe_write(ostream, m_cols.bitmap, ...);
  // 写入所有行数据（包括大字段！）
  wrapper_my_b_safe_write(ostream, m_rows_buf, data_size);
}
```

---

## 存储位置

对于一个包含大字段的 insert 操作，数据会存在多个位置：

1. `TABLE->record[0]`
   - 原始记录缓冲区
   - BLOB 字段存储：指针 + 长度
   - 实际 BLOB 数据在堆上（通过指针引用）
2. `Row_data_memory` (临时缓冲区)
   - 用于打包行数据的临时空间
   - 包含完整的 BLOB 数据副本
3. `Rows_log_event::m_rows_buf` ★核心存储位置★
   - 继承自 `binary_log::Rows_event::row` (`std::vector<uint8_t>`)
   - 存储打包后的行数据
   - 格式：NULL 位图 + 字段值（BLOB 长度 + BLOB 完整数据）
   - 这是大字段的主要内存占用位置
4. `Log_event::temp_buf` (读取 binlog 时)
   - 从 binlog 读取的原始事件数据
   - 包含完整序列化的事件（包括大字段）
5. Binlog cache (事务缓存)
   - 事件在提交前先写入 binlog 缓存
   - 又一份完整的数据副本

---

## 拦截大字段写入的方案

### 拦截点

找到 3 个关键的拦截点：

- `pack()` 最早期，打包单个 BLOB 字段时
- `pack_field()` 中期，打包所有字段时
- `do_add_row_data()` 后期，添加到事件缓冲区

打包函数的层次结构：

```
pack_row()                    (最高层 - 行级打包)
    ↓
pack_field()                  (中间层 - 字段级打包)
    ↓
Field::pack()                 (最底层 - 具体字段打包)
    ↓
Field_blob::pack()            (具体实现 - BLOB字段)
Field_string::pack()          (具体实现 - 字符串字段)
Field_long::pack()            (具体实现 - 整数字段)
...                          (其他字段类型)
```

建议在 `pack()` 或 `pack_field()` 处拦截，尽早发现问题，避免多处内存分配，并能提供明确的字段信息（字段名、大小），还没写入缓冲区，更容易回滚。

---

## 其他相关参数

MySQL 不会单独限制单个字段（例如一个 BLOB）在 binlog 中的大小，但会受到几个参数的间接限制：

- `max_allowed_packet`  
  如果某个 binlog event 太大（例如包含超大的 BLOB），超过该值，写入 binlog 或复制都可能失败。这个参数限制的是最终生成的 event，对字段（比如一个很大的 BLOB）不直接限制。

- `binlog_row_event_max_size`  
  如果一条语句影响了很多行或者单个行的数据太大（如 BLOB 很大），MySQL 会拆分为多个 event。

---

## PolarDB 和 Aurora

调研后发现 PolarDB 和 Aurora 本身没有公开支持一种在事务提交时强制校验 “binlog row event 大小不能超过某个阈值” 的功能。
