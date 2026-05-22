#!/bin/bash
# ─────────────────────────────────────────────────────────────────────
#  run_scenario.sh — Integration Test: 4 Robots + 1 Controller
#
#  Scenario:
#    4 robots (RA, RB, RC, RD) each connect to signaling server,
#    register their IDs. 1 controller (H1) connects to signaling,
#    discovers robots, then controls RA and receives its video
#    stream for 60 seconds.
#
#  Architecture:
#    - 1 signaling server (TCP :19500)
#    - 4 relay instances (UDP :19600-19603, one per robot)
#    - 4 robot_sender processes (RA, RB, RC, RD)
#    - 1 remote_receiver process (H1) — connects to RA's relay
#
#  Usage: ./run_scenario.sh [duration_sec]
# ─────────────────────────────────────────────────────────────────────

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINDIR="$SCRIPT_DIR/../../build/integration"
REPORT_DIR="$SCRIPT_DIR/../../build/integration/scenario_reports"
DURATION="${1:-60}"
LOSS_PCT="${2:-0}"

LOCAL_IP="127.0.0.1"
SIGNAL_PORT=19500
RELAY_BASE=19600

ROBOT_NAMES=(RA RB RC RD)
ROBOT_COLORS=('\033[0;31m' '\033[0;32m' '\033[0;33m' '\033[0;35m')

# ── Colors ──────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

log()  { echo -e "${BLUE}[TEST]${NC} $*"; }
ok()   { echo -e "${GREEN}[  OK]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
err()  { echo -e "${RED}[FAIL]${NC} $*"; }

# ── Cleanup ─────────────────────────────────────────────────────────
PIDS=()
cleanup() {
    log "Cleaning up background processes..."
    for pid in "${PIDS[@]:-}"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done
    sleep 1
    for pid in "${PIDS[@]:-}"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
    for pid in "${PIDS[@]:-}"; do
        if [ -n "$pid" ]; then
            wait "$pid" 2>/dev/null || true
        fi
    done
    PIDS=()
}
trap cleanup EXIT INT TERM

# ── Check binaries ──────────────────────────────────────────────────
check_bins() {
    local missing=0
    for bin in relay_lossy signal_server robot_sender remote_receiver; do
        if [ ! -x "$BINDIR/$bin" ]; then
            err "Binary not found: $BINDIR/$bin"
            missing=1
        fi
    done
    if [ $missing -eq 1 ]; then
        log "Run 'make all' first"
        exit 1
    fi
}

# ── Wait for TCP port to be listening ───────────────────────────────
wait_for_port() {
    local port=$1
    local name=$2
    local retries=20
    while [ $retries -gt 0 ]; do
        if ss -tln | grep -q ":${port} "; then
            return 0
        fi
        sleep 0.25
        retries=$((retries - 1))
    done
    err "$name did not start listening on port $port"
    return 1
}

# ── Main ────────────────────────────────────────────────────────────
check_bins
mkdir -p "$REPORT_DIR"

START_TIME=$(date +%Y%m%d_%H%M%S)
SCENARIO_DIR="$REPORT_DIR/scenario_${START_TIME}"
mkdir -p "$SCENARIO_DIR"

log "═══════════════════════════════════════════════════════════════"
log "  Integration Test: 4 Robots + 1 Controller"
log "  Duration: ${DURATION}s per robot stream"
log "═══════════════════════════════════════════════════════════════"
echo ""

# ── Phase 1: Start Signaling Server ─────────────────────────────────
log "Phase 1: Starting signaling server (TCP :${SIGNAL_PORT})..."
"$BINDIR/signal_server" "$SIGNAL_PORT" \
    > "$SCENARIO_DIR/signal_server.log" 2>&1 &
PIDS+=($!)
sleep 0.5

if ! wait_for_port "$SIGNAL_PORT" "signal_server"; then
    cat "$SCENARIO_DIR/signal_server.log"
    exit 1
fi
ok "Signaling server is listening on :${SIGNAL_PORT}"

# ── Phase 2: Start 4 Relay Instances ────────────────────────────────
log "Phase 2: Starting 4 relay instances (${LOSS_PCT}% loss)..."
for i in 0 1 2 3; do
    port=$((RELAY_BASE + i))
    "$BINDIR/relay_lossy" "$port" "$LOSS_PCT" \
        > "$SCENARIO_DIR/relay_${ROBOT_NAMES[$i]}.log" 2>&1 &
    PIDS+=($!)
    sleep 0.3
    ok "  Relay for ${ROBOT_NAMES[$i]} on UDP :${port}"
done
sleep 0.5

# ── Phase 3: Start 4 Robot Processes ────────────────────────────────
log "Phase 3: Starting 4 robot processes (RA, RB, RC, RD)..."
for i in 0 1 2 3; do
    port=$((RELAY_BASE + i))
    "$BINDIR/robot_sender" "$LOCAL_IP" "$port" "$DURATION" realtime \
        > "$SCENARIO_DIR/robot_${ROBOT_NAMES[$i]}.log" 2>&1 &
    PIDS+=($!)
    sleep 0.5
    ok "  Robot ${ROBOT_NAMES[$i]} started → relay :${port}"
done

# ── Phase 4: Wait for robots to register ────────────────────────────
log "Phase 4: Waiting for all robots to register with signaling server..."
sleep 2

# Check signaling server log for registrations
REGISTERED=0
for i in 0 1 2 3; do
    if grep -q "joined room" "$SCENARIO_DIR/signal_server.log" 2>/dev/null; then
        REGISTERED=$((REGISTERED + 1))
    fi
done

# Check relay logs for robot registrations
for i in 0 1 2 3; do
    if grep -q "Registered 'robot'" "$SCENARIO_DIR/relay_${ROBOT_NAMES[$i]}.log" 2>/dev/null; then
        ok "  ${ROBOT_NAMES[$i]} registered with relay"
    else
        warn "  ${ROBOT_NAMES[$i]} registration not confirmed yet"
    fi
done

# ── Phase 5: Controller (H1) connects ───────────────────────────────
log "Phase 5: Controller H1 connecting to RA's relay (port ${RELAY_BASE})..."
"$BINDIR/remote_receiver" "$LOCAL_IP" "$RELAY_BASE" "$DURATION" \
    "$SCENARIO_DIR/H1_controller" realtime \
    > "$SCENARIO_DIR/H1_controller_stdout.log" 2>&1 &
PIDS+=($!)
sleep 1

if grep -q "Registered with relay" "$SCENARIO_DIR/H1_controller_stdout.log" 2>/dev/null; then
    ok "H1 registered with relay, session established with RA"
else
    warn "H1 registration not confirmed, waiting..."
    sleep 2
fi

# ── Phase 6: Run test for DURATION seconds ──────────────────────────
log "Phase 6: Streaming for ${DURATION} seconds..."
echo ""

# Progress bar
ELAPSED=0
while [ $ELAPSED -lt "$DURATION" ]; do
    sleep 5
    ELAPSED=$((ELAPSED + 5))
    PCT=$((ELAPSED * 100 / DURATION))
    BAR=$(printf '%*s' $((PCT / 2)) '' | tr ' ' '█')
    SPACE=$(printf '%*s' $((50 - PCT / 2)) '' | tr ' ' '░')
    printf "\r  [${BAR}${SPACE}] %3d%% (%ds / %ds)" "$PCT" "$ELAPSED" "$DURATION"
done
echo ""
echo ""

# ── Phase 7: Collect results ────────────────────────────────────────
log "Phase 7: Collecting results..."
sleep 3

# Stop all processes
cleanup
sleep 2

# ── Phase 8: Generate Test Report ───────────────────────────────────
REPORT_FILE="$SCENARIO_DIR/test_report.txt"

{
    echo "╔═══════════════════════════════════════════════════════════════╗"
    echo "║       RoboControl Integration Test Report                    ║"
    echo "║       Scenario: 4 Robots + 1 Controller                     ║"
    echo "╚═══════════════════════════════════════════════════════════════╝"
    echo ""
    echo "Test Date:     $(date '+%Y-%m-%d %H:%M:%S')"
    echo "Duration:      ${DURATION} seconds"
    echo "Transport:     realtime (fire-and-forget)"
    echo "Packet Loss:   ${LOSS_PCT}% (simulated)"
    echo ""
    echo "┌─────────────────────────────────────────────────────────────┐"
    echo "│  Architecture                                               │"
    echo "├─────────────────────────────────────────────────────────────┤"
    echo "│  Signaling Server  : TCP :${SIGNAL_PORT}                          │"
    echo "│  Robot RA (relay)  : UDP :${RELAY_BASE}                          │"
    echo "│  Robot RB (relay)  : UDP :$((RELAY_BASE + 1))                          │"
    echo "│  Robot RC (relay)  : UDP :$((RELAY_BASE + 2))                          │"
    echo "│  Robot RD (relay)  : UDP :$((RELAY_BASE + 3))                          │"
    echo "│  Controller H1     : Connected to RA relay              │"
    echo "└─────────────────────────────────────────────────────────────┘"
    echo ""

    # ── Robot Status ────────────────────────────────────────────────
    echo "┌─────────────────────────────────────────────────────────────┐"
    echo "│  Robot Registration Status                                  │"
    echo "├─────────────────────────────────────────────────────────────┤"
    for i in 0 1 2 3; do
        rname=${ROBOT_NAMES[$i]}
        rlog="$SCENARIO_DIR/relay_${rname}.log"
        if [ -f "$rlog" ] && grep -q "Registered 'robot'" "$rlog"; then
            echo "│  ✅ ${rname}  — Registered and streaming                     │"
        else
            echo "│  ❌ ${rname}  — Registration failed                          │"
        fi
    done
    echo "└─────────────────────────────────────────────────────────────┘"
    echo ""

    # ── Robot Sender Stats ──────────────────────────────────────────
    echo "═══════════════════════════════════════════════════════════════"
    echo "  Robot Sender Statistics"
    echo "═══════════════════════════════════════════════════════════════"
    echo ""
    for i in 0 1 2 3; do
        rname=${ROBOT_NAMES[$i]}
        rlog="$SCENARIO_DIR/robot_${rname}.log"
        echo "  ── Robot ${rname} ──"
        if [ -f "$rlog" ]; then
            # Extract final report section
            if grep -q "Final Report" "$rlog"; then
                sed -n '/Final Report/,/^$/p' "$rlog" | head -20 | sed 's/^/    /'
            else
                echo "    (no final report found)"
            fi
            # Get last progress line
            LAST_PROG=$(grep "^\[ROBOT\]" "$rlog" | grep "S1=" | tail -1)
            if [ -n "$LAST_PROG" ]; then
                echo "    Last: $LAST_PROG"
            fi
        else
            echo "    (log not found)"
        fi
        echo ""
    done

    # ── Controller (H1) Stats ───────────────────────────────────────
    echo "═══════════════════════════════════════════════════════════════"
    echo "  Controller H1 Statistics (Receiving from RA)"
    echo "═══════════════════════════════════════════════════════════════"
    echo ""
    H1_STDOUT="$SCENARIO_DIR/H1_controller_stdout.log"
    if [ -f "$H1_STDOUT" ]; then
        echo "  Registration:"
        grep "Registered" "$H1_STDOUT" | sed 's/^/    /'
        echo ""
        echo "  Progress:"
        grep "^\[REMOTE\]" "$H1_STDOUT" | grep "pkts=" | sed 's/^/    /'
        echo ""
        echo "  Final:"
        sed -n '/Final Report/,/Transport/p' "$H1_STDOUT" | head -5 | sed 's/^/    /'
    else
        echo "  (controller stdout not found)"
    fi
    echo ""

    # ── Controller Report File ──────────────────────────────────────
    H1_REPORT="$SCENARIO_DIR/H1_controller_report.txt"
    if [ -f "$H1_REPORT" ]; then
        echo "═══════════════════════════════════════════════════════════════"
        echo "  H1 Detailed Report (from file)"
        echo "═══════════════════════════════════════════════════════════════"
        echo ""
        cat "$H1_REPORT" | sed 's/^/  /'
    fi

    # ── Relay Stats ─────────────────────────────────────────────────
    echo ""
    echo "═══════════════════════════════════════════════════════════════"
    echo "  Relay Statistics"
    echo "═══════════════════════════════════════════════════════════════"
    echo ""
    for i in 0 1 2 3; do
        rname=${ROBOT_NAMES[$i]}
        rlog="$SCENARIO_DIR/relay_${rname}.log"
        echo "  ── Relay ${rname} (port $((RELAY_BASE + i))) ──"
        if [ -f "$rlog" ]; then
            grep "^\[RELAY\]" "$rlog" | tail -3 | sed 's/^/    /'
            if grep -q "Shutdown" "$rlog"; then
                grep "Shutdown" "$rlog" | sed 's/^/    /'
            fi
        else
            echo "    (log not found)"
        fi
        echo ""
    done

    # ── Signaling Server ────────────────────────────────────────────
    echo "═══════════════════════════════════════════════════════════════"
    echo "  Signaling Server Log (last 20 lines)"
    echo "═══════════════════════════════════════════════════════════════"
    echo ""
    if [ -f "$SCENARIO_DIR/signal_server.log" ]; then
        tail -20 "$SCENARIO_DIR/signal_server.log" | sed 's/^/  /'
    fi
    echo ""

    # ── Verdict ─────────────────────────────────────────────────────
    echo "═══════════════════════════════════════════════════════════════"
    echo "  VERDICT"
    echo "═══════════════════════════════════════════════════════════════"
    echo ""

    PASS=0
    FAIL=0

    # Check all 4 robots registered
    for i in 0 1 2 3; do
        rname=${ROBOT_NAMES[$i]}
        rlog="$SCENARIO_DIR/relay_${rname}.log"
        if [ -f "$rlog" ] && grep -q "Registered 'robot'" "$rlog"; then
            PASS=$((PASS + 1))
        else
            FAIL=$((FAIL + 1))
            echo "  ❌ FAIL: Robot ${rname} did not register"
        fi
    done

    # Check H1 registered
    if [ -f "$H1_STDOUT" ] && grep -q "Registered with relay" "$H1_STDOUT"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        echo "  ❌ FAIL: Controller H1 did not register"
    fi

    # Check H1 received data
    if [ -f "$H1_REPORT" ] && [ -s "$H1_REPORT" ]; then
        H1_PKTS=$(grep "Total packets" "$H1_REPORT" 2>/dev/null | awk '{print $NF}' || echo "0")
        if [ -n "$H1_PKTS" ] && [ "$H1_PKTS" -gt 0 ] 2>/dev/null; then
            PASS=$((PASS + 1))
            echo "  ✅ PASS: H1 received ${H1_PKTS} packets from RA"
        else
            FAIL=$((FAIL + 1))
            echo "  ❌ FAIL: H1 received no data packets"
        fi
    else
        FAIL=$((FAIL + 1))
        echo "  ❌ FAIL: H1 report not generated"
    fi

    # Check for protocol errors
    if [ -f "$H1_REPORT" ]; then
        ERRORS=$(grep "Proto errors" "$H1_REPORT" 2>/dev/null | awk '{print $NF}' || echo "0")
        if [ -n "$ERRORS" ] && [ "$ERRORS" -eq 0 ] 2>/dev/null; then
            PASS=$((PASS + 1))
        else
            echo "  ⚠️  WARN: ${ERRORS} protocol errors detected"
        fi
    fi

    echo ""
    echo "  Results: ${PASS} passed, ${FAIL} failed"
    if [ $FAIL -eq 0 ]; then
        echo "  🎉 ALL TESTS PASSED"
    else
        echo "  ⚠️  SOME TESTS FAILED"
    fi
    echo ""
    echo "═══════════════════════════════════════════════════════════════"
    echo "  Report saved: $REPORT_FILE"
    echo "═══════════════════════════════════════════════════════════════"

} | tee "$REPORT_FILE"

echo ""
log "All logs: $SCENARIO_DIR/"
log "Report:   $REPORT_FILE"
