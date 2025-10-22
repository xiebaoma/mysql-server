# CDC Manager 项目架构梳理文档

## 📋 项目概述

**项目名称**: CDC Manager (Change Data Capture Manager)  
**组织**: Shopee Platform DBTools  
**语言**: Go 1.19  
**类型**: CDC中间件管理平台（非CDC实现，专注于CDC任务的管理和编排）

这是一个用于管理CDC（变更数据捕获）任务的平台系统，不涉及具体的CDC数据捕获实现，而是提供CDC任务的全生命周期管理、资源调度、监控告警、批量操作等功能。

---

## 🏗️ 总体架构

### 核心组件架构

```
┌─────────────────────────────────────────────────────────────────┐
│                      CDC Manager 生态系统                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                   │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐        │
│  │  Server  │  │ Scheduler│  │  Watch   │  │  Stats   │        │
│  │  (API)   │  │ (批量调度)│  │ (监控器) │  │ (统计)   │        │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘        │
│       │             │             │             │                │
│       └─────────────┴─────────────┴─────────────┘                │
│                     │                                             │
│              ┌──────┴──────┐                                      │
│              │             │                                      │
│         ┌────▼────┐   ┌───▼────┐                                 │
│         │  MySQL  │   │  K8s   │                                 │
│         │  (元数据)│   │ (资源) │                                 │
│         └─────────┘   └────────┘                                 │
│                                                                   │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │           DR Controller (灾难恢复控制器)                  │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 🎯 核心服务模块

### 1. **Server (主服务)**
**入口**: `cmd/server/main.go`  
**端口**: 80  
**职责**:
- 提供RESTful API接口供前端/外部调用
- CDC任务的CRUD操作
- Ticket工单系统管理
- 用户权限控制(RBAC)
- 与各种外部服务集成（CMDB、UIC、Seatalk等）

**关键功能模块**:
- Job管理：CDC任务的创建、启停、更新、删除
- Ticket系统：工单流程管理
- 数据流管理：Dataflow配置
- 资源管理：K8s资源配置
- 配额管理：CDC配额控制

### 2. **Scheduler (批量调度器)**
**入口**: `cmd/scheduler/main.go`  
**端口**: 9818  
**职责**:
- 执行批量CDC任务操作
- 提供gRPC接口接收批量任务请求
- 任务调度和执行状态跟踪
- 支持多region部署

**关键特性**:
- 批量任务处理（批量启停、批量更新等）
- 任务队列管理和worker池
- 任务重试机制
- 资源使用监控和上报

### 3. **Watch (监控服务)**
**入口**: `cmd/watch/main.go`  
**端口**: 80  
**职责**:
- CDC任务健康检查
- 定期检查任务存活状态
- 配额使用统计和上报
- 数据库离线检测
- 工单自动处理

**监控项**:
- CDC任务存活检查
- 数据库连接状态
- 分库分表数量检查
- 配额使用情况

### 4. **Stats (统计服务)**
**入口**: `cmd/stats/main.go`  
**端口**: 9818  
**职责**:
- K8s资源使用统计
- CDC任务运行指标收集
- Prometheus指标暴露
- 资源使用趋势分析

### 5. **DR Controller (灾难恢复控制器)**
**入口**: `cmd/dr_controller/main.go`  
**端口**: 80  
**职责**:
- 跨可用区（AZ）切换管理
- 批量任务验证
- 灾难恢复场景下的任务迁移
- 监听Kubernetes资源变化

---

## 📦 核心包结构

### pkg/cdc - CDC核心业务逻辑

```
pkg/cdc/
├── handler/          # HTTP请求处理器
│   ├── job_handler.go        # CDC任务操作
│   ├── ticket_handler.go     # 工单处理
│   ├── dataflow_handler.go   # 数据流配置
│   └── resource_handler.go   # 资源管理
├── service/          # 业务服务层
│   ├── job_service.go            # 任务服务
│   ├── ticket_service.go         # 工单服务
│   ├── job_list_service.go       # 任务列表
│   ├── job_opt_service.go        # 任务操作
│   ├── dataflow_service.go       # 数据流服务
│   ├── k8s_stats.go              # K8s统计
│   └── external/                 # 外部服务集成
├── db/               # 数据访问层
│   ├── job_db.go                 # 任务数据库操作
│   ├── ticket_db.go              # 工单数据库操作
│   ├── k8s_resource_db.go        # K8s资源
│   └── operation_history_db.go   # 操作历史
├── model/            # 数据模型
│   ├── db_model/                 # 数据库模型
│   └── service_model/            # 服务层模型
├── constants/        # 常量定义
│   └── enums/                    # 枚举类型
├── cdc_quota/        # CDC配额管理
├── cdc_check_service/# CDC健康检查
├── cdc_resource/     # CDC资源收集
└── db_meta/          # 元数据管理
```

### pkg/batch_service - 批量任务处理

```
pkg/batch_service/
├── batch_scheduler/              # 批量调度器
│   ├── scheduler_impl/          # 调度器实现
│   ├── scheduler_job_impl/      # 任务实现
│   ├── scheduler_worker_impl/   # Worker实现
│   └── scheduler_job_handler_impl/
│       └── cdc/                 # CDC批量任务处理器
├── batch_scheduler_server/       # gRPC服务器
├── batch_scheduler_client/       # gRPC客户端
├── handler/                      # HTTP处理器
├── service/                      # 业务服务
├── dal/                          # 数据访问层
│   ├── dao/                     # DAO模式
│   └── db/                      # 数据库连接
├── model/                        # 数据模型
└── scheduler_resource/           # 调度器资源管理
```

### pkg/common - 通用组件

```
pkg/common/
├── configs/          # 配置管理
├── config_center/    # 配置中心
├── connector/        # 数据库连接器
├── httpserver/       # HTTP服务器框架
│   ├── middleware/   # 中间件（认证、日志等）
│   └── cache/        # 缓存
├── external_services/# 外部服务集成
│   ├── seatalk/      # Seatalk（企业通讯）
│   ├── uss/          # USS服务
│   └── ...
├── metrics/          # 监控指标
│   ├── metrics/      # Prometheus指标
│   ├── legacy_monitor/# 旧版监控
│   └── metrics_monitor/# 指标监控
├── rbac/             # 权限控制
└── utils/            # 工具函数
```

### pkg/panama - Panama服务集成

专门用于与Panama系统（可能是另一个数据服务）的集成。

### pkg/dr_controller - 灾难恢复

```
pkg/dr_controller/
├── dr_service.go     # DR服务逻辑
└── watcher.go        # K8s资源监听器
```

---

## 🗄️ 数据模型

### 核心实体

#### 1. **CDC Job (CDC任务)**
- 任务基本信息（ID、名称、类型、状态）
- 数据源配置（源数据库、目标Kafka）
- K8s资源配置（CPU、内存、副本数）
- 运行系统类型（DTS、Panama等）
- 业务级别（P0/P1/P2）

#### 2. **Ticket (工单)**
- 工单信息（标题、描述、状态）
- 操作类型（创建、启停、更新、删除）
- 审批流程
- 执行结果

#### 3. **Batch Task (批量任务)**
- 批次信息
- 批量操作类型
- 任务列表
- 执行状态和进度

#### 4. **K8s Resource (K8s资源)**
- Pod信息
- 资源使用情况
- 部署配置

---

## 🔄 关键业务流程

### 1. CDC任务创建流程

```
用户提交创建请求
     ↓
Server API验证参数
     ↓
检查权限和配额
     ↓
创建Ticket工单
     ↓
工单审批（可选）
     ↓
生成K8s资源配置
     ↓
部署到K8s集群
     ↓
更新任务状态
     ↓
返回结果
```

### 2. 批量任务执行流程

```
用户提交批量操作请求
     ↓
Server创建Batch记录
     ↓
调用Scheduler gRPC接口
     ↓
Scheduler创建任务队列
     ↓
Worker池并发执行
     ↓
每个任务独立处理（启停/更新等）
     ↓
更新任务执行状态
     ↓
所有任务完成后更新Batch状态
```

### 3. 监控告警流程

```
Watch服务定期检查
     ↓
检测到异常（任务失败、资源不足等）
     ↓
记录到数据库
     ↓
发送Seatalk告警通知
     ↓
触发自动恢复（可选）
```

---

## 🛠️ 技术栈

### 核心框架
- **Web框架**: Gin (HTTP服务)
- **RPC框架**: gRPC (服务间通信)
- **ORM**: GORM (数据库操作)
- **K8s客户端**: client-go

### 数据存储
- **MySQL**: 元数据存储（任务、工单、配置等）
- **Redis**: 缓存和分布式锁
- **Etcd**: 配置中心和分布式锁

### 消息队列
- **Kafka**: CDC数据流目标（也用于任务管理）

### 监控与日志
- **Prometheus**: 指标采集
- **Logger**: 日志框架（splog、gommon/logger）

### 外部集成
- **CMDB**: 配置管理数据库
- **UIC**: 统一身份认证
- **Seatalk**: 企业通讯/告警
- **XAC**: 权限控制
- **Space SDK**: Shopee内部平台
- **DBA Meta**: 数据库元数据服务
- **USS**: 统一存储服务

---

## 🚀 部署架构

### 环境分类
- **live**: 生产环境
- **liveish**: 准生产环境
- **non-live**: 非生产环境（test/qa）
- **dr-live**: 灾备环境

### 部署方式
所有服务都部署在Kubernetes集群上，通过以下方式管理：

```
build/
├── live/              # 生产环境
│   ├── kustomize/     # Kustomize配置
│   ├── binlog/        # Binlog服务部署
│   ├── scheduler/     # Scheduler部署
│   ├── watch/         # Watch部署
│   ├── stats/         # Stats部署
│   └── dr_controller/ # DR Controller部署
├── liveish/           # 准生产环境
├── non-live/          # 测试环境
└── dr-live/           # 灾备环境
```

每个环境都有独立的：
- Dockerfile（不同服务有不同的镜像）
- Kustomize配置（K8s资源定义）
- 部署脚本（deploy.sh）

---

## 📊 核心功能特性

### 1. 任务管理
- ✅ CDC任务的完整生命周期管理
- ✅ 支持多种运行系统（DTS、Panama等）
- ✅ 灵活的资源配置（CPU、内存、副本数）
- ✅ 任务状态实时跟踪

### 2. 批量操作
- ✅ 批量启动/停止任务
- ✅ 批量更新配置
- ✅ 批量切换运行系统
- ✅ 任务执行进度跟踪

### 3. 工单系统
- ✅ 标准化操作流程
- ✅ 审批机制
- ✅ 操作历史记录
- ✅ 自动化执行

### 4. 监控告警
- ✅ 任务健康检查
- ✅ 资源使用监控
- ✅ 异常自动告警（Seatalk）
- ✅ Prometheus指标暴露

### 5. 配额管理
- ✅ CDC资源配额控制
- ✅ 配额使用统计
- ✅ 配额申请和审批

### 6. 权限控制
- ✅ RBAC权限模型
- ✅ 与XAC集成
- ✅ 操作审计

### 7. 灾难恢复
- ✅ 跨AZ切换
- ✅ 批量任务验证
- ✅ 自动故障转移

---

## 🔌 API接口

### RESTful API
- `POST /api/suez/v1/jobs` - 创建CDC任务
- `GET /api/suez/v1/jobs` - 查询任务列表
- `PUT /api/suez/v1/jobs/{id}` - 更新任务
- `DELETE /api/suez/v1/jobs/{id}` - 删除任务
- `POST /api/suez/v1/tickets` - 创建工单
- `POST /api/suez/v1/batch` - 创建批量任务
- 更多API详见Swagger文档（`docs/swagger.yaml`）

### gRPC API
- `BatchSchedulerService` - 批量调度服务
  - `SubmitBatchTask` - 提交批量任务
  - `GetBatchStatus` - 查询批量任务状态

---

## 📝 配置管理

### 配置来源
1. **本地配置文件**: `config/config.json`
2. **环境变量**: 通过Viper自动读取
3. **配置中心**: 远程配置（Etcd）
4. **数据库配置表**: 动态配置

### 关键配置项
- 数据库连接信息
- K8s集群配置
- Kafka集群信息
- 外部服务端点
- 监控告警配置

---

## 🧪 测试

### 测试策略
- **单元测试**: 各个service、handler层都有对应的`_test.go`
- **集成测试**: 使用mock框架（gomonkey、go-mocket）
- **覆盖率**: 
  - 总体覆盖率要求
  - Diff覆盖率（针对MR变更）：50%（非master）/ 70%（master）

### 测试命令
```bash
make tests       # 运行所有测试
make testcover   # 运行测试并生成覆盖率
make coverage    # 生成覆盖率报告
```

---

## 🐳 容器化

### Docker镜像
- `Dockerfile` - Server主服务镜像
- `scheduler.Dockerfile` - Scheduler服务镜像
- `watch.Dockerfile` - Watch服务镜像
- `stats.Dockerfile` - Stats服务镜像
- `dr_controller.Dockerfile` - DR Controller镜像
- `binlog.Dockerfile` - Binlog服务镜像

### 构建命令
```bash
make server      # 构建server二进制
make scheduler   # 构建scheduler二进制
make watch       # 构建watch二进制
make stats       # 构建stats二进制
make dr_controller # 构建dr_controller二进制
make docker      # 构建Docker镜像
```

---

## 🔐 安全与权限

### RBAC权限控制
- 基于角色的访问控制
- 与XAC（Shopee统一权限系统）集成
- 支持本地RBAC配置（用于测试环境）

### 数据安全
- 数据库连接加密
- 敏感配置加密存储
- 操作审计日志

---

## 📈 监控与可观测性

### Metrics（指标）
- Prometheus格式指标暴露
- 任务执行指标（成功率、失败率、延迟）
- 资源使用指标（CPU、内存）
- 批量任务执行指标

### Logging（日志）
- 结构化日志
- 多级别日志（Debug、Info、Warn、Error）
- 日志轮转

### Tracing（链路追踪）
- 与Shopee内部追踪系统集成

---

## 🎯 业务场景

### 典型使用场景

#### 1. 新建CDC任务
DBA或开发人员通过Web界面创建CDC任务，将MySQL binlog实时同步到Kafka。

#### 2. 批量迁移
运维人员需要将100个CDC任务从旧集群迁移到新集群，使用批量操作功能一次性完成。

#### 3. 灾难恢复
当某个可用区出现故障时，DR Controller自动将任务切换到备用可用区。

#### 4. 配额管理
团队申请CDC资源配额，平台自动审批并控制资源使用。

#### 5. 日常运维
Watch服务自动检测任务异常，通过Seatalk通知相关人员，必要时自动重启任务。

---

## 🚧 扩展点

### 1. 新增CDC类型支持
在`batch_service/scheduler_job_handler_impl`中注册新的任务处理器。

### 2. 新增监控项
在`cdc_check_service`中添加新的检查逻辑。

### 3. 新增外部服务集成
在`common/external_services`中实现新的服务客户端。

### 4. 自定义批量操作
实现`SchedulerJobHandler`接口并注册。

---

## 📚 相关文档

- **Swagger API文档**: `docs/swagger.yaml`
- **Protobuf定义**: `protocol/proto/`
- **部署脚本**: `build/*/deploy*.sh`
- **SQL Schema**: `pkg/cdc/schceme/`

---

## 🤝 依赖服务

### 必需依赖
- MySQL（元数据存储）
- Kubernetes（任务运行环境）
- Kafka（CDC目标，某些场景）

### 可选依赖
- Redis（缓存、分布式锁）
- Etcd（配置中心）
- Prometheus（监控）
- Seatalk（告警通知）

---

## 🎓 关键设计模式

### 1. 工厂模式
- DAO工厂（`dal/dao/dao_factory.go`）
- 任务处理器注册（`scheduler_job_handler_impl`）

### 2. 策略模式
- 不同类型CDC任务的处理策略

### 3. 观察者模式
- K8s资源监听（Watcher）

### 4. 模板方法模式
- 批量任务执行流程

---

## 🔍 代码组织原则

### 分层架构
```
Handler层 → Service层 → DAO层 → DB
   ↓          ↓           ↓
 参数校验   业务逻辑   数据访问
```

### 模块化
- 按功能模块组织代码（cdc、batch_service、common等）
- 清晰的包边界和依赖关系

### 可测试性
- 接口驱动设计
- 依赖注入
- Mock支持

---

## 🎉 总结

CDC Manager是一个企业级的CDC任务管理平台，具有以下特点：

✨ **功能完善**: 涵盖CDC任务全生命周期管理  
🚀 **高可用**: 支持多副本、跨AZ部署  
📊 **可观测**: 完善的监控告警体系  
🔒 **安全可靠**: RBAC权限控制、审计日志  
⚡ **高性能**: 批量操作、并发处理  
🔧 **易扩展**: 清晰的架构设计、良好的代码组织

该系统是Shopee数据平台基础设施的重要组成部分，为各业务线提供稳定可靠的CDC能力。

