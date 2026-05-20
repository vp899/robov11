# Changelog

All notable changes to RoboControl will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [1.0.0] - 2026-05-16

### Added

#### Core Protocol
- Custom binary protocol with 32-byte packet header
- CUBIC congestion control (RFC 8312)
- SACK-based loss detection and retransmission
- AES-256-GCM and ChaCha20-Poly1305 encryption
- HKDF-SHA256 key derivation
- ECDHE key exchange (X25519)

#### Services
- Signaling server — room management, offer/answer relay (1M connections)
- Relay server — UDP media relay with bandwidth accounting (500K sessions)
- Auth service — HMAC-SHA256 token management, device registration
- Billing service — subscription tiers (Free/Pro/Enterprise), usage tracking

#### Infrastructure
- Docker multi-stage builds for all services
- docker-compose full stack (Redis, Nginx, Prometheus, Grafana)
- Kubernetes deployments with HPA and PDB
- GitHub Actions CI/CD (build, test, Docker push, release)
- Linux system tuning scripts for 10M connections

#### Documentation
- Architecture document with system design
- Deployment guide (bare metal, Docker, Kubernetes)
- API reference (signaling, auth, billing)
- Operations manual
- Product requirements
- Fundraising deck outline
- Protocol specification

#### Testing
- Unit tests for protocol, connection, crypto, congestion
- Integration tests for handshake and data transfer
- Docker-based test environment
