# RoboControl 协议规范

## 1. 概述

RoboControl协议是一个基于UDP的可靠传输协议，专为机器人远程控制场景设计。它借鉴了：
- **WebRTC**：P2P连接建立（ICE-lite）
- **QUIC**：安全握手流程
- **TCP**：丢包检测和重传（SACK）
- **RFC 8312**：CUBIC拥塞控制

## 2. 报文格式

### 2.1 报文头（32字节）

```
  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                      Magic (0x52434F4E)                       |  [0-3]
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |        Version (16)           |          Type (16)            |  [4-7]
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                       Sequence Number                         |  [8-11]
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                    Acknowledgment Number                      |  [12-15]
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                                                               |
 |                    Timestamp (64 bits)                         |
 |                                                               |  [16-23]
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |      Payload Length (16)      |          Flags (16)           |  [24-27]
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                       Connection ID                           |  [28-31]
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| 字段 | 偏移 | 大小 | 描述 |
|------|------|------|------|
| Magic | 0 | 4B | 固定值 0x52434F4E ("RCON") |
| Version | 4 | 2B | 协议版本号（当前=1） |
| Type | 6 | 2B | 报文类型（见下表） |
| Seq | 8 | 4B | 序列号 |
| Ack | 12 | 4B | 累积确认号 |
| Timestamp | 16 | 8B | 微秒级时间戳 |
| PayloadLen | 24 | 2B | 载荷长度 |
| Flags | 26 | 2B | 标志位 |
| ConnID | 28 | 4B | 连接ID |

### 2.2 报文类型

| 类型 | 值 | 方向 | 描述 |
|------|----|------|------|
| HELLO | 0x01 | C→S | 发起连接 |
| HELLO_ACK | 0x02 | S→C | 接受连接 |
| DATA | 0x03 | 双向 | 应用数据 |
| ACK | 0x04 | 双向 | 累积确认 |
| NACK | 0x05 | 双向 | 选择性否定确认 |
| PING | 0x06 | 双向 | 心跳探测 |
| PONG | 0x07 | 双向 | 心跳响应 |
| RELAY_REQ | 0x08 | C→S | 请求Relay |
| RELAY_ACK | 0x09 | S→C | Relay已分配 |
| AUTH_REQ | 0x0A | C→S | 认证请求 |
| AUTH_ACK | 0x0B | S→C | 认证结果 |
| FIN | 0x0C | 双向 | 关闭连接 |

### 2.3 标志位

| Bit | 名称 | 描述 |
|-----|------|------|
| 0 | SYN | 同步（握手） |
| 1 | FIN | 结束 |
| 2 | ACK | 确认有效 |
| 3 | ENCRYPTED | 载荷已加密 |
| 4 | SACK | 包含SACK块 |
| 5-15 | Reserved | 保留 |

## 3. 连接状态机

```
    ┌─────────┐
    │ CLOSED  │
    └────┬────┘
         │ send(ClientHello)
         ▼
    ┌────────────┐
    │ CONNECTING │
    └────┬───────┘
         │ recv(ServerHello)
         ▼
    ┌──────────────┐
    │ HANDSHAKING  │
    └────┬─────────┘
         │ recv(Finish) + send(Finish)
         ▼
    ┌─────────────┐
    │ ESTABLISHED │◄────────────────────┐
    └────┬────────┘                     │
         │ send(FIN)                    │ recv/ack
         ▼                              │
    ┌─────────┐                         │
    │ CLOSING │─────────────────────────┘
    └────┬────┘  (timeout: retransmit FIN)
         │ recv(FIN_ACK) or timeout
         ▼
    ┌─────────┐
    │ CLOSED  │
    └─────────┘
```

## 4. 安全握手

### 4.1 握手流程（3-way handshake）

```
Client                                      Server
  │                                           │
  │──── ClientHello ─────────────────────────►│
  │     (version, conn_id, nonce, pubkey)     │
  │                                           │
  │◄──── ServerHello ────────────────────────│
  │     (conn_id, nonce, pubkey, seq_start)   │
  │                                           │
  │     [双方计算 ECDHE 共享密钥]              │
  │     [派生会话密钥 (HKDF-SHA256)]          │
  │                                           │
  │──── Finish (HMAC verify_data) ──────────►│
  │◄─── Finish (HMAC verify_data) ──────────│
  │                                           │
  │     [连接建立，数据传输开始]               │
```

### 4.2 密钥派生

```
shared_secret = X25519(client_privkey, server_pubkey)

session_keys = HKDF-SHA256(
    ikm = shared_secret,
    salt = client_nonce || server_nonce,
    info = "robocontrol v1",
    len = 96
)

→ session_keys = {
    enc_key: 32 bytes,    // AES-256-GCM 或 ChaCha20 密钥
    dec_key: 32 bytes,    // 对端加密密钥
    iv:      12 bytes,    // 初始IV
}
```

### 4.3 数据加密

每个DATA报文的载荷使用DTLS-like记录加密：

```
encrypted_payload = AES-256-GCM(key=enc_key, iv=iv||seq, plaintext)
tag = GCM authentication tag (16 bytes)

→ 报文载荷 = encrypted_payload || tag
```

序列号每发一个包递增，防止重放攻击。

### 4.4 密钥轮换

每发送 `RC_KEY_ROTATION_INTERVAL`（10000）个包后，使用当前密钥派生新密钥：

```
new_key = HKDF-SHA256(ikm=old_key, salt=seq, info="key-rotation")
```

## 5. 可靠传输

### 5.1 SACK确认

接收方通过SACK块告知发送方哪些包已收到：

```
SACK报文:
  cum_ack = 100           // 累积确认到100
  blocks = [
    {start: 102, end: 105},  // 102-105已收到
    {start: 108, end: 110},  // 108-110已收到
  ]
```

### 5.2 丢包检测

| 机制 | 触发条件 | 动作 |
|------|---------|------|
| 快速重传 | 收到3个重复ACK | 立即重传 |
| 超时重传 | RTO到期 | 重传最早未确认包 |
| SACK重传 | SACK显示空洞 | 选择性重传空洞包 |

### 5.3 RTO计算

```
RTTVAR = RTTVAR * 3/4 + |SRTT - RTT_sample| * 1/4
SRTT  = SRTT * 7/8 + RTT_sample * 1/8
RTO   = SRTT + max(G, 4 * RTTVAR)

其中:
  G = 时钟粒度（通常1ms）
  最小RTO = 100ms
  最大RTO = 60000ms
```

## 6. CUBIC拥塞控制

### 6.1 算法流程

```
1. 慢启动阶段 (Slow Start):
   cwnd += MSS  (每收到一个ACK)
   if cwnd >= ssthresh → 进入拥塞避免

2. 拥塞避免阶段 (Congestion Avoidance):
   W_cubic(t) = C * (t - K)^3 + W_max
   cwnd = max(cwnd, W_cubic(t))

3. 丢包事件:
   W_max = cwnd
   ssthresh = cwnd * 0.7 (乘法减少)
   cwnd = cwnd * 0.7
   K = cubic_root(W_max * (1 - beta) / C)
```

### 6.2 CUBIC参数

| 参数 | 值 | 描述 |
|------|----|------|
| C | 0.4 | 缩放常数 |
| beta | 0.7 | 乘法减少因子 |
| init_cwnd | 10 MSS | 初始窗口 |

## 7. P2P穿透

### 7.1 ICE-lite流程

```
Client A (Robot)                Server                Client B (App)
     │                            │                        │
     │──── 收集候选地址 ─────────►│                        │
     │                            │◄──── 收集候选地址 ────│
     │                            │                        │
     │◄─── 交换候选地址 ─────────│──── 交换候选地址 ─────►│
     │                            │                        │
     │◄══════════════════ UDP打洞 ═══════════════════════►│
     │                            │                        │
     │◄══════════════════ P2P直连建立 ═══════════════════►│
     │                            │                        │
```

### 7.2 Relay降级

当P2P穿透失败（对称NAT、防火墙等），自动切换到Relay：

```
Client A ──► Relay Server ──► Client B
              (UDP转发)
```

Relay决策逻辑：
1. P2P打洞超时（5秒）→ 请求Relay
2. Relay分配session_id
3. 双方通过Relay转发数据
4. 定期尝试P2P重新打洞（每30秒）

## 8. 多流复用

每个连接支持最多8个流（stream），通过报文头中的payload前4字节标识：

```
┌─────────────────────────────────────┐
│          32字节报文头                │
├──────┬──────────────────────────────┤
│Stream│       Stream Payload          │
│ ID   │                               │
│(4B)  │  Stream 0: 控制指令           │
│      │  Stream 1-7: 视频帧           │
└──────┴──────────────────────────────┘
```

| Stream ID | 用途 | 特性 |
|-----------|------|------|
| 0 | 控制指令 | 低延迟、高优先级 |
| 1 | 摄像头1 | 高带宽 |
| 2 | 摄像头2 | 高带宽 |
| 3-7 | 摄像头3-7 | 高带宽 |

## 9. 错误码

| 错误码 | 值 | 描述 |
|--------|----|------|
| OK | 0 | 成功 |
| INVALID_MAGIC | 1 | 魔数校验失败 |
| UNSUPPORTED_VERSION | 2 | 不支持的版本 |
| AUTH_FAILED | 3 | 认证失败 |
| RATE_LIMITED | 4 | 限流 |
| SESSION_NOT_FOUND | 5 | 会话不存在 |
| RELAY_UNAVAILABLE | 6 | Relay不可用 |
| INTERNAL_ERROR | 7 | 内部错误 |
| CONNECTION_CLOSED | 8 | 连接已关闭 |

## 10. 扩展机制

协议通过Version字段和Type字段支持扩展：

- **版本协商**：ClientHello携带支持的最高版本，ServerHello选择
- **新报文类型**：0x80-0xFF保留给私有扩展
- **新标志位**：Flags字段高位保留给扩展
- **载荷扩展**：通过payload内部TLV格式扩展字段
