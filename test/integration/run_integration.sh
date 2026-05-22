#!/bin/bash
# ─────────────────────────────────────────────────────────────────────
#  run_integration.sh — Integration test orchestrator
#
#  Runs the full 4-process integration test across multiple loss rates.
#  Each test runs for the specified duration (default 180s = 3 min).
#
#  Processes:
#    1. relay_lossy     — UDP relay with configurable packet loss
#    2. signal_server   — TCP signaling server
#    3. robot_sender    — Sends 2× H.264 streams (1 Mbps each)
#    4. remote_receiver — Receives + validates + measures latency
#
#  Usage: ./run_integration.sh [duration_sec] [loss_rates...] [reliable|realtime]
#    Default: 180 seconds, loss rates 0 5 10 20 30, mode=realtime
# ─────────────────────────────────────────────────────────────────────

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINDIR="$SCRIPT_DIR/../../build/integration"
REPORT_DIR="$SCRIPT_DIR/../../build/integration/reports"

# ── Configuration ───────────────────────────────────────────────────
DURATION="${1:-180}"
shift 2>/dev/null || true

# Parse arguments: find the transport mode (last arg if it's reliable/realtime)
TRANSPORT_MODE="realtime"
LOSS_RATES=()

for arg in "$@"; do
    if [ "$arg" = "reliable" ] || [ "$arg" = "realtime" ]; then
        TRANSPORT_MODE="$arg"
    else
        LOSS_RATES+=("$arg")
    fi
done

# Default loss rates if none specified
if [ ${#LOSS_RATES[@]} -eq 0 ]; then
    LOSS_RATES=(0 5 10 20 30)
fi

RELAY_PORT=19600
SIGNAL_PORT=19500
LOCAL_IP="127.0.0.1"

# ── Colors ──────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

log() { echo -e "${BLUE}[TEST]${NC} $*"; }
ok()  { echo -e "${GREEN}[  OK]${NC} $*"; }
warn(){ echo -e "${YELLOW}[WARN]${NC} $*"; }
err() { echo -e "${RED}[FAIL]${NC} $*"; }
mode(){ echo -e "${CYAN}[MODE]${NC} $*"; }

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

# ── Cleanup ─────────────────────────────────────────────────────────
cleanup() {
    log "Cleaning up background processes..."
    for pid in "${PIDS[@]:-}"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done
    for pid in "${PIDS[@]:-}"; do
        if [ -n "$pid" ]; then
            wait "$pid" 2>/dev/null || true
        fi
    done
    for pid in "${PIDS[@]:-}"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
    PIDS=()
}

trap cleanup EXIT INT TERM

# ── Run a single test ───────────────────────────────────────────────
run_single_test() {
    local loss_pct="$1"
    local test_name="${TRANSPORT_MODE}_loss_${loss_pct}pct"
    local test_report_dir="$REPORT_DIR/$test_name"
    mkdir -p "$test_report_dir"

    log "═══════════════════════════════════════════════════════════════"
    log "  Test: $loss_pct% packet loss, ${DURATION}s duration, mode=${TRANSPORT_MODE}"
    log "═══════════════════════════════════════════════════════════════"

    PIDS=()

    # 1. Start relay server with loss
    log "Starting relay server (port $RELAY_PORT, loss=${loss_pct}%)..."
    "$BINDIR/relay_lossy" "$RELAY_PORT" "$loss_pct" \
        > "$test_report_dir/relay.log" 2>&1 &
    PIDS+=($!)
    sleep 0.5

    # 2. Start signaling server
    log "Starting signaling server (port $SIGNAL_PORT)..."
    "$BINDIR/signal_server" "$SIGNAL_PORT" \
        > "$test_report_dir/signal.log" 2>&1 &
    PIDS+=($!)
    sleep 0.5

    # 3. Start remote receiver (background, runs for DURATION seconds)
    log "Starting remote receiver (mode=$TRANSPORT_MODE)..."
    "$BINDIR/remote_receiver" "$LOCAL_IP" "$RELAY_PORT" "$DURATION" \
        "$test_report_dir/remote" "$TRANSPORT_MODE" \
        > "$test_report_dir/remote_stdout.log" 2>&1 &
    PIDS+=($!)
    sleep 0.5

    # 4. Start robot sender (foreground, runs for DURATION seconds)
    log "Starting robot sender (mode=$TRANSPORT_MODE)..."
    "$BINDIR/robot_sender" "$LOCAL_IP" "$RELAY_PORT" "$DURATION" "$TRANSPORT_MODE" \
        > "$test_report_dir/robot.log" 2>&1 &
    PIDS+=($!)

    # Wait for completion
    log "Test running for ${DURATION} seconds..."
    sleep "$DURATION"

    # Give processes time to flush
    sleep 2

    # Stop all processes
    cleanup

    # Wait for files to be written
    sleep 1

    # Check results
    local report_file="$test_report_dir/remote_report.txt"
    if [ -f "$report_file" ]; then
        ok "Test completed. Report: $report_file"
        echo ""
        cat "$report_file"
        echo ""
    else
        warn "Report file not found: $report_file"
        if [ -f "$test_report_dir/remote_stdout.log" ]; then
            echo "--- remote stdout ---"
            cat "$test_report_dir/remote_stdout.log"
        fi
        if [ -f "$test_report_dir/robot.log" ]; then
            echo "--- robot log ---"
            cat "$test_report_dir/robot.log"
        fi
    fi

    log "───────────────────────────────────────────────────────────────"
}

# ── Generate summary ────────────────────────────────────────────────
generate_summary() {
    local summary_file="$REPORT_DIR/summary_${TRANSPORT_MODE}.txt"
    log "Generating summary report: $summary_file"

    {
        echo "═══════════════════════════════════════════════════════════════"
        echo "  RoboControl Integration Test — Summary"
        echo "  Transport Mode: ${TRANSPORT_MODE}"
        echo "═══════════════════════════════════════════════════════════════"
        echo ""
        echo "  Configuration:"
        echo "    Duration per test: ${DURATION} seconds"
        echo "    Loss rates tested: ${LOSS_RATES[*]}%"
        echo "    Streams per robot: 2 × H.264 @ 1 Mbps each"
        echo "    Total bitrate:     2 Mbps (before loss)"
        echo "    Transport mode:    ${TRANSPORT_MODE}"
        echo ""
        echo "  Results:"
        echo "  ─────────────────────────────────────────────────────────────"

        for loss in "${LOSS_RATES[@]}"; do
            local report_file="$REPORT_DIR/${TRANSPORT_MODE}_loss_${loss}pct/remote_report.txt"
            echo ""
            echo "  Loss rate: ${loss}%"
            if [ -f "$report_file" ]; then
                local total_pkts=$(grep "Total packets" "$report_file" | awk '{print $NF}')
                local errors=$(grep "Proto errors" "$report_file" | awk '{print $NF}')
                local mode_used=$(grep "Transport mode:" "$report_file" | awk '{print $NF}')
                echo "    Mode:          ${mode_used:-N/A}"
                echo "    Packets:       ${total_pkts:-N/A}"
                echo "    Errors:        ${errors:-N/A}"

                # Extract per-stream latency from the report file
                for s in 1 2; do
                    local mean_val=$(awk "/─── Stream $s ───/{found=1} found && /Mean:/{gsub(/[^0-9.]/, \"\", \$NF); print \$NF; found=0}" "$report_file" | head -1)
                    local sd_val=$(awk "/─── Stream $s ───/{found=1} found && /Stddev:/{gsub(/[^0-9.]/, \"\", \$NF); print \$NF; found=0}" "$report_file" | head -1)
                    if [ -n "$mean_val" ]; then
                        echo "    Stream $s latency: mean=${mean_val}ms σ=${sd_val}ms"
                    fi
                done

                # Overall latency
                local overall_mean=$(awk "/Overall/{found=1} found && /Mean:/{gsub(/[^0-9.]/, \"\", \$NF); print \$NF; found=0}" "$report_file" | head -1)
                local overall_sd=$(awk "/Overall/{found=1} found && /Stddev:/{gsub(/[^0-9.]/, \"\", \$NF); print \$NF; found=0}" "$report_file" | head -1)
                if [ -n "$overall_mean" ]; then
                    echo "    Overall latency: mean=${overall_mean}ms σ=${overall_sd}ms"
                fi

                # Throughput
                local total_bps=$(awk '/Total:.*Mbps/{for(i=1;i<=NF;i++) if($(i+1)=="Mbps") {gsub(/[^0-9.]/, "", $i); print $i; exit}}' "$report_file")
                if [ -n "$total_bps" ]; then
                    echo "    Throughput:    ${total_bps} Mbps"
                fi
            else
                echo "    (no report available)"
            fi
        done

        echo ""
        echo "═══════════════════════════════════════════════════════════════"
    } | tee "$summary_file"
}

# ── Main ────────────────────────────────────────────────────────────

check_bins
mkdir -p "$REPORT_DIR"

mode "Transport mode: ${TRANSPORT_MODE}"
log "RoboControl Integration Test Suite"
log "Duration: ${DURATION}s per test"
log "Loss rates: ${LOSS_RATES[*]}%"
log "Binaries: $BINDIR"
echo ""

for loss in "${LOSS_RATES[@]}"; do
    run_single_test "$loss"
done

generate_summary

log "All tests completed. Reports in: $REPORT_DIR"
