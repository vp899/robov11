# RoboControl

**Production-grade remote robot control platform supporting 10M+ concurrent connections**

[English](#overview) | [中文](#概述)

---

## Overview

RoboControl is a high-performance, real-time communication platform designed for remote robot control at massive scale. Built from the ground up in C, it provides:

- **P2P Direct Connection** — DTLS-like handshake with ECDHE key exchange
- **Relay Fallback** — Automatic relay when NAT traversal fails
- **CUBIC Congestion Control** — Bandwidth-efficient media streaming
- **SACK Retransmission** — TCP-grade reliability on UDP
- **Multi-stream Multiplexing** — Control + N camera feeds per robot
- **End-to-End Encryption** — AES-256-GCM / ChaCha20-Poly1305

## 概述

RoboControl 是一个高性能实时机器人远程控制通讯平台，支持千万级并发连接。

核心特性：
- 自研协议栈（非WebRTC包装），更低延迟、更可控
- P2P穿透 + 智能Relay中继，确保连接可靠性
- CUBIC拥塞控制 + SACK重传，兼顾吞吐与可靠性
- 端到端加密，保障数据安全
- 多路摄像头流复用，支持实时视频监控
- SaaS订阅计费，灵活的商业化方案

## Architecture

```
┌─────────────┐    ┌──────────────┐    ┌─────────────┐
│  Robot SDK  │◄──►│  Signaling   │◄──►│  Client App │
│  (Embedded) │    │  Cluster     │    │  (Web/App)  │
└──────┬──────┘    └──────┬───────┘    └──────┬──────┘
       │                  │                    │
       ▼                  ▼                    ▼
┌──────────────┐   ┌──────────────┐   ┌──────────────┐
│  P2P Direct  │   │  Auth Service│   │  Billing Svc │
│  (DTLS/UDP)  │   │  (Redis)     │   │  (Redis)     │
└──────┬───────┘   └──────────────┘   └──────────────┘
       │ (fallback)
       ▼
┌──────────────┐
│  Relay       │
│  Cluster     │
└──────────────┘
```

## Quick Start

### Prerequisites

- Ubuntu 22.04 LTS
- GCC 11+ (C11 support)
- OpenSSL 3.0+
- Redis 7.0+
- Docker & docker-compose (optional)

### Build from Source

```bash
# Install dependencies
sudo apt update
sudo apt install -y build-essential libssl-dev libhiredis-dev

# Clone and build
git clone https://github.com/your-org/robocontrol.git
cd robocontrol
make all -j$(nproc)

# Run tests
make test
```

### Docker Quick Start

```bash
docker-compose up -d
```

This starts: Redis, Signaling (×2), Relay (×2), Auth (×2), Billing (×2), Nginx, Prometheus, Grafana.

### Verify

```bash
# Health checks
curl http://localhost:9501/health  # Signaling
curl http://localhost:9602/health  # Relay
curl http://localhost:9701/health  # Auth
curl http://localhost:9801/health  # Billing
```

## Project Structure

```
robocontrol/
├── include/          # Header files
│   ├── rc_proto.h    # Protocol definitions
│   ├── rc_conn.h     # Connection manager
│   ├── rc_crypto.h   # Encryption
│   ├── rc_congestion.h # CUBIC
│   ├── rc_epoll.h    # Event loop
│   └── rc_relay.h    # Relay
├── src/
│   ├── core/         # Core library (librobocontrol)
│   ├── signaling/    # Signaling server
│   ├── relay/        # Relay server
│   ├── auth/         # Auth service
│   └── billing/      # Billing service
├── test/             # Unit & integration tests
├── config/           # TOML configuration files
├── deploy/           # Docker, K8s, scripts
├── docs/             # Documentation
└── .github/          # CI/CD workflows
```

## Deployment

See [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md) for:
- Linux server setup with system tuning for 10M connections
- Docker Compose deployment
- Kubernetes deployment
- Production hardening

## API Reference

See [docs/API.md](docs/API.md) for:
- Signaling protocol
- Auth API
- Billing API
- Internal service APIs

## Configuration

All services use TOML configuration files in `config/`:

| File | Service | Port |
|------|---------|------|
| `signaling.toml` | Signaling | 9500/9501 |
| `relay.toml` | Relay | 9600/9601/9602 |
| `auth.toml` | Auth | 9700/9701 |
| `billing.toml` | Billing | 9800/9801 |

## Contributing

1. Fork the repository
2. Create a feature branch
3. Write tests for new functionality
4. Ensure all tests pass (`make test`)
5. Submit a pull request

## License

Apache License 2.0 — see [LICENSE](LICENSE) for details.

## Contact

- Website: https://robocontrol.example.com
- Email: contact@robocontrol.example.com
- Discord: https://discord.gg/robocontrol
