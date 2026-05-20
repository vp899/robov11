# RoboControl API 文档

> **版本**: 1.0.0  
> **更新日期**: 2026-05-16  
> **基础 URL**: `https://api.robocontrol.com/api/v1`

---

## 目录

1. [信令协议](#1-信令协议)
2. [Auth API](#2-auth-api)
3. [Billing API](#3-billing-api)
4. [内部 API](#4-内部-api)

---

## 1. 信令协议

### 1.1 WebSocket 连接

```
URL: wss://api.robocontrol.com/ws
协议: RoboControl Signaling Protocol (RCSP) v1.0
```

**连接参数：**

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `token` | string | ✅ | JWT Access Token |
| `device_id` | string | ✅ | 设备唯一标识 |
| `device_type` | string | ✅ | 设备类型：`robot` / `client` |
| `protocol_version` | int | ❌ | 协议版本，默认 1 |

**连接示例：**

```javascript
const ws = new WebSocket(
  'wss://api.robocontrol.com/ws?token=eyJhbG...&device_id=robot-001&device_type=robot'
);

ws.onopen = () => {
  console.log('Connected to signaling server');
};

ws.onmessage = (event) => {
  const msg = JSON.parse(event.data);
  handleMessage(msg);
};
```

### 1.2 消息格式

所有消息均为 JSON 格式，包含以下公共字段：

```json
{
  "type": "string",      // 消息类型（必填）
  "id": "string",        // 消息唯一ID（必填，UUID v4）
  "timestamp": 1684234567890,  // 时间戳（毫秒，必填）
  "from": "string",      // 发送方设备ID
  "to": "string",        // 目标设备ID
  "payload": {}          // 消息负载（类型相关）
}
```

### 1.3 消息类型

#### 1.3.1 连接管理类

**`ping` - 心跳探测**

```json
// 请求
{
  "type": "ping",
  "id": "550e8400-e29b-41d4-a716-446655440000",
  "timestamp": 1684234567890
}

// 响应
{
  "type": "pong",
  "id": "550e8400-e29b-41d4-a716-446655440000",
  "timestamp": 1684234567890
}
```

**`register` - 设备注册**

```json
// 请求
{
  "type": "register",
  "id": "...",
  "timestamp": 1684234567890,
  "payload": {
    "device_id": "robot-001",
    "device_type": "robot",
    "name": "Factory Robot A1",
    "capabilities": ["video", "audio", "control", "telemetry"],
    "metadata": {
      "manufacturer": "RoboCorp",
      "model": "RX-2000",
      "firmware_version": "2.1.0",
      "ip_address": "192.168.1.100"
    }
  }
}

// 响应
{
  "type": "register_ack",
  "id": "...",
  "timestamp": 1684234567890,
  "payload": {
    "status": "ok",
    "session_id": "sess-abc123",
    "server_time": 1684234567890,
    "config": {
      "heartbeat_interval_ms": 30000,
      "reconnect_interval_ms": 5000,
      "max_payload_size": 65536
    }
  }
}
```

**`unregister` - 设备注销**

```json
{
  "type": "unregister",
  "id": "...",
  "timestamp": 1684234567890,
  "payload": {
    "reason": "user_disconnect"
  }
}
```

#### 1.3.2 连接建立类

**`connect_request` - 请求连接设备**

```json
// 请求
{
  "type": "connect_request",
  "id": "...",
  "timestamp": 1684234567890,
  "from": "client-001",
  "to": "robot-001",
  "payload": {
    "connection_type": "control",  // "control" | "view_only" | "file_transfer"
    "preferred_transport": "p2p",  // "p2p" | "relay" | "auto"
    "media": {
      "video": true,
      "audio": true,
      "control": true
    },
    "candidate": {
      "type": "srflx",           // "host" | "srflx" | "relay"
      "address": "203.0.113.1",
      "port": 12345,
      "protocol": "udp"
    }
  }
}

// 响应（接受）
{
  "type": "connect_response",
  "id": "...",
  "timestamp": 1684234567890,
  "from": "robot-001",
  "to": "client-001",
  "payload": {
    "accepted": true,
    "connection_id": "conn-xyz789",
    "candidate": {
      "type": "srflx",
      "address": "198.51.100.1",
      "port": 54321,
      "protocol": "udp"
    },
    "crypto_params": {
      "cipher_suite": "AES_256_GCM",
      "key_exchange": "X25519",
      "public_key": "base64_encoded_public_key..."
    }
  }
}

// 响应（拒绝）
{
  "type": "connect_response",
  "id": "...",
  "timestamp": 1684234567890,
  "from": "robot-001",
  "to": "client-001",
  "payload": {
    "accepted": false,
    "reason": "busy",  // "busy" | "offline" | "unauthorized" | "rate_limited"
    "retry_after_ms": 30000
  }
}
```

**`connect_close` - 关闭连接**

```json
{
  "type": "connect_close",
  "id": "...",
  "timestamp": 1684234567890,
  "payload": {
    "connection_id": "conn-xyz789",
    "reason": "user_disconnect",  // "user_disconnect" | "timeout" | "error"
    "error_code": 0
  }
}
```

#### 1.3.3 媒体控制类

**`media_offer` - 媒体协商**

```json
{
  "type": "media_offer",
  "id": "...",
  "timestamp": 1684234567890,
  "from": "client-001",
  "to": "robot-001",
  "payload": {
    "connection_id": "conn-xyz789",
    "streams": [
      {
        "stream_id": 1,
        "type": "video",
        "codec": "H264",
        "resolution": {"width": 1920, "height": 1080},
        "framerate": 30,
        "bitrate": 4000000
      },
      {
        "stream_id": 2,
        "type": "audio",
        "codec": "OPUS",
        "sample_rate": 48000,
        "channels": 1,
        "bitrate": 64000
      },
      {
        "stream_id": 3,
        "type": "control",
        "reliable": true
      }
    ]
  }
}
```

**`media_answer` - 媒体应答**

```json
{
  "type": "media_answer",
  "id": "...",
  "timestamp": 1684234567890,
  "from": "robot-001",
  "to": "client-001",
  "payload": {
    "connection_id": "conn-xyz789",
    "accepted": true,
    "streams": [
      {
        "stream_id": 1,
        "type": "video",
        "codec": "H264",
        "resolution": {"width": 1920, "height": 1080},
        "framerate": 30,
        "bitrate": 4000000
      }
    ]
  }
}
```

**`media_control` - 媒体控制**

```json
// 暂停/恢复视频
{
  "type": "media_control",
  "id": "...",
  "timestamp": 1684234567890,
  "payload": {
    "connection_id": "conn-xyz789",
    "stream_id": 1,
    "action": "pause"  // "pause" | "resume" | "keyframe" | "bitrate_change"
  }
}

// 调整码率
{
  "type": "media_control",
  "id": "...",
  "timestamp": 1684234567890,
  "payload": {
    "connection_id": "conn-xyz789",
    "stream_id": 1,
    "action": "bitrate_change",
    "bitrate": 2000000
  }
}
```

#### 1.3.4 控制指令类

**`control_command` - 控制指令**

```json
{
  "type": "control_command",
  "id": "...",
  "timestamp": 1684234567890,
  "from": "client-001",
  "to": "robot-001",
  "payload": {
    "connection_id": "conn-xyz789",
    "command_id": 1,
    "priority": "high",  // "critical" | "high" | "normal" | "low"
    "command": "move",
    "params": {
      "direction": "forward",
      "speed": 1.5,
      "duration_ms": 1000
    },
    "require_ack": true
  }
}

// ACK
{
  "type": "control_ack",
  "id": "...",
  "timestamp": 1684234567890,
  "from": "robot-001",
  "to": "client-001",
  "payload": {
    "command_id": 1,
    "status": "executed",  // "received" | "executed" | "failed" | "rejected"
    "error_code": 0,
    "error_message": ""
  }
}
```

**`telemetry` - 遥测数据**

```json
{
  "type": "telemetry",
  "id": "...",
  "timestamp": 1684234567890,
  "from": "robot-001",
  "payload": {
    "connection_id": "conn-xyz789",
    "data": {
      "battery": 85,
      "speed": 1.2,
      "position": {"x": 10.5, "y": 20.3, "z": 0.0},
      "orientation": {"roll": 0.1, "pitch": -0.2, "yaw": 1.57},
      "temperature": 42.5,
      "cpu_usage": 35,
      "memory_usage": 60,
      "network_rtt_ms": 12,
      "errors": []
    }
  }
}
```

#### 1.3.5 错误类

**`error` - 错误通知**

```json
{
  "type": "error",
  "id": "...",
  "timestamp": 1684234567890,
  "payload": {
    "code": 4001,
    "message": "Connection timeout",
    "details": "No response from device within 10s",
    "retryable": true,
    "retry_after_ms": 5000
  }
}
```

### 1.4 错误码

| 错误码 | 说明 | 可重试 |
|--------|------|--------|
| 1000 | 正常关闭 | - |
| 1001 | 服务端关闭 | - |
| 3001 | 认证失败 | ❌ |
| 3002 | Token 过期 | ✅ (刷新 Token) |
| 3003 | 权限不足 | ❌ |
| 4001 | 连接超时 | ✅ |
| 4002 | 设备离线 | ✅ |
| 4003 | 设备忙 | ✅ |
| 4004 | 连接被拒绝 | ❌ |
| 5001 | 服务器内部错误 | ✅ |
| 5002 | 服务不可用 | ✅ |
| 5003 | 限速 | ✅ (等待 retry_after) |

---

## 2. Auth API

### 2.1 POST /auth/register - 用户注册

**请求：**

```http
POST /api/v1/auth/register
Content-Type: application/json

{
  "username": "robot_operator",
  "password": "SecureP@ss123",
  "email": "operator@example.com",
  "company": "RoboCorp",
  "plan": "free"  // "free" | "pro" | "enterprise"
}
```

**响应 (201 Created)：**

```json
{
  "status": "ok",
  "data": {
    "user_id": "usr-abc123",
    "username": "robot_operator",
    "email": "operator@example.com",
    "plan": "free",
    "created_at": "2026-05-16T10:00:00Z",
    "email_verified": false
  }
}
```

**错误响应 (400 Bad Request)：**

```json
{
  "status": "error",
  "error": {
    "code": "VALIDATION_ERROR",
    "message": "Username already exists",
    "details": {
      "field": "username",
      "value": "robot_operator"
    }
  }
}
```

### 2.2 POST /auth/token - 获取 Token

**请求：**

```http
POST /api/v1/auth/token
Content-Type: application/json

{
  "username": "robot_operator",
  "password": "SecureP@ss123",
  "device_id": "client-001",
  "device_type": "client"
}
```

**响应 (200 OK)：**

```json
{
  "status": "ok",
  "data": {
    "access_token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",
    "refresh_token": "dGhpcyBpcyBhIHJlZnJlc2ggdG9rZW4...",
    "token_type": "Bearer",
    "expires_in": 3600,
    "refresh_expires_in": 2592000,
    "scope": "connect:robot-001 connect:robot-002",
    "user_id": "usr-abc123"
  }
}
```

**错误响应 (401 Unauthorized)：**

```json
{
  "status": "error",
  "error": {
    "code": "AUTH_FAILED",
    "message": "Invalid username or password",
    "details": {
      "attempts_remaining": 4
    }
  }
}
```

### 2.3 POST /auth/verify - 验证 Token

**请求：**

```http
POST /api/v1/auth/verify
Content-Type: application/json
Authorization: Bearer eyJhbGciOiJIUzI1NiIs...

{
  "token": "eyJhbGciOiJIUzI1NiIs..."
}
```

**响应 (200 OK)：**

```json
{
  "status": "ok",
  "data": {
    "valid": true,
    "user_id": "usr-abc123",
    "username": "robot_operator",
    "scope": "connect:robot-001",
    "expires_at": "2026-05-16T11:00:00Z"
  }
}
```

### 2.4 POST /auth/refresh - 刷新 Token

**请求：**

```http
POST /api/v1/auth/refresh
Content-Type: application/json

{
  "refresh_token": "dGhpcyBpcyBhIHJlZnJlc2ggdG9rZW4..."
}
```

**响应 (200 OK)：**

```json
{
  "status": "ok",
  "data": {
    "access_token": "eyJhbGciOiJIUzI1NiIs...(new)",
    "refresh_token": "dGhpcyBpcyBhIHJlZnJlc2ggdG9rZW4...(new)",
    "token_type": "Bearer",
    "expires_in": 3600
  }
}
```

### 2.5 POST /auth/device/register - 设备注册

**请求：**

```http
POST /api/v1/auth/device/register
Content-Type: application/json
Authorization: Bearer eyJhbGciOiJIUzI1NiIs...

{
  "device_id": "robot-001",
  "device_type": "robot",
  "name": "Factory Robot A1",
  "public_key": "base64_encoded_public_key...",
  "metadata": {
    "manufacturer": "RoboCorp",
    "model": "RX-2000",
    "firmware_version": "2.1.0"
  }
}
```

**响应 (201 Created)：**

```json
{
  "status": "ok",
  "data": {
    "device_id": "robot-001",
    "registered_at": "2026-05-16T10:00:00Z",
    "device_token": "dev_token_abc123...",
    "capabilities": ["video", "audio", "control", "telemetry"]
  }
}
```

---

## 3. Billing API

### 3.1 POST /billing/subscribe - 订阅计划

**请求：**

```http
POST /api/v1/billing/subscribe
Content-Type: application/json
Authorization: Bearer eyJhbGciOiJIUzI1NiIs...

{
  "plan": "pro",  // "free" | "pro" | "enterprise"
  "billing_cycle": "monthly",  // "monthly" | "annual"
  "payment_method": "alipay",  // "alipay" | "wechat" | "card"
  "coupon_code": "WELCOME20"
}
```

**响应 (200 OK)：**

```json
{
  "status": "ok",
  "data": {
    "subscription_id": "sub-xyz789",
    "plan": "pro",
    "billing_cycle": "monthly",
    "amount": 199.00,
    "currency": "CNY",
    "discount": 39.80,
    "total": 159.20,
    "status": "pending_payment",
    "payment_url": "https://pay.alipay.com/...",
    "expires_at": "2026-06-16T10:00:00Z"
  }
}
```

### 3.2 POST /billing/usage - 查询用量

**请求：**

```http
POST /api/v1/billing/usage
Content-Type: application/json
Authorization: Bearer eyJhbGciOiJIUzI1NiIs...

{
  "start_date": "2026-05-01",
  "end_date": "2026-05-16",
  "granularity": "daily"  // "hourly" | "daily" | "monthly"
}
```

**响应 (200 OK)：**

```json
{
  "status": "ok",
  "data": {
    "user_id": "usr-abc123",
    "period": {
      "start": "2026-05-01",
      "end": "2026-05-16"
    },
    "summary": {
      "total_connections": 1250,
      "total_duration_hours": 3500,
      "total_bandwidth_gb": 128.5,
      "total_cost": 85.30,
      "currency": "CNY"
    },
    "daily": [
      {
        "date": "2026-05-01",
        "connections": 80,
        "duration_hours": 220,
        "bandwidth_gb": 8.2,
        "cost": 5.40
      },
      {
        "date": "2026-05-02",
        "connections": 95,
        "duration_hours": 260,
        "bandwidth_gb": 9.8,
        "cost": 6.30
      }
    ]
  }
}
```

### 3.3 GET /billing/invoice/:id - 获取账单

**请求：**

```http
GET /api/v1/billing/invoice/inv-abc123
Authorization: Bearer eyJhbGciOiJIUzI1NiIs...
```

**响应 (200 OK)：**

```json
{
  "status": "ok",
  "data": {
    "invoice_id": "inv-abc123",
    "user_id": "usr-abc123",
    "subscription_id": "sub-xyz789",
    "period": {
      "start": "2026-05-01",
      "end": "2026-05-31"
    },
    "items": [
      {
        "description": "Pro Plan - Monthly",
        "amount": 199.00
      },
      {
        "description": "Bandwidth Overage (28.5 GB × ¥0.10/GB)",
        "amount": 2.85
      },
      {
        "description": "Coupon: WELCOME20",
        "amount": -39.80
      }
    ],
    "subtotal": 201.85,
    "discount": 39.80,
    "tax": 0,
    "total": 162.05,
    "currency": "CNY",
    "status": "paid",
    "paid_at": "2026-05-01T00:05:00Z",
    "payment_method": "alipay",
    "pdf_url": "https://api.robocontrol.com/invoices/inv-abc123.pdf"
  }
}
```

### 3.4 GET /billing/plan - 查询当前计划

**请求：**

```http
GET /api/v1/billing/plan
Authorization: Bearer eyJhbGciOiJIUzI1NiIs...
```

**响应 (200 OK)：**

```json
{
  "status": "ok",
  "data": {
    "plan": "pro",
    "features": {
      "max_devices": 100,
      "max_concurrent_connections": 50,
      "bandwidth_limit_gb": 100,
      "video_resolution": "1080p",
      "relay_enabled": true,
      "priority_support": true
    },
    "usage": {
      "devices_registered": 25,
      "concurrent_connections_peak": 12,
      "bandwidth_used_gb": 72.3
    },
    "billing": {
      "cycle": "monthly",
      "amount": 199.00,
      "currency": "CNY",
      "next_billing_date": "2026-06-01"
    }
  }
}
```

---

## 4. 内部 API

### 4.1 服务间通信协议

RoboControl 内部服务间通信使用基于 Redis Pub/Sub + HTTP 的混合方案：

```
┌─────────────┐     Redis Pub/Sub      ┌─────────────┐
│  Signaling  │◄──────────────────────►│    Auth     │
│  Server     │                        │   Service   │
└──────┬──────┘                        └─────────────┘
       │
       │  HTTP (内部 API)
       │
       ├──────────────────►┌─────────────┐
       │                   │   Relay     │
       │                   │   Server    │
       │                   └─────────────┘
       │
       └──────────────────►┌─────────────┐
                           │  Billing    │
                           │  Service    │
                           └─────────────┘
```

### 4.2 Redis Pub/Sub 频道

| 频道 | 说明 | 消息格式 |
|------|------|----------|
| `rc:signaling:device_online` | 设备上线通知 | `{device_id, device_type, timestamp}` |
| `rc:signaling:device_offline` | 设备下线通知 | `{device_id, reason, timestamp}` |
| `rc:signaling:connection_created` | 连接创建通知 | `{connection_id, client_id, robot_id}` |
| `rc:signaling:connection_closed` | 连接关闭通知 | `{connection_id, reason}` |
| `rc:auth:token_revoked` | Token 撤销通知 | `{user_id, token_hash}` |
| `rc:relay:session_created` | Relay 会话创建 | `{session_id, conn_a, conn_b}` |
| `rc:relay:bandwidth_alert` | 带宽超限告警 | `{session_id, usage, limit}` |

### 4.3 Redis Key 规范

```
# 设备会话
rc:session:{device_id}              → Hash (device info, connection state)
rc:session:{device_id}:ttl          → TTL: 24h

# 连接映射
rc:conn:{connection_id}             → Hash (connection details)
rc:conn:{connection_id}:ttl         → TTL: 1h after disconnect

# 设备到连接映射
rc:device:{device_id}:connections   → Set (active connection IDs)

# 用户设备列表
rc:user:{user_id}:devices           → Set (registered device IDs)

# 认证
rc:auth:token:{token_hash}          → String (user_id)
rc:auth:refresh:{refresh_hash}      → String (user_id + device_id)
rc:auth:login_attempts:{user_id}    → Counter (失败次数)
rc:auth:login_attempts:{user_id}:ttl → TTL: 5min

# 计费
rc:billing:usage:{user_id}:{date}   → Hash (connections, bandwidth, duration)
rc:billing:rate_limit:{user_id}     → Counter (API 调用次数)
rc:billing:rate_limit:{user_id}:ttl → TTL: 1min

# Relay 会话
rc:relay:session:{session_id}       → Hash (session details)
rc:relay:bandwidth:{session_id}     → Hash (upload/download bytes)

# 集群节点
rc:cluster:nodes                    → Hash (node_id → address)
rc:cluster:nodes:{node_id}:heartbeat → String (timestamp)
rc:cluster:nodes:{node_id}:heartbeat:ttl → TTL: 30s

# 信令队列
rc:queue:signaling:{device_id}      → List (pending messages)
```

### 4.4 内部 HTTP API

#### Signaling → Auth

```http
# 验证 Token
POST http://auth:8081/internal/auth/verify
Content-Type: application/json

{"token": "eyJhbGciOiJIUzI1NiIs..."}

# 查询设备权限
GET http://auth:8081/internal/auth/device/{device_id}/permissions
Authorization: Bearer {internal_token}
```

#### Signaling → Relay

```http
# 创建 Relay 会话
POST http://relay:8082/internal/relay/session
Content-Type: application/json

{
  "connection_id": "conn-xyz789",
  "client_addr": "203.0.113.1:12345",
  "robot_addr": "198.51.100.1:54321",
  "bandwidth_limit": 10485760
}

# 查询 Relay 会话状态
GET http://relay:8082/internal/relay/session/{session_id}
```

#### Signaling → Billing

```http
# 记录连接使用量
POST http://billing:8083/internal/billing/record
Content-Type: application/json

{
  "user_id": "usr-abc123",
  "connection_id": "conn-xyz789",
  "duration_seconds": 3600,
  "bandwidth_bytes": 1073741824,
  "device_id": "robot-001"
}

# 检查配额
GET http://billing:8083/internal/billing/quota/{user_id}
```

### 4.5 内部认证

所有内部 API 调用使用共享密钥认证：

```http
Authorization: Bearer {internal_shared_token}
X-Service-Name: signaling
X-Request-ID: {uuid}
X-Timestamp: {unix_ms}
```

**Token 生成规则：**

```
internal_token = HMAC-SHA256(
  key = INTERNAL_SHARED_SECRET,
  message = "{service_name}:{timestamp}:{request_id}"
)
```

---

## 附录

### A. 计费计划对比

| 功能 | Free | Pro | Enterprise |
|------|------|-----|------------|
| **月费** | ¥0 | ¥199 | 定制 |
| **设备数** | 3 | 100 | 无限 |
| **并发连接** | 1 | 50 | 无限 |
| **带宽** | 5 GB/月 | 100 GB/月 | 无限 |
| **视频分辨率** | 720p | 1080p | 4K |
| **P2P 穿透** | ✅ | ✅ | ✅ |
| **Relay 中继** | ❌ | ✅ | ✅ |
| **API 访问** | ❌ | ✅ | ✅ |
| **优先支持** | ❌ | ✅ | ✅ |
| **私有化部署** | ❌ | ❌ | ✅ |
| **SLA** | - | 99.9% | 99.99% |

### B. SDK 集成示例

```c
/* C SDK 基本用法 */
#include <robocontrol/rc.h>

int main() {
    /* 初始化 */
    rc_config_t config = {
        .device_id = "robot-001",
        .device_type = RC_DEVICE_ROBOT,
        .server_url = "wss://api.robocontrol.com/ws",
        .auth_token = "eyJhbGciOiJIUzI1NiIs...",
    };
    
    rc_context_t *ctx = rc_init(&config);
    
    /* 注册回调 */
    rc_on_connect(ctx, on_connect_callback);
    rc_on_command(ctx, on_command_callback);
    rc_on_disconnect(ctx, on_disconnect_callback);
    
    /* 连接 */
    rc_connect(ctx);
    
    /* 主循环 */
    rc_run(ctx);  /* 阻塞 */
    
    /* 清理 */
    rc_destroy(ctx);
    return 0;
}
```
