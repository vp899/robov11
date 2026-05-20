# RoboControl 运营文档

> **版本**: 1.0.0  
> **更新日期**: 2026-05-16  
> **适用角色**: 运维工程师、SRE、产品经理

---

## 目录

1. [产品运营](#1-产品运营)
2. [监控告警](#2-监控告警)
3. [故障处理](#3-故障处理)
4. [安全运维](#4-安全运维)

---

## 1. 产品运营

### 1.1 用户增长模型

```
┌──────────────────────────────────────────────────────────────┐
│                    用户增长漏斗                                │
│                                                              │
│  访问量 (UV)     ████████████████████████████████  100%      │
│                      │                                      │
│                      ▼                                      │
│  注册转化         ██████████████████████           15%       │
│                      │                                      │
│                      ▼                                      │
│  设备接入         ████████████████                 8%        │
│                      │                                      │
│                      ▼                                      │
│  活跃使用         ██████████                       5%        │
│                      │                                      │
│                      ▼                                      │
│  付费转化         ████                             2%        │
│                                                              │
│  关键指标:                                                    │
│  - 注册转化率: 15%                                           │
│  - 设备接入率: 53% (注册→接入)                                │
│  - 活跃留存率: 62.5% (接入→活跃)                              │
│  - 付费转化率: 40% (活跃→付费)                                │
└──────────────────────────────────────────────────────────────┘
```

**增长策略：**

| 阶段 | 策略 | 目标 |
|------|------|------|
| **冷启动** | 开发者社区推广、开源 SDK | 1000 注册用户 |
| **增长期** | 内容营销、技术博客、行业展会 | 10,000 注册用户 |
| **加速期** | 合作伙伴渠道、企业客户拓展 | 100,000 注册用户 |
| **成熟期** | 品牌建设、生态建设 | 1,000,000 注册用户 |

### 1.2 设备管理

#### 设备生命周期

```
注册 → 配网 → 在线 → 使用中 → 离线 → 休眠 → 退役
 │                                              │
 └──────────── 设备管理面板 ─────────────────────┘
```

#### 设备状态定义

| 状态 | 说明 | 颜色 |
|------|------|------|
| `online` | 在线，可连接 | 🟢 绿色 |
| `busy` | 忙碌，正在被控制 | 🟡 黄色 |
| `idle` | 空闲，等待连接 | 🔵 蓝色 |
| `offline` | 离线 | 🔴 红色 |
| `sleeping` | 休眠 | ⚫ 灰色 |
| `error` | 故障 | 🔴 红色闪烁 |

#### 设备分组管理

```yaml
# 设备分组示例
groups:
  - name: "工厂A区"
    devices: ["robot-001", "robot-002", "robot-003"]
    policy:
      max_concurrent: 3
      allowed_users: ["usr-001", "usr-002"]
      schedule: "08:00-20:00"
  
  - name: "巡检机器人"
    devices: ["patrol-001", "patrol-002"]
    policy:
      max_concurrent: 1
      auto_connect: true
      recording: true
```

### 1.3 容量规划

#### 容量模型

```
单机容量:
  - 信令服务器: 100万 WebSocket 连接/台
  - Relay 服务器: 50万数据会话/台
  - Auth 服务器: 10万 QPS/台

集群容量 (当前):
  - 信令: 3 台 × 100万 = 300万连接
  - Relay: 3 台 × 50万 = 150万会话
  - Auth: 2 台 × 10万 = 20万 QPS

目标容量 (12个月):
  - 信令: 10 台 × 100万 = 1000万连接
  - Relay: 10 台 × 50万 = 500万会话
  - Auth: 5 台 × 10万 = 50万 QPS
```

#### 容量预警阈值

| 指标 | 预警阈值 | 告警阈值 | 说明 |
|------|----------|----------|------|
| CPU 使用率 | 60% | 80% | 持续 5 分钟 |
| 内存使用率 | 70% | 85% | 持续 5 分钟 |
| 连接数 | 70% 最大值 | 85% 最大值 | 即时 |
| 带宽使用率 | 60% | 80% | 持续 1 分钟 |
| 磁盘使用率 | 70% | 85% | 每小时检查 |
| Redis 内存 | 60% | 80% | 即时 |

#### 扩容决策流程

```
触发预警
    │
    ▼
分析瓶颈类型 ────► CPU 密集 ────► 增加计算节点
    │
    ├───────────► 内存不足 ────► 增加节点 或 升级配置
    │
    ├───────────► 带宽饱和 ────► 增加节点 + 升级网卡
    │
    └───────────► 连接饱和 ────► 增加信令节点
```

---

## 2. 监控告警

### 2.1 关键指标

#### 系统指标

| 指标名 | 类型 | 说明 |
|--------|------|------|
| `rc_system_cpu_usage` | Gauge | CPU 使用率 (%) |
| `rc_system_memory_usage` | Gauge | 内存使用率 (%) |
| `rc_system_disk_usage` | Gauge | 磁盘使用率 (%) |
| `rc_system_network_rx_bytes` | Counter | 网络接收字节数 |
| `rc_system_network_tx_bytes` | Counter | 网络发送字节数 |
| `rc_system_open_fds` | Gauge | 打开的文件描述符数 |

#### 业务指标

| 指标名 | 类型 | 说明 |
|--------|------|------|
| `rc_connections_active` | Gauge | 当前活跃连接数 |
| `rc_connections_total` | Counter | 总连接数（累计） |
| `rc_connections_rate` | Gauge | 连接建立速率 (conn/s) |
| `rc_packets_rx_total` | Counter | 接收报文总数 |
| `rc_packets_tx_total` | Counter | 发送报文总数 |
| `rc_packets_drop_total` | Counter | 丢包总数 |
| `rc_retransmit_total` | Counter | 重传总数 |
| `rc_rtt_avg_ms` | Gauge | 平均 RTT (ms) |
| `rc_rtt_p99_ms` | Gauge | P99 RTT (ms) |
| `rc_bandwidth_rx_bps` | Gauge | 接收带宽 (bps) |
| `rc_bandwidth_tx_bps` | Gauge | 发送带宽 (bps) |
| `rc_p2p_success_rate` | Gauge | P2P 穿透成功率 (%) |
| `rc_relay_sessions_active` | Gauge | 当前 Relay 会话数 |
| `rc_auth_requests_total` | Counter | Auth 请求总数 |
| `rc_auth_failures_total` | Counter | Auth 失败总数 |

#### 服务质量指标

| 指标名 | 类型 | 说明 |
|--------|------|------|
| `rc_sla_availability` | Gauge | 服务可用性 (%) |
| `rc_sla_latency_p50_ms` | Gauge | P50 端到端延迟 (ms) |
| `rc_sla_latency_p99_ms` | Gauge | P99 端到端延迟 (ms) |
| `rc_sla_error_rate` | Gauge | 错误率 (%) |
| `rc_sla_video_frame_loss` | Gauge | 视频帧丢失率 (%) |

### 2.2 告警阈值

#### P0 - 紧急（立即响应）

| 告警名称 | 条件 | 持续时间 | 说明 |
|----------|------|----------|------|
| 服务不可用 | 服务健康检查失败 | 30s | 服务宕机 |
| 连接数骤降 | 连接数下降 >50% | 1min | 可能故障 |
| 错误率飙升 | 错误率 >10% | 1min | 严重异常 |
| 磁盘满 | 磁盘使用率 >95% | 即时 | 服务可能停止 |
| Redis 不可用 | Redis 连接失败 | 10s | 会话丢失 |

#### P1 - 严重（30分钟内响应）

| 告警名称 | 条件 | 持续时间 | 说明 |
|----------|------|----------|------|
| CPU 过高 | CPU >80% | 5min | 需要扩容 |
| 内存过高 | 内存 >85% | 5min | 可能 OOM |
| 延迟过高 | P99 RTT >200ms | 3min | 用户体验下降 |
| 丢包率高 | 丢包率 >5% | 3min | 网络质量差 |
| P2P 成功率低 | 成功率 <80% | 5min | 影响连接质量 |

#### P2 - 警告（4小时内响应）

| 告警名称 | 条件 | 持续时间 | 说明 |
|----------|------|----------|------|
| CPU 偏高 | CPU >60% | 15min | 关注趋势 |
| 连接数增长 | 接近容量 70% | 30min | 规划扩容 |
| 日志异常 | 错误日志突增 | 10min | 排查原因 |
| 证书即将过期 | <30天过期 | 每天检查 | 更新证书 |

#### P3 - 信息（工作日内响应）

| 告警名称 | 条件 | 说明 |
|----------|------|------|
| 版本更新 | 新版本发布 | 评估升级 |
| 安全公告 | CVE 发布 | 评估影响 |
| 容量报告 | 每周生成 | 容量规划 |

### 2.3 On-Call 流程

```
┌──────────────────────────────────────────────────────────────┐
│                    On-Call 值班流程                            │
│                                                              │
│  告警触发                                                     │
│      │                                                       │
│      ▼                                                       │
│  PagerDuty/钉钉通知 ──► 值班工程师收到                         │
│                              │                               │
│                              ▼                               │
│                      5分钟内确认告警                           │
│                              │                               │
│                      ┌───────┴───────┐                       │
│                      ▼               ▼                       │
│              可以自行处理      需要升级                       │
│                      │               │                       │
│                      ▼               ▼                       │
│              30分钟内修复     升级到 P1 值班负责人              │
│                      │               │                       │
│                      ▼               ▼                       │
│              记录 Postmortem   15分钟内响应                    │
│                      │               │                       │
│                      └───────┬───────┘                       │
│                              ▼                               │
│                      故障修复 + 验证                          │
│                              │                               │
│                              ▼                               │
│                      24小时内提交 Postmortem                  │
│                                                              │
│  值班轮换: 每周一轮换，周一上午 10:00 交接                     │
│  值班补偿: P0 响应 ¥500/次，P1 响应 ¥200/次                   │
└──────────────────────────────────────────────────────────────┘
```

---

## 3. 故障处理

### 3.1 常见故障及解决方案

#### 故障 1：信令服务无响应

**症状：**
- WebSocket 连接超时
- 客户端显示"连接失败"
- 监控面板连接数为 0

**排查步骤：**

```bash
# 1. 检查服务状态
sudo systemctl status robocontrol-signaling

# 2. 检查端口监听
ss -tlnp | grep 8080

# 3. 检查日志
tail -100 /var/log/robocontrol/signaling.log | grep -i error

# 4. 检查 Redis
redis-cli ping

# 5. 检查系统资源
top -bn1 | head -20
free -h
df -h

# 6. 检查文件描述符
cat /proc/$(pgrep rc_signaling)/limits | grep "Max open files"
ls /proc/$(pgrep rc_signaling)/fd | wc -l
```

**解决方案：**

```bash
# 如果是 OOM
sudo systemctl restart robocontrol-signaling

# 如果是文件描述符耗尽
ulimit -n 10485760
sudo systemctl restart robocontrol-signaling

# 如果是 Redis 连接失败
sudo systemctl restart redis-server
sudo systemctl restart robocontrol-signaling

# 如果是端口被占用
sudo lsof -i :8080
sudo kill -9 <PID>
sudo systemctl start robocontrol-signaling
```

#### 故障 2：P2P 穿透失败率高

**症状：**
- P2P 成功率 <80%
- 大量连接走 Relay
- 用户反馈延迟高

**排查步骤：**

```bash
# 1. 检查 STUN 服务器
stun robocontrol.com:3478

# 2. 检查 UDP 端口
ss -ulnp | grep 10000

# 3. 检查防火墙
sudo iptables -L -n | grep udp
sudo ufw status

# 4. 检查 NAT 类型分布
curl -s http://localhost:9090/metrics | grep rc_nat_type

# 5. 检查 Relay 负载
curl -s http://localhost:9090/metrics | grep rc_relay_sessions
```

**解决方案：**

```bash
# 开放 UDP 端口范围
sudo ufw allow 10000:60000/udp

# 增加 STUN 服务器
# 在 signaling.yaml 中配置多个 STUN 服务器

# 增加 Relay 节点
# 部署新的 Relay 服务器并加入集群
```

#### 故障 3：视频卡顿/花屏

**症状：**
- 视频画面卡顿
- 画面出现马赛克或花屏
- 帧率下降

**排查步骤：**

```bash
# 1. 检查带宽使用
iftop -i eth0

# 2. 检查丢包率
curl -s http://localhost:9090/metrics | grep rc_packets_drop

# 3. 检查重传率
curl -s http://localhost:9090/metrics | grep rc_retransmit

# 4. 检查 RTT
curl -s http://localhost:9090/metrics | grep rc_rtt

# 5. 检查网卡队列
ethtool -S eth0 | grep -i drop
cat /proc/net/softnet_stat
```

**解决方案：**

```bash
# 增加网卡队列
ethtool -L eth0 combined 16

# 增加 UDP 缓冲区
sysctl -w net.core.rmem_max=33554432
sysctl -w net.core.wmem_max=33554432

# 检查是否有带宽瓶颈
# 考虑升级网卡或增加节点
```

#### 故障 4：Redis 内存溢出

**症状：**
- Redis 报 OOM 错误
- 会话数据丢失
- 新连接无法建立

**排查步骤：**

```bash
# 1. 检查 Redis 内存
redis-cli info memory

# 2. 检查大 Key
redis-cli --bigkeys

# 3. 检查 Key 数量
redis-cli dbsize

# 4. 检查淘汰策略
redis-cli config get maxmemory-policy
```

**解决方案：**

```bash
# 增加 Redis 内存限制
redis-cli config set maxmemory 16gb

# 设置淘汰策略
redis-cli config set maxmemory-policy allkeys-lru

# 清理过期 Key
redis-cli eval "for i, k in ipairs(redis.call('keys', 'rc:session:*')) do redis.call('del', k) end" 0

# 持久化重启
redis-cli bgsave
sudo systemctl restart redis-server
```

### 3.2 故障升级流程

```
┌──────────────────────────────────────────────────────────────┐
│                    故障升级流程                                │
│                                                              │
│  Level 1: 值班工程师 (0-30min)                               │
│  ├── 尝试自行修复                                            │
│  ├── 记录故障现象和排查步骤                                   │
│  └── 30分钟内无法解决 → 升级到 L2                            │
│                                                              │
│  Level 2: 高级工程师 (30min-2h)                              │
│  ├── 深入排查根因                                            │
│  ├── 尝试临时修复方案                                        │
│  └── 2小时内无法解决 → 升级到 L3                             │
│                                                              │
│  Level 3: 技术负责人 (2h-4h)                                 │
│  ├── 评估影响范围                                            │
│  ├── 决定是否回滚                                            │
│  ├── 协调跨团队资源                                          │
│  └── 4小时内无法解决 → 升级到 L4                             │
│                                                              │
│  Level 4: CTO / VP Engineering (4h+)                        │
│  ├── 启动应急响应                                            │
│  ├── 决定是否降级服务                                        │
│  ├── 通知客户和合作伙伴                                      │
│  └── 事后复盘                                                │
└──────────────────────────────────────────────────────────────┘
```

### 3.3 灾备方案

#### 多活架构

```
┌──────────────────────────────────────────────────────────────┐
│                    多活架构                                    │
│                                                              │
│  主集群 (上海)                        备集群 (北京)           │
│  ┌─────────────────┐                ┌─────────────────┐     │
│  │  Signaling ×3   │                │  Signaling ×3   │     │
│  │  Relay ×3       │◄── 实时同步 ──►│  Relay ×3       │     │
│  │  Auth ×2        │                │  Auth ×2        │     │
│  │  Redis Cluster  │                │  Redis Cluster  │     │
│  └─────────────────┘                └─────────────────┘     │
│          │                                    │              │
│          ▼                                    ▼              │
│  ┌─────────────────┐                ┌─────────────────┐     │
│  │  GeoDNS         │                │  GeoDNS         │     │
│  │  (华东/华南)     │                │  (华北/东北)     │     │
│  └─────────────────┘                └─────────────────┘     │
│                                                              │
│  故障切换:                                                    │
│  1. GeoDNS 检测主集群不可用                                   │
│  2. 自动将流量切换到备集群                                    │
│  3. 切换时间 <30s                                             │
│  4. 数据一致性: 最终一致                                      │
└──────────────────────────────────────────────────────────────┘
```

#### 数据备份策略

| 数据类型 | 备份频率 | 保留周期 | 存储位置 |
|----------|----------|----------|----------|
| Redis 数据 | 每小时 RDB | 7 天 | 本地 + OSS |
| 用户数据 | 每天全量 | 30 天 | PostgreSQL + OSS |
| 账单数据 | 实时同步 | 永久 | PostgreSQL + OSS |
| 日志数据 | 实时采集 | 30 天 | ELK + OSS |
| 配置文件 | Git 版本控制 | 永久 | GitLab |

#### 灾难恢复流程

```
1. 评估损失
   - 数据丢失范围
   - 服务中断时长
   - 影响用户数

2. 启动灾备
   - 切换 DNS 到备集群
   - 验证备集群服务正常
   - 通知用户服务已恢复

3. 数据恢复
   - 从最近备份恢复数据
   - 验证数据完整性
   - 补偿丢失的增量数据

4. 切回主集群
   - 修复主集群问题
   - 数据同步到主集群
   - 灰度切流回主集群
   - 全量切回

5. 事后复盘
   - 编写 Postmortem
   - 制定改进措施
   - 更新应急预案
```

---

## 4. 安全运维

### 4.1 密钥管理

#### 密钥分类

| 密钥类型 | 用途 | 轮换周期 | 存储方式 |
|----------|------|----------|----------|
| JWT Secret | Token 签名 | 90 天 | 环境变量 + Vault |
| TLS 证书 | HTTPS/WSS | 90 天 (Let's Encrypt 自动续期) | 文件系统 |
| ECDHE 密钥对 | 握手密钥交换 | 每次连接 | 内存（不持久化） |
| AES 会话密钥 | 数据加密 | 每 12h 或 64GB | 内存（不持久化） |
| Redis 密码 | Redis 认证 | 180 天 | 环境变量 + Vault |
| API Key | 第三方集成 | 365 天 | Vault |

#### 密钥轮换流程

```bash
# JWT Secret 轮换（零停机）
# 1. 生成新密钥
NEW_SECRET=$(openssl rand -base64 64)

# 2. 配置双密钥验证（新旧共存）
# signaling.yaml 中添加 old_jwt_secret

# 3. 部署新配置
sudo systemctl reload robocontrol-signaling

# 4. 等待所有旧 Token 过期（最长 1 小时）

# 5. 移除旧密钥配置

# TLS 证书自动续期
# Let's Encrypt certbot
sudo certbot renew --deploy-hook "sudo systemctl reload nginx"
```

### 4.2 证书管理

```bash
# 申请证书（Let's Encrypt）
sudo certbot certonly --nginx -d api.robocontrol.com

# 证书自动续期 (crontab)
0 0 1 * * /usr/bin/certbot renew --quiet --deploy-hook "/usr/bin/systemctl reload nginx"

# 检查证书状态
sudo certbot certificates

# 手动续期
sudo certbot renew

# 内部 CA 证书（用于服务间 mTLS）
# 生成 CA
openssl req -x509 -newkey rsa:4096 -keyout ca.key -out ca.crt -days 3650 -nodes \
  -subj "/CN=RoboControl Internal CA"

# 生成服务证书
openssl req -newkey rsa:2048 -keyout server.key -out server.csr \
  -subj "/CN=signaling.robocontrol.internal"
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out server.crt -days 365
```

### 4.3 安全审计

#### 审计日志

```json
// 审计日志格式
{
  "timestamp": "2026-05-16T10:00:00Z",
  "event_type": "auth.login",
  "user_id": "usr-abc123",
  "ip_address": "203.0.113.1",
  "user_agent": "RoboControl-SDK/1.0",
  "result": "success",
  "details": {
    "device_id": "client-001",
    "mfa_used": true
  }
}
```

#### 审计事件类型

| 事件类型 | 说明 | 级别 |
|----------|------|------|
| `auth.login` | 用户登录 | INFO |
| `auth.login_failed` | 登录失败 | WARNING |
| `auth.logout` | 用户登出 | INFO |
| `auth.token_refresh` | Token 刷新 | INFO |
| `auth.password_change` | 密码修改 | WARNING |
| `device.register` | 设备注册 | INFO |
| `device.remove` | 设备删除 | WARNING |
| `connection.create` | 连接创建 | INFO |
| `connection.close` | 连接关闭 | INFO |
| `admin.config_change` | 配置变更 | CRITICAL |
| `admin.user_create` | 用户创建 | WARNING |
| `admin.user_delete` | 用户删除 | CRITICAL |
| `security.key_rotation` | 密钥轮换 | INFO |
| `security.cert_update` | 证书更新 | INFO |
| `security.suspicious` | 可疑活动 | CRITICAL |

#### 安全扫描

```bash
# 定期安全扫描（每月）
# 1. 依赖漏洞扫描
./scripts/audit_deps.sh

# 2. 配置检查
./scripts/check_config.sh

# 3. 端口扫描
nmap -sV api.robocontrol.com

# 4. SSL 配置检查
sslscan api.robocontrol.com

# 5. 渗透测试（季度，由专业安全团队执行）
```

#### 合规要求

| 标准 | 状态 | 说明 |
|------|------|------|
| **等保 2.0 三级** | 进行中 | 信息系统安全等级保护 |
| **GDPR** | 计划中 | 欧盟数据保护（如涉及海外用户） |
| **SOC 2 Type II** | 计划中 | 服务组织控制报告 |
| **ISO 27001** | 远期目标 | 信息安全管理体系 |

---

## 附录

### A. 常用命令速查

```bash
# 服务状态
sudo systemctl status robocontrol-*

# 重启所有服务
sudo systemctl restart robocontrol-signaling robocontrol-auth robocontrol-relay robocontrol-billing

# 查看连接数
ss -ulnp | grep 10000 | wc -l

# 查看 Redis 状态
redis-cli info stats

# 查看 Nginx 状态
sudo nginx -t
sudo systemctl status nginx

# 查看系统负载
uptime
vmstat 1 5
iostat -x 1 5
```

### B. 重要联系人

| 角色 | 姓名 | 联系方式 | 职责 |
|------|------|----------|------|
| On-Call Lead | [TBD] | [TBD] | 一线故障响应 |
| SRE 负责人 | [TBD] | [TBD] | 基础设施运维 |
| 安全负责人 | [TBD] | [TBD] | 安全事件响应 |
| CTO | [TBD] | [TBD] | 重大故障决策 |
