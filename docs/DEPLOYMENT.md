# RoboControl 部署文档

> **版本**: 1.0.0  
> **更新日期**: 2026-05-16  
> **适用环境**: Linux (Ubuntu 22.04 LTS / CentOS 8+)

---

## 目录

1. [环境要求](#1-环境要求)
2. [系统调优](#2-系统调优)
3. [部署步骤](#3-部署步骤)
4. [Docker 部署](#4-docker-部署)
5. [Kubernetes 部署](#5-kubernetes-部署)
6. [运维命令](#6-运维命令)

---

## 1. 环境要求

### 1.1 硬件要求

| 组件 | 最低要求 | 推荐配置 | 说明 |
|------|----------|----------|------|
| **CPU** | 4核 | 8核+ | 推荐 Intel Xeon / AMD EPYC |
| **内存** | 16GB | 32GB+ | 10M 连接需 ~21GB |
| **磁盘** | 100GB SSD | 500GB NVMe SSD | 日志存储 + Redis 持久化 |
| **网卡** | 1Gbps | 10Gbps+ | 推荐 Mellanox / Intel X710 |
| **网卡数** | 1 | 2+ | 业务网卡 + 管理网卡分离 |

### 1.2 软件要求

| 软件 | 版本 | 说明 |
|------|------|------|
| **操作系统** | Ubuntu 22.04 LTS / CentOS 8+ | 推荐 Ubuntu |
| **内核** | 5.15+ | 需要 `epoll`、`SO_REUSEPORT` 支持 |
| **GCC** | 11.0+ | C17 标准支持 |
| **CMake** | 3.20+ | 构建系统 |
| **Redis** | 7.0+ | 会话存储、Pub/Sub |
| **Nginx** | 1.24+ | 反向代理、WebSocket |
| **OpenSSL** | 3.0+ | 加密库 |

### 1.3 网络要求

- 公网 IP（至少 1 个，用于 STUN/Relay）
- UDP 端口范围：10000-60000（可配置）
- TCP 端口：80 (HTTP)、443 (HTTPS)、8080 (WebSocket)
- 防火墙规则允许 UDP 入站

---

## 2. 系统调优

### 2.1 文件描述符限制

```bash
# /etc/security/limits.conf
# 设置用户级文件描述符限制
* soft nofile 10485760
* hard nofile 10485760
root soft nofile 10485760
root hard nofile 10485760

# 设置进程级限制
* soft nproc 655360
* hard nproc 655360
```

```bash
# 立即生效（当前会话）
ulimit -n 10485760
ulimit -u 655360

# 验证
ulimit -a
```

### 2.2 内核参数调优

```bash
# /etc/sysctl.d/99-robocontrol.conf

# ============================================================
# 网络核心参数
# ============================================================

# 最大 socket 监听队列长度
net.core.somaxconn = 65535

# TCP SYN 队列长度
net.ipv4.tcp_max_syn_backlog = 65535

# 网络设备接收队列长度
net.core.netdev_max_backlog = 65535

# 网络设备 budget（每轮处理的包数）
net.core.netdev_budget = 600
net.core.netdev_budget_usecs = 8000

# ============================================================
# 缓冲区参数
# ============================================================

# Socket 接收缓冲区
net.core.rmem_default = 262144
net.core.rmem_max = 16777216

# Socket 发送缓冲区
net.core.wmem_default = 262144
net.core.wmem_max = 16777216

# UDP 缓冲区
net.ipv4.udp_mem = 8192 131072 16777216
net.ipv4.udp_rmem_min = 8192
net.ipv4.udp_wmem_min = 8192

# ============================================================
# TCP 优化（用于信令 WebSocket）
# ============================================================

# TCP 缓冲区
net.ipv4.tcp_rmem = 4096 87380 16777216
net.ipv4.tcp_wmem = 4096 65536 16777216

# TCP keepalive
net.ipv4.tcp_keepalive_time = 600
net.ipv4.tcp_keepalive_intvl = 30
net.ipv4.tcp_keepalive_probes = 3

# TCP 连接复用
net.ipv4.tcp_tw_reuse = 1
net.ipv4.tcp_fin_timeout = 15

# TCP fastopen
net.ipv4.tcp_fastopen = 3

# ============================================================
# 文件系统
# ============================================================

# 系统最大文件描述符
fs.file-max = 10485760

# inotify 限制
fs.inotify.max_user_watches = 524288
fs.inotify.max_user_instances = 512

# ============================================================
# 内存
# ============================================================

# 虚拟内存
vm.max_map_count = 262144
vm.swappiness = 10

# ============================================================
# 连接跟踪（如果使用 iptables）
# ============================================================

# net.netfilter.nf_conntrack_max = 10485760
# net.netfilter.nf_conntrack_udp_timeout = 30
# net.netfilter.nf_conntrack_udp_timeout_stream = 120
```

```bash
# 应用配置
sudo sysctl -p /etc/sysctl.d/99-robocontrol.conf

# 验证关键参数
sysctl net.core.somaxconn
sysctl fs.file-max
```

### 2.3 网卡优化

```bash
# 查看网卡名称
ip link show

# 设置网卡多队列（假设 eth0）
ethtool -L eth0 combined 16

# 启用 RSS（Receive Side Scaling）
ethtool -G eth0 rx 4096 tx 4096

# 启用 UDP GRO (Generic Receive Offload)
ethtool -K eth0 gro on

# 设置 IRQ 亲和性（将网卡中断绑定到不同 CPU）
# 假设 8 个队列，绑定到 CPU 0-7
for i in $(seq 0 7); do
    irq_num=$(grep "eth0-$i" /proc/interrupts | awk '{print $1}' | tr -d ':')
    echo $((1 << $i)) > /proc/irq/$irq_num/smp_affinity
done

# 持久化：写入 /etc/rc.local 或 systemd service
```

### 2.4 HugePages（可选，大内存场景）

```bash
# 预留 4GB HugePages
echo 2048 > /proc/sys/vm/nr_hugepages

# 持久化
echo "vm.nr_hugepages = 2048" >> /etc/sysctl.d/99-robocontrol.conf
```

---

## 3. 部署步骤

### 3.1 安装依赖

#### Ubuntu 22.04

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    libev-dev \
    libhiredis-dev \
    libjson-c-dev \
    libyaml-dev \
    redis-server \
    nginx \
    jq \
    htop \
    iftop \
    nethogs \
    strace \
    ltrace
```

#### CentOS 8+

```bash
sudo dnf groupinstall -y "Development Tools"
sudo dnf install -y \
    cmake3 \
    git \
    openssl-devel \
    libev-devel \
    hiredis-devel \
    json-c-devel \
    libyaml-devel \
    redis \
    nginx \
    jq \
    htop \
    iftop \
    nethogs \
    strace \
    ltrace

# CMake3 别名
sudo alternatives --install /usr/local/bin/cmake cmake /usr/bin/cmake3 20
```

### 3.2 编译项目

```bash
# 克隆代码
git clone https://github.com/robocontrol/robocontrol.git
cd robocontrol

# 创建构建目录
mkdir -p build && cd build

# 配置（Release 模式，启用优化）
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/opt/robocontrol \
    -DENABLE_AESNI=ON \
    -DENABLE_AVX2=ON \
    -DENABLE_TESTS=ON

# 编译（并行编译，使用所有 CPU 核心）
make -j$(nproc)

# 运行测试
ctest --output-on-failure

# 安装
sudo make install
```

**编译选项说明：**

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `CMAKE_BUILD_TYPE` | Release | 构建类型：Debug/Release/RelWithDebInfo |
| `ENABLE_AESNI` | ON | 启用 AES-NI 硬件加速 |
| `ENABLE_AVX2` | ON | 启用 AVX2 SIMD 优化 |
| `ENABLE_TESTS` | ON | 编译单元测试 |
| `MAX_CONNECTIONS` | 10000000 | 最大连接数（编译时常量） |
| `LOG_LEVEL` | INFO | 默认日志级别 |

### 3.3 配置 Redis

```bash
# 编辑 Redis 配置
sudo vim /etc/redis/redis.conf
```

```conf
# /etc/redis/redis.conf

# 绑定地址（仅本地访问）
bind 127.0.0.1

# 端口
port 6379

# 最大内存
maxmemory 8gb

# 内存策略：LRU 淘汰
maxmemory-policy allkeys-lru

# 持久化（可选，会话数据可丢失）
save ""
appendonly no

# 连接数限制
maxclients 10000

# 超时
timeout 300

# TCP keepalive
tcp-keepalive 60

# 日志
loglevel notice
logfile /var/log/redis/redis-server.log
```

```bash
# 启动 Redis
sudo systemctl enable redis-server
sudo systemctl start redis-server

# 验证
redis-cli ping
# 应返回: PONG
```

### 3.4 配置各服务

#### 3.4.1 信令服务配置

```yaml
# /opt/robocontrol/etc/signaling.yaml

server:
  # 监听地址
  listen_addr: "0.0.0.0"
  listen_port: 8080
  
  # WebSocket 路径
  ws_path: "/ws"
  
  # HTTP API 路径
  api_path: "/api/v1"
  
  # Worker 线程数（推荐 CPU 核心数）
  worker_threads: 8
  
  # 最大连接数
  max_connections: 1000000

redis:
  # Redis 连接
  host: "127.0.0.1"
  port: 6379
  db: 0
  password: ""
  
  # 连接池大小
  pool_size: 64
  
  # 超时 (ms)
  connect_timeout: 3000
  command_timeout: 1000

logging:
  level: "info"
  file: "/var/log/robocontrol/signaling.log"
  max_size: "100MB"
  max_files: 10
  format: "json"

tls:
  enabled: true
  cert_file: "/opt/robocontrol/etc/tls/server.crt"
  key_file: "/opt/robocontrol/etc/tls/server.key"
  ca_file: "/opt/robocontrol/etc/tls/ca.crt"
```

#### 3.4.2 Auth 服务配置

```yaml
# /opt/robocontrol/etc/auth.yaml

server:
  listen_addr: "0.0.0.0"
  listen_port: 8081
  worker_threads: 4

redis:
  host: "127.0.0.1"
  port: 6379
  db: 1
  password: ""
  pool_size: 32

jwt:
  # JWT 签名密钥（生产环境请使用强密钥）
  secret: "CHANGE_ME_TO_A_RANDOM_STRING_64_CHARS"
  # Token 有效期
  access_token_ttl: 3600      # 1 hour
  refresh_token_ttl: 2592000  # 30 days
  # 签名算法
  algorithm: "HS256"

rate_limit:
  # 登录限速
  login:
    max_attempts: 5
    window_seconds: 300
  # API 限速
  api:
    max_requests: 1000
    window_seconds: 60
```

#### 3.4.3 Relay 服务配置

```yaml
# /opt/robocontrol/etc/relay.yaml

server:
  # UDP 监听（数据转发）
  udp_listen_addr: "0.0.0.0"
  udp_listen_port: 10000
  udp_port_range: "10000-60000"
  
  # HTTP API（管理接口）
  api_listen_addr: "127.0.0.1"
  api_listen_port: 8082
  
  worker_threads: 8
  max_sessions: 500000

bandwidth:
  # 默认带宽限制 (bytes/s)
  default_upload_limit: 10485760    # 10 MB/s
  default_download_limit: 10485760  # 10 MB/s
  
  # 全局带宽限制
  global_limit: 10737418240  # 10 GB/s

redis:
  host: "127.0.0.1"
  port: 6379
  db: 2
  pool_size: 32

logging:
  level: "info"
  file: "/var/log/robocontrol/relay.log"
```

#### 3.4.4 Billing 服务配置

```yaml
# /opt/robocontrol/etc/billing.yaml

server:
  listen_addr: "0.0.0.0"
  listen_port: 8083
  worker_threads: 2

redis:
  host: "127.0.0.1"
  port: 6379
  db: 3
  pool_size: 16

database:
  # PostgreSQL（账单持久化）
  host: "127.0.0.1"
  port: 5432
  name: "robocontrol_billing"
  user: "robocontrol"
  password: "CHANGE_ME"
  pool_size: 16

billing:
  # 计费周期
  cycle: "monthly"
  # 费率（每 GB 流量）
  bandwidth_rate: 0.1
  # 费率（每连接小时）
  connection_rate: 0.01
```

### 3.5 启动服务

```bash
# 创建 systemd 服务文件
sudo tee /etc/systemd/system/robocontrol-signaling.service << 'EOF'
[Unit]
Description=RoboControl Signaling Server
After=network.target redis.service
Wants=redis.service

[Service]
Type=simple
User=robocontrol
Group=robocontrol
ExecStart=/opt/robocontrol/bin/rc_signaling -c /opt/robocontrol/etc/signaling.yaml
ExecReload=/bin/kill -HUP $MAINPID
Restart=always
RestartSec=5
LimitNOFILE=10485760
LimitNPROC=655360

# 安全加固
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/log/robocontrol

[Install]
WantedBy=multi-user.target
EOF

# 类似地创建其他服务
sudo cp /opt/robocontrol/etc/systemd/*.service /etc/systemd/system/

# 创建日志目录
sudo mkdir -p /var/log/robocontrol
sudo chown robocontrol:robocontrol /var/log/robocontrol

# 创建用户
sudo useradd -r -s /sbin/nologin robocontrol

# 重载 systemd
sudo systemctl daemon-reload

# 启动所有服务
sudo systemctl enable --now robocontrol-signaling
sudo systemctl enable --now robocontrol-auth
sudo systemctl enable --now robocontrol-relay
sudo systemctl enable --now robocontrol-billing

# 查看状态
sudo systemctl status robocontrol-*
```

### 3.6 验证部署

```bash
# 检查服务状态
sudo systemctl status robocontrol-signaling
sudo systemctl status robocontrol-auth
sudo systemctl status robocontrol-relay
sudo systemctl status robocontrol-billing

# 检查端口监听
ss -tlnp | grep -E '8080|8081|8082|8083'
ss -ulnp | grep -E '10000'

# 检查日志
tail -f /var/log/robocontrol/signaling.log

# 健康检查
curl -s http://localhost:8080/health | jq .
curl -s http://localhost:8081/health | jq .

# 测试 WebSocket 连接
# 安装 wscat: npm install -g wscat
wscat -c ws://localhost:8080/ws
# 输入: {"type":"ping"}
# 期望返回: {"type":"pong","timestamp":...}

# 测试认证
curl -s -X POST http://localhost:8081/api/v1/auth/register \
  -H "Content-Type: application/json" \
  -d '{"username":"test","password":"test123","email":"test@example.com"}' | jq .

curl -s -X POST http://localhost:8081/api/v1/auth/token \
  -H "Content-Type: application/json" \
  -d '{"username":"test","password":"test123"}' | jq .
```

### 3.7 配置 Nginx

```nginx
# /etc/nginx/sites-available/robocontrol

upstream signaling_backend {
    least_conn;
    server 127.0.0.1:8080;
    # 多实例时添加更多节点
    # server 127.0.0.1:8084;
    # server 127.0.0.1:8085;
}

upstream auth_backend {
    server 127.0.0.1:8081;
}

upstream billing_backend {
    server 127.0.0.1:8083;
}

# HTTP → HTTPS 重定向
server {
    listen 80;
    server_name api.robocontrol.com;
    return 301 https://$host$request_uri;
}

# 主服务
server {
    listen 443 ssl http2;
    server_name api.robocontrol.com;

    # TLS 配置
    ssl_certificate /etc/letsencrypt/live/api.robocontrol.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/api.robocontrol.com/privkey.pem;
    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_ciphers ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384;
    ssl_prefer_server_ciphers off;
    ssl_session_cache shared:SSL:10m;
    ssl_session_timeout 1d;
    ssl_session_tickets off;

    # 安全头
    add_header Strict-Transport-Security "max-age=63072000" always;
    add_header X-Content-Type-Options nosniff;
    add_header X-Frame-Options DENY;

    # WebSocket 代理
    location /ws {
        proxy_pass http://signaling_backend;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        
        # WebSocket 超时
        proxy_read_timeout 86400s;
        proxy_send_timeout 86400s;
        
        # 缓冲
        proxy_buffering off;
    }

    # Auth API
    location /api/v1/auth/ {
        proxy_pass http://auth_backend;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }

    # Billing API
    location /api/v1/billing/ {
        proxy_pass http://billing_backend;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }

    # 信令 API
    location /api/v1/ {
        proxy_pass http://signaling_backend;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    }

    # 健康检查
    location /health {
        proxy_pass http://signaling_backend;
        access_log off;
    }

    # 访问日志
    access_log /var/log/nginx/robocontrol.access.log;
    error_log /var/log/nginx/robocontrol.error.log;
}
```

```bash
# 启用配置
sudo ln -s /etc/nginx/sites-available/robocontrol /etc/nginx/sites-enabled/
sudo rm -f /etc/nginx/sites-enabled/default

# 测试配置
sudo nginx -t

# 重载 Nginx
sudo systemctl reload nginx
```

### 3.8 配置监控

```bash
# 安装 Prometheus + Grafana（可选，或使用已有监控）
# 这里仅配置 RoboControl 内置 metrics 端点

# 在 signaling.yaml 中启用 metrics
cat >> /opt/robocontrol/etc/signaling.yaml << 'EOF'

metrics:
  enabled: true
  listen_addr: "127.0.0.1"
  listen_port: 9090
  path: "/metrics"
EOF
```

```yaml
# Prometheus 配置 (/etc/prometheus/prometheus.yml)
scrape_configs:
  - job_name: 'robocontrol-signaling'
    static_configs:
      - targets: ['localhost:9090']
    scrape_interval: 15s
  
  - job_name: 'robocontrol-relay'
    static_configs:
      - targets: ['localhost:9091']
    scrape_interval: 15s
```

---

## 4. Docker 部署

### 4.1 Docker Compose 一键部署

```yaml
# docker-compose.yml
version: '3.8'

services:
  # Redis
  redis:
    image: redis:7-alpine
    command: redis-server /usr/local/etc/redis/redis.conf
    volumes:
      - ./config/redis.conf:/usr/local/etc/redis/redis.conf
      - redis_data:/data
    ports:
      - "6379:6379"
    restart: unless-stopped
    healthcheck:
      test: ["CMD", "redis-cli", "ping"]
      interval: 10s
      timeout: 5s
      retries: 3

  # Signaling Server
  signaling:
    build:
      context: .
      dockerfile: docker/Dockerfile.signaling
    ports:
      - "8080:8080"
      - "9090:9090"
    volumes:
      - ./config/signaling.yaml:/etc/robocontrol/signaling.yaml
      - signaling_logs:/var/log/robocontrol
    depends_on:
      redis:
        condition: service_healthy
    restart: unless-stopped
    environment:
      - RC_CONFIG=/etc/robocontrol/signaling.yaml
      - RC_LOG_LEVEL=info
    deploy:
      resources:
        limits:
          cpus: '4'
          memory: 4G
        reservations:
          cpus: '2'
          memory: 2G

  # Auth Service
  auth:
    build:
      context: .
      dockerfile: docker/Dockerfile.auth
    ports:
      - "8081:8081"
    volumes:
      - ./config/auth.yaml:/etc/robocontrol/auth.yaml
    depends_on:
      redis:
        condition: service_healthy
    restart: unless-stopped
    environment:
      - RC_CONFIG=/etc/robocontrol/auth.yaml
    deploy:
      resources:
        limits:
          cpus: '2'
          memory: 1G

  # Relay Server
  relay:
    build:
      context: .
      dockerfile: docker/Dockerfile.relay
    ports:
      - "10000-10100:10000-10100/udp"
      - "8082:8082"
    volumes:
      - ./config/relay.yaml:/etc/robocontrol/relay.yaml
    depends_on:
      redis:
        condition: service_healthy
    restart: unless-stopped
    environment:
      - RC_CONFIG=/etc/robocontrol/relay.yaml
    deploy:
      resources:
        limits:
          cpus: '4'
          memory: 4G

  # Billing Service
  billing:
    build:
      context: .
      dockerfile: docker/Dockerfile.billing
    ports:
      - "8083:8083"
    volumes:
      - ./config/billing.yaml:/etc/robocontrol/billing.yaml
    depends_on:
      redis:
        condition: service_healthy
    restart: unless-stopped
    environment:
      - RC_CONFIG=/etc/robocontrol/billing.yaml
    deploy:
      resources:
        limits:
          cpus: '1'
          memory: 512M

  # Nginx
  nginx:
    image: nginx:1.24-alpine
    ports:
      - "80:80"
      - "443:443"
    volumes:
      - ./config/nginx.conf:/etc/nginx/conf.d/default.conf
      - ./certs:/etc/nginx/certs
    depends_on:
      - signaling
      - auth
      - billing
    restart: unless-stopped

volumes:
  redis_data:
  signaling_logs:
```

```bash
# 一键启动
docker compose up -d

# 查看状态
docker compose ps

# 查看日志
docker compose logs -f signaling

# 停止
docker compose down
```

### 4.2 手动 Docker 部署

```bash
# 构建镜像
docker build -f docker/Dockerfile.signaling -t robocontrol/signaling:latest .
docker build -f docker/Dockerfile.auth -t robocontrol/auth:latest .
docker build -f docker/Dockerfile.relay -t robocontrol/relay:latest .
docker build -f docker/Dockerfile.billing -t robocontrol/billing:latest .

# 运行 Redis
docker run -d --name rc-redis \
  -p 6379:6379 \
  -v redis_data:/data \
  redis:7-alpine

# 运行 Signaling
docker run -d --name rc-signaling \
  -p 8080:8080 \
  --link rc-redis:redis \
  -v $(pwd)/config/signaling.yaml:/etc/robocontrol/signaling.yaml \
  robocontrol/signaling:latest
```

### 4.3 镜像构建

```dockerfile
# docker/Dockerfile.signaling
FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential cmake git libssl-dev libev-dev libhiredis-dev \
    libjson-c-dev libyaml-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .
RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF && \
    make -j$(nproc)

FROM ubuntu:22.04
RUN apt-get update && apt-get install -y \
    libssl3 libev4 libhiredis0.14 libjson-c5 libyaml-0-2 \
    && rm -rf /var/lib/apt/lists/*

RUN useradd -r -s /sbin/nologin robocontrol

COPY --from=builder /build/build/bin/rc_signaling /usr/local/bin/
COPY --from=builder /build/config/signaling.yaml /etc/robocontrol/

USER robocontrol
EXPOSE 8080 9090
CMD ["rc_signaling", "-c", "/etc/robocontrol/signaling.yaml"]
```

---

## 5. Kubernetes 部署

### 5.1 K8s 集群准备

**前提条件：**
- Kubernetes 1.24+
- kubectl 已配置
- Helm 3.0+（可选）
- 集群至少 3 个节点，每个节点 8 核 32GB

```bash
# 创建命名空间
kubectl create namespace robocontrol

# 创建 ConfigMap
kubectl create configmap robocontrol-config \
  --from-file=config/ \
  -n robocontrol

# 创建 Secret
kubectl create secret generic robocontrol-secrets \
  --from-literal=jwt-secret="YOUR_JWT_SECRET" \
  --from-literal=redis-password="YOUR_REDIS_PASSWORD" \
  -n robocontrol
```

### 5.2 部署步骤

```yaml
# k8s/signaling-deployment.yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: signaling
  namespace: robocontrol
  labels:
    app: robocontrol
    component: signaling
spec:
  replicas: 3
  selector:
    matchLabels:
      app: robocontrol
      component: signaling
  template:
    metadata:
      labels:
        app: robocontrol
        component: signaling
    spec:
      containers:
      - name: signaling
        image: robocontrol/signaling:latest
        ports:
        - containerPort: 8080
          name: http
        - containerPort: 9090
          name: metrics
        env:
        - name: RC_CONFIG
          value: "/etc/robocontrol/signaling.yaml"
        - name: RC_LOG_LEVEL
          value: "info"
        resources:
          requests:
            cpu: "2"
            memory: "2Gi"
          limits:
            cpu: "4"
            memory: "4Gi"
        volumeMounts:
        - name: config
          mountPath: /etc/robocontrol
        livenessProbe:
          httpGet:
            path: /health
            port: 8080
          initialDelaySeconds: 10
          periodSeconds: 10
        readinessProbe:
          httpGet:
            path: /health
            port: 8080
          initialDelaySeconds: 5
          periodSeconds: 5
      volumes:
      - name: config
        configMap:
          name: robocontrol-config
---
apiVersion: v1
kind: Service
metadata:
  name: signaling
  namespace: robocontrol
spec:
  selector:
    app: robocontrol
    component: signaling
  ports:
  - name: http
    port: 8080
    targetPort: 8080
  - name: metrics
    port: 9090
    targetPort: 9090
  type: ClusterIP
---
# UDP Relay Service (需要 LoadBalancer 或 NodePort)
apiVersion: v1
kind: Service
metadata:
  name: relay-udp
  namespace: robocontrol
spec:
  selector:
    app: robocontrol
    component: relay
  ports:
  - name: udp
    port: 10000
    targetPort: 10000
    protocol: UDP
  type: LoadBalancer
  externalTrafficPolicy: Local
```

```yaml
# k8s/relay-deployment.yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: relay
  namespace: robocontrol
spec:
  replicas: 3
  selector:
    matchLabels:
      app: robocontrol
      component: relay
  template:
    metadata:
      labels:
        app: robocontrol
        component: relay
    spec:
      containers:
      - name: relay
        image: robocontrol/relay:latest
        ports:
        - containerPort: 10000
          protocol: UDP
          name: udp
        - containerPort: 8082
          name: api
        resources:
          requests:
            cpu: "2"
            memory: "2Gi"
          limits:
            cpu: "4"
            memory: "8Gi"
        securityContext:
          capabilities:
            add:
            - NET_RAW
```

```bash
# 部署所有组件
kubectl apply -f k8s/

# 查看状态
kubectl get pods -n robocontrol
kubectl get svc -n robocontrol

# 查看日志
kubectl logs -f deployment/signaling -n robocontrol

# 扩缩容
kubectl scale deployment signaling --replicas=5 -n robocontrol
```

### 5.3 扩缩容策略

```yaml
# k8s/hpa.yaml
apiVersion: autoscaling/v2
kind: HorizontalPodAutoscaler
metadata:
  name: signaling-hpa
  namespace: robocontrol
spec:
  scaleTargetRef:
    apiVersion: apps/v1
    kind: Deployment
    name: signaling
  minReplicas: 3
  maxReplicas: 20
  metrics:
  - type: Resource
    resource:
      name: cpu
      target:
        type: Utilization
        averageUtilization: 70
  - type: Resource
    resource:
      name: memory
      target:
        type: Utilization
        averageUtilization: 80
  behavior:
    scaleUp:
      stabilizationWindowSeconds: 60
      policies:
      - type: Pods
        value: 2
        periodSeconds: 60
    scaleDown:
      stabilizationWindowSeconds: 300
      policies:
      - type: Pods
        value: 1
        periodSeconds: 120
```

```bash
# 应用 HPA
kubectl apply -f k8s/hpa.yaml

# 查看 HPA 状态
kubectl get hpa -n robocontrol
```

---

## 6. 运维命令

### 6.1 服务管理

```bash
# ========== systemd 管理 ==========

# 启动/停止/重启
sudo systemctl start robocontrol-signaling
sudo systemctl stop robocontrol-signaling
sudo systemctl restart robocontrol-signaling

# 查看状态
sudo systemctl status robocontrol-signaling

# 开机自启
sudo systemctl enable robocontrol-signaling

# 重载配置（不重启）
sudo systemctl reload robocontrol-signaling

# ========== Docker 管理 ==========

# 启动/停止
docker compose up -d
docker compose down

# 重启单个服务
docker compose restart signaling

# 查看状态
docker compose ps

# 查看资源使用
docker stats

# ========== Kubernetes 管理 ==========

# 滚动更新
kubectl set image deployment/signaling signaling=robocontrol/signaling:v1.1.0 -n robocontrol

# 回滚
kubectl rollout undo deployment/signaling -n robocontrol

# 查看历史
kubectl rollout history deployment/signaling -n robocontrol
```

### 6.2 日志查看

```bash
# ========== systemd 日志 ==========

# 实时日志
sudo journalctl -u robocontrol-signaling -f

# 最近 100 行
sudo journalctl -u robocontrol-signaling -n 100

# 按时间筛选
sudo journalctl -u robocontrol-signaling --since "2026-05-16 10:00" --until "2026-05-16 12:00"

# 错误日志
sudo journalctl -u robocontrol-signaling -p err

# ========== 文件日志 ==========

# 实时查看
tail -f /var/log/robocontrol/signaling.log

# 搜索错误
grep -i "error\|fatal\|panic" /var/log/robocontrol/signaling.log

# 按时间筛选
awk '/2026-05-16 10:/' /var/log/robocontrol/signaling.log

# ========== Docker 日志 ==========

docker compose logs -f signaling --tail 100
docker compose logs signaling --since 1h

# ========== Kubernetes 日志 ==========

kubectl logs -f deployment/signaling -n robocontrol --tail 100
kubectl logs deployment/signaling -n robocontrol --since 1h
```

### 6.3 性能监控

```bash
# ========== 连接数监控 ==========

# 查看 UDP 连接数
ss -ulnp | grep robocontrol | wc -l

# 查看 TCP 连接数
ss -tnp | grep robocontrol | wc -l

# 查看详细连接状态
ss -ulnp | grep :10000

# ========== 系统资源 ==========

# CPU 和内存
top -p $(pgrep rc_signaling)

# 网络流量
iftop -i eth0

# 磁盘 I/O
iotop -p $(pgrep rc_signaling)

# 进程级网络
nethogs eth0

# ========== 内置 Metrics ==========

# Prometheus 格式指标
curl -s http://localhost:9090/metrics

# 关键指标
curl -s http://localhost:9090/metrics | grep -E 'rc_connections_active|rc_packets_total|rc_retransmit_total'

# ========== 性能基准 ==========

# 连接建立速率测试
./build/bin/rc_bench_connect -n 100000 -c 1000 -h localhost -p 10000

# 吞吐量测试
./build/bin/rc_bench_throughput -n 1000000 -s 1024 -h localhost -p 10000

# 延迟测试
./build/bin/rc_bench_latency -n 10000 -h localhost -p 10000
```

### 6.4 故障排查

```bash
# ========== 常见问题 ==========

# 1. 服务启动失败 - 检查端口占用
sudo ss -tlnp | grep 8080
sudo lsof -i :8080

# 2. 连接数上不去 - 检查文件描述符限制
cat /proc/$(pgrep rc_signaling)/limits | grep "Max open files"
ls /proc/$(pgrep rc_signaling)/fd | wc -l

# 3. 丢包率高 - 检查网卡队列
ethtool -S eth0 | grep -i drop
cat /proc/net/udp | awk '{print $3}' | sort -rn | head

# 4. Redis 连接失败
redis-cli ping
redis-cli info clients

# 5. 内存泄漏排查
valgrind --leak-check=full ./build/bin/rc_signaling -c config/signaling.yaml

# 6. 核心转储分析
ulimit -c unlimited
# 重启服务，等待 core dump
gdb ./build/bin/rc_signaling /tmp/core.robocontrol.*

# ========== 性能调优 ==========

# 查看 epoll 事件分布
strace -p $(pgrep rc_signaling) -e epoll_wait -c

# 查看系统调用统计
perf stat -p $(pgrep rc_signaling) sleep 10

# CPU 火焰图
perf record -p $(pgrep rc_signaling) -g sleep 30
perf script | stackcollapse-perf.pl | flamegraph.pl > flamegraph.svg
```

---

## 附录

### A. 端口清单

| 服务 | 端口 | 协议 | 说明 |
|------|------|------|------|
| Signaling | 8080 | TCP/WS | WebSocket + HTTP API |
| Auth | 8081 | TCP/HTTP | 认证服务 |
| Relay API | 8082 | TCP/HTTP | Relay 管理接口 |
| Billing | 8083 | TCP/HTTP | 计费服务 |
| Relay Data | 10000-60000 | UDP | 数据转发 |
| Redis | 6379 | TCP | 会话存储 |
| Nginx | 80/443 | TCP/HTTPS | 反向代理 |
| Metrics | 9090 | TCP/HTTP | Prometheus 指标 |

### B. 文件布局

```
/opt/robocontrol/
├── bin/
│   ├── rc_signaling
│   ├── rc_auth
│   ├── rc_relay
│   └── rc_billing
├── etc/
│   ├── signaling.yaml
│   ├── auth.yaml
│   ├── relay.yaml
│   ├── billing.yaml
│   └── tls/
│       ├── server.crt
│       ├── server.key
│       └── ca.crt
├── lib/
│   └── librc.so
└── share/
    └── docs/

/var/log/robocontrol/
├── signaling.log
├── auth.log
├── relay.log
└── billing.log
```

### C. 环境变量

| 变量 | 说明 | 默认值 |
|------|------|--------|
| `RC_CONFIG` | 配置文件路径 | `/opt/robocontrol/etc/signaling.yaml` |
| `RC_LOG_LEVEL` | 日志级别 | `info` |
| `RC_LOG_FILE` | 日志文件路径 | `/var/log/robocontrol/signaling.log` |
| `RC_REDIS_URL` | Redis 连接 URL | `redis://127.0.0.1:6379` |
| `RC_WORKER_THREADS` | Worker 线程数 | CPU 核心数 |
| `RC_MAX_CONNECTIONS` | 最大连接数 | `10000000` |
