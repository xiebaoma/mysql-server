# 批量调度器服务 (Batch Scheduler) 架构详解

## 📋 目录
- [1. 概述](#1-概述)
- [2. 核心架构](#2-核心架构)
- [3. 接口设计](#3-接口设计)
- [4. 数据模型](#4-数据模型)
- [5. 状态机设计](#5-状态机设计)
- [6. 关键流程](#6-关键流程)
- [7. 并发控制](#7-并发控制)
- [8. 扩展机制](#8-扩展机制)

---

## 1. 概述

### 1.1 服务定位
批量调度器服务是CDC Manager中的核心组件，负责**异步处理大量CDC任务的批量操作**。它采用**生产者-消费者模式**，将批量任务分解为单个任务，通过Worker池并发执行。

### 1.2 核心职责
- ✅ **批量任务调度**: 将批量操作分解为单个任务并调度执行
- ✅ **Worker池管理**: 维护固定大小的Worker池，实现并发执行
- ✅ **任务状态管理**: 跟踪批量任务和子任务的执行状态
- ✅ **重试与回滚**: 支持任务失败重试和批量操作回滚
- ✅ **超时控制**: 每个任务有独立的超时控制机制
- ✅ **异常恢复**: 任务执行失败时的自动恢复机制

### 1.3 技术特点
- 🔥 **高并发**: Worker池模式，支持32个并发Worker
- 🔥 **大容量**: 任务队列容量1000，支持批量操作
- 🔥 **可靠性**: 事务保证、乐观锁、分布式锁
- 🔥 **可扩展**: 插件式任务处理器注册机制
- 🔥 **可观测**: Prometheus指标监控

---

## 2. 核心架构

### 2.1 整体架构图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                       Batch Scheduler Service                           │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                           │
│  ┌────────────────────────────────────────────────────────────────────┐ │
│  │              SchedulerServer (调度服务器)                          │ │
│  │  ┌──────────────────────────────────────────────────────────────┐ │ │
│  │  │  定期扫描 (20s/次)                                           │ │ │
│  │  │  1. 查询未完成的BatchJob                                     │ │ │
│  │  │  2. 维护Batch状态                                            │ │ │
│  │  │  3. 分发BatchTask到Scheduler                                │ │ │
│  │  └──────────────────────────────────────────────────────────────┘ │ │
│  └──────────────────┬───────────────────────────────────────────────┘ │
│                     │                                                   │
│  ┌──────────────────▼───────────────────────────────────────────────┐ │
│  │           BatchScheduler (批量调度器)                            │ │
│  │  ┌────────────────────────────────────────────────────────────┐ │ │
│  │  │  Job Queue (任务队列)                                      │ │ │
│  │  │  Capacity: 1000                                            │ │ │
│  │  │  [Task1] [Task2] [Task3] ... [TaskN]                      │ │ │
│  │  └────────────────────────────────────────────────────────────┘ │ │
│  │                                                                   │ │
│  │  ┌────────────────────────────────────────────────────────────┐ │ │
│  │  │  Dispatcher (分派器)                                       │ │ │
│  │  │  从JobQueue取任务 → 分配给空闲Worker                       │ │ │
│  │  └────────────────────────────────────────────────────────────┘ │ │
│  └──────────────────┬───────────────────────────────────────────────┘ │
│                     │                                                   │
│  ┌──────────────────▼───────────────────────────────────────────────┐ │
│  │            Worker Pool (Worker池)                                │ │
│  │  Size: 32                                                         │ │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐         ┌─────────┐        │ │
│  │  │Worker #1│ │Worker #2│ │Worker #3│   ...   │Worker#32│        │ │
│  │  └────┬────┘ └────┬────┘ └────┬────┘         └────┬────┘        │ │
│  │       │           │           │                    │              │ │
│  └───────┼───────────┼───────────┼────────────────────┼──────────────┘ │
│          │           │           │                    │                │
│  ┌───────▼───────────▼───────────▼────────────────────▼──────────────┐ │
│  │              BatchTaskJob (批量任务作业)                          │ │
│  │  ┌──────────────────────────────────────────────────────────────┐│ │
│  │  │  1. 获取任务锁 (AcquireBatchTask)                            ││ │
│  │  │  2. 验证任务状态 (ValidateBatchTaskFinished)                 ││ │
│  │  │  3. 处理任务 (ProcessBatchTask)                              ││ │
│  │  │  4. 释放任务锁 (ReleaseBatchTask)                            ││ │
│  │  └──────────────────────────────────────────────────────────────┘│ │
│  └───────────────────────────┬──────────────────────────────────────┘ │
│                              │                                         │
│  ┌───────────────────────────▼──────────────────────────────────────┐ │
│  │        SchedulerJobHandler (任务处理器)                          │ │
│  │  ┌────────────────────┐  ┌────────────────────┐                  │ │
│  │  │ CDCBatchTaskHandler│  │  其他Handler扩展   │                  │ │
│  │  │  - 启动CDC任务     │  │                    │                  │ │
│  │  │  - 停止CDC任务     │  │                    │                  │ │
│  │  │  - 切换AZ          │  │                    │                  │ │
│  │  │  - 重置GTID        │  │                    │                  │ │
│  │  │  - 更改配置        │  │                    │                  │ │
│  │  │  - 切换运行系统    │  │                    │                  │ │
│  │  └────────────────────┘  └────────────────────┘                  │ │
│  └──────────────────────────────────────────────────────────────────┘ │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.2 组件说明

#### 2.2.1 SchedulerServer (调度服务器)
**文件**: `pkg/batch_service/batch_scheduler_server/scheduler_server.go`

**职责**:
- 定时扫描数据库中的未完成批量任务（每20秒一次）
- 维护Batch和BatchTask的状态
- 将符合条件的BatchTask分发到BatchScheduler

**关键配置**:
```go
poolSize: 32              // Worker池大小
queueSize: 1000           // 任务队列大小
scanInterval: 20s         // 扫描间隔
blockingCountLimit: 10    // 阻塞次数限制（超过则重启调度器）
```

**核心逻辑**:
```go
// 扫描批量任务
func (ss *SchedulerServer) scanBatch() {
    // 1. 查询所有未完成的BatchJob (状态为pending/running/rollback等)
    batchList := queryUnfinishedBatches()
    
    // 2. 遍历每个Batch
    for _, batch := range batchList {
        // 维护Batch状态
        isEligible, tasks := MaintainBatchStatus(batch.BatchId)
        
        // 3. 如果Batch可处理，分发其所有Task
        if isEligible {
            ss.dispatchBatchTasks(tasks, batch.Creator)
        }
    }
}

// 分发批量任务
func (ss *SchedulerServer) dispatchBatchTasks(tasks []*BatchTask, creator string) {
    for _, task := range tasks {
        // 只处理pending/retry_pending/rollback_pending状态的任务
        if task.Status == pending || task.Status == retry_pending || task.Status == rollback_pending {
            // 创建BatchTaskJob
            job := NewBatchTaskJob(taskParam, task.TargetType)
            // 提交到调度器
            ss.batchScheduler.Execute(job)
        }
    }
}
```

#### 2.2.2 BatchScheduler (批量调度器)
**文件**: `pkg/batch_service/batch_scheduler/scheduler_impl/batch_scheduler_impl.go`

**职责**:
- 维护任务队列（Job Queue）
- 维护Worker池（Worker Pool）
- 实现Dispatcher逻辑（从队列取任务分配给Worker）

**数据结构**:
```go
type BatchScheduler struct {
    JobQueueMapRWLock  *sync.RWMutex                    // 队列映射锁
    jobQueueMap        map[uint64]bool                  // 跟踪哪些任务已入队
    jobQueue           chan interfaces.SchedulerJob     // 任务队列
    workerPool         chan chan interfaces.SchedulerJob // Worker池
    workerCloseChanMap map[int]chan bool                // Worker关闭通道
    CloseChan          chan bool                        // 调度器关闭通道
    poolSize           int                              // Worker池大小
    queueSize          int                              // 队列大小
    gormConnection     *connection_impl.GormConnection  // 数据库连接
}
```

**核心流程**:
```go
// 初始化调度器
func (scheduler *BatchScheduler) Init() {
    // 1. 创建Worker池
    for i := 0; i < poolSize; i++ {
        worker := NewBatchWorker(i, workerPool, ...)
        worker.Start()
    }
    
    // 2. 启动Dispatcher
    go scheduler.Dispatch()
}

// Dispatcher（分派器）
func (scheduler *BatchScheduler) Dispatch() {
    for {
        select {
        case job := <-scheduler.jobQueue:
            // 从jobQueue取出一个任务
            
            // 等待一个空闲的Worker
            workerChannel := <-scheduler.workerPool
            
            // 将任务分配给Worker
            workerChannel <- job
            
            // 从队列映射中移除
            scheduler.jobQueueMap[job.GetJobTaskId()] = false
            
        case <-scheduler.CloseChan:
            scheduler.Close()
            return
        }
    }
}

// 执行任务（入队）
func (scheduler *BatchScheduler) Execute(job SchedulerJob) error {
    // 1. 检查任务是否已在队列中（去重）
    if scheduler.jobQueueMap[job.GetJobTaskId()] {
        return nil // 跳过重复任务
    }
    
    // 2. 检查队列是否已满
    if len(scheduler.jobQueue) == scheduler.queueSize {
        return errors.New("queue is full")
    }
    
    // 3. 任务入队
    scheduler.jobQueue <- job
    scheduler.jobQueueMap[job.GetJobTaskId()] = true
    
    return nil
}
```

#### 2.2.3 BatchWorker (批量Worker)
**文件**: `pkg/batch_service/batch_scheduler/scheduler_worker_impl/batch_worker_impl.go`

**职责**:
- 从Worker池获取任务
- 执行任务（调用BatchTaskJob.RunJob）
- 超时控制
- 异常处理

**工作流程**:
```go
func (worker *BatchWorker) Start() {
    go func() {
        for {
            // 1. 将自己放回Worker池（表示空闲）
            worker.workerPool <- worker.jobChan
            
            select {
            case job := <-worker.jobChan:
                // 2. 接收到任务，开始处理
                worker.Process(job)
                
            case <-worker.poolClose:
                // 3. 接收到关闭信号
                return
            }
        }
    }()
}

func (worker *BatchWorker) Process(job SchedulerJob) (interface{}, error) {
    // 1. 设置超时
    timeout := job.GetWorkerTimeoutParam() // 根据任务类型确定超时时间
    ctx, cancel := context.WithTimeout(context.Background(), timeout * time.Second)
    defer cancel()
    
    // 2. 创建完成通道
    finishChan := make(chan bool)
    var jobFinished = false
    var mutex = &sync.Mutex{}
    
    // 3. 启动任务执行
    go func() {
        err := job.RunJob(finishChan, worker.batchTaskDao, mutex)
        mutex.Lock()
        jobFinished = true
        mutex.Unlock()
    }()
    
    // 4. 监控超时
    go func() {
        <-ctx.Done()
        mutex.Lock()
        if !jobFinished {
            // 超时：释放任务，设置为pending状态，等待重试
            ReleaseBatchTask(job.GetJobTaskId(), "timeout", worker.batchTaskDao)
            finishChan <- true
        }
        mutex.Unlock()
    }()
    
    // 5. 等待任务完成
    <-finishChan
    return nil, nil
}
```

#### 2.2.4 BatchTaskJob (批量任务作业)
**文件**: `pkg/batch_service/batch_scheduler/scheduler_job_impl/batch_task_job.go`

**职责**:
- 封装单个批量任务的执行逻辑
- 任务状态管理（获取锁、释放锁）
- 调用具体的任务处理器

**执行流程**:
```go
func (job *BatchTaskJob) RunJob(finishChan chan bool, taskDao BatchTaskDao, mutex *sync.Mutex) error {
    defer func() {
        // 任务完成，通知Worker
        finishChan <- true
    }()
    
    // 1. 获取任务锁（Acquire）
    mutex.Lock()
    hasAcquired, err := AcquireBatchTask(taskId, expectedStatus, taskDao)
    mutex.Unlock()
    
    if !hasAcquired {
        return nil // 任务已被其他Worker获取或状态已变更
    }
    
    // 2. 验证任务是否已完成（幂等性检查）
    hasFinished, err := job.taskHandler.ValidateBatchTaskFinished()
    if hasFinished {
        // 任务已完成，直接释放
        mutex.Lock()
        ReleaseBatchTask(taskId, "", taskDao)
        mutex.Unlock()
        return nil
    }
    
    // 3. 执行任务
    err = job.taskHandler.ProcessBatchTask()
    
    // 4. 释放任务锁（Release）
    errMsg := ""
    if err != nil {
        errMsg = err.Error()
    }
    mutex.Lock()
    ReleaseBatchTask(taskId, errMsg, taskDao)
    mutex.Unlock()
    
    return err
}
```

#### 2.2.5 SchedulerJobHandler (任务处理器)
**文件**: `pkg/batch_service/batch_scheduler/scheduler_job_handler_impl/cdc/cdc_batch_task_handler.go`

**职责**:
- 实现具体的批量操作逻辑
- 不同任务类型有不同的处理器

**CDC批量任务处理器**:
```go
type CDCBatchTaskHandler struct {
    taskParam CDCBatchTaskJobParam
}

func (handler *CDCBatchTaskHandler) ProcessBatchTask() error {
    switch handler.taskParam.BatchType {
    case BatchTypeCDCStartJob:
        return StartCDCJob(targetId, user)
        
    case BatchTypeCDCStopJob:
        return StopCDCJob(targetId, user)
        
    case BatchTypeCDCSwitchAZ:
        return SwitchCDCAZ(targetId, user, az, taskId)
        
    case BatchTypeCDCResetGTID:
        return ResetGTID(targetName, user, params, taskId)
        
    case BatchTypeCDCChangeConfig:
        return JobUpdate(targetId, user, params)
        
    case BatchTypeCDCSwitchRunSystem:
        return SwitchRunSystem(targetId, user, runSystem)
        
    case BatchTypePanamaMigrateToCDC:
        return MigrateToCDC(params, taskId, env)
        
    default:
        return errors.New("unrecognized batch type")
    }
}

// 每种任务类型的超时时间不同
func (handler *CDCBatchTaskHandler) GetWorkerTimeoutParam() int {
    switch handler.taskParam.BatchType {
    case BatchTypeCDCStartJob, BatchTypeCDCStopJob, BatchTypeCDCChangeConfig:
        return 20  // 20秒
    case BatchTypeCDCSwitchAZ, BatchTypeCDCSwitchRunSystem:
        return 30  // 30秒
    case BatchTypeCDCResetGTID:
        return 480 // 8分钟
    default:
        return 20
    }
}
```

---

## 3. 接口设计

### 3.1 核心接口

#### 3.1.1 Scheduler 接口
```go
type Scheduler interface {
    Init()                        // 初始化调度器（创建Worker池）
    Dispatch()                    // 启动分派器
    Execute(SchedulerJob) error   // 提交任务到队列
    Close()                       // 关闭调度器
    GetQueuedJobCount() int       // 获取队列中的任务数
    GetJobQueueSize() int         // 获取队列容量
    GetWorkerPoolSize() int       // 获取Worker池大小
    GetAvaialbleWorkerCount() int // 获取空闲Worker数
}
```

#### 3.1.2 SchedulerJob 接口
```go
type SchedulerJob interface {
    // 运行任务
    RunJob(chan bool, BatchTaskDao, *sync.Mutex) error
    
    // 获取任务ID
    GetJobTaskId() uint64
    
    // 获取目标类型
    GetJobTargetType() string
    
    // 获取超时参数
    GetWorkerTimeoutParam() int
}
```

#### 3.1.3 SchedulerJobHandler 接口
```go
type SchedulerJobHandler interface {
    GetTaskId() uint64                                      // 获取任务ID
    GetTaskStatus() BatchStatus                             // 获取任务状态
    BuildTargetParam(*BatchTask, string) (string, error)    // 构建目标参数
    ValidateBatchTaskFinished() (bool, error)               // 验证任务是否已完成
    ProcessBatchTask() error                                // 处理批量任务
    GetWorkerTimeoutParam() int                             // 获取超时参数
    GetTargetType() string                                  // 获取目标类型
    GetTaskType() string                                    // 获取任务类型
}
```

#### 3.1.4 SchedulerJobHandlerRegister 接口
```go
type SchedulerJobHandlerRegister interface {
    // 获取任务处理器
    GetJobHandler(BatchTargetType, string) (SchedulerJobHandler, error)
    
    // 注册任务处理器
    RegisterJobHandler(BatchTargetType, func(string) (SchedulerJobHandler, error))
}
```

### 3.2 gRPC接口

**文件**: `protocol/proto/batch_scheduler.proto`

```protobuf
service BatchSchedulerService {
    rpc TriggerEvent (TriggerEventRequest) returns (TriggerEventResponse);
}

message TriggerEventRequest {
    EventType event_type = 1;  // 事件类型（CREATE_BATCH）
    bytes req_data = 2;        // 请求数据
    bytes job_data = 3;        // 批量Job数据
    int64 batch_id = 4;        // 批量ID
}
```

**用途**: Server服务在创建Batch后，通过gRPC通知Scheduler服务立即处理。

---

## 4. 数据模型

### 4.1 BatchJob (批量作业)

**表名**: `batch_job`  
**说明**: 一个BatchJob包含多个BatchTask，代表一次批量操作

```go
type BatchJob struct {
    BatchId      int64                  // 批量ID（主键）
    BatchName    string                 // 批量名称
    CreateTime   int64                  // 创建时间
    UpdateTime   int64                  // 更新时间
    Version      int64                  // 版本号（乐观锁）
    Creator      string                 // 创建者
    CMDBService  string                 // CMDB服务名
    Env          string                 // 环境（live/test等）
    Region       string                 // 区域（sg/us等）
    AZ           string                 // 可用区
    BatchType    string                 // 批量类型（cdc_job_start等）
    Status       string                 // 批量状态
    ExtraInfo    map[string]interface{} // 额外信息
}
```

### 4.2 BatchTask (批量任务)

**表名**: `batch_task`  
**说明**: 批量操作中的单个任务

```go
type BatchTask struct {
    TaskId      uint64          // 任务ID（主键）
    BatchId     uint64          // 所属批量ID
    BatchType   BatchType       // 批量类型
    TargetId    uint64          // 目标ID（CDC Job ID）
    TargetName  string          // 目标名称
    TargetType  BatchTargetType // 目标类型（cdc/panama）
    TargetEnv   string          // 目标环境
    TargetParam string          // 目标参数（JSON）
    Status      BatchStatus     // 任务状态
    Info        string          // 执行信息（错误信息等）
    RetryTime   uint            // 重试次数
    CreateTime  int64           // 创建时间
    UpdateTime  int64           // 更新时间
    Version     uint            // 版本号（乐观锁）
    Env         string          // 环境
    Region      string          // 区域
}
```

### 4.3 TargetParam 结构

**CDC任务参数**:
```go
type CDCTaskParamRecord struct {
    BeforeUpdate CDCBatchTargetParam // 更新前的参数（用于回滚）
    AfterUpdate  CDCBatchTargetParam // 更新后的参数
}

type CDCBatchTargetParam struct {
    // 切换AZ相关
    SwitchAZ struct {
        AZ string
    }
    
    // 重置GTID相关
    ResetGtid struct {
        GTID string
    }
    
    // 更改配置相关
    ChangeConfig struct {
        Cpu            *int
        Memory         *int
        MaxPackageSize *int
        Replicas       *int
    }
    
    // 切换运行系统相关
    SwitchRunSystem struct {
        RunSystem string
    }
    
    // 迁移相关
    MigratePanama struct {
        // ...
    }
}
```

---

## 5. 状态机设计

### 5.1 BatchTask 状态转换图

```
                     ┌─────────────────────────────────┐
                     │        pending (待处理)         │
                     └──────────┬──────────────────────┘
                                │ AcquireBatchTask
                                │ (Worker获取任务锁)
                                ▼
                     ┌─────────────────────────────────┐
                     │       running (执行中)          │
                     └──────────┬──────────────────────┘
                                │ ReleaseBatchTask
                ┌───────────────┼───────────────┐
                │               │               │
                ▼               ▼               ▼
    ┌───────────────┐ ┌───────────────┐ ┌───────────────┐
    │    success    │ │     fail      │ │  pending(超时)│
    │   (成功)      │ │    (失败)     │ │   (重新入队)  │
    └───────┬───────┘ └───────────────┘ └───────┬───────┘
            │                                    │
            │                                    │ 超时后重试
            │                                    │ (retry < 2)
            │                                    └──────┐
            │                                           │
            ▼                                           ▼
    ┌───────────────────────────────────┐    ┌──────────────────┐
    │   rollback_pending (待回滚)       │◄───│ retry_pending    │
    └───────────┬───────────────────────┘    │ (待重试)         │
                │                             └──────────────────┘
                │ AcquireBatchTask
                ▼
    ┌───────────────────────────────────┐
    │   rollback_running (回滚中)       │
    └───────────┬───────────────────────┘
                │ ReleaseBatchTask
        ┌───────┴───────┐
        ▼               ▼
┌───────────────┐ ┌───────────────────┐
│rollback_success│ │ rollback_fail    │
│ (回滚成功)    │ │  (回滚失败)      │
└───────────────┘ └───────────────────┘

         用户取消操作
              ▼
    ┌───────────────────┐
    │     cancel        │
    │    (已取消)       │
    └───────────────────┘
```

### 5.2 状态枚举

```go
const (
    BatchStatusPending         = "pending"          // 待处理
    BatchStatusRunning         = "running"          // 执行中
    BatchStatusSuccess         = "success"          // 成功
    BatchStatusFail            = "fail"             // 失败
    BatchStatusPartialFail     = "partial_fail"     // 部分失败
    BatchStatusRollbackPending = "rollback_pending" // 待回滚
    BatchStatusRollbackRunning = "rollback_running" // 回滚中
    BatchStatusRollbackSuccess = "rollback_success" // 回滚成功
    BatchStatusRollbackFail    = "rollback_fail"    // 回滚失败
    BatchStatusCancel          = "cancel"           // 已取消
    BatchStatusRetryPending    = "retry_pending"    // 待重试
    BatchStatusUnknown         = "unknown"          // 未知
)
```

### 5.3 BatchJob 状态转换

BatchJob的状态由其所有BatchTask的状态聚合而来：

```go
func MaintainBatchStatus(batchId uint64) (bool, []*BatchTask) {
    // 1. 获取Batch下所有Task
    tasks := GetBatchTasks(batchId)
    
    // 2. 统计各状态任务数
    var pendingCount, runningCount, successCount, failCount int
    for _, task := range tasks {
        switch task.Status {
        case BatchStatusPending, BatchStatusRetryPending:
            pendingCount++
        case BatchStatusRunning:
            runningCount++
        case BatchStatusSuccess:
            successCount++
        case BatchStatusFail:
            failCount++
        }
    }
    
    // 3. 更新Batch状态
    var batchStatus BatchStatus
    totalCount := len(tasks)
    
    if successCount == totalCount {
        batchStatus = BatchStatusSuccess  // 全部成功
    } else if failCount == totalCount {
        batchStatus = BatchStatusFail     // 全部失败
    } else if successCount + failCount == totalCount {
        batchStatus = BatchStatusPartialFail // 部分成功部分失败
    } else if runningCount > 0 || pendingCount > 0 {
        batchStatus = BatchStatusRunning  // 仍有任务执行中
    }
    
    UpdateBatchJobStatus(batchId, batchStatus)
    
    // 4. 判断Batch是否可继续处理
    isEligible := (batchStatus == BatchStatusRunning || batchStatus == BatchStatusPending)
    
    return isEligible, tasks
}
```

---

## 6. 关键流程

### 6.1 批量任务创建与执行流程

```
┌─────────┐                                              ┌──────────┐
│  用户   │                                              │  Server  │
└────┬────┘                                              └─────┬────┘
     │                                                         │
     │ 1. POST /api/batch (创建批量任务)                       │
     ├────────────────────────────────────────────────────────►│
     │                                                         │
     │                                                         │ 2. 创建BatchJob
     │                                                         │    INSERT INTO batch_job
     │                                                         ├────────┐
     │                                                         │        │
     │                                                         │◄───────┘
     │                                                         │
     │                                                         │ 3. 创建N个BatchTask
     │                                                         │    INSERT INTO batch_task (status=pending)
     │                                                         ├────────┐
     │                                                         │        │
     │                                                         │◄───────┘
     │                                                         │
     │                                                         │ 4. 触发Scheduler事件
     │                                                  ┌──────┤    (gRPC调用)
     │                                                  │      │
     │                                                  │      │
     │                                         ┌────────▼──────┴────┐
     │                                         │   Scheduler         │
     │                                         │                     │
     │                                         │ 5. 立即扫描Batch    │
     │                                         │    或等待下次定时扫描│
     │                                         └────────┬────────────┘
     │                                                  │
     │                                                  │ 6. dispatchBatchTasks
     │                                                  │    for each task:
     │                                         ┌────────▼────────────┐
     │                                         │  BatchScheduler     │
     │                                         │                     │
     │                                         │ 7. Execute(job)     │
     │                                         │    job → jobQueue   │
     │                                         └────────┬────────────┘
     │                                                  │
     │                                                  │ 8. Dispatcher分派
     │                                         ┌────────▼────────────┐
     │                                         │   Worker Pool       │
     │                                         │                     │
     │                                         │ 9. Worker处理任务   │
     │                                         │    - AcquireBatchTask│
     │                                         │    - ProcessBatchTask│
     │                                         │    - ReleaseBatchTask│
     │                                         └─────────────────────┘
     │                                                  │
     │                                                  │ 10. 更新task状态
     │                                                  │     (success/fail)
     │                                                  │
     │◄─────────────────────────────────────────────────┤ 11. 返回结果
     │                                                  │     (异步)
     │                                                  │
```

### 6.2 任务获取与释放流程（Acquire & Release）

```
┌──────────────────────────────────────────────────────────────────┐
│                    AcquireBatchTask (获取任务锁)                  │
├──────────────────────────────────────────────────────────────────┤
│                                                                    │
│  1. 开始事务 (Isolation Level: READ_UNCOMMITTED)                 │
│     BEGIN TRANSACTION;                                            │
│                                                                    │
│  2. 脏读检查（快速失败）                                          │
│     SELECT * FROM batch_task WHERE task_id = ? ;                  │
│     IF status != expectedStatus THEN                              │
│         ROLLBACK; RETURN false;                                   │
│     END IF;                                                       │
│                                                                    │
│  3. 行锁获取                                                      │
│     SELECT * FROM batch_task WHERE task_id = ? FOR UPDATE;        │
│                                                                    │
│  4. 再次验证状态（防止并发修改）                                  │
│     IF status != expectedStatus THEN                              │
│         ROLLBACK; RETURN false;                                   │
│     END IF;                                                       │
│                                                                    │
│  5. 检查Batch是否被取消                                           │
│     batch := GetBatchJob(batchId);                                │
│     IF batch.Status == "cancel" THEN                              │
│         ROLLBACK; RETURN error;                                   │
│     END IF;                                                       │
│                                                                    │
│  6. 更新任务状态                                                  │
│     newStatus := GetAcquireStatus(task.Status);                   │
│     UPDATE batch_task SET status = newStatus WHERE task_id = ?;   │
│                                                                    │
│  7. 保持事务（不提交）                                            │
│     // 事务保持打开，直到ReleaseBatchTask时才提交                │
│                                                                    │
│  RETURN true;                                                     │
│                                                                    │
└──────────────────────────────────────────────────────────────────┘

状态转换:
  pending → running
  rollback_pending → rollback_running
  retry_pending → running

┌──────────────────────────────────────────────────────────────────┐
│                  ReleaseBatchTask (释放任务锁)                    │
├──────────────────────────────────────────────────────────────────┤
│                                                                    │
│  1. 检查是否有正在进行的事务                                      │
│     IF !HasOngoingTx() THEN                                       │
│         // 超时场景，需要重新获取锁                               │
│         BEGIN TRANSACTION (Isolation Level: REPEATABLE_READ);     │
│         SELECT * FROM batch_task WHERE task_id = ? FOR UPDATE;    │
│     END IF;                                                       │
│                                                                    │
│  2. 获取任务信息                                                  │
│     task := GetBatchTask(task_id);                                │
│                                                                    │
│  3. 检查任务是否可更新                                            │
│     IF task.Status IN (cancel, success, fail, ...) THEN           │
│         ROLLBACK; RETURN error;                                   │
│     END IF;                                                       │
│                                                                    │
│  4. 处理失败重试                                                  │
│     IF hasFailed AND isTimeout THEN                               │
│         IF task.RetryTime < 2 THEN                                │
│             UPDATE retry_time = retry_time + 1;                   │
│             status = pending; // 重新入队                         │
│         ELSE                                                      │
│             status = fail;                                        │
│         END IF;                                                   │
│     END IF;                                                       │
│                                                                    │
│  5. 确定释放后的状态                                              │
│     newStatus := GetReleaseStatus(task, hasFailed, isTimeout);    │
│                                                                    │
│  6. 更新任务状态                                                  │
│     UPDATE batch_task SET                                         │
│         status = newStatus,                                       │
│         info = errMsg,                                            │
│         retry_time = ...                                          │
│     WHERE task_id = ?;                                            │
│                                                                    │
│  7. 提交事务                                                      │
│     COMMIT;                                                       │
│                                                                    │
│  RETURN nil;                                                      │
│                                                                    │
└──────────────────────────────────────────────────────────────────┘

状态转换:
  running → success (成功)
  running → fail (失败)
  running → pending (超时重试)
  rollback_running → rollback_success (回滚成功)
  rollback_running → rollback_fail (回滚失败)
```

### 6.3 超时重试机制

```go
// Worker超时监控
func (worker *BatchWorker) Process(job SchedulerJob) {
    timeout := job.GetWorkerTimeoutParam() * time.Second
    ctx, cancel := context.WithTimeout(context.Background(), timeout)
    defer cancel()
    
    var jobFinished = false
    finishChan := make(chan bool)
    mutex := &sync.Mutex{}
    
    // 启动任务执行
    go func() {
        job.RunJob(finishChan, taskDao, mutex)
        mutex.Lock()
        jobFinished = true
        mutex.Unlock()
    }()
    
    // 监控超时
    go func() {
        <-ctx.Done()  // 等待超时
        
        mutex.Lock()
        defer mutex.Unlock()
        
        if !jobFinished {
            // 超时处理
            taskId := job.GetJobTaskId()
            
            // 释放任务，标记为超时
            // 如果重试次数 < 2，状态设为pending（重新入队）
            // 否则状态设为fail
            ReleaseBatchTask(taskId, "timeout", taskDao)
            
            // 通知任务结束
            if isOpen(finishChan) {
                finishChan <- true
            }
        }
    }()
    
    // 等待任务完成或超时
    <-finishChan
}
```

### 6.4 并发控制与死锁预防

**1. 行锁 + 乐观锁**:
```go
// Acquire时
SELECT * FROM batch_task WHERE task_id = ? FOR UPDATE;  // 行锁
UPDATE batch_task SET status = ?, version = version + 1 WHERE task_id = ? AND version = ?;  // 乐观锁

// Release时（同一事务）
UPDATE batch_task SET status = ?, info = ? WHERE task_id = ?;
COMMIT;
```

**2. 脏读优化（快速失败）**:
```go
// 设置事务隔离级别为READ_UNCOMMITTED
tx.Begin(sql.LevelReadUncommitted)

// 先读取未提交的数据（脏读）
task := SELECT * FROM batch_task WHERE task_id = ?;
if task.Status != expectedStatus {
    // 快速失败，避免等待行锁
    tx.Rollback()
    return false
}

// 状态符合预期，再获取行锁
task := SELECT * FROM batch_task WHERE task_id = ? FOR UPDATE;
```

**3. 任务去重（防止重复入队）**:
```go
// 使用jobQueueMap跟踪已入队的任务
scheduler.JobQueueMapRWLock.RLock()
if scheduler.jobQueueMap[taskId] {
    // 任务已在队列中，跳过
    scheduler.JobQueueMapRWLock.RUnlock()
    return nil
}
scheduler.JobQueueMapRWLock.RUnlock()

// 任务入队
scheduler.jobQueue <- job
scheduler.JobQueueMapRWLock.Lock()
scheduler.jobQueueMap[taskId] = true
scheduler.JobQueueMapRWLock.Unlock()
```

---

## 7. 并发控制

### 7.1 并发模型

```
                    ┌─────────────────────────────────────┐
                    │  SchedulerServer (1个goroutine)    │
                    │  定时扫描，分发任务                 │
                    └──────────────┬──────────────────────┘
                                   │
                                   │ 分发N个BatchTask
                                   ▼
                    ┌─────────────────────────────────────┐
                    │    BatchScheduler (1个goroutine)    │
                    │    Dispatcher分派任务               │
                    └──────────────┬──────────────────────┘
                                   │
                                   │ 分配给空闲Worker
                                   ▼
            ┌───────────────┬──────┴──────┬────────────────┐
            │               │             │                │
      ┌─────▼────┐   ┌─────▼────┐  ┌────▼─────┐   ┌─────▼────┐
      │ Worker#1 │   │ Worker#2 │  │ Worker#3 │...│ Worker#32│
      │(goroutine│   │(goroutine│  │(goroutine│   │(goroutine│
      └──────────┘   └──────────┘  └──────────┘   └──────────┘
            │               │             │                │
            └───────────────┴─────────────┴────────────────┘
                                   │
                                   │ 并发执行
                                   ▼
                         数据库（行锁控制并发）
```

### 7.2 同步机制

#### 7.2.1 Channel通信
```go
jobQueue           chan SchedulerJob           // 任务队列（buffered channel）
workerPool         chan chan SchedulerJob      // Worker池（buffered channel）
worker.jobChan     chan SchedulerJob           // Worker任务通道
CloseChan          chan bool                   // 关闭信号
finishChan         chan bool                   // 任务完成信号
```

#### 7.2.2 互斥锁
```go
JobQueueMapRWLock  *sync.RWMutex  // 保护jobQueueMap的读写锁
mutex              *sync.Mutex    // 保护Worker内共享变量的互斥锁
```

#### 7.2.3 数据库事务
```go
// 长事务：Acquire到Release期间保持事务
BEGIN TRANSACTION;
SELECT ... FOR UPDATE;  // 获取行锁
... (任务执行) ...
UPDATE ...;             // 更新状态
COMMIT;
```

### 7.3 并发安全保证

**1. 任务唯一性保证**:
- `jobQueueMap`：防止同一任务重复入队
- 数据库`SELECT ... FOR UPDATE`：防止多个Worker同时处理同一任务

**2. 状态一致性保证**:
- 乐观锁（version字段）：防止并发修改冲突
- 行锁：确保状态更新的原子性

**3. 超时处理**:
- 每个Worker有独立的超时监控goroutine
- 使用`mutex`保护`jobFinished`标志，防止竞态条件

**4. 优雅关闭**:
```go
// 关闭SchedulerServer
ss.SchedulerServerCloseChan <- true

// 关闭Scheduler
scheduler.CloseChan <- true

// 关闭所有Worker
for _, closeChan := range workerCloseChanMap {
    closeChan <- true
}
```

---

## 8. 扩展机制

### 8.1 任务处理器注册机制

**设计思想**: 采用**注册模式**，支持动态注册不同类型的批量任务处理器。

**注册流程**:
```go
// 1. 初始化注册器
func main() {
    scheduler_job_handler_impl.InitBatchTaskHandlerRegister()
    
    // 2. 注册CDC任务处理器
    register := scheduler_job_handler_impl.GetBatchTaskHandlerRegisterInstance()
    register.RegisterJobHandler(
        enums.BatchTargetTypeCDCJob,
        cdc.NewCDCBatchTaskHandler,  // 工厂函数
    )
    
    // 3. 注册Panama任务处理器
    register.RegisterJobHandler(
        enums.BatchTargetTypePanamaJob,
        cdc.NewCDCBatchTaskHandler,  // 复用CDC处理器
    )
    
    // 4. 注册其他类型处理器（扩展）
    register.RegisterJobHandler(
        enums.BatchTargetTypeXXX,
        xxx.NewXXXBatchTaskHandler,
    )
}

// 获取处理器
handler, err := register.GetJobHandler(targetType, paramJSON)
```

**实现代码**:
```go
type BatchTaskHandlerRegister struct {
    jobHandlerRegisterMap map[BatchTargetType]func(string) (SchedulerJobHandler, error)
}

func (register *BatchTaskHandlerRegister) RegisterJobHandler(
    taskType BatchTargetType,
    initFunc func(string) (SchedulerJobHandler, error),
) {
    register.jobHandlerRegisterMap[taskType] = initFunc
}

func (register *BatchTaskHandlerRegister) GetJobHandler(
    taskType BatchTargetType,
    paramJSON string,
) (SchedulerJobHandler, error) {
    initFunc := register.jobHandlerRegisterMap[taskType]
    if initFunc == nil {
        return nil, errors.New("unrecognized target type")
    }
    return initFunc(paramJSON)
}
```

### 8.2 新增批量操作类型

**步骤**:

1. **定义批量类型枚举**:
```go
// pkg/batch_service/constants/enums/batch_type.go
const (
    BatchTypeCDCStartJob        BatchType = "cdc_job_start"
    BatchTypeCDCStopJob         BatchType = "cdc_job_stop"
    // ... 其他类型 ...
    BatchTypeNewOperation       BatchType = "new_operation"  // 新增
)
```

2. **实现处理逻辑**:
```go
// pkg/batch_service/batch_scheduler/scheduler_job_handler_impl/cdc/cdc_batch_task_handler.go
func (handler *CDCBatchTaskHandler) ProcessBatchTask() error {
    switch handler.taskParam.BatchType {
    // ... 已有类型 ...
    case enums.BatchTypeNewOperation:
        return ProcessNewOperation(handler.taskParam)  // 新增处理函数
    default:
        return errors.New("unrecognized batch type")
    }
}
```

3. **设置超时时间**:
```go
func (handler *CDCBatchTaskHandler) GetWorkerTimeoutParam() int {
    switch handler.taskParam.BatchType {
    // ... 已有类型 ...
    case enums.BatchTypeNewOperation:
        return 60  // 60秒超时
    default:
        return 20
    }
}
```

### 8.3 新增目标类型

**步骤**:

1. **定义目标类型枚举**:
```go
// pkg/batch_service/constants/enums/batch_target_type.go
const (
    BatchTargetTypeCDCJob    BatchTargetType = "cdc"
    BatchTargetTypePanamaJob BatchTargetType = "panama"
    BatchTargetTypeNewTarget BatchTargetType = "new_target"  // 新增
)
```

2. **实现新的处理器**:
```go
// pkg/batch_service/batch_scheduler/scheduler_job_handler_impl/new_target/new_target_handler.go
package new_target

type NewTargetBatchTaskHandler struct {
    taskParam NewTargetBatchTaskJobParam
}

func NewNewTargetBatchTaskHandler(paramJSON string) (interfaces.SchedulerJobHandler, error) {
    var taskParam NewTargetBatchTaskJobParam
    err := json.Unmarshal([]byte(paramJSON), &taskParam)
    if err != nil {
        return nil, err
    }
    return &NewTargetBatchTaskHandler{taskParam: taskParam}, nil
}

func (handler *NewTargetBatchTaskHandler) ProcessBatchTask() error {
    // 实现具体的批量操作逻辑
    return nil
}

// 实现SchedulerJobHandler接口的其他方法...
```

3. **注册处理器**:
```go
// cmd/scheduler/main.go 或 cmd/server/main.go
func main() {
    // ...
    scheduler_job_handler_impl.GetBatchTaskHandlerRegisterInstance().RegisterJobHandler(
        enums.BatchTargetTypeNewTarget,
        new_target.NewNewTargetBatchTaskHandler,
    )
    // ...
}
```

---

## 9. 监控与指标

### 9.1 Prometheus指标

```go
// 任务队列使用率
metrics.BatchSchedulerJobQueueUsageSummary.WithLabelValues("live").
    Observe(float64(queuedJobCount / queueSize) * 100)

// Worker池使用率
metrics.BatchSchedulerWorkerUsageSummary.WithLabelValues("live").
    Observe(float64(availableWorkerCount / poolSize) * 100)

// 任务执行时间
metrics.BatchTaskExecutionTimeGauge.
    WithLabelValues(taskId, taskType).
    Set(float64(executionTime.Milliseconds()))

// 任务状态维护失败计数
metrics.BatchTaskStatusMaintainFailGauge.WithLabelValues(taskId).Inc()

// Batch状态维护失败计数
metrics.BatchJobStatusMaintainFailGauge.WithLabelValues(batchId).Inc()
```

### 9.2 日志记录

```go
// 关键操作日志
logger.Logger().WithField("type", "batch scheduler").
    WithField("id", taskId).
    WithError(err).
    Info/Warn/Error("message")
```

### 9.3 健康检查

SchedulerServer会检测队列阻塞情况：
```go
// 如果连续blockingCountLimit次扫描，队列中的任务数都在增加
// 则重启调度器
if blockingCount > ss.blockingCountLimit {
    ss.batchScheduler.CloseChan <- true
    logger.Logger().Warn("restarting batch scheduler due to increasing queued job number")
    ss.scanBatch()  // 重新启动
}
```

---

## 10. 最佳实践

### 10.1 性能优化

**1. Worker池大小调优**:
```go
poolSize := 32  // 根据任务执行时间和并发需求调整
```

**2. 队列容量调优**:
```go
queueSize := 1000  // 避免队列满导致任务无法入队
```

**3. 数据库连接池**:
```go
// 每个Worker有独立的数据库连接
for i := 0; i < poolSize; i++ {
    gormClient, _ := gormConnection.ConnectGorm()
    worker := NewBatchWorker(i, workerPool, taskDao)
}
```

**4. 批量查询优化**:
```go
// 一次性查询多个Batch的所有Task
batchList := queryUnfinishedBatches(limit=100)
```

### 10.2 可靠性保障

**1. 幂等性**:
```go
// 执行任务前先验证是否已完成
hasFinished, _ := taskHandler.ValidateBatchTaskFinished()
if hasFinished {
    ReleaseBatchTask(taskId, "", taskDao)
    return nil
}
```

**2. 事务保证**:
```go
// Acquire和Release在同一事务中
BEGIN TRANSACTION;
SELECT ... FOR UPDATE;
// ... 任务执行 ...
UPDATE ...;
COMMIT;
```

**3. 失败重试**:
```go
// 超时后自动重试（最多2次）
if task.RetryTime < 2 && isTimeout {
    task.Status = pending  // 重新入队
    task.RetryTime++
}
```

### 10.3 故障恢复

**1. 调度器自动重启**:
```go
// 检测到队列阻塞时自动重启
if blockingCount > blockingCountLimit {
    ss.batchScheduler.CloseChan <- true
    ss.scanBatch()
}
```

**2. 任务状态恢复**:
```go
// 服务重启后，pending状态的任务会被重新扫描并执行
scanBatch()  // 定时扫描所有未完成的Batch
```

---

## 11. 常见问题

### Q1: 如何防止同一任务被多个Worker同时处理？

**A**: 通过数据库行锁和`jobQueueMap`双重保证：
1. `jobQueueMap`防止重复入队
2. `SELECT ... FOR UPDATE`防止多个Worker同时获取任务锁

### Q2: 任务超时后会怎样？

**A**: 
- 如果重试次数 < 2：任务状态设为`pending`，重新入队等待执行
- 否则：任务状态设为`fail`
- 超时时间根据任务类型不同而不同（20秒 ~ 8分钟）

### Q3: 如何取消正在执行的批量任务？

**A**: 
1. 用户调用取消接口，BatchJob状态设为`cancel`
2. SchedulerServer扫描时发现Batch已取消，不再分发新Task
3. 正在执行的Task完成后，状态更新为`cancel`

### Q4: 批量操作如何支持回滚？

**A**: 
1. `TargetParam`中保存`BeforeUpdate`和`AfterUpdate`参数
2. 用户触发回滚后，Task状态设为`rollback_pending`
3. Worker执行时使用`BeforeUpdate`参数恢复原状态

### Q5: 调度器服务如何实现高可用？

**A**: 
1. 支持多region部署（通过`region`字段隔离）
2. 每个region的Scheduler只处理本region的Batch
3. 数据库行锁确保即使多个Scheduler同时运行也不会冲突

---

## 12. 总结

批量调度器服务是一个**高可靠、高并发、可扩展**的异步任务调度系统，核心设计特点：

✅ **生产者-消费者模式**: SchedulerServer生产任务，Worker池消费任务  
✅ **任务状态机**: 完善的状态转换机制，支持重试和回滚  
✅ **并发控制**: 行锁 + 乐观锁 + Channel通信  
✅ **超时控制**: 每个任务独立超时监控，支持自动重试  
✅ **可扩展性**: 插件式任务处理器注册机制  
✅ **可观测性**: Prometheus指标 + 结构化日志  

该系统是CDC Manager实现批量操作的核心引擎，保证了大规模CDC任务管理的稳定性和效率。

