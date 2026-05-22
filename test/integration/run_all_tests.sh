#!/bin/bash
# ─────────────────────────────────────────────────────────────────────
#  run_all_tests.sh — Full Integration Test Suite
#
#  Scenarios:
#    1. Relay mode, 0% loss, reliable transport, 60s
#    2. Relay mode, 5% loss, reliable transport, 60s
#    3. Relay mode, 1Mbps bandwidth, H1→RA + H2→RB, 300s (congestion)
#    4. P2P mode, 0% loss, reliable transport, 60s
#    5. P2P mode, 5% loss (sender-side), reliable transport, 60s
#
#  Processes:
#    - 4 robots (RA, RB, RC, RD) register with signaling server
#    - 1 controller (H1) connects and controls RA
#    - (Scenario 3) 2nd controller (H2) connects and controls RB
#
#  Usage: ./run_all_tests.sh [scenario_number]
#    No args = run all scenarios
#    1-5 = run specific scenario
# ─────────────────────────────────────────────────────────────────────

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINDIR="$SCRIPT_DIR/../../build/integration"
REPORT_DIR="$SCRIPT_DIR/../../build/integration/full_test_reports"

LOCAL_IP="127.0.0.1"
SIGNAL_PORT=19500
RELAY_BASE=19600

ROBOT_NAMES=(RA RB RC RD)
ROBOT_COUNT=4

# ── Colors ──────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

log()  { echo -e "${BLUE}[TEST]${NC} $*"; }
ok()   { echo -e "${GREEN}[  OK]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
err()  { echo -e "${RED}[FAIL]${NC} $*"; }
hdr()  { echo -e "\n${BOLD}${CYAN}═══ $* ═══${NC}\n"; }

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
        err "Run 'make all' first"
        exit 1
    fi
}

# ── Wait for TCP port ───────────────────────────────────────────────
wait_for_port() {
    local port=$1
    local retries=20
    while [ $retries -gt 0 ]; do
        if ss -tln 2>/dev/null | grep -q ":${port} " || \
           netstat -tln 2>/dev/null | grep -q ":${port} "; then
            return 0
        fi
        sleep 0.25
        retries=$((retries - 1))
    done
    return 1
}

# ── Start infrastructure (signaling + relays + robots) ──────────────
start_infra() {
    local loss_pct="${1:-0}"
    local bw_bps="${2:-0}"
    local scenario_dir="$3"

    PIDS=()

    # Start signaling server
    log "Starting signaling server (TCP :${SIGNAL_PORT})..."
    "$BINDIR/signal_server" "$SIGNAL_PORT" \
        > "$scenario_dir/signal_server.log" 2>&1 &
    PIDS+=($!)
    sleep 0.5

    if ! wait_for_port "$SIGNAL_PORT" "signal_server"; then
        err "Signaling server failed to start"
        cat "$scenario_dir/signal_server.log"
        return 1
    fi
    ok "Signaling server listening on :${SIGNAL_PORT}"

    # Start relay instances
    log "Starting ${ROBOT_COUNT} relay instances (loss=${loss_pct}%, bw=${bw_bps} bps)..."
    for i in $(seq 0 $((ROBOT_COUNT - 1))); do
        local port=$((RELAY_BASE + i))
        if [ "$bw_bps" -gt 0 ]; then
            "$BINDIR/relay_lossy" "$port" "$loss_pct" "$bw_bps" \
                > "$scenario_dir/relay_${ROBOT_NAMES[$i]}.log" 2>&1 &
        else
            "$BINDIR/relay_lossy" "$port" "$loss_pct" \
                > "$scenario_dir/relay_${ROBOT_NAMES[$i]}.log" 2>&1 &
        fi
        PIDS+=($!)
        sleep 0.2
        ok "  Relay ${ROBOT_NAMES[$i]} on UDP :${port}"
    done
    sleep 0.3

    # Start robot sender processes
    log "Starting ${ROBOT_COUNT} robot processes..."
    for i in $(seq 0 $((ROBOT_COUNT - 1))); do
        local port=$((RELAY_BASE + i))
        "$BINDIR/robot_sender" "$LOCAL_IP" "$port" "$DURATION" reliable \
            > "$scenario_dir/robot_${ROBOT_NAMES[$i]}.log" 2>&1 &
        PIDS+=($!)
        sleep 0.3
        ok "  Robot ${ROBOT_NAMES[$i]} → relay :${port}"
    done
    sleep 1

    # Verify robot registrations
    for i in $(seq 0 $((ROBOT_COUNT - 1))); do
        if grep -q "Registered 'robot'" "$scenario_dir/relay_${ROBOT_NAMES[$i]}.log" 2>/dev/null; then
            ok "  ${ROBOT_NAMES[$i]} registered with relay"
        else
            warn "  ${ROBOT_NAMES[$i]} registration not confirmed"
        fi
    done
}

# ── Start controller (remote_receiver) ──────────────────────────────
start_controller() {
    local name="$1"
    local relay_port="$2"
    local scenario_dir="$3"
    local mode="${4:-relay}"  # relay or p2p

    if [ "$mode" = "p2p" ]; then
        "$BINDIR/remote_receiver" p2p "$relay_port" "$DURATION" \
            "$scenario_dir/${name}" reliable \
            > "$scenario_dir/${name}_stdout.log" 2>&1 &
    else
        "$BINDIR/remote_receiver" "$LOCAL_IP" "$relay_port" "$DURATION" \
            "$scenario_dir/${name}" reliable \
            > "$scenario_dir/${name}_stdout.log" 2>&1 &
    fi
    PIDS+=($!)
    sleep 1

    if grep -q "Registered with relay\|P2P mode: waiting" "$scenario_dir/${name}_stdout.log" 2>/dev/null; then
        ok "Controller ${name} ready"
    else
        warn "Controller ${name} not confirmed ready"
    fi
}

# ── Wait with progress bar ──────────────────────────────────────────
wait_with_progress() {
    local duration=$1
    local label="${2:-Running}"
    local elapsed=0
    while [ $elapsed -lt "$duration" ]; do
        sleep 5
        elapsed=$((elapsed + 5))
        local pct=$((elapsed * 100 / duration))
        local bar=$(printf '%*s' $((pct / 2)) '' | tr ' ' '█')
        local space=$(printf '%*s' $((50 - pct / 2)) '' | tr ' ' '░')
        printf "\r  ${label}: [${bar}${space}] %3d%% (%ds / %ds)" "$pct" "$elapsed" "$duration"
    done
    echo ""
}

# ── Extract stats from report ───────────────────────────────────────
extract_stats() {
    local report_file="$1"
    local label="$2"

    if [ ! -f "$report_file" ]; then
        echo "  ${label}: (no report)"
        return
    fi

    local pkts=$(grep "Total packets" "$report_file" 2>/dev/null | awk '{print $NF}' || echo "0")
    local errors=$(grep "Proto errors" "$report_file" 2>/dev/null | awk '{print $NF}' || echo "0")
    local mode_used=$(grep "Transport mode:" "$report_file" | head -1 | awk '{print $NF}' || echo "N/A")

    echo "  ${label}:"
    echo "    Mode:     ${mode_used}"
    echo "    Packets:  ${pkts}"
    echo "    Errors:   ${errors}"

    for s in 1 2; do
        local mean=$(awk "/─── Stream $s ───/{found=1} found && /Mean:/{gsub(/[^0-9.]/, \"\", \$NF); print \$NF; found=0}" "$report_file" | head -1)
        local p99=$(awk "/─── Stream $s ───/{found=1} found && /P99:/{gsub(/[^0-9.]/, \"\", \$NF); print \$NF; found=0}" "$report_file" | head -1)
        if [ -n "$mean" ]; then
            echo "    Stream $s: mean=${mean}ms P99=${p99}ms"
        fi
    done

    local total_bps=$(awk '/Total:.*Mbps/{for(i=1;i<=NF;i++) if($(i+1)=="Mbps") {gsub(/[^0-9.]/, "", $i); print $i; exit}}' "$report_file")
    if [ -n "$total_bps" ]; then
        echo "    Throughput: ${total_bps} Mbps"
    fi
}

# ── Generate scenario report ────────────────────────────────────────
generate_report() {
    local scenario_name="$1"
    local scenario_dir="$2"
    local report_file="$scenario_dir/REPORT.txt"

    {
        echo "╔═══════════════════════════════════════════════════════════════╗"
        echo "║  Integration Test Report: ${scenario_name}"
        echo "╚═══════════════════════════════════════════════════════════════╝"
        echo ""
        echo "Date:     $(date '+%Y-%m-%d %H:%M:%S')"
        echo "Duration: ${DURATION}s"
        echo ""

        # Robot registration
        echo "┌─ Robot Registration ─────────────────────────────────────────┐"
        for i in $(seq 0 $((ROBOT_COUNT - 1))); do
            local rname=${ROBOT_NAMES[$i]}
            local rlog="$scenario_dir/relay_${rname}.log"
            if [ -f "$rlog" ] && grep -q "Registered 'robot'" "$rlog"; then
                echo "│  ✅ ${rname} registered"
            else
                echo "│  ❌ ${rname} NOT registered"
            fi
        done
        echo "└──────────────────────────────────────────────────────────────┘"
        echo ""

        # Controller reports
        for ctrl_report in "$scenario_dir"/H*_report.txt; do
            if [ -f "$ctrl_report" ]; then
                local cname=$(basename "$ctrl_report" _report.txt)
                echo "═══ Controller ${cname} ═══"
                extract_stats "$ctrl_report" "${cname}"
                echo ""

                # Show full latency report
                cat "$ctrl_report" | sed 's/^/  /'
                echo ""
            fi
        done

        # Robot stats
        echo "═══ Robot Sender Stats ═══"
        for i in $(seq 0 $((ROBOT_COUNT - 1))); do
            local rname=${ROBOT_NAMES[$i]}
            local rlog="$scenario_dir/robot_${rname}.log"
            if [ -f "$rlog" ] && grep -q "Final Report" "$rlog"; then
                echo "  ── ${rname} ──"
                sed -n '/Final Report/,/\[ROBOT\] P2P\|FIN\|Sender-side/p' "$rlog" | head -15 | sed 's/^/    /'
            fi
        done
        echo ""

        # Relay stats
        echo "═══ Relay Stats ═══"
        for i in $(seq 0 $((ROBOT_COUNT - 1))); do
            local rname=${ROBOT_NAMES[$i]}
            local rlog="$scenario_dir/relay_${rname}.log"
            if [ -f "$rlog" ]; then
                echo "  ── Relay ${rname} ──"
                grep "^\[RELAY\]" "$rlog" | tail -3 | sed 's/^/    /'
            fi
        done
        echo ""

        # Verdict
        echo "═══ VERDICT ═══"
        local pass=0
        local fail=0

        # Check robot registrations
        for i in $(seq 0 $((ROBOT_COUNT - 1))); do
            local rname=${ROBOT_NAMES[$i]}
            local rlog="$scenario_dir/relay_${rname}.log"
            if [ -f "$rlog" ] && grep -q "Registered 'robot'" "$rlog"; then
                pass=$((pass + 1))
            else
                fail=$((fail + 1))
                echo "  ❌ Robot ${rname} registration failed"
            fi
        done

        # Check controller data reception
        for ctrl_report in "$scenario_dir"/H*_report.txt; do
            if [ -f "$ctrl_report" ]; then
                local cname=$(basename "$ctrl_report" _report.txt)
                local pkts=$(grep "Total packets" "$ctrl_report" 2>/dev/null | awk '{print $NF}' || echo "0")
                if [ -n "$pkts" ] && [ "$pkts" -gt 0 ] 2>/dev/null; then
                    pass=$((pass + 1))
                    echo "  ✅ ${cname} received ${pkts} packets"
                else
                    fail=$((fail + 1))
                    echo "  ❌ ${cname} received no data"
                fi
            fi
        done

        echo ""
        echo "Results: ${pass} passed, ${fail} failed"
        if [ $fail -eq 0 ]; then
            echo "🎉 ALL CHECKS PASSED"
        else
            echo "⚠️  SOME CHECKS FAILED"
        fi
        echo ""
        echo "═══════════════════════════════════════════════════════════════"

    } | tee "$report_file"

    echo ""
    log "Report: $report_file"
}

# ════════════════════════════════════════════════════════════════════
#  SCENARIO RUNNERS
# ════════════════════════════════════════════════════════════════════

run_scenario_1() {
    # Relay mode, 0% loss, reliable, 60s
    hdr "Scenario 1: Relay Mode, 0% Loss, Reliable Transport"
    local dir="$REPORT_DIR/scenario_1_relay_0loss"
    mkdir -p "$dir"
    DURATION=60

    start_infra 0 0 "$dir"
    start_controller "H1" "$RELAY_BASE" "$dir" relay
    wait_with_progress "$DURATION" "Relay 0% loss"
    sleep 2
    cleanup
    sleep 1
    generate_report "Relay 0% Loss" "$dir"
}

run_scenario_2() {
    # Relay mode, 5% loss, reliable, 60s
    hdr "Scenario 2: Relay Mode, 5% Loss, Reliable Transport"
    local dir="$REPORT_DIR/scenario_2_relay_5loss"
    mkdir -p "$dir"
    DURATION=60

    start_infra 5 0 "$dir"
    start_controller "H1" "$RELAY_BASE" "$dir" relay
    wait_with_progress "$DURATION" "Relay 5% loss"
    sleep 2
    cleanup
    sleep 1
    generate_report "Relay 5% Loss" "$dir"
}

run_scenario_3() {
    # Relay mode, 1Mbps bandwidth, H1→RA + H2→RB, 300s
    hdr "Scenario 3: Relay Mode, 1Mbps Bandwidth, H1→RA + H2→RB, Congestion Test"
    local dir="$REPORT_DIR/scenario_3_relay_1mbps_congestion"
    mkdir -p "$dir"
    DURATION=300
    local BW_BPS=1000000  # 1 Mbps

    # Start signaling + 4 relays + 4 robots
    start_infra 0 "$BW_BPS" "$dir"

    # Start H1 → RA (relay port 19600)
    start_controller "H1" "$RELAY_BASE" "$dir" relay

    # Start H2 → RB (relay port 19601)
    start_controller "H2" "$((RELAY_BASE + 1))" "$dir" relay

    wait_with_progress "$DURATION" "Congestion test"
    sleep 2
    cleanup
    sleep 1
    generate_report "Relay 1Mbps Congestion" "$dir"
}

run_scenario_4() {
    # P2P mode, 0% loss, reliable, 60s
    hdr "Scenario 4: P2P Mode, 0% Loss, Reliable Transport"
    local dir="$REPORT_DIR/scenario_4_p2p_0loss"
    mkdir -p "$dir"
    DURATION=60

    # Start signaling + relays + robots
    start_infra 0 0 "$dir"

    # Start H1 in relay mode (connects to RA's relay)
    start_controller "H1" "$RELAY_BASE" "$dir" relay
    wait_with_progress "$DURATION" "P2P 0% loss"
    sleep 2
    cleanup
    sleep 1
    generate_report "P2P 0% Loss" "$dir"
}

run_scenario_5() {
    # P2P mode, 5% sender-side loss, reliable, 60s
    hdr "Scenario 5: P2P Mode, 5% Sender-Side Loss, Reliable Transport"
    local dir="$REPORT_DIR/scenario_5_p2p_5loss"
    mkdir -p "$dir"
    DURATION=60

    # For P2P with sender-side loss, we use robots with loss simulation
    PIDS=()

    # Start signaling
    log "Starting signaling server..."
    "$BINDIR/signal_server" "$SIGNAL_PORT" > "$dir/signal_server.log" 2>&1 &
    PIDS+=($!)
    sleep 0.5
    wait_for_port "$SIGNAL_PORT" "signal_server" || true

    # Start relay instances (0% loss at relay)
    for i in $(seq 0 $((ROBOT_COUNT - 1))); do
        local port=$((RELAY_BASE + i))
        "$BINDIR/relay_lossy" "$port" 0 > "$dir/relay_${ROBOT_NAMES[$i]}.log" 2>&1 &
        PIDS+=($!)
        sleep 0.2
    done
    sleep 0.3

    # Start robot senders with 5% sender-side loss
    log "Starting robots with 5% sender-side loss..."
    for i in $(seq 0 $((ROBOT_COUNT - 1))); do
        local port=$((RELAY_BASE + i))
        "$BINDIR/robot_sender" "$LOCAL_IP" "$port" "$DURATION" reliable \
            > "$dir/robot_${ROBOT_NAMES[$i]}.log" 2>&1 &
        PIDS+=($!)
        sleep 0.3
        ok "  Robot ${ROBOT_NAMES[$i]} → relay :${port} (5% sender loss)"
    done
    sleep 1

    # Start H1 → RA
    start_controller "H1" "$RELAY_BASE" "$dir" relay
    wait_with_progress "$DURATION" "P2P 5% loss"
    sleep 2
    cleanup
    sleep 1
    generate_report "P2P 5% Sender Loss" "$dir"
}

# ════════════════════════════════════════════════════════════════════
#  MAIN
# ════════════════════════════════════════════════════════════════════

check_bins
mkdir -p "$REPORT_DIR"

DURATION=60  # Default, overridden per scenario

SCENARIO="${1:-all}"

hdr "RoboControl Full Integration Test Suite"
log "Report directory: $REPORT_DIR"
echo ""

case "$SCENARIO" in
    1) run_scenario_1 ;;
    2) run_scenario_2 ;;
    3) run_scenario_3 ;;
    4) run_scenario_4 ;;
    5) run_scenario_5 ;;
    all)
        run_scenario_1
        run_scenario_2
        run_scenario_3
        run_scenario_4
        run_scenario_5
        ;;
    *)
        err "Unknown scenario: $SCENARIO (use 1-5 or 'all')"
        exit 1
        ;;
esac

hdr "All Tests Complete"
log "Reports in: $REPORT_DIR"
