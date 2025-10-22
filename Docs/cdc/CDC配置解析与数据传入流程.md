# CDC 配置解析与数据传入流程

## 概述
CDC 中间件作为**被调用的服务**，需要从外部接收各种配置参数。这个文档详细说明配置是如何被解析和使用的。

---

## 一、配置参数定义（使用 ezflag 库）

### 1.1 ezflag 库简介
CDC 使用 `github.com/zr-hebo/utils/ezflag` 库来管理命令行参数，类似于 Go 标准库的 `flag`，但功能更强大。

### 1.2 核心配置参数定义

#### A. 源数据库配置 (src_config.go)
```go
var (
    // 源数据库类型
    SourceTypeFlag = ezflag.NewStringVar(
        "source_type", "mysql.no_schema",
        "source instance type. Now it can be transfer, mysql. It is required.", true)
    
    // 源数据库地址（关键！）
    // 格式：ip:port 或 ip:port,ip:port （支持多个地址）
    SourceHostFlag = ezflag.NewStringVar(
        "source_host", "",
        "mysql source host. It is required.", false)
    
    // 源数据库分组
    SourceGroupFlag = ezflag.NewStringVar(
        "source_group", "0",
        "mysql service group. It is not required.", true)
    
    // 源数据库用户名
    SourceUserFlag = ezflag.NewStringVar(
        "source_user", "",
        "user name connect source instance with replication privilege. It is required.", false)
    
    // 源数据库密码
    SourcePasswdFlag = ezflag.NewStringVar(
        "source_password", "",
        "user password connect source instance. It is required.", false)
    
    // 起始位点（GTID）
    StartPosFlag = ezflag.NewStringVar(
        "start_position", "",
        "start dump binlog position, default is empty. It is not required.", false)
    
    // 停止位点（GTID）
    StopPosFlag = ezflag.NewStringVar(
        "stop_position", "",
        "dump binlog stop position, default is empty. It is not required.", false)
    
    // 起始时间
    StartTimeFlag = ezflag.NewStringVar(
        "start_datetime", "",
        "start dump binlog begin time, default is empty. It is not required.", false)
    
    // 停止时间
    StopTimeFlag = ezflag.NewStringVar(
        "stop_datetime", "",
        "dump binlog stop time, default is empty. It is not required.", false)
    
    // 并发 worker 数量
    SourceWorkerNum = ezflag.NewIntVar(
        "source_worker_num", 1,
        "max source worker number, default is 1. It is not required.", false)
    
    // 从库地址（可选，用于读取数据）
    SlaveHost = ezflag.NewStringVar(
        "slave_host", "",
        "slave host in format,[ip]:[port],[ip]:[port]..", false)
)
```

#### B. 目标数据库配置 (sink_config.go)
```go
var (
    // 目标数据库类型
    SinkType = ezflag.NewStringVar(
        "sink_type", "mysql",
        "source instance type. it can be transfer, mysql, postgre, redis, shard, clickhouse. It is required.", false)
    
    // 目标数据库地址
    SinkHostFlag = ezflag.NewStringVar(
        "sink_host", "",
        "sink server host. It is required.", false)
    
    // 目标数据库分组
    SinkGroupFlag = ezflag.NewStringVar(
        "sink_group", "",
        "sink server group. It is not required.", false)
    
    // 目标数据库名称
    SinkDBFlag = ezflag.NewStringVar(
        "sink_db", "",
        "sink target database name. It is not required.", false)
    
    // 目标数据库用户名
    SinkUserFlag = ezflag.NewStringVar(
        "sink_user", "",
        "sink user name. It is required.", false)
    
    // 目标数据库密码
    SinkPasswdFlag = ezflag.NewStringVar(
        "sink_password", "",
        "sink user password. It is required.", false)
    
    // 并发 worker 数量
    SinkWorkerNum = ezflag.NewIntVar(
        "sink_worker_num", 1,
        "concurrent execute worker number, default is 1. It is not required.", false)
)
```

#### C. 元数据服务配置 (metadata.go)
```go
var (
    // 元数据服务类型
    MetadataTypeVar = ezflag.NewStringVar(
        "metadata_type", "consul",
        "metadata service type: etcd, consul, mysql, redis, local. It is required.", true)
    
    // 元数据服务地址
    MetadataServerVar = ezflag.NewStringVar(
        "metadata_server", "",
        "metadata server address. It is required.", false)
    
    // 元数据服务用户名
    MetadataUserVar = ezflag.NewStringVar(
        "metadata_user", "",
        "metadata user name. It is not required.", false)
    
    // 元数据服务密码
    metadataPasswdVar = ezflag.NewStringVar(
        "metadata_password", "",
        "metadata password. It is not required.", false)
)
```

#### D. 其他核心配置 (config.go)
```go
var (
    // 任务名称（唯一标识）
    taskNameFlag = ezflag.NewStringVar(
        "task_name", "",
        "the id differentiate different sync tasks. default is empty. It is not required.", false)
    
    // 任务组名称
    jobNameFlag = ezflag.NewStringVar(
        "job_name", "dts",
        "the the job name of one group of sync tasks. default is empty. It is not required.", false)
    
    // 子命令（dump/apply/sync/diff/precheck）
    SubCmd = ezflag.NewStringVar(
        "sub_cmd", "sync",
        "sub_cmd: sub command execute in transfer, it can be dump, apply, sync, diff or help. It is required.", true)
    
    // Web 服务端口
    WebServicePort = ezflag.NewIntVar(
        "web_service_port", 7082,
        "web service port, default is 7082. It is not required.", false)
    
    // 全量 dump 批次大小
    DumpBatchSize = ezflag.NewIntVar(
        "dump_batch_size", 5000,
        "full data dump batch size, default is 5000. It is not required.", false)
    
    // 插入批次大小
    InsertBatchSize = ezflag.NewIntVar(
        "insert_batch_size", 1024,
        "insert batch size, default is 1024. It is not required.", false)
    
    // 速度限制（TPS）
    LimitTPSFlag = ezflag.NewIntVar(
        "speed_limit", 5000,
        "speed limit for sync, dump, diff and so on, default is 5000. It is not required.", false)
    
    // 内存限制（MB）
    MaxMemoryMb = ezflag.NewIntVar(
        "max_memory_mb", 2048,
        "max memory for dumper in mb unit", false)
    
    // 是否同步 DDL
    SyncDDL = ezflag.NewBoolVar(
        "sync_ddl", false,
        "sync ddl statement or not, default is false. It is not required.", false)
)
```

---

## 二、配置解析流程

### 2.1 入口：showCommand() 函数
```go
// cmd/sub_cmd.go
func showCommand() {
    fmt.Println("++++++++++++++++++++++++")
    
    // 1. 备份原始命令行参数
    tmpRawArgs := deepcopy.Copy(os.Args)
    rawArgs := tmpRawArgs.([]string)
    
    // 2. 解析命令行参数（核心步骤）
    err := ezflag.Parse(reservedKeys)
    if err != nil {
        log.Warningf("parse command line error occur:%s", err.Error())
    }
    
    // 3. 准备密码（从参数中复制到内存）
    config.PreparePasswd()
    
    // 4. 隐写敏感信息后打印命令
    fmt.Printf("start service with command: %s\n\n", steganography(rawArgs))
}
```

**reservedKeys**：保留的标准 glog 参数
```go
var (
    reservedKeys = []string{"alsologtostderr", "logtostderr", "log_dir", "help"}
)
```

### 2.2 密码准备：PreparePasswd()
```go
// config/passwd.go
func PreparePasswd() {
    GetSourcePasswd()
    GetSinkPasswd()
    GetConflictMQPasswd()
    GetMetadataPasswd()
}

func GetSourcePasswd() string {
    if sourcePasswd == "" {
        // 从 flag 获取密码
        tmpPasswd := SourcePasswdFlag.MustGetVal()
        
        // 复制到新的内存空间（安全考虑）
        copyPasswd := make([]byte, len(tmpPasswd))
        copy(copyPasswd, tmpPasswd)
        sourcePasswd = string(copyPasswd)
    }
    return sourcePasswd
}
```

**安全机制**：
- 密码被复制到单独的内存空间
- 原始命令行参数中的密码会被隐写（替换为 `******`）

### 2.3 敏感信息隐写
```go
func steganography(rawArgs []string) string {
    os.Args = rawArgs
    fields := make([]string, 0, len(os.Args))
    
    for _, field := range os.Args {
        filedName := strings.TrimLeft(strings.Split(field, "=")[0], "-")
        
        // 如果参数名包含 password 或 passwd，隐藏值
        if strings.Contains(filedName, "password") || strings.Contains(filedName, "passwd") {
            fields = append(fields, fmt.Sprintf("--%s=******", filedName))
        } else {
            fields = append(fields, field)
        }
    }
    
    return strings.Join(fields, " ")
}
```

### 2.4 配置初始化：InitConfig()
```go
// config/hot_reload.go
func InitConfig() {
    // 将 flag 值加载到全局变量（方便访问）
    LimitTPS = LimitTPSFlag.MustGetVal()
    IncRateLimit = IncRateLimitFlag.MustGetVal()
    DebugMode = debugModeFlag.MustGetVal()
    ShowMessage = showMessageFlag.MustGetVal()
    ShowSinkSQL = showSinkSQLFlag.MustGetVal()
    ShowEngineDetail = showEngineDetailVar.MustGetVal()
    
    // 生成任务标识（用于 SQL hint）
    TransferHint = fmt.Sprintf("/*+DTS:%s*/", GetTaskName())
    
    RunTimeAutonomy = Autonomy.MustGetVal()
}
```

---

## 三、Host:Port 解析流程

### 3.1 入口：NewInstance()
```go
// internal/instance/instance.go
func NewInstance(instTag, dbType, hosts, user, passwd, db string) (inst Instance, err error) {
    log.Infof("NewInstance, instTag: %s, dbType: %s, hosts: %s, user: %s, passwd: %s, db: %s", 
              instTag, dbType, hosts, user, passwd, db)
    
    if hosts == "" {
        return
    }
    
    // 1. 提取第一个地址（支持逗号分隔的多地址）
    host := strings.Split(hosts, ",")[0]
    
    // 2. 生成实例签名（用于连接池复用）
    instSign := fmt.Sprintf("%s:%s:%s#%s", dbType, instTag, host, db)
    
    // 3. 检查是否已存在实例
    inst = sharedInstances[instSign]
    if inst != nil {
        log.Infof("instance %s is exist", instSign)
        return
    }
    
    // 4. 根据数据库类型创建实例
    if strings.Contains(dbType, TypeMySQL) {
        inst, err = mi.NewMySQLInstance(hosts, user, passwd, db)
    } else if strings.Contains(dbType, TypeClickhouse) {
        inst, err = clickhouse.NewClickhouseInstance(hosts, user, passwd, db)
    } else if strings.Contains(dbType, TypePostgres) {
        inst, err = postgre.NewPostgresInstance(hosts, user, passwd, db)
    }
    // ... 其他数据库类型
    
    // 5. 缓存实例
    if err == nil {
        sharedInstances[instSign] = inst
    }
    return
}
```

**hosts 参数格式**：
- 单地址：`192.168.1.100:3306`
- 多地址：`192.168.1.100:3306,192.168.1.101:3306`（仅使用第一个）

### 3.2 MySQL 实例的 Host:Port 解析
```go
// internal/instance/mysql/instance_mysql.go
func NewMySQLInstance(hosts, user, passwd, dbName string) (mi *MySQLInstance, err error) {
    // 1. 提取第一个地址
    firstHost := strings.Split(hosts, ",")[0]
    
    // 2. 分离 IP 和 Port
    fields := strings.Split(firstHost, ":")
    ip := fields[0]                              // 例如：192.168.1.100
    portVal, err := strconv.ParseInt(fields[1], 10, 64)  // 例如：3306
    if err != nil {
        return
    }
    port := int(portVal)
    
    // 3. 创建 MySQL 连接
    mysqlDU, err := du.NewMySQLWithTimeout(ip, port, user, passwd, dbName, config.QueryTimeout.MustGetVal())
    if err != nil {
        return
    }
    
    // 4. 配置连接池参数
    mysqlDU.InterpolateParams = true
    mysqlDU.Compress = config.CompressModeFlag.MustGetVal()
    mysqlDU.SetMaxOpenConns(
        int(math.Max(float64(config.SinkWorkerNum.MustGetVal()), 
                     float64(config.SourceWorkerNum.MustGetVal()))*2 + 2))
    mysqlDU.SetMaxIdleConns(config.SinkWorkerNum.MustGetVal())
    mysqlDU.SetRetryTimes(10)
    mysqlDU.MaxLifetime = config.ConnectionMaxLifetime.MustGetVal()
    mysqlDU.MaxIdleTime = config.ConnectionMaxIdleTime.MustGetVal()
    mysqlDU.MultiStatements = true
    
    // 5. 配置 SSL
    if config.SSLModeFlag.MustGetVal() {
        mysqlDU.UseSSL = true
    }
    
    // 6. 返回实例
    mi = &MySQLInstance{
        ip:      ip,
        port:    port,
        dbName:  dbName,
        mysqlDU: mysqlDU,
    }
    return
}
```

### 3.3 其他数据库的解析（相同模式）

**PostgreSQL**:
```go
func NewPostgresInstance(hosts, user, passwd, dbName string) (pi *PostgreInstance, err error) {
    fields := strings.Split(strings.Split(hosts, ",")[0], ":")
    ip := fields[0]
    portVal, err := strconv.ParseInt(fields[1], 10, 64)
    port := int(portVal)
    
    // 构造 PostgreSQL 连接字符串
    connConf, err := pgx.ParseConnectionString(fmt.Sprintf(
        "postgres://%s:%s@%s:%d/%s", user, passwd, ip, port, dbName))
    // ...
}
```

**OceanBase**:
```go
func NewObInstance(hosts string, user, passwd, dbName string) (mi *OceanbaseInstance, err error) {
    fields := strings.Split(strings.Split(hosts, ",")[0], ":")
    ip := fields[0]
    portVal, err := strconv.ParseInt(fields[1], 10, 64)
    port := int(portVal)
    // ...
}
```

**ClickHouse**:
```go
func NewClickhouseInstance(hosts, user, passwd, dbName string) (chi *ClickhouseInstance, err error) {
    fields := strings.Split(strings.Split(hosts, ",")[0], ":")
    ip := fields[0]
    portVal, err := strconv.ParseInt(fields[1], 10, 64)
    port := int(portVal)
    // ...
}
```

---

## 四、完整的配置流转流程

```
┌─────────────────────────────────────────────────┐
│  1. 外部调用（启动 CDC）                           │
│                                                 │
│  ./transfer \                                   │
│    --sub_cmd=sync \                            │
│    --source_host=192.168.1.100:3306 \         │
│    --source_user=repl_user \                   │
│    --source_password=passwd123 \               │
│    --sink_host=192.168.1.200:3306 \           │
│    --sink_user=target_user \                   │
│    --sink_password=passwd456 \                 │
│    --metadata_type=consul \                    │
│    --metadata_server=127.0.0.1:8500 \         │
│    --task_name=order_sync \                    │
│    --job_name=prod_sync                        │
└─────────────────┬───────────────────────────────┘
                  │
                  ↓
┌─────────────────────────────────────────────────┐
│  2. main() → showCommand()                      │
│                                                 │
│  - ezflag.Parse(reservedKeys)                  │
│    解析所有命令行参数到 Flag 变量中              │
│                                                 │
│  - config.PreparePasswd()                      │
│    提取并安全存储密码                           │
│                                                 │
│  - steganography(rawArgs)                      │
│    隐藏密码后打印命令                           │
└─────────────────┬───────────────────────────────┘
                  │
                  ↓
┌─────────────────────────────────────────────────┐
│  3. prepareEnv() → config.InitConfig()          │
│                                                 │
│  将 Flag 值加载到全局变量：                      │
│  - LimitTPS = LimitTPSFlag.MustGetVal()        │
│  - DebugMode = debugModeFlag.MustGetVal()      │
│  - TransferHint = "/*+DTS:order_sync*/"        │
└─────────────────┬───────────────────────────────┘
                  │
                  ↓
┌─────────────────────────────────────────────────┐
│  4. source.StartDump()                          │
│                                                 │
│  获取源配置：                                    │
│  - instHosts = config.SourceHostFlag.MustGetVal() │
│    → "192.168.1.100:3306"                      │
│  - sourceUser = config.GetSourceUser()         │
│    → "repl_user"                               │
│  - sourcePasswd = config.GetSourcePasswd()     │
│    → "passwd123"                               │
└─────────────────┬───────────────────────────────┘
                  │
                  ↓
┌─────────────────────────────────────────────────┐
│  5. NewDumpService() → NewDumpInstance()        │
│                                                 │
│  传递参数：                                      │
│  - hosts: "192.168.1.100:3306"                 │
│  - user: "repl_user"                           │
│  - passwd: "passwd123"                         │
└─────────────────┬───────────────────────────────┘
                  │
                  ↓
┌─────────────────────────────────────────────────┐
│  6. instance.NewInstance()                      │
│                                                 │
│  - instTag: "source"                           │
│  - dbType: "mysql"                             │
│  - hosts: "192.168.1.100:3306"                 │
└─────────────────┬───────────────────────────────┘
                  │
                  ↓
┌─────────────────────────────────────────────────┐
│  7. mi.NewMySQLInstance()                       │
│                                                 │
│  解析 host:port：                               │
│  - hosts.Split(",")[0] → "192.168.1.100:3306"  │
│  - fields = Split(":")                         │
│    ├─ ip = fields[0] = "192.168.1.100"        │
│    └─ port = parseInt(fields[1]) = 3306       │
│                                                 │
│  创建连接：                                      │
│  - du.NewMySQLWithTimeout(                     │
│      "192.168.1.100", 3306,                    │
│      "repl_user", "passwd123", "", timeout)    │
└─────────────────┬───────────────────────────────┘
                  │
                  ↓
┌─────────────────────────────────────────────────┐
│  8. 返回 MySQLInstance                          │
│                                                 │
│  mi := &MySQLInstance{                         │
│      ip:      "192.168.1.100",                 │
│      port:    3306,                            │
│      dbName:  "",                              │
│      mysqlDU: *db.MySQL,  // 连接池            │
│  }                                              │
└─────────────────────────────────────────────────┘
```

---

## 五、调用方式示例

### 5.1 完整命令行示例
```bash
#!/bin/bash

./transfer \
  # 子命令
  --sub_cmd=sync \
  
  # 源数据库配置
  --source_type=mysql.no_schema \
  --source_host=192.168.1.100:3306 \
  --source_user=repl_user \
  --source_password='MyP@ssw0rd!' \
  
  # 目标数据库配置
  --sink_type=mysql \
  --sink_host=192.168.1.200:3306 \
  --sink_user=target_user \
  --sink_password='TargetP@ss!' \
  --sink_db=target_db \
  
  # 元数据服务配置
  --metadata_type=consul \
  --metadata_server=127.0.0.1:8500 \
  
  # 任务标识
  --task_name=order_sync_task \
  --job_name=production_sync \
  --namespace=order_system \
  
  # 同步范围
  --start_position='' \
  --stop_position='' \
  
  # 性能配置
  --dump_batch_size=5000 \
  --insert_batch_size=1024 \
  --source_worker_num=4 \
  --sink_worker_num=8 \
  --speed_limit=10000 \
  --max_memory_mb=4096 \
  
  # 功能开关
  --sync_ddl=true \
  --init_table_schema=true \
  --ignore_duplicate_record=true \
  
  # 日志配置
  --log_dir=/var/log/cdc \
  --logtostderr=false \
  
  # Web 服务
  --web_service_port=7082
```

### 5.2 最小配置示例
```bash
./transfer \
  --sub_cmd=sync \
  --source_host=192.168.1.100:3306 \
  --source_user=repl_user \
  --source_password='passwd' \
  --sink_host=192.168.1.200:3306 \
  --sink_user=sink_user \
  --sink_password='passwd' \
  --metadata_type=local \
  --task_name=test_sync
```

### 5.3 通过配置文件（如果支持）
某些参数可能支持从配置文件读取（具体取决于实现）：
```json
{
  "sub_cmd": "sync",
  "source": {
    "type": "mysql.no_schema",
    "host": "192.168.1.100:3306",
    "user": "repl_user",
    "password": "MyP@ssw0rd!"
  },
  "sink": {
    "type": "mysql",
    "host": "192.168.1.200:3306",
    "user": "target_user",
    "password": "TargetP@ss!",
    "database": "target_db"
  },
  "metadata": {
    "type": "consul",
    "server": "127.0.0.1:8500"
  },
  "task": {
    "name": "order_sync_task",
    "job_name": "production_sync"
  }
}
```

---

## 六、参数获取的多种方式

### 6.1 直接通过 Flag 获取
```go
func StartDump(ctxBase context.Context) {
    // 方式1: 通过 Flag 的 MustGetVal() 方法
    instHosts := config.SourceHostFlag.MustGetVal()
    sourceUser := config.GetSourceUser()
    sourcePasswd := config.GetSourcePasswd()
    
    // ...
}
```

### 6.2 通过封装的 Getter 函数
```go
// config/src_config.go
func GetStartPos() string {
    if StartPos == "" {
        StartPos = StartPosFlag.MustGetVal()
    }
    return StartPos
}

func GetSourceConcurrentNum() int {
    return SourceWorkerNum.MustGetVal()
}
```

### 6.3 通过全局变量（已加载）
```go
// config/hot_reload.go
var (
    LimitTPS         = -1
    IncRateLimit     = false
    DebugMode        bool
    ShowMessage      bool
)

func InitConfig() {
    // 在 prepareEnv 阶段加载
    LimitTPS = LimitTPSFlag.MustGetVal()
    DebugMode = debugModeFlag.MustGetVal()
}

// 使用时直接访问
func GetLimitTPS() int {
    return LimitTPS
}
```

---

## 七、特殊参数处理

### 7.1 密码参数的安全处理
```go
// 1. 解析后立即复制到单独内存
func PreparePasswd() {
    tmpPasswd := SourcePasswdFlag.MustGetVal()
    copyPasswd := make([]byte, len(tmpPasswd))
    copy(copyPasswd, tmpPasswd)
    sourcePasswd = string(copyPasswd)
}

// 2. 日志中隐藏密码
func steganography(rawArgs []string) string {
    // 将 --source_password=xxx 替换为 --source_password=******
}
```

### 7.2 Host 参数支持多地址
```go
// 输入：192.168.1.100:3306,192.168.1.101:3306,192.168.1.102:3306
// 解析：只使用第一个地址
host := strings.Split(hosts, ",")[0]  // "192.168.1.100:3306"
```

### 7.3 GTID 位点参数
```go
// 起始位点可以是：
// 1. 空字符串 "" → 从元数据读取
// 2. "from_scratch" → 从头开始
// 3. "newest" → 从最新位置开始
// 4. 具体 GTID → "3e11fa47-71ca-11e1-9e33-c80aa9429562:1-5"

startPos := config.GetStartPos()
if startPos == config.ChangePosFromScratch {
    // 清理元数据，从头开始
    metadata.CleanTableInfo(ctx)
} else if startPos == config.ChangePosNewest {
    // 获取当前最新位点
    startPos, err = src.dumper.LatestPosition(ctx)
}
```

### 7.4 时间参数解析
```go
// 输入格式：2024-01-01 10:00:00
startTimeStr := config.StartTimeFlag.MustGetVal()
if startTimeStr != "" {
    startTimeVal, err := time.ParseInLocation(
        config.DatetimeFormat,  // "2006-01-02 15:04:05"
        startTimeStr, 
        time.Local)
    
    startTime = uint64(startTimeVal.Unix())
}
```

---

## 八、配置热更新机制

CDC 支持部分配置的热更新，无需重启服务：

### 8.1 热更新的配置
```go
// config/hot_reload.go
type InstanceConfig struct {
    Host string `json:"host"`
    Port int    `json:"port"`
}

var (
    // 监听配置变更的 Channel
    SinkChangeEvent   = make(chan *InstanceConfig, 1)
    SourceChangeEvent = make(chan *InstanceConfig, 1)
)
```

### 8.2 监听配置变更
```go
func StartDump(ctxBase context.Context) {
    for {
        // 正常运行
        go dumper.Run(ctx, startPos, stopPos)
        
        select {
        // 监听源配置变更
        case instConfig = <-config.SourceChangeEvent:
            instHosts = instConfig.Host
            cancel()  // 取消当前 dumper
            // 循环重新创建新的 dumper
        
        case <-dumper.FinishChan():
            return
        }
    }
}
```

---

## 九、调用流程完整示意图

```
外部调用（CLI/Manager）
    ↓ 传入命令行参数
┌─────────────────────────────────────┐
│  main()                             │
│  - showCommand()                    │
│    ├─ ezflag.Parse()               │  解析所有参数
│    ├─ PreparePasswd()              │  安全存储密码
│    └─ steganography()              │  隐藏敏感信息
└─────────────────┬───────────────────┘
                  │
                  ↓
┌─────────────────────────────────────┐
│  prepareEnv()                       │
│  - config.InitConfig()              │  加载配置到全局变量
└─────────────────┬───────────────────┘
                  │
                  ↓
┌─────────────────────────────────────┐
│  execSync() / execDump() / ...      │
│                                     │
│  获取配置参数：                      │
│  - SourceHostFlag.MustGetVal()     │  → "192.168.1.100:3306"
│  - GetSourceUser()                 │  → "repl_user"
│  - GetSourcePasswd()               │  → "passwd123"
└─────────────────┬───────────────────┘
                  │
                  ↓
┌─────────────────────────────────────┐
│  NewDumpService()                   │
│  传递：hosts, user, passwd          │
└─────────────────┬───────────────────┘
                  │
                  ↓
┌─────────────────────────────────────┐
│  instance.NewInstance()             │
│  - 解析 hosts 字符串                │
│  - 提取第一个地址                   │
│  - Split(hosts, ",")[0]            │
└─────────────────┬───────────────────┘
                  │
                  ↓
┌─────────────────────────────────────┐
│  NewMySQLInstance()                 │
│  - Split host:port                 │
│  - fields = Split(":")             │
│  - ip = fields[0]                  │  "192.168.1.100"
│  - port = parseInt(fields[1])      │  3306
│  - du.NewMySQL(ip, port, ...)     │  创建连接
└─────────────────┬───────────────────┘
                  │
                  ↓
┌─────────────────────────────────────┐
│  MySQLInstance                      │
│  {                                  │
│    ip: "192.168.1.100",            │
│    port: 3306,                     │
│    mysqlDU: *MySQL连接池            │
│  }                                  │
└─────────────────────────────────────┘
```

---

## 十、总结

### 配置解析的关键步骤：

1. **命令行参数定义**：通过 `ezflag.NewStringVar/NewIntVar/NewBoolVar` 定义所有参数
2. **参数解析**：在 `showCommand()` 中调用 `ezflag.Parse()` 解析
3. **密码安全处理**：`PreparePasswd()` 复制密码到独立内存，`steganography()` 隐藏日志
4. **配置初始化**：`InitConfig()` 将 flag 值加载到全局变量
5. **Host:Port 解析**：在 `NewInstance()` 系列函数中通过 `strings.Split()` 解析
6. **连接创建**：使用解析后的 IP 和 Port 创建数据库连接

### 支持的参数格式：

- **Host**: `ip:port` 或 `ip:port,ip:port,ip:port`（多地址，使用第一个）
- **GTID**: 标准 MySQL GTID 格式或特殊关键字（`from_scratch`, `newest`）
- **时间**: `2006-01-02 15:04:05` 格式
- **密码**: 任意字符串，会被安全处理

这个 CDC 中间件的配置系统设计得非常完善，既支持命令行参数，又有安全机制保护敏感信息，还支持部分配置的热更新！

