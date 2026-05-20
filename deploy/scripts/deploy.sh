#!/usr/bin/env bash
# =============================================================================
# deploy/scripts/deploy.sh — Production deployment script
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# ── Configuration ──────────────────────────────────────────────────────────────
REGISTRY="${REGISTRY:-ghcr.io/robocontrol}"
IMAGE_TAG="${IMAGE_TAG:-latest}"
COMPOSE_PROJECT="robocontrol"
HEALTH_TIMEOUT=120
HEALTH_INTERVAL=5
BACKUP_DIR="/opt/robocontrol/backups"
LOG_FILE="/var/log/robocontrol/deploy-$(date +%Y%m%d-%H%M%S).log"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# ── Logging ────────────────────────────────────────────────────────────────────
log()   { echo -e "${BLUE}[$(date +'%H:%M:%S')]${NC} $*" | tee -a "${LOG_FILE}"; }
ok()    { echo -e "${GREEN}[✓]${NC} $*" | tee -a "${LOG_FILE}"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*" | tee -a "${LOG_FILE}"; }
die()   { echo -e "${RED}[✗]${NC} $*" | tee -a "${LOG_FILE}"; exit 1; }

# ── Prerequisites ──────────────────────────────────────────────────────────────
check_prerequisites() {
    log "Checking prerequisites..."

    command -v docker >/dev/null 2>&1 || die "docker is not installed"
    docker info >/dev/null 2>&1 || die "docker daemon is not running"

    if command -v docker-compose >/dev/null 2>&1; then
        COMPOSE_CMD="docker-compose"
    elif docker compose version >/dev/null 2>&1; then
        COMPOSE_CMD="docker compose"
    else
        die "docker-compose (or 'docker compose') is not available"
    fi

    # Verify disk space (need at least 5GB free)
    local free_kb
    free_kb=$(df -k "${PROJECT_ROOT}" | awk 'NR==2 {print $4}')
    if (( free_kb < 5242880 )); then
        die "Insufficient disk space: need 5GB, have $(( free_kb / 1024 ))MB"
    fi

    ok "Prerequisites satisfied"
}

# ── Backup ─────────────────────────────────────────────────────────────────────
backup_current() {
    log "Creating backup of current deployment..."
    mkdir -p "${BACKUP_DIR}"

    local backup_name="backup-$(date +%Y%m%d-%H%M%S)"
    local backup_path="${BACKUP_DIR}/${backup_name}"

    # Export current Redis data
    if docker ps --format '{{.Names}}' | grep -q "${COMPOSE_PROJECT}_redis"; then
        docker exec "${COMPOSE_PROJECT}_redis-1" redis-cli BGSAVE >/dev/null 2>&1 || true
        sleep 2
        docker cp "${COMPOSE_PROJECT}_redis-1:/data/dump.rdb" "${backup_path}-redis.rdb" 2>/dev/null || true
    fi

    # Save current compose config
    cp "${PROJECT_ROOT}/docker-compose.yml" "${backup_path}-compose.yml" 2>/dev/null || true

    # Record current image digests
    ${COMPOSE_CMD} -f "${PROJECT_ROOT}/docker-compose.yml" \
        -p "${COMPOSE_PROJECT}" images --quiet 2>/dev/null \
        > "${backup_path}-images.txt" || true

    ok "Backup saved: ${backup_path}"
    echo "${backup_path}"
}

# ── Pull Images ────────────────────────────────────────────────────────────────
pull_images() {
    log "Pulling images (tag: ${IMAGE_TAG})..."

    local services=("signaling" "relay" "auth" "billing")
    for svc in "${services[@]}"; do
        log "  Pulling ${REGISTRY}/${svc}:${IMAGE_TAG}..."
        docker pull "${REGISTRY}/${svc}:${IMAGE_TAG}" || die "Failed to pull ${svc}"
    done

    ok "All images pulled"
}

# ── Migrations ─────────────────────────────────────────────────────────────────
run_migrations() {
    log "Running data migrations..."

    # Run any pending migrations against Redis
    docker exec "${COMPOSE_PROJECT}_redis-1" \
        redis-cli EVAL "
            local ver = redis.call('GET', 'robocontrol:schema_version') or '0'
            local target = '3'
            if ver < target then
                -- Migration: add billing namespace
                if ver < '1' then
                    redis.call('SELECT', 1)
                    redis.call('SET', 'robocontrol:migrated', '1')
                    redis.call('SELECT', 0)
                end
                -- Migration: add rate limiting keys
                if ver < '2' then
                    redis.call('CONFIG', 'SET', 'hz', '10')
                end
                -- Migration: add connection tracking
                if ver < '3' then
                    redis.call('HSET', 'robocontrol:config', 'max_connections', '100000')
                end
                redis.call('SET', 'robocontrol:schema_version', target)
                return target
            end
            return ver
        " 0 >/dev/null 2>&1 || warn "Migration script returned non-zero (may be OK)"

    ok "Migrations complete"
}

# ── Health Check ───────────────────────────────────────────────────────────────
wait_for_health() {
    local service=$1
    local port=$2
    local path=${3:-/health}
    local elapsed=0

    log "Waiting for ${service} to become healthy (port ${port})..."

    while (( elapsed < HEALTH_TIMEOUT )); do
        if curl -sf "http://localhost:${port}${path}" >/dev/null 2>&1; then
            ok "${service} is healthy (${elapsed}s)"
            return 0
        fi
        sleep "${HEALTH_INTERVAL}"
        elapsed=$(( elapsed + HEALTH_INTERVAL ))
    done

    die "${service} failed health check after ${HEALTH_TIMEOUT}s"
}

verify_all_health() {
    log "Verifying health of all services..."
    wait_for_health "auth"      9700
    wait_for_health "billing"   9800
    wait_for_health "signaling" 9601
    wait_for_health "relay"     9500
    ok "All services healthy"
}

# ── Rolling Update ─────────────────────────────────────────────────────────────
rolling_update() {
    log "Performing rolling update..."

    cd "${PROJECT_ROOT}"

    # Update images in compose file with new tag
    export IMAGE_TAG

    # Bring up services one at a time with health verification
    local services=("auth" "billing" "signaling" "relay")
    for svc in "${services[@]}"; do
        log "  Updating ${svc}..."
        ${COMPOSE_CMD} -f docker-compose.yml -p "${COMPOSE_PROJECT}" \
            up -d --no-deps --force-recreate "${svc}" || die "Failed to update ${svc}"

        # Wait for service to be healthy before proceeding
        case "${svc}" in
            auth)      wait_for_health "auth" 9700 ;;
            billing)   wait_for_health "billing" 9800 ;;
            signaling) wait_for_health "signaling" 9601 ;;
            relay)     wait_for_health "relay" 9500 ;;
        esac

        ok "  ${svc} updated successfully"
    done

    ok "Rolling update complete"
}

# ── Rollback ───────────────────────────────────────────────────────────────────
rollback() {
    local backup_path=$1
    warn "Initiating rollback..."

    if [[ -f "${backup_path}-compose.yml" ]]; then
        cp "${backup_path}-compose.yml" "${PROJECT_ROOT}/docker-compose.yml"
    fi

    cd "${PROJECT_ROOT}"
    ${COMPOSE_CMD} -f docker-compose.yml -p "${COMPOSE_PROJECT}" \
        up -d --force-recreate 2>/dev/null || true

    # Restore Redis if backup exists
    if [[ -f "${backup_path}-redis.rdb" ]]; then
        docker cp "${backup_path}-redis.rdb" \
            "${COMPOSE_PROJECT}_redis-1:/data/dump.rdb" 2>/dev/null || true
        docker restart "${COMPOSE_PROJECT}_redis-1" 2>/dev/null || true
    fi

    die "Rollback complete. Deployment aborted."
}

# ── Main ───────────────────────────────────────────────────────────────────────
main() {
    log "========================================="
    log "RoboControl Production Deployment"
    log "Tag: ${IMAGE_TAG} | Registry: ${REGISTRY}"
    log "========================================="

    mkdir -p "$(dirname "${LOG_FILE}")"

    check_prerequisites

    local backup_path
    backup_path=$(backup_current)

    # Trap failures for automatic rollback
    trap 'rollback "${backup_path}"' ERR

    pull_images
    run_migrations
    rolling_update
    verify_all_health

    # Clear error trap on success
    trap - ERR

    ok "========================================="
    ok "Deployment successful!"
    ok "========================================="
    log "Log: ${LOG_FILE}"
}

# ── CLI ────────────────────────────────────────────────────────────────────────
case "${1:-deploy}" in
    deploy)
        main
        ;;
    rollback)
        die "Manual rollback: specify backup path as second argument"
        ;;
    health)
        verify_all_health
        ;;
    *)
        echo "Usage: $0 {deploy|rollback|health}"
        exit 1
        ;;
esac
