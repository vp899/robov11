#!/bin/bash
# ──────────────────────────────────────────────────────────────────────
# RoboControl Test Runner
# ──────────────────────────────────────────────────────────────────────
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "═══════════════════════════════════════════════════════════"
echo " RoboControl Test Suite"
echo "═══════════════════════════════════════════════════════════"
echo ""

# Build everything
echo -e "${YELLOW}[1/2] Building...${NC}"
cd "$PROJECT_DIR"
make clean 2>/dev/null || true
make all test -j$(nproc) 2>&1 | tail -5

echo ""
echo -e "${YELLOW}[2/2] Running tests...${NC}"
echo "───────────────────────────────────────────────────────────"

PASSED=0
FAILED=0
TESTS=(
    "test_proto"
    "test_conn"
    "test_crypto"
    "test_congestion"
    "test_integration"
)

for test in "${TESTS[@]}"; do
    echo -n "  Running $test... "
    if "$BUILD_DIR/bin/$test" > /dev/null 2>&1; then
        echo -e "${GREEN}PASS${NC}"
        PASSED=$((PASSED + 1))
    else
        echo -e "${RED}FAIL${NC}"
        echo "    Output:"
        "$BUILD_DIR/bin/$test" 2>&1 | sed 's/^/    /'
        FAILED=$((FAILED + 1))
    fi
done

echo ""
echo "═══════════════════════════════════════════════════════════"
echo -e " Results: ${GREEN}$PASSED passed${NC}, ${RED}$FAILED failed${NC} (${#TESTS[@]} total)"
echo "═══════════════════════════════════════════════════════════"

if [ $FAILED -gt 0 ]; then
    exit 1
fi
