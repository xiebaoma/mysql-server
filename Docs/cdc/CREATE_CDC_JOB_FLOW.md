# 创建CDC任务完整代码调用流程

## 📋 概述

本文档详细梳理从用户请求到MySQL binlog同步到Kafka的完整代码调用流程。

---

## 🎯 业务场景

**目标**: 创建一个CDC任务，实时将MySQL数据库的binlog变更同步到Kafka

**涉及组件**:
- MySQL（数据源）
- CDC Manager（管理平台）
- Kubernetes（运行环境）
- CDC Worker（实际的binlog采集程序）
- Kafka（目标消息队列）

---

## 🔄 完整调用流程图

```
┌──────────────────────────────────────────────────────────────────┐
│                    用户发起创建CDC任务请求                        │
│              POST /api/tools/add_cdc_job                         │
└──────────────────┬───────────────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────────────┐
│  Handler层: AddCDCJob()                                          │
│  文件: pkg/cdc/handler/tools_handler.go:182                      │
│                                                                   │
│  1. 解析请求参数                                                  │
│     - MySQL集群名称                                              │
│     - MySQL数据库列表                                            │
│     - Kafka集群和Topic                                           │
│     - 环境/区域/可用区                                            │
│                                                                   │
│  2. 构造 CDCJob 对象                                              │
│     job := db_model.CDCJob{                                      │
│         Env: "test/live",                                        │
│         JobName: "cluster-xxxx-uuid",                            │
│         ImageVersion: "v2.x.x",                                  │
│         Region: "sg/us",                                         │
│         Az: "sg10/us3",                                          │
│         CPU: 1000 (1核),                                         │
│         Memory: 2 (2GB),                                         │
│         JobStatus: CDCJobStatusStopped,                          │
│         RunSystem: "k8s",                                        │
│     }                                                            │
│                                                                   │
│  3. 构造 CDCJobDetail 对象（每个数据库一条）                      │
│     for each database {                                          │
│         detail := db_model.CDCJobDetail{                         │
│             MysqlHost: "db-master-xxx",                          │
│             MysqlDatabase: "shopee_order_db",                    │
│             Topic: "cdc_topic_shopee_order",                     │
│             KafkaCluster: "kafka-cluster-sg",                    │
│             KafkaAddress: "kafka.sg.shopee.io:9092",             │
│         }                                                        │
│     }                                                            │
└──────────────────┬───────────────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────────────┐
│  Service层: BuildConfigForCDC()                                  │
│  文件: pkg/cdc/service/ticket_handler_service.go:716             │
│                                                                   │
│  构建CDC任务的JSON配置文件                                        │
│                                                                   │
│  config := CDCConfig{                                            │
│      Env: "live",                                                │
│      Name: job.JobName,                                          │
│      SourceInstance: "db-master-order-sg.shopee.io:6606",       │
│      SourceType: "mysql.no_schema",                              │
│      SourceUser: "rds_rep_gds",                                  │
│      SourcePassword: "****",                                     │
│      TargetType: "cdc",                                          │
│      SinkType: "cdc",                                            │
│                                                                   │
│      ExtraConfig: {                                              │
│          MetadataType: "mysql",  // 或 "http"                    │
│          MetadataAddr: "db-cdc-meta.shopee.io:6606",            │
│          MetadataNamespace: "shopee_cdc_meta_db",               │
│          MetadataUser: "cdc_meta",                               │
│          MetadataPasswd: "****",                                 │
│          Region: "sg",                                           │
│          Step: "db_stream_sync,",                                │
│      },                                                          │
│                                                                   │
│      CDCStreams: {                                               │
│          AllConfig: {                                            │
│              SendModel: "",                                      │
│              MergePurgeBinlog: false,                            │
│          },                                                      │
│          MQStreams: [                                            │
│              {                                                   │
│                  Type: "kafka",                                  │
│                  FilterRule: "^(shopee_order_db_00000000)\..*$",│
│                  Server: "kafka.sg.shopee.io:9092",             │
│                  Topic: "cdc_topic_shopee_order",                │
│                  User: "kafka-user",                             │
│                  Passwd: "****",                                 │
│                  PartitionKey: "db_name.table_name",             │
│                  ACKs: "WaitForAll",                             │
│                  FlushBatch: 50,                                 │
│                  MaxMessageBytes: 1048588,                       │
│              }                                                   │
│          ]                                                       │
│      }                                                           │
│  }                                                               │
│                                                                   │
│  job.CDCConfig = json.Marshal(config)                            │
└──────────────────┬───────────────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────────────┐
│  数据库层: 保存到MySQL                                            │
│  文件: pkg/cdc/db/job_db.go                                      │
│                                                                   │
│  db.ExecInTx(                                                    │
│      // 事务1: 插入 cdc_job 表                                   │
│      func(tx *gorm.DB) error {                                   │
│          INSERT INTO cdc_job (                                   │
│              env, job_name, cdc_config,                          │
│              image_version, region, az,                          │
│              cpu, memory, business_level,                        │
│              job_status, run_system,                             │
│              create_time, update_time                            │
│          ) VALUES (...)                                          │
│      },                                                          │
│                                                                   │
│      // 事务2: 插入 cdc_job_detail 表（多条）                    │
│      func(tx *gorm.DB) error {                                   │
│          for each detail {                                       │
│              INSERT INTO cdc_job_detail (                        │
│                  env, job_name,                                  │
│                  mysql_host, mysql_database,                     │
│                  topic, kafka_cluster, kafka_address,            │
│                  create_time, update_time                        │
│              ) VALUES (...)                                      │
│          }                                                       │
│      }                                                           │
│  )                                                               │
│                                                                   │
│  状态: job_status = "stopped" (初始状态，未启动)                 │
└──────────────────┬───────────────────────────────────────────────┘
                   │
                   │ ✅ CDC任务创建完成，返回成功
                   │
┌──────────────────▼───────────────────────────────────────────────┐
│                  用户启动CDC任务                                  │
│              POST /api/cdc/job/start                             │
│              Body: { "id": 12345 }                               │
└──────────────────┬───────────────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────────────┐
│  Handler层: CDCJobStart()                                        │
│  文件: pkg/cdc/handler/job_handler.go:159                        │
│                                                                   │
│  1. 解析请求参数（jobID）                                         │
│  2. 获取用户信息（user email）                                    │
│  3. 调用服务层 StartCDCJob()                                      │
└──────────────────┬───────────────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────────────┐
│  Service层: StartCDCJob()                                        │
│  文件: pkg/cdc/service/job_service.go:119                        │
│                                                                   │
│  1. 从数据库获取CDC任务                                           │
│     job, err := db.GetCDCJob(id)                                 │
│                                                                   │
│  2. 验证任务状态                                                  │
│     if job.JobStatus != CDCJobStatusStopped {                    │
│         return error("任务状态不是stopped，无法启动")            │
│     }                                                            │
│                                                                   │
│  3. 记录操作历史                                                  │
│     ohr := OperationHistoryRecord{                               │
│         Operator: user,                                          │
│         OperationType: CDCOperationStartJob,                     │
│         Reason: "Start CDC Job",                                 │
│     }                                                            │
│                                                                   │
│  4. 在数据库事务中执行：                                          │
│     db.ExecInTx(                                                 │
│         // 事务1: 运行CDC任务                                    │
│         func(tx) { RunCDCJob(job) },                             │
│                                                                   │
│         // 事务2: 更新任务状态为running                           │
│         func(tx) {                                               │
│             UPDATE cdc_job                                       │
│             SET job_status = 'running'                           │
│             WHERE id = ?                                         │
│         },                                                       │
│                                                                   │
│         // 事务3: 插入操作历史                                   │
│         func(tx) { InsertOperationHistory(ohr) }                 │
│     )                                                            │
└──────────────────┬───────────────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────────────┐
│  Service层: RunCDCJob()                                          │
│  文件: pkg/cdc/service/job_opt_service.go:15                     │
│                                                                   │
│  根据运行系统类型分发：                                           │
│                                                                   │
│  switch job.RunSystem {                                          │
│      case "k8s":                                                 │
│          ┌─────────────────────────────────────────┐            │
│          │ K8s部署方式 (主流方式)                  │            │
│          │ k8s.CreateCDCDeploy(job)                │            │
│          └─────────────────────────────────────────┘            │
│                                                                   │
│      case "controller":                                          │
│          ┌─────────────────────────────────────────┐            │
│          │ Controller方式（HA模式）                │            │
│          │ cdc_controller.StartControllerJob(job)  │            │
│          └─────────────────────────────────────────┘            │
│  }                                                               │
│                                                                   │
│  成功后上报配额信息：                                             │
│  go cdc_quota.CDCQuotaLogReport(job, CreateQuotaFlowApply)      │
└──────────────────┬───────────────────────────────────────────────┘
                   │
                   │ 以K8s部署为例（主流方式）↓
                   │
                   ▼
┌──────────────────────────────────────────────────────────────────┐
│  K8s层: CreateCDCDeploy()                                        │
│  文件: pkg/cdc/service/external/k8s/k8s_service.go:96            │
│                                                                   │
│  步骤1: 创建ConfigMap                                             │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ createConfigMap(job)                                       │ │
│  │                                                            │ │
│  │ 1. 获取K8s客户端                                           │ │
│  │    client := connector.GetInstance().GetCDCClientSets(    │ │
│  │        job.Env, job.Region, job.Az                        │ │
│  │    )                                                       │ │
│  │                                                            │ │
│  │ 2. 先删除旧的ConfigMap（如果存在）                         │ │
│  │    client.CoreV1().ConfigMaps(namespace).Delete(          │ │
│  │        getDeployName(job.JobName),                        │ │
│  │        metav1.DeleteOptions{}                             │ │
│  │    )                                                       │ │
│  │                                                            │ │
│  │ 3. 创建新的ConfigMap                                       │ │
│  │    configMap := &ConfigMap{                               │ │
│  │        ObjectMeta: metav1.ObjectMeta{                     │ │
│  │            Name: job.JobName,                             │ │
│  │            Namespace: "cdc-live/cdc-test",                │ │
│  │        },                                                  │ │
│  │        Data: map[string]string{                           │ │
│  │            "config.json": job.CDCConfig,  // CDC配置JSON  │ │
│  │        },                                                  │ │
│  │    }                                                       │ │
│  │                                                            │ │
│  │    client.CoreV1().ConfigMaps(namespace).Create(          │ │
│  │        context.TODO(),                                    │ │
│  │        configMap,                                         │ │
│  │        metav1.CreateOptions{}                             │ │
│  │    )                                                       │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                   │
│  步骤2: 创建Deployment                                            │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ buildDeployment(job)                                       │ │
│  │                                                            │ │
│  │ deployment := &Deployment{                                │ │
│  │     ObjectMeta: metav1.ObjectMeta{                        │ │
│  │         Name: job.JobName,                                │ │
│  │     },                                                     │ │
│  │     Spec: DeploymentSpec{                                 │ │
│  │         Selector: &LabelSelector{                         │ │
│  │             MatchLabels: map[string]string{               │ │
│  │                 "job-name": job.JobName,                  │ │
│  │             },                                            │ │
│  │         },                                                 │ │
│  │         Template: PodTemplateSpec{                        │ │
│  │             ObjectMeta: metav1.ObjectMeta{                │ │
│  │                 Labels: map[string]string{                │ │
│  │                     "job-name": job.JobName,              │ │
│  │                 },                                        │ │
│  │             },                                            │ │
│  │             Spec: PodSpec{                                │ │
│  │                 Containers: []Container{                  │ │
│  │                     {                                     │ │
│  │                         Name: job.JobName,                │ │
│  │                         Image: job.ImageVersion,  // CDC镜像│ │
│  │                                                            │ │
│  │                         // 挂载ConfigMap作为配置文件        │ │
│  │                         VolumeMounts: []VolumeMount{      │ │
│  │                             {                             │ │
│  │                                 Name: job.JobName,        │ │
│  │                                 MountPath: "/workspace/config/",│ │
│  │                                 ReadOnly: true,           │ │
│  │                             },                            │ │
│  │                         },                                │ │
│  │                                                            │ │
│  │                         // 资源限制                        │ │
│  │                         Resources: ResourceRequirements{  │ │
│  │                             Limits: {                     │ │
│  │                                 CPU: "1000m",  // 1核      │ │
│  │                                 Memory: "2Gi", // 2GB      │ │
│  │                             },                            │ │
│  │                         },                                │ │
│  │                                                            │ │
│  │                         // 启动命令                        │ │
│  │                         Command: ["streamer"],            │ │
│  │                         Args: [                           │ │
│  │                             "--local",                    │ │
│  │                             "--stdout",                   │ │
│  │                             "--config",                   │ │
│  │                             "/workspace/config/config.json"│ │
│  │                         ],                                │ │
│  │                     }                                     │ │
│  │                 },                                        │ │
│  │                                                            │ │
│  │                 // 挂载ConfigMap卷                         │ │
│  │                 Volumes: []Volume{                        │ │
│  │                     {                                     │ │
│  │                         Name: job.JobName,                │ │
│  │                         VolumeSource: VolumeSource{       │ │
│  │                             ConfigMap: &ConfigMapVolumeSource{│ │
│  │                                 LocalObjectReference: {   │ │
│  │                                     Name: job.JobName,    │ │
│  │                                 },                        │ │
│  │                             },                            │ │
│  │                         },                                │ │
│  │                     }                                     │ │
│  │                 },                                        │ │
│  │             },                                            │ │
│  │         },                                                 │ │
│  │     },                                                     │ │
│  │ }                                                          │ │
│  │                                                            │ │
│  │ client.AppsV1().Deployments(namespace).Create(            │ │
│  │     context.TODO(),                                       │ │
│  │     deployment,                                           │ │
│  │     metav1.CreateOptions{}                                │ │
│  │ )                                                          │ │
│  └────────────────────────────────────────────────────────────┘ │
└──────────────────┬───────────────────────────────────────────────┘
                   │
                   │ ✅ K8s Deployment创建成功
                   │
                   ▼
┌──────────────────────────────────────────────────────────────────┐
│              Kubernetes集群调度Pod                                │
│                                                                   │
│  1. K8s调度器选择合适的节点                                       │
│  2. Kubelet拉取CDC镜像（streamer）                                │
│  3. 创建Pod并挂载ConfigMap                                        │
│  4. 启动容器                                                      │
└──────────────────┬───────────────────────────────────────────────┘
                   │
                   ▼
┌──────────────────────────────────────────────────────────────────┐
│              CDC Worker容器启动                                   │
│                                                                   │
│  容器内执行命令:                                                  │
│  streamer --local --stdout --config /workspace/config/config.json│
│                                                                   │
│  步骤1: 读取配置文件                                              │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ 从 /workspace/config/config.json 读取CDC配置                │ │
│  │ - MySQL连接信息                                            │ │
│  │ - Kafka连接信息                                            │ │
│  │ - 元数据存储信息                                           │ │
│  │ - 过滤规则                                                 │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                   │
│  步骤2: 连接元数据存储                                            │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ 连接到元数据MySQL或HTTP服务                                │ │
│  │ - 查询上次同步的GTID位置                                   │ │
│  │ - 如果是首次启动，从当前位置开始                           │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                   │
│  步骤3: 连接源MySQL数据库                                         │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ 使用配置的账号密码连接MySQL                                │ │
│  │ - User: rds_rep_gds                                        │ │
│  │ - Host: db-master-order-sg.shopee.io:6606                 │ │
│  │                                                            │ │
│  │ 发起binlog dump请求：                                      │ │
│  │ COM_BINLOG_DUMP_GTID                                       │ │
│  │ - GTID: xxxxx:1-12345                                     │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                   │
│  步骤4: 接收并解析binlog事件                                      │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ MySQL Server持续推送binlog事件：                           │ │
│  │                                                            │ │
│  │ - ROTATE_EVENT (切换日志文件)                             │ │
│  │ - FORMAT_DESCRIPTION_EVENT                                │ │
│  │ - GTID_EVENT                                              │ │
│  │ - QUERY_EVENT (DDL: CREATE/ALTER/DROP)                   │ │
│  │ - TABLE_MAP_EVENT (表结构映射)                            │ │
│  │ - WRITE_ROWS_EVENT (INSERT操作)                           │ │
│  │ - UPDATE_ROWS_EVENT (UPDATE操作)                          │ │
│  │ - DELETE_ROWS_EVENT (DELETE操作)                          │ │
│  │ - XID_EVENT (事务提交)                                    │ │
│  │                                                            │ │
│  │ CDC Worker解析这些事件：                                   │ │
│  │ 1. 解析表结构（TABLE_MAP_EVENT）                          │ │
│  │ 2. 解析DML操作（WRITE/UPDATE/DELETE_ROWS_EVENT）          │ │
│  │ 3. 提取变更数据：                                          │ │
│  │    {                                                       │ │
│  │        "database": "shopee_order_db_00000000",            │ │
│  │        "table": "order_info",                             │ │
│  │        "type": "insert",                                  │ │
│  │        "data": {                                          │ │
│  │            "order_id": 123456,                            │ │
│  │            "user_id": 789,                                │ │
│  │            "amount": 99.99,                               │ │
│  │            "create_time": "2023-10-22 10:00:00"           │ │
│  │        }                                                   │ │
│  │    }                                                       │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                   │
│  步骤5: 过滤和转换                                                │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ 1. 应用FilterRule过滤：                                    │ │
│  │    "^(shopee_order_db_00000000)\..*$"                     │ │
│  │    只处理匹配的数据库.表                                   │ │
│  │                                                            │ │
│  │ 2. 应用DML过滤规则（如果配置）：                            │ │
│  │    跳过特定类型的DML（如只要INSERT和UPDATE）              │ │
│  │                                                            │ │
│  │ 3. 计算分区键：                                            │ │
│  │    PartitionKey = "db_name.table_name"                    │ │
│  │    → "shopee_order_db_00000000.order_info"                │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                   │
│  步骤6: 发送到Kafka                                               │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ 连接Kafka集群：                                            │ │
│  │ - Server: kafka.sg.shopee.io:9092                         │ │
│  │ - Topic: cdc_topic_shopee_order                           │ │
│  │ - User/Password: kafka认证                                │ │
│  │                                                            │ │
│  │ 发送消息：                                                 │ │
│  │ producer.Send({                                            │ │
│  │     Topic: "cdc_topic_shopee_order",                      │ │
│  │     Partition: hash(partition_key) % partition_count,     │ │
│  │     Key: "shopee_order_db_00000000.order_info",           │ │
│  │     Value: json.Marshal(binlogEvent),                     │ │
│  │     Headers: {                                            │ │
│  │         "database": "shopee_order_db_00000000",           │ │
│  │         "table": "order_info",                            │ │
│  │         "operation": "insert",                            │ │
│  │         "gtid": "xxxxx:1-12345",                          │ │
│  │     }                                                      │ │
│  │ })                                                         │ │
│  │                                                            │ │
│  │ 批量发送配置：                                              │ │
│  │ - FlushBatch: 50条消息批量发送                             │ │
│  │ - ACKs: WaitForAll (等待所有副本确认)                      │ │
│  │ - MaxMessageBytes: 1048588 (约1MB)                        │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                   │
│  步骤7: 更新同步位点                                              │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ 定期将当前GTID位点保存到元数据存储：                        │ │
│  │                                                            │ │
│  │ UPDATE metadata SET                                        │ │
│  │     gtid_set = 'xxxxx:1-12345',                           │ │
│  │     binlog_file = 'mysql-bin.000123',                     │ │
│  │     binlog_pos = 987654,                                  │ │
│  │     update_time = NOW()                                   │ │
│  │ WHERE job_name = 'shopee_order-abc123'                    │ │
│  │                                                            │ │
│  │ 用途：任务重启后可以从上次位置继续                          │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                   │
│  步骤8: 监控与上报                                                │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ 上报Prometheus指标：                                       │ │
│  │ - cdc_binlog_events_total (处理的事件总数)                 │ │
│  │ - cdc_binlog_lag_seconds (同步延迟)                        │ │
│  │ - cdc_kafka_send_success (Kafka发送成功数)                 │ │
│  │ - cdc_kafka_send_failed (Kafka发送失败数)                  │ │
│  └────────────────────────────────────────────────────────────┘ │
└──────────────────┬───────────────────────────────────────────────┘
                   │
                   │ 🔄 持续运行，实时同步binlog
                   │
                   ▼
┌──────────────────────────────────────────────────────────────────┐
│              下游Kafka消费者处理                                  │
│                                                                   │
│  消费者订阅Topic: cdc_topic_shopee_order                          │
│                                                                   │
│  接收到的消息格式：                                               │
│  {                                                                │
│      "database": "shopee_order_db_00000000",                     │
│      "table": "order_info",                                      │
│      "type": "insert",                                           │
│      "timestamp": 1698036000,                                    │
│      "gtid": "xxxxx:1-12345",                                    │
│      "data": {                                                   │
│          "order_id": 123456,                                     │
│          "user_id": 789,                                         │
│          "amount": 99.99,                                        │
│          "status": "created",                                    │
│          "create_time": "2023-10-22 10:00:00"                    │
│      },                                                          │
│      "old_data": null  // UPDATE时才有                           │
│  }                                                               │
│                                                                   │
│  应用场景：                                                       │
│  - 数据同步到数据仓库                                             │
│  - 实时数据分析                                                   │
│  - 缓存刷新                                                       │
│  - 搜索引擎索引更新                                               │
│  - 触发下游业务逻辑                                               │
└──────────────────────────────────────────────────────────────────┘
```

---

## 📝 关键数据结构

### 1. CDCJob (任务主表)

```go
type CDCJob struct {
    ID            uint64           // 任务ID
    Env           string           // 环境: live/test
    JobName       string           // 任务名称: cluster-uuid
    CDCConfig     string           // CDC配置JSON
    ImageVersion  string           // Docker镜像版本
    Region        string           // 区域: sg/us/br
    Az            string           // 可用区: sg10/us3
    CPU           int              // CPU (m): 1000=1核
    Memory        int              // 内存 (GB): 2
    BusinessLevel BusinessLevel    // 业务级别: P0/P1/P2
    JobStatus     CDCJobStatus     // 状态: stopped/running/warning
    RunSystem     CDCRunSystem     // 运行系统: k8s/controller
    CreateTime    int64            // 创建时间
    UpdateTime    int64            // 更新时间
}
```

### 2. CDCJobDetail (任务详情表)

```go
type CDCJobDetail struct {
    ID                    uint64   // 详情ID
    Env                   string   // 环境
    JobName               string   // 任务名称
    MysqlHost             string   // MySQL主机
    MysqlDatabase         string   // 数据库名
    Topic                 string   // Kafka Topic
    KafkaCluster          string   // Kafka集群名
    KafkaAddress          string   // Kafka地址
    KafkaProducerUser     string   // Kafka用户
    KafkaProducerPassword string   // Kafka密码
    KafkaMaxMessageBytes  int64    // 最大消息大小
    DMLFilteredType       string   // DML过滤类型
    KafkaAck              string   // ACK模式: WaitForAll/WaitForLocal
    CreateTime            int64    // 创建时间
    UpdateTime            int64    // 更新时间
}
```

### 3. CDCConfig (CDC配置JSON)

```go
type CDCConfig struct {
    Env            string       // 环境
    Name           string       // 任务名称
    SourceInstance string       // 源MySQL地址
    SourceType     string       // 源类型: mysql.no_schema
    SourceUser     string       // MySQL用户
    SourcePassword string       // MySQL密码
    TargetType     string       // 目标类型: cdc
    SinkType       string       // Sink类型: cdc
    TaskName       string       // 任务名
    
    ExtraConfig    ExtraConfig  // 额外配置
    CDCStreams     CDCStreams   // CDC流配置
    ConfigDetail   ConfigDetail // 配置详情
}

type ExtraConfig struct {
    MetadataType      string   // 元数据类型: mysql/http
    MetadataAddr      string   // 元数据地址
    MetadataNamespace string   // 元数据命名空间
    MetadataUser      string   // 元数据用户
    MetadataPasswd    string   // 元数据密码
    Region            string   // 区域
    Step              string   // 步骤: db_stream_sync,
}

type CDCStreams struct {
    AllConfig      AllConfig        // 全局配置
    MQStreams      []MQStream       // MQ流配置（支持多个Kafka）
    DMLFilterRules []DMLFilterRule  // DML过滤规则
}

type MQStream struct {
    Type            string  // 类型: kafka
    FilterRule      string  // 过滤规则（正则）
    Server          string  // Kafka地址
    Topic           string  // Topic名称
    User            string  // Kafka用户
    Passwd          string  // Kafka密码
    PartitionKey    string  // 分区键: db_name.table_name
    ACKs            string  // ACK模式
    FlushBatch      int     // 批量大小
    MaxMessageBytes int64   // 最大消息字节数
}
```

---

## 🔍 关键代码文件索引

| 层次 | 文件路径 | 主要功能 |
|-----|---------|---------|
| **Handler层** | pkg/cdc/handler/tools_handler.go:182 | AddCDCJob() - 接收创建请求 |
| **Handler层** | pkg/cdc/handler/job_handler.go:159 | CDCJobStart() - 接收启动请求 |
| **Service层** | pkg/cdc/service/job_service.go:119 | StartCDCJob() - 启动任务 |
| **Service层** | pkg/cdc/service/job_opt_service.go:15 | RunCDCJob() - 运行任务 |
| **Service层** | pkg/cdc/service/ticket_handler_service.go:716 | BuildConfigForCDC() - 构建配置 |
| **K8s层** | pkg/cdc/service/external/k8s/k8s_service.go:96 | CreateCDCDeploy() - 创建Deployment |
| **K8s层** | pkg/cdc/service/external/k8s/k8s_service.go:112 | createConfigMap() - 创建ConfigMap |
| **K8s层** | pkg/cdc/service/external/k8s/k8s_service.go:134 | buildDeployment() - 构建Deployment对象 |
| **DB层** | pkg/cdc/db/job_db.go | InsertCDCJobWithTx() - 插入任务 |
| **Model层** | pkg/cdc/model/db_model/job_model.go | CDCJob结构定义 |
| **Model层** | pkg/cdc/model/service_model/cdc_config.go | CDCConfig结构定义 |

---

## ⚙️ 配置示例

### 完整的CDC配置JSON示例

```json
{
  "env": "live",
  "name": "shopee_order_logistics_v2_br-c31b11eb",
  "biz_type": "",
  "priority_level": 1,
  
  "extra_config": {
    "job_tag": {
      "biz_type": "",
      "job_type": "cdc",
      "priority": "1",
      "task_type": "db-message"
    },
    "metadata_addr": "db-master-cdc-meta-sg1-live.shopeemobile.com:6606",
    "metadata_namespace": "shopee_cdc_meta_db",
    "metadata_passwd": "****",
    "metadata_type": "mysql",
    "metadata_user": "cdc_meta",
    "region": "sg",
    "step": "db_stream_sync,"
  },
  
  "source_instance": "db-master-order-sg-00000000-sg1-live.shopeemobile.com:6606",
  "source_password": "****",
  "source_type": "mysql.no_schema",
  "source_user": "rds_rep_gds",
  
  "target_type": "cdc",
  "sink_type": "cdc",
  "task_name": "shopee_order_logistics_v2_br-c31b11eb",
  
  "cdc_streams": {
    "all_config": {
      "key": "val",
      "send_model": "",
      "merge_purge_binlog": false,
      "need_filtered_dml_type": ""
    },
    "dml_filter_rules": [],
    "mq_streams": [
      {
        "type": "kafka",
        "filter_rule": "^(shopee_order_logistics_v2_co_db_00000000)\\..*$",
        "server": "kafka.mbp.ap-sg-1-general-a.live.mq.shopee.io:9092",
        "topic": "cdc_live_shopee_order_logistics_v2_co_db_00000000",
        "user": "cdc-write-user",
        "passwd": "****",
        "partition_key": "db_name.table_name",
        "acks": "WaitForAll",
        "flush_batch": 50,
        "max_message_bytes": 1048588
      }
    ]
  },
  
  "config_detail": {
    "runtime": {
      "sync_rate_limit_qps": 0,
      "read_concurrency": 0,
      "write_concurrency": 0,
      "inc_rate_limit": false
    },
    "stream_sync": true,
    "resource": {
      "cpu": 0,
      "memory": ""
    }
  },
  
  "switch_az_info": {
    "version": 0
  }
}
```

---

## 🎯 核心流程总结

1. **创建阶段** (停止状态)
   - 用户提交 → Handler解析 → Service构建配置 → 保存到数据库
   - 状态: `stopped`

2. **启动阶段** (启动任务)
   - 用户触发启动 → Service验证 → 创建K8s资源 → 更新状态为running

3. **K8s部署阶段**
   - 创建ConfigMap (CDC配置文件)
   - 创建Deployment (CDC Worker容器)

4. **CDC Worker运行阶段**
   - 读取配置 → 连接MySQL → 接收binlog → 解析事件 → 发送Kafka → 更新位点

5. **持续同步阶段**
   - 实时接收binlog事件
   - 过滤和转换数据
   - 批量发送到Kafka
   - 定期保存同步位点

---

## 🚨 关键注意事项

1. **事务保证**: 创建和启动任务都使用数据库事务保证一致性

2. **幂等性**: K8s资源创建前先删除旧资源，避免重复创建

3. **配置管理**: CDC配置以ConfigMap方式挂载，支持动态更新

4. **位点管理**: 通过元数据存储保存GTID位点，支持任务重启恢复

5. **监控告警**: 集成Prometheus和Seatalk，实时监控任务状态

6. **配额控制**: 任务启动后上报配额信息，进行资源管理

7. **多环境支持**: 支持test/live/liveish等多环境部署

8. **高可用**: 支持K8s和Controller两种运行模式

---

## 📊 状态流转

```
┌──────────┐   创建    ┌──────────┐   启动    ┌──────────┐
│   无     │ ────────> │ stopped  │ ────────> │ running  │
└──────────┘           └──────────┘           └──────────┘
                             ▲                      │
                             │       停止           │
                             └──────────────────────┘
                                      │
                                      │ 异常
                                      ▼
                                ┌──────────┐
                                │ warning  │
                                └──────────┘
```

---

## 🎓 总结

这是一个完整的企业级CDC管理系统，从HTTP API到K8s容器部署，再到binlog实时同步到Kafka的全流程实现。核心特点：

✅ **分层架构**: Handler → Service → K8s → CDC Worker  
✅ **声明式配置**: 通过JSON配置驱动CDC行为  
✅ **容器化部署**: K8s管理CDC Worker生命周期  
✅ **高可靠性**: 事务保证、位点管理、自动恢复  
✅ **可扩展性**: 支持多数据库、多Kafka、多环境  
✅ **可观测性**: Prometheus监控 + Seatalk告警  

整个流程实现了MySQL binlog到Kafka的实时数据同步能力，为下游数据分析、缓存刷新、搜索索引等场景提供基础支撑。

