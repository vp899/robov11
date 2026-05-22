#!/usr/bin/env bash
# =============================================================================
# deploy/scripts/setup-linux.sh — Linux server setup for RoboControl
# Supports Ubuntu 20.04/22.04 and Debian 11/12
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROBOCONTROL_USER="robocontrol"
ROBOCONTROL_GROUP="robocontrol"
INSTALL_DIR="/opt/robocontrol"
CONFIG_DIR="/etc/robocontrol"
LOG_DIR="/var/log/robocontrol"
CERT_DIR="/etc/robocontrol/certs"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log()   { echo -e "${GREEN}[+]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
die()   { echo -e "${RED}[-]${NC} $*"; exit 1; }

# Must run as root
[[ $EUID -eq 0 ]] || die "This script must be run as root"

# ── 1. Install System Dependencies ────────────────────────────────────────────
install_dependencies() {
    log "Installing system dependencies..."

    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y --no-install-recommends \
        build-essential \
        gcc \
        g++ \
        make \
        cmake \
        pkg-config \
        libssl-dev \
        libhiredis-dev \
        libjson-c-dev \
        libcurl4-openssl-dev \
        ca-certificates \
        curl \
        wget \
        gnupg \
        lsb-release \
        git \
        jq \
        htop \
        iotop \
        sysstat \
        net-tools \
        iproute2 \
        iptables \
        nftables \
        fail2ban \
        unattended-upgrades \
        logrotate \
        rsync \
        tmux \
        unzip \
        sqlite3

    # Install Docker
    if ! command -v docker >/dev/null 2>&1; then
        log "Installing Docker..."
        curl -fsSL https://download.docker.com/linux/ubuntu/gpg | gpg --dearmor -o /usr/share/keyrings/docker-archive-keyring.gpg
        echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/docker-archive-keyring.gpg] \
            https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" \
            > /etc/apt/sources.list.d/docker.list
        apt-get update -qq
        apt-get install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin
        systemctl enable docker
        systemctl start docker
    fi

    # Install Docker Compose standalone
    if ! command -v docker-compose >/dev/null 2>&1; then
        local compose_version="v2.23.0"
        curl -fsSL "https://github.com/docker/compose/releases/download/${compose_version}/docker-compose-$(uname -s)-$(uname -m)" \
            -o /usr/local/bin/docker-compose
        chmod +x /usr/local/bin/docker-compose
    fi

    log "Dependencies installed"
}

# ── 2. Create Service User ────────────────────────────────────────────────────
create_service_user() {
    log "Creating service user..."

    if ! getent group "${ROBOCONTROL_GROUP}" >/dev/null; then
        groupadd -r "${ROBOCONTROL_GROUP}"
    fi

    if ! getent passwd "${ROBOCONTROL_USER}" >/dev/null; then
        useradd -r -g "${ROBOCONTROL_GROUP}" \
                -d "${INSTALL_DIR}" \
                -s /sbin/nologin \
                -c "RoboControl service account" \
                "${ROBOCONTROL_USER}"
    fi

    # Add to docker group for container management
    usermod -aG docker "${ROBOCONTROL_USER}" 2>/dev/null || true

    log "Service user created: ${ROBOCONTROL_USER}"
}

# ── 3. Configure System Limits ────────────────────────────────────────────────
configure_system_limits() {
    log "Configuring system limits for 10M connections..."

    # sysctl tuning
    cat > /etc/sysctl.d/99-robocontrol.conf << 'EOF'
# ── RoboControl Network Tuning ──

# Increase max open file descriptors (10M connections)
fs.file-max = 10485760
fs.nr_open = 10485760

# TCP buffer sizes
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216
net.core.rmem_default = 1048576
net.core.wmem_default = 1048576
net.ipv4.tcp_rmem = 4096 1048576 16777216
net.ipv4.tcp_wmem = 4096 1048576 16777216

# Connection tracking
net.netfilter.nf_conntrack_max = 10485760
net.nf_conntrack_max = 10485760

# TCP keepalive (detect dead connections faster)
net.ipv4.tcp_keepalive_time = 60
net.ipv4.tcp_keepalive_intvl = 10
net.ipv4.tcp_keepalive_probes = 6

# TCP backlog and connection handling
net.core.somaxconn = 65535
net.core.netdev_max_backlog = 65535
net.ipv4.tcp_max_syn_backlog = 65535

# TCP reuse and recycling
net.ipv4.tcp_tw_reuse = 1
net.ipv4.tcp_fin_timeout = 15
net.ipv4.tcp_max_tw_buckets = 2000000

# Port range for outgoing connections
net.ipv4.ip_local_port_range = 1024 65535

# SYN flood protection
net.ipv4.tcp_syncookies = 1
net.ipv4.tcp_max_orphans = 262144

# Enable TCP Fast Open
net.ipv4.tcp_fastopen = 3

# Memory overcommit (Redis needs this)
vm.overcommit_memory = 1

# Disable swap for performance
vm.swappiness = 1

# Increase socket buffer size
net.core.optmem_max = 204800
EOF

    sysctl -p /etc/sysctl.d/99-robocontrol.conf >/dev/null 2>&1 || warn "Some sysctl values could not be applied"

    # ulimit configuration
    cat > /etc/security/limits.d/99-robocontrol.conf << EOF
# RoboControl file descriptor limits
${ROBOCONTROL_USER}  soft  nofile  10485760
${ROBOCONTROL_USER}  hard  nofile  10485760
${ROBOCONTROL_USER}  soft  nproc   65535
${ROBOCONTROL_USER}  hard  nproc   65535
*                    soft  nofile  1048576
*                    hard  nofile  10485760
EOF

    # systemd override for service user
    mkdir -p /etc/systemd/system/robocontrol.service.d
    cat > /etc/systemd/system/robocontrol.service.d/limits.conf << EOF
[Service]
LimitNOFILE=10485760
LimitNPROC=65535
LimitCORE=infinity
LimitMEMLOCK=infinity
EOF

    log "System limits configured"
}

# ── 4. Create Directory Structure ─────────────────────────────────────────────
create_directories() {
    log "Creating directory structure..."

    local dirs=(
        "${INSTALL_DIR}/bin"
        "${INSTALL_DIR}/conf"
        "${INSTALL_DIR}/logs"
        "${INSTALL_DIR}/data"
        "${INSTALL_DIR}/backups"
        "${CONFIG_DIR}"
        "${CONFIG_DIR}/certs"
        "${LOG_DIR}"
    )

    for dir in "${dirs[@]}"; do
        mkdir -p "${dir}"
    done

    chown -R "${ROBOCONTROL_USER}:${ROBOCONTROL_GROUP}" \
        "${INSTALL_DIR}" "${LOG_DIR}"

    log "Directory structure created"
}

# ── 5. Configure Firewall ─────────────────────────────────────────────────────
configure_firewall() {
    log "Configuring firewall (nftables)..."

    # Use nftables
    cat > /etc/nftables.conf << 'EOF'
#!/usr/sbin/nft -f

flush ruleset

table inet robocontrol {
    chain input {
        type filter hook input priority 0; policy drop;

        # Allow established/related connections
        ct state established,related accept

        # Allow loopback
        iif "lo" accept

        # Allow ICMP (ping)
        ip protocol icmp accept
        ip6 nexthdr icmpv6 accept

        # SSH (rate-limited)
        tcp dport 22 ct state new limit rate 10/minute accept

        # HTTP/HTTPS (public)
        tcp dport { 80, 443 } accept

        # Relay ports (public for robot connections)
        tcp dport { 9500, 9501 } accept

        # Signaling WebSocket (public)
        tcp dport { 9600, 9601, 9602 } accept

        # Auth service (internal — only from nginx/relay)
        tcp dport 9700 ip saddr { 172.20.0.0/16, 10.0.0.0/8, 192.168.0.0/16 } accept

        # Billing service (internal)
        tcp dport 9800 ip saddr { 172.20.0.0/16, 10.0.0.0/8, 192.168.0.0/16 } accept

        # Prometheus (internal monitoring)
        tcp dport 9090 ip saddr { 172.20.0.0/16, 10.0.0.0/8, 192.168.0.0/16 } accept

        # Grafana (internal dashboards)
        tcp dport 3000 ip saddr { 172.20.0.0/16, 10.0.0.0/8, 192.168.0.0/16 } accept

        # Drop everything else with logging
        limit rate 5/minute log prefix "nftables-dropped: " level warn
        drop
    }

    chain forward {
        type filter hook forward priority 0; policy drop;
        ct state established,related accept
        iifname "docker0" accept
        oifname "docker0" accept
        accept
    }

    chain output {
        type filter hook output priority 0; policy accept;
    }
}
EOF

    # Apply nftables rules
    nft -f /etc/nftables.conf 2>/dev/null || warn "nftables rules could not be applied (may need reboot)"
    systemctl enable nftables 2>/dev/null || true

    log "Firewall configured"
}

# ── 6. Configure Log Rotation ─────────────────────────────────────────────────
configure_logrotate() {
    log "Configuring log rotation..."

    cat > /etc/logrotate.d/robocontrol << EOF
${LOG_DIR}/*.log {
    daily
    rotate 30
    compress
    delaycompress
    notifempty
    missingok
    create 0640 ${ROBOCONTROL_USER} ${ROBOCONTROL_GROUP}
    sharedscripts
    postrotate
        systemctl reload robocontrol-relay 2>/dev/null || true
        systemctl reload robocontrol-signaling 2>/dev/null || true
        systemctl reload robocontrol-auth 2>/dev/null || true
        systemctl reload robocontrol-billing 2>/dev/null || true
    endscript
}

/var/lib/docker/containers/*/*.log {
    daily
    rotate 7
    compress
    delaycompress
    notifempty
    missingok
    copytruncate
    maxsize 100M
}
EOF

    log "Log rotation configured"
}

# ── 7. TLS Certificate Setup ──────────────────────────────────────────────────
setup_tls() {
    log "Setting up TLS certificates..."

    local cert_dir="${CERT_DIR}"
    mkdir -p "${cert_dir}"

    if [[ ! -f "${cert_dir}/server.crt" ]]; then
        log "  Generating self-signed certificate (replace with real cert for production)..."

        openssl req -x509 -nodes -days 365 \
            -newkey rsa:2048 \
            -keyout "${cert_dir}/server.key" \
            -out "${cert_dir}/server.crt" \
            -subj "/C=US/ST=California/L=SanFrancisco/O=RoboControl/CN=robocontrol.example.com" \
            -addext "subjectAltName=DNS:robocontrol.example.com,DNS:*.robocontrol.example.com" \
            2>/dev/null

        chmod 600 "${cert_dir}/server.key"
        chmod 644 "${cert_dir}/server.crt"
        chown -R "${ROBOCONTROL_USER}:${ROBOCONTROL_GROUP}" "${cert_dir}"

        warn "Self-signed certificate generated. Replace with CA-signed cert for production."
    else
        log "  TLS certificates already exist, skipping"
    fi

    log "TLS setup complete"
}

# ── 8. Setup Systemd Services ─────────────────────────────────────────────────
setup_systemd_services() {
    log "Setting up systemd services..."

    # Main service (docker-compose based)
    cat > /etc/systemd/system/robocontrol.service << EOF
[Unit]
Description=RoboControl Platform
Documentation=https://docs.robocontrol.example.com
After=docker.service redis.service
Requires=docker.service
StartLimitIntervalSec=300
StartLimitBurst=5

[Service]
Type=oneshot
RemainAfterExit=yes
User=${ROBOCONTROL_USER}
Group=${ROBOCONTROL_GROUP}
WorkingDirectory=${INSTALL_DIR}
ExecStart=/usr/bin/docker compose -f ${INSTALL_DIR}/docker-compose.yml up -d
ExecStop=/usr/bin/docker compose -f ${INSTALL_DIR}/docker-compose.yml down
ExecReload=/usr/bin/docker compose -f ${INSTALL_DIR}/docker-compose.yml restart
TimeoutStartSec=300
TimeoutStopSec=120
Restart=on-failure
RestartSec=30

[Install]
WantedBy=multi-user.target
EOF

    # Reload systemd
    systemctl daemon-reload
    systemctl enable robocontrol.service

    log "Systemd services configured"
}

# ── 9. Enable Unattended Upgrades ─────────────────────────────────────────────
enable_auto_updates() {
    log "Configuring automatic security updates..."

    cat > /etc/apt/apt.conf.d/50unattended-upgrades << 'EOF'
Unattended-Upgrade::Allowed-Origins {
    "${distro_id}:${distro_codename}";
    "${distro_id}:${distro_codename}-security";
    "${distro_id}ESMApps:${distro_codename}-apps-security";
    "${distro_id}ESM:${distro_codename}-infra-security";
};
Unattended-Upgrade::Package-Blacklist {
};
Unattended-Upgrade::AutoFixInterruptedDpkg "true";
Unattended-Upgrade::MinimalSteps "true";
Unattended-Upgrade::Remove-Unused-Dependencies "true";
Unattended-Upgrade::Automatic-Reboot "false";
EOF

    systemctl enable unattended-upgrades 2>/dev/null || true

    log "Auto-updates configured"
}

# ── 10. Verify Setup ──────────────────────────────────────────────────────────
verify_setup() {
    log "Verifying setup..."

    local errors=0

    # Check sysctl
    if [[ "$(cat /proc/sys/fs/file-max)" -lt 1000000 ]]; then
        warn "fs.file-max is lower than expected"
        (( errors++ )) || true
    fi

    # Check user exists
    if ! id "${ROBOCONTROL_USER}" >/dev/null 2>&1; then
        warn "Service user not found"
        (( errors++ )) || true
    fi

    # Check Docker
    if ! docker info >/dev/null 2>&1; then
        warn "Docker not running"
        (( errors++ )) || true
    fi

    # Check certificates
    if [[ ! -f "${CERT_DIR}/server.crt" ]]; then
        warn "TLS certificate not found"
        (( errors++ )) || true
    fi

    if (( errors > 0 )); then
        warn "Setup complete with ${errors} warnings"
    else
        log "Setup verified successfully"
    fi

    log ""
    log "========================================="
    log "RoboControl server setup complete!"
    log "========================================="
    log ""
    log "Next steps:"
    log "  1. Copy docker-compose.yml and configs to ${INSTALL_DIR}/"
    log "  2. Place real TLS certificates in ${CERT_DIR}/"
    log "  3. Configure secrets in ${CONFIG_DIR}/secrets/"
    log "  4. Start the platform: systemctl start robocontrol"
    log "  5. Check status:       systemctl status robocontrol"
    log ""
}

# ── Main ───────────────────────────────────────────────────────────────────────
main() {
    log "========================================="
    log "RoboControl Linux Server Setup"
    log "========================================="
    log ""

    install_dependencies
    create_service_user
    configure_system_limits
    create_directories
    configure_firewall
    configure_logrotate
    setup_tls
    setup_systemd_services
    enable_auto_updates
    verify_setup
}

main "$@"
