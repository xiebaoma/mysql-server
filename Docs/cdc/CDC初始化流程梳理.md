# CDC 中间件初始化流程梳理

## 概述
这是一个用 Go 编写的 CDC (Change Data Capture) 中间件，核心实现被上层 manager 调用，负责与源数据库交互并完成数据同步。在正式执行 dump 和 apply 工作之前，系统会经过一系列完整的初始化步骤。

---

## 一、程序入口与命令解析 (main.go)

### 1.1 启动流程
```go
func main() {
    defer log.Flush()
    
    // 1. 显示命令行参数
    showCommand()
    
    // 2. 显示版本信息
    showVersion(config.VersionFlag.MustGetVal())
    
    // 3. 创建根上下文
    env.RootCtx, env.Cancel = context.WithCancel(context.Background())
    
    // 4. 准备运行环境（核心初始化）
    prepareEnv(env.RootCtx)
    
    // 5. 根据子命令执行不同操作
    switch config.SubCmd.MustGetVal() {
    case config.SubCommandPreCheck:
        execPrecheck(ctx)
    case config.SubCommandDump:
        execDump(ctx)      // 仅 dump
    case config.SubCommandApply:
        execApply(ctx)     // 仅 apply
    case config.SubCommandSync:
        execSync(ctx)      // dump + apply 同步模式
    case config.SubCommandDiff:
        execDiff(ctx)
    }
}
```

### 1.2 密码处理与参数隐写
- 在 `showCommand()` 中调用 `config.PreparePasswd()` 解析密码参数
- 使用 `steganography()` 函数隐藏敏感信息（密码等）在日志中显示

---

## 二、环境准备阶段 (prepareEnv)

这是**最核心的初始化阶段**，位于 `cmd/env.go` 中：

```go
func prepareEnv(ctx context.Context) {
    // 1. 初始化配置
    config.InitConfig()
    
    // 2. 设置日志大小限制
    glog.MaxSize = uint64(config.MaxLogSize.MustGetVal() * 1024 * 1024)
    
    // 3. 启动退出信号监听（优雅退出）
    go env.ListenExitSignal()
    
    // 4. 检查副本模式环境
    err = checkReplicaModeEnv()
    
    // 5. 注册监控指标
    metrics.RegisterMetrics()
    
    // 6. 自动刷新存活时间
    go util.AutoRefreshAliveTime(ctx)
    
    // 7. 初始化滑动窗口（用于数据确认）
    confirm_window.InitSlidingWindow(ctx)
    
    // 8. 启动 Web 服务（监控和管理接口）
    web.StartWebService(ctx)
    
    // 9. 启动监控数据收集
    go monitoring.DoMetricsStaff(ctx)
    
    // 10. 启动速度限制器自动刷新
    go speed_limit.SpeedLimiter.AutoRefresh(ctx)
    
    // 11. 初始化表匹配规则
    err = table_match.InitMatchTableRules()
    
    // 12. 初始化节流器（内存、速率等）
    err = throttler.InitThrottler(ctx)
    
    // 13. 初始化消息队列环境
    message_queue.InitMessageQueueEnv(ctx)
    
    // 14. 打开元数据服务（关键步骤）
    metadata.OpenService(ctx)
    
    // 15. 初始化自定义规则（必须在元数据服务之后）
    user_rules.InitCustomRule(ctx)
    
    // 16. 刷新 dump 批次大小
    go speed_limit.RefreshDumpBatchSize(ctx)
    
    // 17. 收集资源使用情况
    env.CollectResourceUsage(ctx)
    
    // 18. 收集 CDC MTTR 指标
    go metadata.CollectCDCMTTR(ctx)
}
```

### 2.1 关键子系统初始化详解

#### A. 元数据服务 (metadata.OpenService)
```go
func OpenService(ctx context.Context) {
    // 1. 根据配置类型创建元数据服务（etcd/consul/mysql/redis等）
    stv := config.MetadataTypeVar.MustGetVal()
    factory := allFactories[stv]
    MDService, err = factory.Create(ssv, vuv, vpv)
    
    // 2. 保存 HTTP 端口信息到元数据
    SaveHTTPPort(ctx)
    
    // 3. 设置 IP:Port 信息
    SetIPPort(ctx)
    
    // 4. 清空上次的错误信息
    SetLastError(ctx, "")
    
    // 5. 启动位点定期保存协程
    go periodSavePosition(ctx)
}
```

#### B. 节流器初始化 (throttler.InitThrottler)
包含多种节流器：
- **内存节流器** (`MemoryThrottler`): 控制内存使用，防止 OOM
  - 监控堆内存使用
  - 根据阈值自动限流
  - 使用滑动窗口机制
- **速率限制器**: 控制同步速率（TPS/QPS）
- **带宽限制器**: 控制网络流量

#### C. 消息队列初始化
```go
func InitMessageQueueEnv(ctx context.Context) {
    // 初始化冲突检测的消息队列生产者
    err = initConflictProducer(ctx)
    // 支持 Kafka、Pulsar 等多种 MQ
}
```

---

## 三、Dump 服务初始化 (source.StartDump)

### 3.1 启动流程
```go
func StartDump(ctxBase context.Context) {
    // 1. 获取源数据库连接信息
    instHosts := config.SourceHostFlag.MustGetVal()
    sourceUser := config.GetSourceUser()
    sourcePasswd := config.GetSourcePasswd()
    
    for {
        // 2. 创建 DumpService 实例
        dumper, err = NewDumpService(instHosts, sourceGroup, sourceUser, sourcePasswd)
        
        // 3. 预检查（关键步骤）
        err = dumper.PrepareCheck(ctx)
        
        // 4. 准备运行环境（获取位点）
        startPos, stopPos := dumper.PrepareRunningEnv(ctx)
        
        // 5. 显示表信息（加载需要同步的表）
        err = dumper.ShowTables(ctx)
        
        // 6. 启动 dump 工作
        go dumper.Run(ctx, startPos, stopPos)
        
        // 7. 监听完成/错误/配置变更信号
        select {
        case <-dumper.FinishChan():
        case err = <-dumper.ErrorChan():
        case instConfig = <-config.SourceChangeEvent:
        }
    }
}
```

### 3.2 创建 Dump 实例
```go
func NewDumpService(...) {
    // 根据类型创建不同的 dumper
    switch dumperType {
    case config.DumperMysqlNoSchema:
        dumpInst, err = dmns.NewDumpInstance(instHosts, sourceUser, sourcePasswd)
    }
    
    // 封装为 TPCDumpService（全量+增量两阶段）
    src = &TPCDumpService{
        dumper:    dumpInst,
        errorChan: make(chan error, 1),
    }
}
```

### 3.3 MySQL Dump 实例创建
```go
func NewDumpInstance(hosts, user, passwd string) {
    // 1. 创建底层数据库实例连接
    inst, err := instance.NewInstance("source", "mysql", hosts, user, passwd, "")
    
    // 2. 设置连接池大小
    inst.SetMaxOpenConns(config.GetSourceConcurrentNum() + 2)
    
    // 3. 初始化 MySQLDumperNoSchema 结构
    ds = &MySQLDumperNoSchema{
        Name:         "MySQLNoSchemaDumper",
        replUser:     user,
        replPasswd:   passwd,
        allTables:    make(map[string]map[string]*table.Table),
        finish:       make(chan struct{}, 1),
        Instance:     inst,
        currentHints: make(map[string]string),
    }
}
```

### 3.4 预检查阶段 (PrepareCheck)
```go
func (src *TPCDumpService) PrepareCheck(ctx context.Context) error {
    return src.dumper.VerifyVariables(ctx)
}
```

**验证 MySQL 变量配置**：
```go
func VerifyVariables(ctx context.Context) error {
    // 检查以下 MySQL 配置项是否符合要求：
    
    // 1. binlog_row_value_options 必须为空（不支持）
    // 2. binlog_row_metadata 必须为 "MINIMAL"
    // 3. binlog_encryption 必须为 "OFF"（不支持加密 binlog）
    // 4. binlog_rotate_encryption_master_key_at_startup 必须为 "OFF"
    // 5. binlog_transaction_compression 必须为 "OFF"（不支持压缩）
    // 6. sql_generate_invisible_primary_key 必须为 "OFF"
    
    // 任何不符合的配置都会导致初始化失败
}
```

### 3.5 准备运行环境 (PrepareRunningEnv)
```go
func (src *TPCDumpService) PrepareRunningEnv(ctx context.Context) (startPos, stopPos string) {
    // 1. 获取配置的起始位点
    startPos = config.GetStartPos()
    stopPos = config.StopPosFlag.MustGetVal()
    
    // 2. 处理不同的起始位点场景
    if startPos == "" {
        // 从元数据读取保存的 GTID 位点
        startPosPtr, err = metadata.GetChangePosition(ctx)
        if startPosPtr != nil {
            startPos = *startPosPtr
        }
    } else if startPos == config.ChangePosFromScratch {
        // 从头开始同步，清理之前的元数据
        metadata.CleanTableInfo(ctx)
        metadata.SetInitChangePosition(ctx, "")
        startPos = ""
    }
    
    // 3. 解析停止位点（GTID Set）
    if stopPos != "" && stopPos != config.PositionEndAfterFull {
        stopGTIDSet, err = mysql.ParseGTIDSet(mysql.MySQLFlavor, stopPos)
    }
    
    // 4. 解析起始和停止时间
    if startTimeStr != "" {
        startTimeVal, err = time.ParseInLocation(config.DatetimeFormat, startTimeStr, time.Local)
        startTime = uint64(startTimeVal.Unix())
    }
    
    // 5. 初始化流式同步环境
    src.dumper.InitStreamEnv(ctx, startPos, startTime, stopGTIDSet, stopTime)
}
```

### 3.6 初始化流式环境 (InitStreamEnv)
```go
func (mdns *MySQLDumperNoSchema) InitStreamEnv(
    ctx context.Context, initPos string, startTime uint64, 
    endPos mysql.GTIDSet, endTime uint64) {
    
    // 1. 保存位点和时间信息
    mdns.initPos = initPos
    mdns.startTime = startTime
    mdns.endPos = endPos
    mdns.endTime = endTime
    
    // 2. 获取源数据库的全局变量
    inst := mdns.Instance.(*mi.MySQLInstance)
    
    // 获取 report_host
    reportVal, _ := inst.ShowVariable(ctx, "report_host")
    if reportVal != nil {
        config.ReportHost = reportVal.(string)
    }
    
    // 获取 hostname
    hostVal, _ := inst.ShowVariable(ctx, "hostname")
    if hostVal != nil {
        config.HostName = hostVal.(string)
    }
    
    // 3. 启动监控指标收集
    go initMetric(ctx, mdns.Instance)
}
```

### 3.7 显示表信息 (ShowTables)
```go
func (src *TPCDumpService) ShowTables(ctx context.Context) (err error) {
    // 1. 加载所有需要同步的表
    tablesInGroup, err := src.dumper.ShowTables(ctx)
    
    src.tablesInGroup = tablesInGroup
    
    // 2. 统计表数量
    var num = 0
    for _, tbls := range src.tablesInGroup {
        num += len(tbls)
    }
    
    // 3. 获取外键引用关系
    referMap, err := src.dumper.GetForeignReferMap(ctx, GetDatabasesFromTableGroups(tablesInGroup))
    src.referMap = referMap
    
    // 4. 更新监控指标
    metrics.GaugeSyncTableNum.Set(float64(num))
}
```

### 3.8 运行 Dump (Run)
```go
func (src *TPCDumpService) Run(ctx context.Context, startPos, stopPos string) {
    // 1. 如果没有起始位点
    if startPos == "" {
        if config.SourceTypeFlag.MustGetVal() == config.DumperMysqlNoSchema {
            // CDC 类型：只做增量
            startPos, err = src.dumper.LatestPosition(ctx)
            src.dumper.SetStreamStartPos(startPos)
        } else {
            // 普通类型：先全量再增量
            metadata.SetFullStatus(ctx)
            
            // 加载初始位点
            startPos, err = src.loadInitChangePosition(ctx)
            
            // 清理表结构缓存
            metadata.CleanTableSchema(ctx)
            
            // 设置外键引用关系
            src.dumper.SetReferMap(ctx, src.referMap)
            
            // 执行全量 dump
            src.dumper.BatchDump(ctx, src.tablesInGroup, startPos)
            
            // 初始化流式同步起始位点
            src.dumper.InitStreamStartPos(ctx, startPos)
            
            // 等待全量数据完成
            concurrent.WaitWithDuration(ctx, time.Minute+time.Second*30)
        }
    }
    
    // 2. 只同步全量数据的情况
    if stopPos == config.PositionEndAfterFull {
        src.dumper.Finish(ctx)
        return
    }
    
    // 3. 开始增量同步（读取 binlog）
    confirm_window.SetMaxChangePosition(startPos)
    metadata.SetIncrStatus(ctx)
    
    // 重置监控指标
    metrics.GaugeFullTotalRowNum.Set(0)
    metrics.GaugeFullFinishRowNum.Set(0)
    
    // 启动流式 dump（binlog 读取）
    err = src.dumper.StreamDump(ctx)
    src.errorChan <- err
}
```

---

## 四、Apply 服务初始化 (sink.StartApply)

### 4.1 启动流程
```go
func StartApply(ctxBase context.Context) {
    // 1. 获取目标数据库连接信息
    instHost := config.SinkHostFlag.MustGetVal()
    sinkUser := config.SinkUserFlag.MustGetVal()
    sinkPasswd := config.GetSinkPasswd()
    sinkDB := config.SinkDBFlag.MustGetVal()
    
    for {
        // 2. 创建数据应用器
        applier, err = NewDataApplier(onceCtx, instHost, sinkGroup, sinkUser, sinkPasswd, sinkDB)
        
        // 3. 启动应用器
        go applier.Run(onceCtx)
        
        // 4. 监听完成/错误/配置变更信号
        select {
        case instConfig := <-config.SinkChangeEvent:
        case <-ctxBase.Done():
        case err = <-applier.ErrorChan():
        }
    }
}
```

### 4.2 创建 Applier
```go
func NewDataApplier(ctx context.Context, ...) (applier Applier, err error) {
    var workerNum = config.SinkWorkerNum.MustGetVal()
    
    // 根据 worker 数量和目标类型选择应用器模式
    if workerNum <= 1 || 
       config.SinkType.MustGetVal() == config.SinkTypeDataWarehouseClickhouse ||
       config.SinkType.MustGetVal() == config.SinkTypeCDC {
        // 单线程模式
        return newSimpleApplier(ctx, hosts, sinkGroup, sinkUser, sinkPasswd, sinkDB)
    }
    
    // 多线程 DAG 模式
    return newDAGApplier(ctx, hosts, sinkGroup, sinkUser, sinkPasswd, sinkDB, workerNum)
}
```

### 4.3 创建 Sinker
根据目标类型创建不同的 Sinker：
```go
func newSinker(ctx context.Context, id int, host, sinkGroup, sinkUser, sinkPasswd, sinkDB string) {
    sinkType := config.SinkType.MustGetVal()
    
    // 根据配置选择不同的 sink 类型
    switch sinkType {
    case config.SinkTypeMysql:
        sinker = apply_mysql.NewApplier(ctx, id, host, sinkGroup, sinkUser, sinkPasswd, sinkDB)
    case config.SinkTypeOceanBase:
        sinker = apply_ob.NewApplier(ctx, id, host, sinkGroup, sinkUser, sinkPasswd, sinkDB)
    case config.SinkTypeDataWarehouseClickhouse:
        sinker = apply_warehouse.NewApplierForClickhouse(...)
    case config.SinkTypePostgres:
        sinker = apply_postgres.NewApplier(...)
    case config.SinkTypeRedis:
        sinker = apply_redis.NewApplier(...)
    case config.SinkTypeElasticsearch:
        sinker = apply_es.NewApplier(...)
    // ... 其他类型
    }
}
```

### 4.4 SimpleApplier 运行逻辑
```go
func (ss *SimpleApplier) Run(stopCtx context.Context) {
    for {
        // 1. 从本地队列消费消息
        msg = cache_queue.LocalQueue.Consume(stopCtx)
        if msg == nil {
            continue
        }
        
        // 2. 更新监控指标
        metrics.MessageCounterSink.Inc()
        metrics.MessageDelaySummary.Observe(...)
        
        // 3. 添加到确认窗口
        confirm_window.AddPendingElement(msg)
        
        // 4. 处理心跳和空消息
        if msg.IsHeartbeat() || (msg.IsEmpty() && !config.ReplicaModeFlag.MustGetVal()) {
            confirm_window.AckElement(msg.GetConfirmInfo())
            continue
        }
        
        // 5. 执行数据应用
        err = ss.sinker.Execute(stopCtx, packet)
        
        // 6. 内存节流处理
        if throttler.MemoryThrottlerInst != nil {
            throttler.MemoryThrottlerInst.Consume(msg)
        }
    }
}
```

### 4.5 DAGApplier 运行逻辑
```go
func (ds *DAGApplier) Run(ctx context.Context) {
    // 1. 启动 DAG 引擎
    go ds.engine.Start(ctx)
    
    for {
        // 2. 从队列消费消息
        msg = cache_queue.LocalQueue.Consume(ctx)
        
        // 3. 处理消息类型转换（批量/流式）
        if lastMessageType == message.MessageBatch && 
           dataMsg.MessageType == message.MessageStream {
            // 等待所有批量消息完成
            for ds.engine.Nodes.Size() > 0 {
                concurrent.WaitWithDuration(ctx, time.Second)
            }
        }
        
        // 4. 处理独占任务（DDL 等需要独占执行）
        needExclusive := dataMsg.NeedExclusive()
        for needExclusive {
            if runningPacketNum > 0 || ds.engine.HaveExclusiveTaskRunning() {
                concurrent.WaitWithDuration(ctx, time.Second)
            } else {
                break
            }
        }
        
        // 5. 将消息投递到 DAG 引擎
        ds.engine.FeedPacket(dataMsg)
    }
}
```

---

## 五、Sync 模式（Dump + Apply）

```go
func execSync(ctx context.Context) {
    log.Info("starting sync service...")
    
    // 1. 启动 Apply 服务（消费数据）
    go sink.StartApply(ctx)
    
    // 2. 启动触发器消息处理
    go trigger.DoTriggerMessage(ctx)
    
    // 3. 启动 Dump 服务（生产数据）
    source.StartDump(ctx)
}
```

**数据流向**：
```
Source DB (MySQL) 
    ↓
StreamDump (读取 binlog)
    ↓
cache_queue.LocalQueue (本地队列)
    ↓
Applier (消费队列)
    ↓
Sinker (写入目标)
    ↓
Target DB
```

---

## 六、核心组件与数据流

### 6.1 确认窗口 (confirm_window)
- 管理待确认的消息
- 追踪消息处理状态
- 支持消息确认和回滚
- 用于保证 at-least-once 语义

### 6.2 本地队列 (cache_queue.LocalQueue)
- Producer（Dump）写入消息
- Consumer（Apply）读取消息
- 解耦 dump 和 apply 的速度差异

### 6.3 位点管理
```
元数据存储 (etcd/consul/mysql)
    ↓
位点类型：
- InitChangePosition: 初始位点
- ChangePosition: 当前位点
- ConfirmedChangePosition: 已确认位点
    ↓
定期保存机制 (periodSavePosition)
```

### 6.4 监控体系
- **Prometheus Metrics**: 暴露各类监控指标
  - 同步延迟
  - 消息处理速率
  - 表同步进度
  - 内存使用情况
  - 网络延迟
- **Web 服务**: 提供 HTTP 接口查询状态

---

## 七、初始化流程图

```
main()
  ├─> showCommand() & showVersion()
  ├─> prepareEnv()
  │     ├─> config.InitConfig()
  │     ├─> metrics.RegisterMetrics()
  │     ├─> confirm_window.InitSlidingWindow()
  │     ├─> web.StartWebService()
  │     ├─> table_match.InitMatchTableRules()
  │     ├─> throttler.InitThrottler()
  │     │     ├─> MemoryThrottler
  │     │     ├─> RateLimiter
  │     │     └─> BandwidthLimiter
  │     ├─> message_queue.InitMessageQueueEnv()
  │     ├─> metadata.OpenService()
  │     │     ├─> 创建元数据服务连接
  │     │     ├─> SaveHTTPPort()
  │     │     ├─> SetIPPort()
  │     │     └─> go periodSavePosition()
  │     └─> user_rules.InitCustomRule()
  │
  └─> execSync() / execDump() / execApply()
        │
        ├─> source.StartDump()
        │     ├─> NewDumpService()
        │     │     └─> dmns.NewDumpInstance()
        │     │           ├─> instance.NewInstance() (数据库连接)
        │     │           └─> 初始化结构体
        │     ├─> PrepareCheck()
        │     │     └─> VerifyVariables() (检查 MySQL 配置)
        │     ├─> PrepareRunningEnv()
        │     │     ├─> 获取/解析起始位点
        │     │     ├─> 解析停止位点
        │     │     ├─> 解析时间范围
        │     │     └─> InitStreamEnv()
        │     │           ├─> 保存位点信息
        │     │           ├─> 获取源数据库变量
        │     │           └─> 启动监控
        │     ├─> ShowTables()
        │     │     ├─> 加载需要同步的表
        │     │     └─> 获取外键关系
        │     └─> Run()
        │           ├─> 全量 dump (BatchDump)
        │           └─> 增量 dump (StreamDump)
        │                 ├─> 连接 binlog
        │                 ├─> 读取 binlog 事件
        │                 ├─> 解析事件
        │                 └─> 发送到本地队列
        │
        └─> sink.StartApply()
              ├─> NewDataApplier()
              │     ├─> newSinker() (根据目标类型)
              │     └─> newSimpleApplier() / newDAGApplier()
              └─> applier.Run()
                    ├─> 从本地队列消费
                    ├─> 添加到确认窗口
                    ├─> sinker.Execute() (执行数据应用)
                    └─> 确认消息
```

---

## 八、与源数据库的交互步骤

### 8.1 连接建立
1. 从配置获取源数据库连接信息（host, port, user, password）
2. 通过 `instance.NewInstance()` 创建数据库实例
3. 设置连接池参数（max_open_conns）
4. 测试连接可用性

### 8.2 配置验证
在 `VerifyVariables()` 中检查 MySQL 的关键配置：
- binlog 格式必须是 ROW
- binlog_row_metadata 必须是 MINIMAL
- 不能开启 binlog 加密
- 不能开启 binlog 压缩
- 不能开启某些自动主键生成特性

### 8.3 元信息获取
```sql
-- 获取 MySQL 版本
SHOW VARIABLES LIKE 'version';

-- 获取 binlog 状态
SHOW MASTER STATUS; -- 或 SHOW BINARY LOG STATUS;

-- 获取已执行的 GTID
Executed_Gtid_Set

-- 获取全局变量
SHOW VARIABLES LIKE 'report_host';
SHOW VARIABLES LIKE 'hostname';
```

### 8.4 表信息加载
```sql
-- 列出所有需要同步的表（根据配置的过滤规则）
SHOW TABLES FROM database_name;

-- 获取表结构
SHOW CREATE TABLE table_name;

-- 获取外键关系
SELECT * FROM information_schema.KEY_COLUMN_USAGE 
WHERE REFERENCED_TABLE_NAME IS NOT NULL;
```

### 8.5 全量 Dump
```sql
-- 对每张表执行
SELECT * FROM table_name 
WHERE ... (根据分片规则)
LIMIT batch_size;
```

### 8.6 增量 Dump (Binlog)
1. 使用 MySQL binlog 复制协议连接
2. 从指定 GTID 位点开始订阅
3. 持续接收 binlog 事件
4. 解析 binlog 事件（INSERT/UPDATE/DELETE/DDL）
5. 转换为内部消息格式
6. 发送到本地队列

---

## 九、关键特性

### 9.1 断点续传
- 通过元数据服务保存当前 GTID 位点
- 程序重启后从上次位点继续
- 支持手动指定起始位点

### 9.2 速率控制
- 内存节流：防止内存溢出
- TPS 限制：控制同步速率
- 带宽限制：控制网络流量

### 9.3 数据一致性
- 全量阶段：记录起始 GTID，确保增量从正确位点开始
- 增量阶段：使用 GTID 保证事务完整性
- 确认窗口：确保消息被正确处理后才推进位点

### 9.4 高可用支持
- 监听配置变更（热更新源/目标地址）
- 优雅退出机制
- 错误自动恢复

### 9.5 多种目标支持
- MySQL / OceanBase
- ClickHouse / Doris (数据仓库)
- PostgreSQL / CockroachDB
- Redis
- Elasticsearch
- Kafka / Pulsar (消息队列)

---

## 十、初始化失败处理

任何初始化步骤失败都会调用 `metadata.Fatalf()`：
1. 将错误信息写入元数据服务
2. 设置任务状态为 ERROR
3. 记录详细错误日志
4. 终止程序运行

常见失败原因：
- 源/目标数据库连接失败
- MySQL 配置不符合要求
- 元数据服务不可用
- 权限不足
- 网络问题

---

## 总结

CDC 中间件的初始化是一个复杂而严谨的过程，包括：

1. **配置解析与验证**
2. **多个子系统初始化**（元数据、监控、节流、队列等）
3. **与源数据库建立连接并验证环境**
4. **加载表信息和元数据**
5. **确定同步起始位点**
6. **启动 Dump 和 Apply 服务**

整个流程设计充分考虑了：
- **可靠性**：位点保存、错误处理、断点续传
- **性能**：并发处理、内存控制、速率限制
- **可观测性**：监控指标、日志、Web 接口
- **灵活性**：多种源和目标、配置热更新

这使得该 CDC 中间件能够稳定地运行在生产环境中，支持大规模数据同步场景。

