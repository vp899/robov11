# RoboControl 系统架构文档

> **版本**: 1.0.0  
> **更新日期**: 2026-05-16  
> **维护团队**: RoboControl Core Team

---

## 目录

1. [系统概述](#1-系统概述)
2. [整体架构图](#2-整体架构图)
3. [核心模块详细设计](#3-核心模块详细设计)
4. [千万并发架构设计](#4-千万并发架构设计)
5. [网络拓扑](#5-网络拓扑)
6. [数据流图](#6-数据流图)
7. [技术选型对比](#7-技术选型对比)

---

## 1. 系统概述

### 1.1 产品定位

RoboControl 是面向**千万级机器人远程控制**的实时通讯平台。系统基于自研的 UDP 协议栈，提供超低延迟（<50ms）的远程指令控制、高带宽实时视频传输、以及可靠的 P2P 穿透能力。

**核心能力：**

| 能力 | 指标 | 说明 |
|------|------|------|
| P2P 穿透 | 成功率 >92% | 支持 Symmetric NAT、Port Restricted Cone 等复杂 NAT 场景 |
| 实时视频传输 | 1080p@30fps, <100ms | 自适应码率，支持 H.264/H.265 |
| 远程指令控制 | <50ms E2E | 优先级队列，保证关键指令零丢包 |
| 并发连接 | 10M fd/server | 单机千万级连接，水平扩展至无限 |
| 安全性 | E2E 加密 | AES-256-GCM / ChaCha20-Poly1305，密钥轮换 |

### 1.2 设计目标

1. **极致低延迟**：端到端控制指令延迟 <50ms（同地域）、<150ms（跨地域）
2. **超高并发**：单机支持 1000 万并发连接，集群支持亿级扩展
3. **高可用性**：SLA 99.99%，故障自动切换 <3s
4. **强安全性**：端到端加密，零信任架构，防中间人攻击
5. **轻量嵌入式**：Robot SDK <200KB，支持 RTOS 和 Linux 嵌入式平台

### 1.3 适用场景

- 工业机器人远程操控（AGV、机械臂）
- 服务机器人远程巡检
- 无人机远程驾驶
- 医疗机器人远程手术辅助
- 教育/科研机器人远程实验

---

## 2. 整体架构图

### 2.1 系统全景

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          RoboControl 系统全景                               │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   ┌─────────────┐    ┌──────────────┐    ┌─────────────┐                   │
│   │  Robot SDK  │◄──►│  Signaling   │◄──►│  Client App │                   │
│   │  (Embedded) │    │  Cluster     │    │  (Web/App)  │                   │
│   └──────┬──────┘    └──────┬───────┘    └──────┬──────┘                   │
│          │                  │                    │                          │
│          ▼                  ▼                    ▼                          │
│   ┌──────────────┐   ┌──────────────┐   ┌──────────────┐                   │
│   │  P2P Direct  │   │  Auth Service│   │  Billing Svc │                   │
│   │  (DTLS/UDP)  │   │  (Redis)     │   │  (Redis)     │                   │
│   └──────┬───────┘   └──────────────┘   └──────────────┘                   │
│          │                                                                  │
│          ▼ (fallback)                                                       │
│   ┌──────────────┐                                                          │
│   │  Relay       │                                                          │
│   │  Cluster     │                                                          │
│   └──────────────┘                                                          │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 分层架构

```
┌─────────────────────────────────────────────────────────────────┐
│                      应用层 (Application Layer)                  │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐       │
│  │Robot SDK │  │Web Client│  │Mobile App│  │Admin Panel│       │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘       │
├─────────────────────────────────────────────────────────────────┤
│                      服务层 (Service Layer)                      │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐       │
│  │Signaling │  │  Auth    │  │ Billing  │  │Monitor   │       │
│  │ Cluster  │  │ Service  │  │ Service  │  │ Service  │       │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘       │
├─────────────────────────────────────────────────────────────────┤
│                      传输层 (Transport Layer)                    │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐       │
│  │  P2P     │  │  Relay   │  │Congestion│  │ Retrans  │       │
│  │  Engine  │  │  Cluster │  │ Control  │  │  mit     │       │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘       │
├─────────────────────────────────────────────────────────────────┤
│                      协议层 (Protocol Layer)                     │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐       │
│  │  Proto   │  │Handshake │  │  Crypto  │  │  Frame   │       │
│  │  Codec   │  │  Engine  │  │  Engine  │  │  Parser  │       │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘       │
├─────────────────────────────────────────────────────────────────┤
│                      内核层 (Kernel Layer)                       │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐       │
│  │  epoll   │  │  Timer   │  │  Memory  │  │  Ring    │       │
│  │  Engine  │  │  Wheel   │  │  Pool    │  │  Buffer  │       │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘       │
└─────────────────────────────────────────────────────────────────┘
```

### 2.3 模块依赖关系

```
                    ┌──────────┐
                    │ rc_app   │  (应用入口)
                    └────┬─────┘
                         │
          ┌──────────────┼──────────────┐
          ▼              ▼              ▼
    ┌──────────┐  ┌──────────┐  ┌──────────┐
    │ rc_conn  │  │ rc_p2p   │  │ rc_relay │
    └────┬─────┘  └────┬─────┘  └────┬─────┘
         │              │              │
         ├──────────────┼──────────────┤
         ▼              ▼              ▼
    ┌──────────┐  ┌──────────┐  ┌──────────┐
    │ rc_proto │  │rc_handshk│  │rc_crypto │
    └────┬─────┘  └────┬─────┘  └────┬─────┘
         │              │              │
         └──────────────┼──────────────┘
                        ▼
              ┌──────────────────┐
              │    rc_epoll      │  (事件循环)
              └────────┬─────────┘
                       │
              ┌────────┼────────┐
              ▼        ▼        ▼
        ┌────────┐ ┌────────┐ ┌────────┐
        │rc_timer│ │rc_mempool│ │rc_log│
        └────────┘ └────────┘ └────────┘
```

---

## 3. 核心模块详细设计

### 3.1 协议层 (rc_proto)

#### 3.1.1 报文格式

所有 RoboControl 报文基于 UDP，采用统一的二进制帧格式：

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|Version| Type  |    Flags      |         Stream ID             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      Sequence Number                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Acknowledgment Number                      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Window  |      Payload Length         |     Header Checksum   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Timestamp (64-bit)                        |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                     Payload (变长)                            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

**字段说明：**

| 字段 | 位数 | 说明 |
|------|------|------|
| Version | 4 bit | 协议版本号，当前为 `0x1` |
| Type | 4 bit | 报文类型：0x0=DATA, 0x1=ACK, 0x2=SYN, 0x3=SYN-ACK, 0x4=FIN, 0x5=FIN-ACK, 0x6=NACK, 0x7=PING, 0x8=PONG |
| Flags | 8 bit | 控制标志：bit0=加密, bit1=压缩, bit2=紧急, bit3=需要ACK, bit4=SACK, bit5-7=保留 |
| Stream ID | 16 bit | 流标识，支持多路复用 |
| Sequence Number | 32 bit | 序列号 |
| Acknowledgment Number | 32 bit | 确认号 |
| Window | 6 bit | 滑动窗口大小（2^n 个报文） |
| Payload Length | 10 bit | 负载长度，最大 1023 字节 |
| Header Checksum | 16 bit | 头部校验和（CRC-16） |
| Timestamp | 64 bit | 微秒级时间戳 |

#### 3.1.2 序列化策略

```c
/* 报文头部结构体 */
typedef struct __attribute__((packed)) {
    uint8_t  version_type;      /* version(4bit) + type(4bit) */
    uint8_t  flags;             /* 控制标志 */
    uint16_t stream_id;         /* 网络字节序 */
    uint32_t seq_num;           /* 网络字节序 */
    uint32_t ack_num;           /* 网络字节序 */
    uint16_t window_payload;    /* window(6bit) + payload_len(10bit) */
    uint16_t checksum;          /* CRC-16 */
    uint64_t timestamp;         /* 微秒时间戳，网络字节序 */
} rc_proto_header_t;

#define RC_PROTO_HEADER_SIZE  sizeof(rc_proto_header_t)  /* 20 bytes */
#define RC_PROTO_MAX_PAYLOAD  1023
#define RC_PROTO_VERSION      0x1
```

#### 3.1.3 校验机制

- **头部校验**：CRC-16/CCITT，覆盖整个头部 20 字节
- **负载校验**：CRC-32C（可选，通过 Flags 控制）
- **完整性校验**：加密模式下由 AEAD Tag 保证（GCM/Poly1305）

```c
static inline uint16_t rc_proto_checksum(const rc_proto_header_t *hdr) {
    uint16_t sum = 0;
    const uint16_t *p = (const uint16_t *)hdr;
    for (size_t i = 0; i < RC_PROTO_HEADER_SIZE / 2; i++) {
        sum ^= p[i];  /* XOR-based fast checksum for header */
    }
    return sum;
}
```

### 3.2 连接管理 (rc_conn)

#### 3.2.1 连接状态机

```
                    ┌─────────┐
                    │  INIT   │
                    └────┬────┘
                         │ send SYN
                         ▼
                    ┌─────────┐
              ┌────►│SYN_SENT │
              │     └────┬────┘
              │          │ recv SYN-ACK
              │          ▼
              │     ┌─────────┐
              │     │ESTABLISH│ ◄──── 正常数据传输
              │     └────┬────┘
              │          │ send FIN
              │          ▼
              │     ┌─────────┐
              │     │FIN_SENT │
              │     └────┬────┘
              │          │ recv FIN-ACK
              │          ▼
              │     ┌─────────┐
              │     │ CLOSED  │
              │     └─────────┘
              │
              │ 超时重试
              │ (最多3次)
              │
    ┌─────────┴──┐
    │ SYN_TIMEOUT│
    └────────────┘
```

**状态定义：**

```c
typedef enum {
    RC_CONN_INIT        = 0,   /* 初始状态 */
    RC_CONN_SYN_SENT    = 1,   /* SYN 已发送 */
    RC_CONN_SYN_RECV    = 2,   /* SYN 已接收（服务端） */
    RC_CONN_ESTABLISHED = 3,   /* 已建立连接 */
    RC_CONN_FIN_SENT    = 4,   /* FIN 已发送 */
    RC_CONN_FIN_WAIT    = 5,   /* 等待 FIN-ACK */
    RC_CONN_CLOSED      = 6,   /* 已关闭 */
    RC_CONN_ERROR       = 7,   /* 错误状态 */
} rc_conn_state_t;
```

#### 3.2.2 连接池设计

```c
/* 连接对象 */
typedef struct rc_conn {
    uint64_t        conn_id;          /* 唯一连接ID */
    rc_conn_state_t state;            /* 当前状态 */
    struct sockaddr_storage peer_addr; /* 对端地址 */
    uint32_t        local_seq;        /* 本地序列号 */
    uint32_t        remote_seq;       /* 远程序列号 */
    uint16_t        window_size;      /* 滑动窗口大小 */
    rc_crypto_ctx_t crypto_ctx;       /* 加密上下文 */
    rc_congestion_t congestion;       /* 拥塞控制状态 */
    rc_stream_t    *streams;          /* 多流链表 */
    uint64_t        created_at;       /* 创建时间 */
    uint64_t        last_active;      /* 最后活跃时间 */
    uint32_t        timeout_ms;       /* 超时时间 */
    struct rc_conn *hash_next;        /* 哈希链表 */
} rc_conn_t;

/* 连接池 */
typedef struct rc_conn_pool {
    rc_conn_t     **hash_table;       /* 哈希表 */
    uint32_t        hash_size;        /* 哈希桶数量 */
    rc_conn_t      *free_list;        /* 空闲连接链表 */
    uint32_t        max_conns;        /* 最大连接数 */
    uint32_t        active_conns;     /* 当前活跃连接数 */
    pthread_mutex_t lock;             /* 并发锁 */
} rc_conn_pool_t;
```

#### 3.2.3 哈希表设计

采用**开放寻址法 + 二次探测**，哈希函数使用 MurmurHash3：

```c
#define RC_CONN_HASH(conn_id, size) \
    (murmurhash3(&(conn_id), sizeof(conn_id), 0x9747b28c) % (size))

/* 二次探测 */
#define RC_CONN_PROBE(idx, attempt, size) \
    ((idx + attempt * attempt) % (size))
```

**负载因子控制**：
- 扩容阈值：0.75
- 缩容阈值：0.25
- 初始大小：65536（2^16）
- 最大大小：2^30

### 3.3 安全握手 (rc_handshake)

#### 3.3.1 DTLS-like 握手流程

```
    Client (Robot)                          Server (Control)
         │                                        │
         │──── ClientHello ──────────────────────►│
         │     (random_c, cipher_suites,          │
         │      supported_groups, key_share_c)    │
         │                                        │
         │◄─── ServerHello ──────────────────────│
         │     (random_s, cipher_suite,           │
         │      key_share_s, cert_s)              │
         │                                        │
         │◄─── ServerHelloDone ─────────────────│
         │                                        │
         │     [ECDHE 密钥计算]                    │
         │     shared_secret = ECDHE(priv_c, pub_s)│
         │                                        │
         │──── ClientFinished ─────────────────►│
         │     (verify_data_c, encrypted)         │
         │                                        │
         │◄─── ServerFinished ──────────────────│
         │     (verify_data_s, encrypted)         │
         │                                        │
         │◄═══════════ 加密数据通道 ═════════════►│
         │                                        │
```

#### 3.3.2 ECDHE 密钥交换

```c
/* 密钥交换算法选择 */
typedef enum {
    RC_KEX_X25519    = 0x001D,  /* 首选：Curve25519 */
    RC_KEX_P256      = 0x0017,  /* 备选：NIST P-256 */
    RC_KEX_P384      = 0x0018,  /* 高安全：NIST P-384 */
} rc_kex_algorithm_t;

/* 密钥派生 (HKDF-SHA256) */
typedef struct {
    uint8_t  master_secret[32];      /* 主密钥 */
    uint8_t  client_write_key[32];   /* 客户端写密钥 */
    uint8_t  server_write_key[32];   /* 服务端写密钥 */
    uint8_t  client_write_iv[12];    /* 客户端写IV */
    uint8_t  server_write_iv[12];    /* 服务端写IV */
} rc_key_material_t;

/* 密钥派生函数 */
int rc_handshake_derive_keys(
    const uint8_t *shared_secret, size_t secret_len,
    const uint8_t *random_c, const uint8_t *random_s,
    rc_key_material_t *keys
);
```

#### 3.3.3 握手超时与重试

| 阶段 | 超时时间 | 重试次数 | 说明 |
|------|----------|----------|------|
| ClientHello | 3s | 3 | SYN 级别重试 |
| ServerHello | 5s | 2 | 服务端响应 |
| ClientFinished | 3s | 2 | 握手完成确认 |
| 整体握手 | 15s | - | 超时则连接失败 |

### 3.4 加密引擎 (rc_crypto)

#### 3.4.1 支持的密码套件

```c
typedef enum {
    RC_CIPHER_AES_256_GCM          = 0x0001,  /* 推荐：硬件加速 */
    RC_CIPHER_CHACHA20_POLY1305    = 0x0002,  /* 软件优化 */
    RC_CIPHER_AES_128_GCM          = 0x0003,  /* 轻量级 */
    RC_CIPHER_NONE                 = 0x0000,  /* 调试用 */
} rc_cipher_suite_t;
```

#### 3.4.2 AES-256-GCM 加密流程

```
┌──────────────────────────────────────────────┐
│              AES-256-GCM 加密                  │
│                                              │
│  Plaintext ──► ┌──────────┐ ──► Ciphertext  │
│                │ AES-256  │                  │
│  Key(256-bit) ─┤  GCM     │                  │
│  IV(96-bit)  ──┤          │──► Auth Tag(128b)│
│  AAD ─────────┘──────────┘                  │
│                                              │
│  AAD = Header(20 bytes) + Stream_ID(4 bytes) │
└──────────────────────────────────────────────┘
```

#### 3.4.3 密钥轮换机制

```c
/* 密钥轮换策略 */
typedef struct {
    uint64_t max_bytes;        /* 最大加密字节数: 2^36 (64GB) */
    uint64_t max_duration_ms;  /* 最大使用时间: 12h */
    uint32_t max_packets;      /* 最大报文数: 2^28 */
} rc_key_rotation_policy_t;

/* 轮换触发条件（满足任一即触发） */
#define RC_KEY_ROTATE_MAX_BYTES    (1ULL << 36)  /* 64 GB */
#define RC_KEY_ROTATE_MAX_DURATION (12 * 3600 * 1000)  /* 12 hours */
#define RC_KEY_ROTATE_MAX_PACKETS  (1U << 28)    /* 268M packets */

/* 轮换过程（零停机） */
int rc_crypto_rotate_key(rc_crypto_ctx_t *ctx) {
    /* 1. 生成新密钥材料 */
    /* 2. 发送 KeyUpdate 消息 */
    /* 3. 等待对端确认 */
    /* 4. 切换到新密钥 */
    /* 5. 安全擦除旧密钥 */
}
```

#### 3.4.4 性能优化

```c
/* 硬件加速检测 */
static bool rc_crypto_has_aesni(void) {
    uint32_t eax, ebx, ecx, edx;
    __cpuid(1, eax, ebx, ecx, edx);
    return (ecx & (1 << 25)) != 0;  /* AES-NI */
}

/* 批量加密（SIMD优化） */
int rc_crypto_encrypt_batch(
    rc_crypto_ctx_t *ctx,
    rc_packet_t **pkts, uint32_t count
) {
    if (rc_crypto_has_aesni()) {
        return rc_crypto_aesni_encrypt_batch(ctx, pkts, count);
    }
    return rc_crypto_chacha20_encrypt_batch(ctx, pkts, count);
}
```

### 3.5 拥塞控制 (rc_congestion)

#### 3.5.1 CUBIC 算法详解

```
拥塞窗口 (cwnd)
    ▲
    │          /\        /\
    │         /  \      /  \
    │        /    \    /    \      W_max
    │ ──────/      \  /      \─────────
    │      /        \/        \
    │     /                    \
    │────/                      \──────
    │   ▲               ▲
    │   │               │
    │   TCP-friendly    CUBIC concave region
    │   region          (快速恢复)
    └──────────────────────────────────► 时间

CUBIC 函数: W(t) = C × (t - K)³ + W_max
其中:
  K = ∛(W_max × β / C)
  C = 0.4 (CUBIC 常数)
  β = 0.7 (乘法减少因子)
```

```c
/* CUBIC 拥塞控制状态 */
typedef struct {
    uint32_t cwnd;           /* 当前拥塞窗口 (bytes) */
    uint32_t ssthresh;       /* 慢启动阈值 */
    uint32_t w_max;          /* 上一次丢包时的窗口 */
    uint32_t k;              /* 时间偏移 */
    uint64_t epoch_start;    /* 当前 epoch 开始时间 */
    uint32_t last_max_cwnd;  /* 上一次最大窗口 */
    uint32_t tcp_cwnd;       /* TCP-friendly 窗口 */
    uint32_t rtt_min;        /* 最小 RTT (us) */
    uint32_t rtt_curr;       /* 当前 RTT (us) */
    rc_congestion_algo_t algo;  /* 算法类型 */
} rc_congestion_t;

/* CUBIC 窗口更新 */
void rc_congestion_cubic_update(rc_congestion_t *c, uint64_t now_us) {
    uint64_t t = now_us - c->epoch_start;
    double t_sec = (double)t / 1000000.0;
    double k_sec = (double)c->k / 1000000.0;
    double delta = t_sec - k_sec;
    
    /* W(t) = C × (t - K)³ + W_max */
    double w_cubic = 0.4 * delta * delta * delta + (double)c->w_max;
    
    /* TCP-friendly 窗口 */
    double w_tcp = (double)c->w_max * 0.7 + 3.0 * (1.0 - 0.7) / (1.0 + 0.7) * t_sec * 1460.0 / c->rtt_min;
    
    c->cwnd = (uint32_t)fmax(w_cubic, w_tcp);
}
```

#### 3.5.2 与 TCP BBR 对比

| 特性 | CUBIC | BBR |
|------|-------|-----|
| **算法类型** | 丢包驱动 | 带宽/延迟驱动 |
| **丢包敏感度** | 高（丢包即减窗） | 低（基于测量） |
| **高延迟网络** | 性能下降 | 表现优异 |
| **实现复杂度** | 中等 | 高 |
| **公平性** | 好（与CUBIC公平） | 差（可能抢占CUBIC） |
| **适用场景** | 数据中心、低延迟网络 | 长距离、高延迟网络 |
| **RoboControl选择** | ✅ 默认 | 🔜 未来支持 |

**选择 CUBIC 的原因：**
1. 机器人控制场景主要在局域网/城域网，RTT <10ms，CUBIC 足够
2. CUBIC 实现简单、稳定，经过 15+ 年生产验证
3. 丢包驱动模型对实时控制流更友好（快速响应丢包）
4. BBR 在低 RTT 场景下优势不明显，且公平性问题可能影响其他流量

### 3.6 丢包检测 (rc_retransmit)

#### 3.6.1 SACK 机制

```
发送方报文:  [1] [2] [3] [4] [5] [6] [7] [8]
接收方收到:  [1] [2]     [4] [5]     [7] [8]
                   └─丢失─┘     └─丢失─┘

SACK Block 报告:
  Block 1: [2, 5]  (已收到 2-4)
  Block 2: [7, 8]  (已收到 7-8)

发送方推断:
  丢失: [3], [6] → 立即重传
```

```c
/* SACK 块 */
typedef struct {
    uint32_t left;   /* 左边界（不含） */
    uint32_t right;  /* 右边界（含） */
} rc_sack_block_t;

#define RC_SACK_MAX_BLOCKS  4

/* SACK 信息 */
typedef struct {
    uint32_t        num_blocks;
    rc_sack_block_t blocks[RC_SACK_MAX_BLOCKS];
} rc_sack_t;
```

#### 3.6.2 RTO 计算

基于 RFC 6298 的 RTO 计算，适配微秒精度：

```c
/* RTO 计算状态 */
typedef struct {
    uint32_t srtt;        /* 平滑 RTT (us) */
    uint32_t rttvar;      /* RTT 变异 (us) */
    uint32_t rto;         /* 当前 RTO (us) */
    uint32_t rto_min;     /* 最小 RTO: 100ms */
    uint32_t rto_max;     /* 最大 RTO: 3s */
    bool     srtt_valid;  /* SRTT 是否已初始化 */
} rc_rto_t;

/* RTO 更新 */
void rc_rto_update(rc_rto_t *r, uint32_t measured_rtt) {
    if (!r->srtt_valid) {
        r->srtt = measured_rtt;
        r->rttvar = measured_rtt / 2;
        r->srtt_valid = true;
    } else {
        /* RTTVAR = (1 - β) × RTTVAR + β × |SRTT - R| */
        r->rttvar = (3 * r->rttvar + abs_diff(r->srtt, measured_rtt)) / 4;
        /* SRTT = (1 - α) × SRTT + α × R */
        r->srtt = (7 * r->srtt + measured_rtt) / 8;
    }
    /* RTO = SRTT + max(G, 4 × RTTVAR) */
    r->rto = r->srtt + MAX(1000, 4 * r->rttvar);  /* G = 1ms */
    r->rto = CLAMP(r->rto, r->rto_min, r->rto_max);
}
```

#### 3.6.3 快速重传

```
发送方:  [1] [2] [3] [4] [5] [6] [7] [8]
         ──► ──► ──► ──► ──► ──► ──► ──►
ACK 收到: ACK1 ACK2     ACK4 ACK5 ACK6
                        └─3个重复ACK─┘
                              │
                              ▼
                    触发快速重传 [3]
                    cwnd = cwnd × 0.7
```

### 3.7 P2P 穿透 (rc_p2p)

#### 3.7.1 ICE-lite 流程

```
    Robot (NAT-A)              Signaling              Client (NAT-B)
         │                       │                        │
         │──► STUN Binding ──────┼───────────────────────►│
         │   (获取 reflexive addr)│                        │
         │                       │                        │
         │◄── STUN Response ─────┼───────────────────────┤
         │   (返回 reflexive addr)│                        │
         │                       │                        │
         │──── Candidate ───────►│◄───── Candidate ──────│
         │   (host + reflexive)  │   (host + reflexive)   │
         │                       │                        │
         │◄─── Candidate Pairs ──┼───── Candidate Pairs ─►│
         │                       │                        │
         │═══ Connectivity Check (STUN Binding) ══════════│
         │   (同时尝试所有候选对)   │                        │
         │                       │                        │
         │◄═══ 最优路径建立 ═════════════════════════════►│
         │   (P2P直连 或 Relay)   │                        │
```

#### 3.7.2 NAT 类型检测

```c
typedef enum {
    RC_NAT_UNKNOWN           = 0,
    RC_NAT_NONE              = 1,  /* 无 NAT（公网 IP） */
    RC_NAT_FULL_CONE         = 2,  /* Full Cone NAT */
    RC_NAT_RESTRICTED_CONE   = 3,  /* Address Restricted Cone */
    RC_NAT_PORT_RESTRICTED   = 4,  /* Port Restricted Cone */
    RC_NAT_SYMMETRIC         = 5,  /* Symmetric NAT */
    RC_NAT_BLOCKED           = 6,  /* UDP 被防火墙阻止 */
} rc_nat_type_t;

/* NAT 检测算法（RFC 3489 简化版） */
rc_nat_type_t rc_p2p_detect_nat(rc_p2p_ctx_t *ctx) {
    /* 步骤1：测试本地地址 vs STUN 服务器 */
    /* 步骤2：改变 STUN 服务器 IP 和端口 */
    /* 步骤3：对比两次响应判断 NAT 类型 */
}
```

#### 3.7.3 打洞策略

| NAT-A \ NAT-B | Full Cone | Restricted | Port Restricted | Symmetric |
|----------------|-----------|------------|-----------------|-----------|
| **Full Cone** | ✅ 直连 | ✅ 直连 | ✅ 直连 | ⚠️ 端口预测 |
| **Restricted** | ✅ 直连 | ✅ 直连 | ✅ 直连 | ⚠️ 端口预测 |
| **Port Restricted** | ✅ 直连 | ✅ 直连 | ✅ 直连 | ❌ Relay |
| **Symmetric** | ⚠️ 端口预测 | ⚠️ 端口预测 | ❌ Relay | ❌ Relay |

### 3.8 中继服务 (rc_relay)

#### 3.8.1 会话管理

```c
/* Relay 会话 */
typedef struct rc_relay_session {
    uint64_t    session_id;        /* 会话ID */
    uint64_t    conn_a;            /* 连接A的ID */
    uint64_t    conn_b;            /* 连接B的ID */
    uint64_t    bytes_relayed;     /* 已中继字节数 */
    uint64_t    created_at;        /* 创建时间 */
    uint32_t    bandwidth_limit;   /* 带宽限制 (bytes/s) */
    rc_relay_state_t state;        /* 会话状态 */
    rbtree_node_t rb_node;         /* 红黑树节点（按过期时间排序） */
} rc_relay_session_t;
```

#### 3.8.2 带宽计量

```c
/* 令牌桶限速 */
typedef struct {
    uint64_t tokens;        /* 当前令牌数 */
    uint64_t max_tokens;    /* 最大令牌数 */
    uint64_t rate;          /* 令牌生成速率 (bytes/s) */
    uint64_t last_refill;   /* 上次填充时间 */
} rc_token_bucket_t;

/* 带宽计量器 */
typedef struct {
    rc_token_bucket_t upload;    /* 上行限速 */
    rc_token_bucket_t download;  /* 下行限速 */
    uint64_t          total_tx;  /* 总发送字节 */
    uint64_t          total_rx;  /* 总接收字节 */
} rc_bandwidth_meter_t;
```

#### 3.8.3 多流复用

```
┌─────────────────────────────────────────────────┐
│                Relay 会话                        │
│                                                 │
│  Stream 0: 控制指令 (高优先级, 保证送达)          │
│  Stream 1: 视频流   (中优先级, 可容忍丢包)        │
│  Stream 2: 音频流   (中优先级, 低延迟)            │
│  Stream 3: 状态同步 (低优先级, 定期发送)          │
│                                                 │
│  ┌──────────────────────────────────────────┐   │
│  │         调度器 (Round-Robin + Priority)   │   │
│  │                                          │   │
│  │  Q0 (控制) ──► 100% 带宽保障             │   │
│  │  Q1 (视频) ──►  60% 带宽上限             │   │
│  │  Q2 (音频) ──►  20% 带宽上限             │   │
│  │  Q3 (状态) ──►  10% 带宽上限             │   │
│  └──────────────────────────────────────────┘   │
└─────────────────────────────────────────────────┘
```

### 3.9 事件循环 (rc_epoll)

#### 3.9.1 epoll 优化

```c
/* 高性能事件循环 */
typedef struct rc_event_loop {
    int              epfd;           /* epoll fd */
    struct epoll_event *events;      /* 事件数组 */
    uint32_t         max_events;     /* 最大事件数: 65536 */
    uint32_t         num_fds;        /* 当前监听 fd 数 */
    rc_timer_wheel_t *timer_wheel;   /* 定时器轮 */
    rc_mempool_t     *mempool;       /* 内存池 */
    bool             running;        /* 运行标志 */
} rc_event_loop_t;

/* 初始化（支持 10M fd） */
int rc_epoll_init(rc_event_loop_t *loop, uint32_t max_fds) {
    /* 使用 EPOLLONESHOT 避免惊群 */
    /* 使用 EPOLLET (边缘触发) 减少系统调用 */
    loop->epfd = epoll_create1(EPOLL_CLOEXEC);
    
    /* 预分配事件数组 */
    loop->events = calloc(max_fds, sizeof(struct epoll_event));
    loop->max_events = max_fds;
    
    /* 初始化定时器轮 */
    loop->timer_wheel = rc_timer_wheel_create(1000, 4);  /* 1ms 精度，4 层 */
    
    return 0;
}

/* 主循环 */
void rc_epoll_run(rc_event_loop_t *loop) {
    while (loop->running) {
        int timeout = rc_timer_wheel_next_timeout(loop->timer_wheel);
        int n = epoll_wait(loop->epfd, loop->events, loop->max_events, timeout);
        
        for (int i = 0; i < n; i++) {
            rc_conn_t *conn = loop->events[i].data.ptr;
            if (loop->events[i].events & EPOLLIN) {
                rc_conn_on_readable(conn);
            }
            if (loop->events[i].events & EPOLLOUT) {
                rc_conn_on_writable(conn);
            }
        }
        
        rc_timer_wheel_process(loop->timer_wheel);
    }
}
```

#### 3.9.2 定时器轮 (Hierarchical Timer Wheel)

```
Layer 0 (1ms 精度):   [0] [1] [2] ... [255]     (256 slots = 256ms)
Layer 1 (256ms 精度): [0] [1] [2] ... [255]     (256 slots = 65.536s)
Layer 2 (65.5s 精度): [0] [1] [2] ... [255]     (256 slots = 4.66h)
Layer 3 (4.66h 精度): [0] [1] [2] ... [255]     (256 slots = 49.7d)

总覆盖范围: 256ms × 256 × 256 × 256 ≈ 49.7 天
```

#### 3.9.3 10M fd 支持方案

| 层面 | 配置 | 说明 |
|------|------|------|
| **进程级** | `ulimit -n 10485760` | 文件描述符限制 |
| **内核级** | `fs.file-max = 10485760` | 系统最大 fd |
| **epoll** | `EPOLLONESHOT` | 避免多线程竞争 |
| **内存** | ~80 bytes/conn | 10M × 80B = 800MB |
| **哈希表** | 16M 桶 | 负载因子 ~0.625 |

---

## 4. 千万并发架构设计

### 4.1 单机连接数优化

```
┌─────────────────────────────────────────────────────────────┐
│                    单机 10M fd 架构                           │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              应用层 (多线程 Worker)                    │   │
│  │  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐       │   │
│  │  │Worker 0│ │Worker 1│ │Worker 2│ │Worker N│       │   │
│  │  │(CPU 0) │ │(CPU 1) │ │(CPU 2) │ │(CPU N) │       │   │
│  │  └────────┘ └────────┘ └────────┘ └────────┘       │   │
│  └──────────────────────────────────────────────────────┘   │
│                           │                                  │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              内核层                                    │   │
│  │  ┌─────────────────────────────────────────────────┐ │   │
│  │  │  epoll (per-CPU sharding)                       │ │   │
│  │  │  - EPOLLONESHOT + EPOLLET                       │ │   │
│  │  │  - SO_REUSEPORT (多 fd 绑定同端口)              │ │   │
│  │  └─────────────────────────────────────────────────┘ │   │
│  │  ┌─────────────────────────────────────────────────┐ │   │
│  │  │  内存优化                                       │ │   │
│  │  │  - 连接对象池 (80B/conn)                        │ │   │
│  │  │  - 收发缓冲区池 (2KB/conn)                      │ │   │
│  │  │  - 零拷贝收发 (MSG_ZEROCOPY)                    │ │   │
│  │  └─────────────────────────────────────────────────┘ │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                             │
│  资源估算:                                                   │
│  - 连接对象:  10M × 80B  = 800 MB                          │
│  - 收发缓冲:  10M × 2KB  = 20 GB                           │
│  - 哈希表:    16M × 8B   = 128 MB                          │
│  - 总内存:    ~21 GB                                       │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 水平扩展策略

```
                    ┌──────────────┐
                    │  L4 LB       │
                    │  (DPVS/LVS) │
                    └──────┬───────┘
                           │
            ┌──────────────┼──────────────┐
            ▼              ▼              ▼
    ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
    │  Server 1    │ │  Server 2    │ │  Server N    │
    │  10M conns   │ │  10M conns   │ │  10M conns   │
    │  100Gbps NIC │ │  100Gbps NIC │ │  100Gbps NIC │
    └──────────────┘ └──────────────┘ └──────────────┘

扩展方式:
1. 信令层: 一致性哈希 (device_id % server_count)
2. 数据层: 直接路由 (client ↔ robot 同 server 最优)
3. Relay层: 最近节点调度 (基于 RTT 测量)
```

### 4.3 负载均衡方案

```c
/* 一致性哈希 */
typedef struct rc_consistent_hash {
    rc_node_t   *nodes;         /* 物理节点列表 */
    uint32_t     num_nodes;     /* 节点数量 */
    uint32_t     virtual_nodes; /* 每个物理节点的虚拟节点数: 150 */
    rbtree_t     ring;          /* 哈希环 (红黑树实现) */
} rc_consistent_hash_t;

/* 节点选择 */
rc_node_t *rc_hash_get_node(rc_consistent_hash_t *ch, const uint8_t *key, size_t key_len) {
    uint32_t hash = murmurhash3(key, key_len, 0x12345678);
    /* 在环上顺时针查找最近的虚拟节点 */
    rbtree_node_t *node = rbtree_lower_bound(ch->ring, hash);
    return (rc_node_t *)node->value;
}
```

### 4.4 数据分片策略

| 数据类型 | 分片方式 | 说明 |
|----------|----------|------|
| 连接状态 | 按设备ID哈希 | 保证同一设备路由到同一节点 |
| 会话信息 | 按设备ID哈希 | 同连接状态 |
| 信令消息 | 按设备ID哈希 | 保证顺序 |
| 监控数据 | 按时间分片 | 便于聚合查询 |
| 用户数据 | 按用户ID哈希 | 支持水平扩展 |

---

## 5. 网络拓扑

### 5.1 信令集群设计

```
┌──────────────────────────────────────────────────────────────┐
│                     信令集群架构                               │
│                                                              │
│  ┌─────────────┐   ┌─────────────┐   ┌─────────────┐       │
│  │  Signaling  │   │  Signaling  │   │  Signaling  │       │
│  │  Server 1   │◄─►│  Server 2   │◄─►│  Server N   │       │
│  │  (WS+HTTP)  │   │  (WS+HTTP)  │   │  (WS+HTTP)  │       │
│  └──────┬──────┘   └──────┬──────┘   └──────┬──────┘       │
│         │                  │                  │              │
│         └──────────────────┼──────────────────┘              │
│                            │                                 │
│                    ┌───────┴───────┐                         │
│                    │  Redis Cluster │                         │
│                    │  (Pub/Sub +   │                         │
│                    │   Session)    │                         │
│                    └───────────────┘                         │
└──────────────────────────────────────────────────────────────┘
```

### 5.2 Relay 集群设计

```
┌──────────────────────────────────────────────────────────────┐
│                     Relay 集群架构                             │
│                                                              │
│  华东区域                           华南区域                   │
│  ┌─────────────┐                   ┌─────────────┐           │
│  │  Relay SH-1 │                   │  Relay SZ-1 │           │
│  │  10Gbps     │◄─────── DCI ─────►│  10Gbps     │           │
│  └─────────────┘                   └─────────────┘           │
│  ┌─────────────┐                   ┌─────────────┐           │
│  │  Relay SH-2 │                   │  Relay SZ-2 │           │
│  │  10Gbps     │                   │  10Gbps     │           │
│  └─────────────┘                   └─────────────┘           │
│                                                              │
│  北京区域                           海外区域                   │
│  ┌─────────────┐                   ┌─────────────┐           │
│  │  Relay BJ-1 │                   │  Relay HK-1 │           │
│  │  10Gbps     │                   │  10Gbps     │           │
│  └─────────────┘                   └─────────────┘           │
│                                                              │
│  调度策略:                                                    │
│  1. 测量 client↔relay RTT                                    │
│  2. 选择 RTT 最低的 relay                                    │
│  3. 如果 client 和 robot 同区域，尝试直连                      │
└──────────────────────────────────────────────────────────────┘
```

### 5.3 地理分布式部署

```
                    ┌─────────────────┐
                    │   Global LB     │
                    │  (GeoDNS + Anycast) │
                    └────────┬────────┘
                             │
         ┌───────────────────┼───────────────────┐
         ▼                   ▼                   ▼
  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐
  │  中国区域    │    │  亚太区域    │    │  全球区域    │
  │             │    │             │    │             │
  │ ┌─────────┐ │    │ ┌─────────┐ │    │ ┌─────────┐ │
  │ │上海集群  │ │    │ │东京集群  │ │    │ │美西集群  │ │
  │ │北京集群  │ │    │ │新加坡   │ │    │ │法兰克福  │ │
  │ │深圳集群  │ │    │ │悉尼集群  │ │    │ │圣保罗   │ │
  │ └─────────┘ │    │ └─────────┘ │    │ └─────────┘ │
  └─────────────┘    └─────────────┘    └─────────────┘
```

### 5.4 CDN 集成

```
视频流 CDN 分发:

    Robot ──► Relay ──► Transcoder ──► CDN Origin ──► CDN Edge ──► Client

    - 实时视频：WebRTC-style 分发，边缘节点缓存 I-frame
    - 回放视频：标准 CDN 缓存策略
    - 静态资源：SDK 下载、文档、控制面板
```

---

## 6. 数据流图

### 6.1 控制指令流（低延迟 <50ms）

```
┌──────────┐                                              ┌──────────┐
│  Client  │                                              │  Robot   │
│          │                                              │          │
│ 准备指令  │                                              │          │
│ (1ms)    │                                              │          │
│    │     │                                              │          │
│    ▼     │                                              │          │
│ 序列化    │                                              │          │
│ 加密     │                                              │          │
│ (0.5ms)  │                                              │          │
│    │     │                                              │          │
│    ▼     │                                              │          │
│ 发送UDP  │──── 网络传输 (5-15ms) ──────────────────────►│ 收到UDP   │
│ (0.1ms)  │                                              │ (0.1ms)  │
│          │                                              │    │     │
│          │                                              │    ▼     │
│          │                                              │ 解密     │
│          │                                              │ 反序列化  │
│          │                                              │ (0.5ms)  │
│          │                                              │    │     │
│          │                                              │    ▼     │
│          │                                              │ 执行指令  │
│          │                                              │ (1-5ms)  │
│          │                                              │    │     │
│          │◄──── ACK + 状态回报 (5-15ms) ───────────────│    ▼     │
│ 收到ACK  │                                              │ 完成     │
│ (0.1ms)  │                                              │          │
└──────────┘                                              └──────────┘

总延迟: <50ms (同地域) / <150ms (跨地域)
```

### 6.2 视频流（高带宽 1080p@30fps）

```
┌──────────┐                                              ┌──────────┐
│  Robot   │                                              │  Client  │
│          │                                              │          │
│ 摄像头    │                                              │          │
│ 1080p    │                                              │          │
│ YUV420   │                                              │          │
│    │     │                                              │          │
│    ▼     │                                              │          │
│ H.264    │                                              │          │
│ 编码     │                                              │          │
│ (4ms)    │                                              │          │
│    │     │                                              │          │
│    ▼     │                                              │          │
│ 分片     │                                              │          │
│ 加密     │                                              │          │
│ (1ms)    │                                              │          │
│    │     │                                              │          │
│    ▼     │                                              │          │
│ 发送     │──── UDP 批量发送 (10-30ms) ────────────────►│ 接收     │
│ 2Mbps    │                                              │    │     │
│          │                                              │    ▼     │
│          │                                              │ 解密     │
│          │                                              │ 重组     │
│          │                                              │ (1ms)    │
│          │                                              │    │     │
│          │                                              │    ▼     │
│          │                                              │ H.264    │
│          │                                              │ 解码     │
│          │                                              │ (4ms)    │
│          │                                              │    │     │
│          │                                              │    ▼     │
│          │                                              │ 渲染     │
│          │                                              │ (1ms)    │
└──────────┘                                              └──────────┘

码率: 2-8 Mbps (1080p@30fps)
帧间延迟: <100ms
```

### 6.3 状态同步流

```
┌──────────┐                                              ┌──────────┐
│  Robot   │                                              │  Client  │
│          │                                              │          │
│ 传感器    │                                              │          │
│ 数据采集  │                                              │          │
│ (10Hz)   │                                              │          │
│    │     │                                              │          │
│    ▼     │                                              │          │
│ 压缩     │                                              │          │
│ (delta   │                                              │          │
│ encoding)│                                              │          │
│    │     │                                              │          │
│    ▼     │                                              │          │
│ 发送     │──── UDP ───────────────────────────────────►│ 接收     │
│ (每100ms)│                                              │    │     │
│          │                                              │    ▼     │
│          │                                              │ 状态更新  │
│          │                                              │ UI刷新   │
│          │                                              │          │
└──────────┘                                              └──────────┘

数据量: ~1KB/100ms = 10KB/s
内容: 电量、速度、位置、温度、错误码等
```

---

## 7. 技术选型对比

### 7.1 vs WebRTC

| 维度 | RoboControl | WebRTC |
|------|-------------|--------|
| **协议复杂度** | 轻量，~5K行C代码 | 极重，>500K行C++代码 |
| **握手延迟** | 1-RTT | 2-4 RTT (DTLS + ICE + SDP) |
| **嵌入式支持** | ✅ <200KB SDK | ❌ >10MB，需要完整浏览器栈 |
| **P2P穿透** | 自研 ICE-lite | 标准 ICE (复杂但兼容性好) |
| **视频编码** | 灵活选择 | 强制 VP8/VP9/H.264 |
| **拥塞控制** | CUBIC (可扩展) | GCC (Google拥塞控制) |
| **信令** | 自定义二进制 | SDP (文本，解析开销大) |
| **适用场景** | 嵌入式、机器人、IoT | 浏览器、视频会议 |

**RoboControl 优势：**
1. 极轻量，适合资源受限的嵌入式设备
2. 握手延迟低，适合实时控制场景
3. 协议完全可控，可针对机器人场景深度优化
4. 无浏览器依赖，跨平台一致性好

### 7.2 vs QUIC

| 维度 | RoboControl | QUIC |
|------|-------------|------|
| **传输基础** | UDP (原生) | UDP (原生) |
| **握手** | 1-RTT (简化) | 1-RTT / 0-RTT |
| **拥塞控制** | CUBIC (内置) | 可插拔 (通常 CUBIC/BBR) |
| **多路复用** | Stream ID (简化) | Stream ID (完整) |
| **可靠性** | 可选 (控制指令可靠，视频不可靠) | 默认可靠 |
| **头部压缩** | 无 (二进制已紧凑) | QPACK |
| **实现语言** | C (极简) | 多种 (通常 Rust/C++) |
| **适用场景** | 实时控制、机器人 | Web传输、通用 |

**借鉴 QUIC 的设计：**
1. 握手流程（简化版 TLS 1.3）
2. 连接迁移（Connection ID）
3. 流多路复用
4. 0-RTT 数据发送

### 7.3 vs TCP

| 维度 | RoboControl | TCP |
|------|-------------|-----|
| **传输基础** | UDP | TCP |
| **拥塞控制** | CUBIC (自定义) | CUBIC/BBR (内核) |
| **重传** | 自定义 SACK | 内核 SACK |
| **延迟** | 更低（无内核缓冲） | 较高（Nagle、缓冲） |
| **可控性** | 完全控制 | 受限于内核参数 |
| **可靠性** | 可选 | 必须 |
| **头部开销** | 20 bytes | 20+ bytes |
| **NAT穿透** | ✅ UDP 更容易 | ❌ 困难 |
| **适用场景** | 实时控制、游戏 | 文件传输、Web |

**选择 UDP + 自定义协议栈的原因：**
1. **延迟可控**：绕过内核 TCP 栈，避免 Nagle 算法、延迟 ACK 等引入的延迟
2. **NAT 友好**：UDP 天然比 TCP 更容易穿透 NAT
3. **可靠性可选**：控制指令需要可靠传输，但视频流可以容忍少量丢包
4. **完全可控**：拥塞控制、重传策略、优先级调度等完全由应用层控制
5. **轻量级**：无需维护 TCP 状态机的全部复杂性

---

## 附录

### A. 术语表

| 术语 | 全称 | 说明 |
|------|------|------|
| NAT | Network Address Translation | 网络地址转换 |
| STUN | Session Traversal Utilities for NAT | NAT 穿透工具 |
| ICE | Interactive Connectivity Establishment | 交互式连接建立 |
| DTLS | Datagram Transport Layer Security | 数据报传输层安全 |
| ECDHE | Elliptic Curve Diffie-Hellman Ephemeral | 椭圆曲线临时密钥交换 |
| SACK | Selective Acknowledgment | 选择性确认 |
| RTO | Retransmission Timeout | 重传超时 |
| CUBIC | - | TCP 拥塞控制算法 |
| BBR | Bottleneck Bandwidth and Round-trip propagation time | Google 拥塞控制算法 |
| AEAD | Authenticated Encryption with Associated Data | 认证加密 |
| HKDF | HMAC-based Key Derivation Function | 基于 HMAC 的密钥派生 |

### B. 参考文档

- [RFC 6298 - Computing TCP's Retransmission Timer](https://tools.ietf.org/html/rfc6298)
- [RFC 8312 - CUBIC for TCP](https://tools.ietf.org/html/rfc8312)
- [RFC 8446 - TLS 1.3](https://tools.ietf.org/html/rfc8446)
- [RFC 8489 - STUN](https://tools.ietf.org/html/rfc8489)
- [RFC 8445 - ICE](https://tools.ietf.org/html/rfc8445)
- [RFC 9000 - QUIC](https://tools.ietf.org/html/rfc9000)
