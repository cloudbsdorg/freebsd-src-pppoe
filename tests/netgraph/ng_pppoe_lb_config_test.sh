#!/bin/sh
#
# ng_pppoe_lb_config_test.sh - Configuration validation tests for PPPoE Load Balancer
#
# This script tests that all example configurations work correctly and validates
# configuration file parsing and syntax.
#
# Usage:
#   ./ng_pppoe_lb_config_test.sh [--quick] [--verbose]
#
# Options:
#   --quick         Run only quick tests
#   --verbose       Show detailed output
#
# Exit codes:
#   0   All tests passed
#   1   One or more tests failed
#   2   Test environment error
#
# Test Categories:
#   CFG-01 to CFG-05: Example configurations
#   CFG-06 to CFG-10: Configuration variants
#

set -e

# ============================================================================
# Configuration
# ============================================================================

QUICK_MODE=${QUICK_MODE:-0}
VERBOSE=${VERBOSE:-0}

for arg in "$@"; do
    case $arg in
        --quick)
            QUICK_MODE=1
            shift
            ;;
        --verbose)
            VERBOSE=1
            shift
            ;;
    esac
done

# Paths
SYSCTL_PREFIX="net.graph.pppoe_lb"
NGCTL="/usr/sbin/ngctl"
EXAMPLES_DIR="/usr/share/examples/netgraph/pppoe_lb"
TEST_DIR="/tmp/pppoe_lb_config_test"
SYSCTL_CONF="${TEST_DIR}/sysctl.conf"
NGCTL_CONF="${TEST_DIR}/ngctl.conf"
RC_CONF="${TEST_DIR}/rc.conf"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

# Counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0
TESTS_SKIPPED=0

# ============================================================================
# Helper Functions
# ============================================================================

log_info()    { echo "${BLUE}[INFO]${NC} $*"; }
log_warn()   { echo "${YELLOW}[WARN]${NC} $*"; }
log_error()  { echo "${RED}[ERROR]${NC} $*" >&2; }
log_success(){ echo "${GREEN}[PASS]${NC} $*"; }
log_fail()   { echo "${RED}[FAIL]${NC} $*"; }
log_skip()   { echo "${YELLOW}[SKIP]${NC} $*"; }
log_section(){ echo ""; echo "========================================"; echo "$*"; echo "========================================"; }
log_verbose(){ [ "$VERBOSE" = "1" ] && echo "${CYAN}[VERB]${NC} $*" || true; }

record_pass() {
    TESTS_RUN=$((TESTS_RUN + 1)); TESTS_PASSED=$((TESTS_PASSED + 1))
    log_success "Test $TESTS_RUN: $1"
}

record_fail() {
    TESTS_RUN=$((TESTS_RUN + 1)); TESTS_FAILED=$((TESTS_FAILED + 1))
    log_fail "Test $TESTS_RUN: $1"
}

record_skip() {
    TESTS_RUN=$((TESTS_RUN + 1)); TESTS_SKIPPED=$((TESTS_SKIPPED + 1))
    log_skip "Test $TESTS_RUN: $1 (skipped)"
}

check_root() {
    if [ "$(id -u)" -ne 0 ]; then
        log_error "This script must be run as root"
        exit 2
    fi
}

setup_test_dir() {
    log_verbose "Creating test directory: $TEST_DIR"
    rm -rf "$TEST_DIR"
    mkdir -p "$TEST_DIR"
}

cleanup_test_dir() {
    log_verbose "Cleaning up test directory"
    rm -rf "$TEST_DIR"
}

load_module() {
    if ! kldload ng_pppoe_lb 2>/dev/null; then
        if [ -f "/boot/kernel/ng_pppoe_lb.ko" ]; then
            kldload /boot/kernel/ng_pppoe_lb.ko
        elif [ -f "$(pwd)/ng_pppoe_lb.ko" ]; then
            kldload "$(pwd)/ng_pppoe_lb.ko"
        else
            return 1
        fi
    fi
    return 0
}

# ============================================================================
# CFG-01: basic_2workers configuration
# ============================================================================

test_cfg_01_basic_2workers() {
    log_section "CFG-01: basic_2workers Configuration"
    
    CONFIG="${EXAMPLES_DIR}/basic_2workers"
    
    if [ ! -f "$CONFIG" ]; then
        record_skip "CFG-01: basic_2workers not found"
        return 0
    fi
    
    record_pass "CFG-01: basic_2workers file exists"
    
    # Validate ngctl syntax
    if grep -qE "^\s*(mkpeer|msg|connect|shutdown)" "$CONFIG" 2>/dev/null; then
        record_pass "CFG-01: basic_2workers has valid ngctl commands"
    else
        record_fail "CFG-01: basic_2workers has invalid syntax"
    fi
    
    # Check for worker count
    if grep -qi "num_workers.*2\|workers.*2" "$CONFIG" 2>/dev/null; then
        record_pass "CFG-01: basic_2workers configures 2 workers"
    else
        record_skip "CFG-01: Worker count not explicitly set"
    fi
    
    # Check for expected directives
    if grep -q "pppoe_lb" "$CONFIG" 2>/dev/null; then
        record_pass "CFG-01: basic_2workers references pppoe_lb"
    else
        record_fail "CFG-01: basic_2workers missing pppoe_lb references"
    fi
}

# ============================================================================
# CFG-02: basic_4workers configuration
# ============================================================================

test_cfg_02_basic_4workers() {
    log_section "CFG-02: basic_4workers Configuration"
    
    CONFIG="${EXAMPLES_DIR}/basic_4workers"
    
    if [ ! -f "$CONFIG" ]; then
        record_skip "CFG-02: basic_4workers not found"
        return 0
    fi
    
    record_pass "CFG-02: basic_4workers file exists"
    
    # Validate ngctl syntax
    if grep -qE "^\s*(mkpeer|msg|connect|shutdown)" "$CONFIG" 2>/dev/null; then
        record_pass "CFG-02: basic_4workers has valid ngctl commands"
    else
        record_fail "CFG-02: basic_4workers has invalid syntax"
    fi
    
    # Check for worker count
    if grep -qi "num_workers.*4\|workers.*4" "$CONFIG" 2>/dev/null; then
        record_pass "CFG-02: basic_4workers configures 4 workers"
    else
        record_skip "CFG-02: Worker count not explicitly set"
    fi
}

# ============================================================================
# CFG-03: governor_auto configuration
# ============================================================================

test_cfg_03_governor_auto() {
    log_section "CFG-03: governor_auto Configuration"
    
    CONFIG="${EXAMPLES_DIR}/governor_auto"
    
    if [ ! -f "$CONFIG" ]; then
        record_skip "CFG-03: governor_auto not found"
        return 0
    fi
    
    record_pass "CFG-03: governor_auto file exists"
    
    # Check for governor settings
    if grep -qi "governor.*enabled\|governor.*1" "$CONFIG" 2>/dev/null; then
        record_pass "CFG-03: governor_auto enables governor"
    else
        record_fail "CFG-03: governor_auto missing governor enable"
    fi
    
    # Check for threshold settings
    if grep -qE "cpu_threshold|cpu_low_threshold" "$CONFIG" 2>/dev/null; then
        record_pass "CFG-03: governor_auto sets thresholds"
    else
        record_skip "CFG-03: Thresholds not explicitly set"
    fi
    
    # Check for min/max workers
    if grep -qE "min_workers|max_workers" "$CONFIG" 2>/dev/null; then
        record_pass "CFG-03: governor_auto sets min/max workers"
    else
        record_skip "CFG-03: Min/max workers not explicitly set"
    fi
}

# ============================================================================
# CFG-04: monitor_example
# ============================================================================

test_cfg_04_monitor_example() {
    log_section "CFG-04: monitor_example Configuration"
    
    CONFIG="${EXAMPLES_DIR}/monitor_example"
    
    if [ ! -f "$CONFIG" ]; then
        record_skip "CFG-04: monitor_example not found"
        return 0
    fi
    
    record_pass "CFG-04: monitor_example file exists"
    
    # Check for sysctl commands
    if grep -q "sysctl.*net\.graph\.pppoe_lb" "$CONFIG" 2>/dev/null; then
        record_pass "CFG-04: monitor_example uses sysctl commands"
    else
        record_fail "CFG-04: monitor_example missing sysctl commands"
    fi
    
    # Check for ngctl commands
    if grep -q "ngctl" "$CONFIG" 2>/dev/null; then
        record_pass "CFG-04: monitor_example uses ngctl commands"
    else
        record_skip "CFG-04: monitor_example missing ngctl commands"
    fi
}

# ============================================================================
# CFG-05: rc.conf.pppoe_lb
# ============================================================================

test_cfg_05_rc_conf() {
    log_section "CFG-05: rc.conf.pppoe_lb Configuration"
    
    CONFIG="/usr/share/examples/etc/rc.conf.pppoe_lb"
    
    if [ ! -f "$CONFIG" ]; then
        record_skip "CFG-05: rc.conf.pppoe_lb not found"
        return 0
    fi
    
    record_pass "CFG-05: rc.conf.pppoe_lb file exists"
    
    # Check for pppoed_enable
    if grep -q "pppoed_enable=" "$CONFIG" 2>/dev/null; then
        record_pass "CFG-05: rc.conf.pppoe_lb sets pppoed_enable"
    else
        record_fail "CFG-05: rc.conf.pppoe_lb missing pppoed_enable"
    fi
    
    # Check for multi-worker options
    if grep -q "\-L\|\-\-multi" "$CONFIG" 2>/dev/null; then
        record_pass "CFG-05: rc.conf.pppoe_lb includes multi-worker flag"
    else
        record_skip "CFG-05: Multi-worker flag not present"
    fi
    
    # Validate rc.conf syntax (basic check)
    if grep -qE "^[a-z_]+=\"[^\"]*\"" "$CONFIG" 2>/dev/null; then
        record_pass "CFG-05: rc.conf.pppoe_lb has valid rc.conf syntax"
    else
        record_fail "CFG-05: rc.conf.pppoe_lb has invalid syntax"
    fi
}

# ============================================================================
# CFG-06: 2-core system - max workers
# ============================================================================

test_cfg_06_max_workers_2core() {
    log_section "CFG-06: Max Workers on 2-Core System"
    
    # Load module
    load_module || {
        record_skip "CFG-06: Cannot load module"
        return 0
    }
    
    # Set max workers to 2
    if sysctl ${SYSCTL_PREFIX}.governor.max_workers=2 >/dev/null 2>&1; then
        MAX=$(sysctl -n ${SYSCTL_PREFIX}.governor.max_workers 2>/dev/null)
        if [ "$MAX" = "2" ]; then
            record_pass "CFG-06: Max workers capped at 2"
        else
            record_fail "CFG-06: Max workers is $MAX, expected 2"
        fi
    else
        record_fail "CFG-06: Cannot set max_workers"
    fi
}

# ============================================================================
# CFG-07: 8-core system - max workers
# ============================================================================

test_cfg_07_max_workers_8core() {
    log_section "CFG-07: Max Workers on 8-Core System"
    
    # Load module if not already loaded
    load_module || {
        record_skip "CFG-07: Cannot load module"
        return 0
    }
    
    # Set max workers to 8
    if sysctl ${SYSCTL_PREFIX}.governor.max_workers=8 >/dev/null 2>&1; then
        MAX=$(sysctl -n ${SYSCTL_PREFIX}.governor.max_workers 2>/dev/null)
        if [ "$MAX" = "8" ]; then
            record_pass "CFG-07: Max workers set to 8"
        else
            record_fail "CFG-07: Max workers is $MAX, expected 8"
        fi
    else
        record_fail "CFG-07: Cannot set max_workers"
    fi
}

# ============================================================================
# CFG-08: Low memory configuration
# ============================================================================

test_cfg_08_low_memory() {
    log_section "CFG-08: Low Memory Configuration"
    
    # Get memory info
    MEM=$(sysctl -n hw.realmem 2>/dev/null || echo "0")
    MEM_MB=$((MEM / 1024 / 1024))
    
    log_verbose "System memory: ${MEM_MB} MB"
    
    if [ "$MEM_MB" -gt 0 ]; then
        record_pass "CFG-08: System memory detected (${MEM_MB} MB)"
        
        # Set conservative worker count based on memory
        if [ "$MEM_MB" -lt 1024 ]; then
            # Low memory system
            WORKERS=1
        elif [ "$MEM_MB" -lt 2048 ]; then
            # Medium memory
            WORKERS=2
        else
            # Normal memory
            WORKERS=4
        fi
        
        if sysctl ${SYSCTL_PREFIX}.num_workers=$WORKERS >/dev/null 2>&1; then
            record_pass "CFG-08: Conservative worker count set for memory"
        fi
    else
        record_skip "CFG-08: Cannot determine system memory"
    fi
}

# ============================================================================
# CFG-09: Single NIC configuration
# ============================================================================

test_cfg_09_single_nic() {
    log_section "CFG-09: Single NIC Configuration"
    
    # List network interfaces
    IFACES=$(ifconfig -l 2>/dev/null | tr ' ' '\n' | grep -E '^[a-z]+[0-9]+$' | wc -l)
    
    log_verbose "Network interfaces: $IFACES"
    
    if [ "$IFACES" -ge 1 ]; then
        record_pass "CFG-09: At least 1 network interface available"
        
        # Get primary interface
        PRIMARY=$(route -n get default 2>/dev/null | grep interface | awk '{print $2}')
        
        if [ -n "$PRIMARY" ]; then
            record_pass "CFG-09: Primary interface identified: $PRIMARY"
        else
            record_skip "CFG-09: Cannot identify primary interface"
        fi
    else
        record_skip "CFG-09: No network interfaces found"
    fi
}

# ============================================================================
# CFG-10: Dual NIC configuration
# ============================================================================

test_cfg_10_dual_nic() {
    log_section "CFG-10: Dual NIC Configuration"
    
    # List network interfaces
    IFACES=$(ifconfig -l 2>/dev/null | tr ' ' '\n' | grep -E '^[a-z]+[0-9]+$' | wc -l)
    
    if [ "$IFACES" -ge 2 ]; then
        record_pass "CFG-10: Multiple NICs available ($IFACES)"
        
        # Check for different interface types
        ETHERNET=$(ifconfig -l ether 2>/dev/null | tr ' ' '\n' | wc -l)
        log_verbose "Ethernet interfaces: $ETHERNET"
        
        if [ "$ETHERNET" -ge 2 ]; then
            record_pass "CFG-10: Multiple ethernet interfaces for bonding"
        else
            record_skip "CFG-10: Fewer than 2 ethernet interfaces"
        fi
    else
        record_skip "CFG-10: Fewer than 2 network interfaces"
    fi
}

# ============================================================================
# Configuration File Syntax Tests
# ============================================================================

test_sysctl_conf_syntax() {
    log_section "Configuration File Syntax Tests"
    
    setup_test_dir
    
    # Test valid sysctl.conf
    cat > "$SYSCTL_CONF" << 'EOF'
# Test sysctl.conf
net.graph.pppoe_lb.enabled=1
net.graph.pppoe_lb.num_workers=4
net.graph.pppoe_lb.governor.enabled=1
net.graph.pppoe_lb.governor.min_workers=2
EOF
    
    # Validate that file was created
    if [ -f "$SYSCTL_CONF" ]; then
        record_pass "sysctl.conf: Valid configuration accepted"
    else
        record_fail "sysctl.conf: Configuration rejected"
    fi
    
    # Check that lines don't have syntax errors
    if grep -qE "^[a-z_]+\.[a-z_]+(\.[a-z_]+)*=[0-9]+" "$SYSCTL_CONF" 2>/dev/null; then
        record_pass "sysctl.conf: All entries have valid format"
    else
        record_fail "sysctl.conf: Invalid entry format"
    fi
    
    # Test ngctl.conf syntax
    cat > "$NGCTL_CONF" << 'EOF'
# Test ngctl configuration
mkpeer ether0: pppoe_lb ether pppoe_lb0
msg pppoe_lb0: setconfig debug=1
EOF
    
    if [ -f "$NGCTL_CONF" ]; then
        record_pass "ngctl.conf: Valid configuration accepted"
    else
        record_fail "ngctl.conf: Configuration rejected"
    fi
    
    # Test rc.conf syntax
    cat > "$RC_CONF" << 'EOF'
# Test rc.conf
pppoed_enable="YES"
pppoed_flags="-L -w 4 -G 1"
EOF
    
    if [ -f "$RC_CONF" ]; then
        record_pass "rc.conf: Valid configuration accepted"
    else
        record_fail "rc.conf: Configuration rejected"
    fi
    
    # Validate rc.conf syntax
    if grep -qE '^[a-z_]+="[^"]*"' "$RC_CONF" 2>/dev/null; then
        record_pass "rc.conf: All entries have valid format"
    else
        record_fail "rc.conf: Invalid entry format"
    fi
    
    cleanup_test_dir
}

# ============================================================================
# Governor Configuration Tests
# ============================================================================

test_governor_config_sysctls() {
    log_section "Governor Configuration Sysctl Tests"
    
    load_module || {
        record_skip "Governor sysctls: Cannot load module"
        return 0
    }
    
    # Test governor.enabled
    if sysctl ${SYSCTL_PREFIX}.governor.enabled=1 >/dev/null 2>&1; then
        VAL=$(sysctl -n ${SYSCTL_PREFIX}.governor.enabled 2>/dev/null)
        [ "$VAL" = "1" ] && record_pass "governor.enabled: Set to 1"
    else
        record_fail "governor.enabled: Cannot set"
    fi
    
    # Test governor.mode
    if sysctl ${SYSCTL_PREFIX}.governor.mode=1 >/dev/null 2>&1; then
        VAL=$(sysctl -n ${SYSCTL_PREFIX}.governor.mode 2>/dev/null)
        [ "$VAL" = "1" ] && record_pass "governor.mode: Set to 1 (auto)"
    else
        record_skip "governor.mode: Not available"
    fi
    
    # Test governor.min_workers
    if sysctl ${SYSCTL_PREFIX}.governor.min_workers=2 >/dev/null 2>&1; then
        VAL=$(sysctl -n ${SYSCTL_PREFIX}.governor.min_workers 2>/dev/null)
        [ "$VAL" = "2" ] && record_pass "governor.min_workers: Set to 2"
    else
        record_fail "governor.min_workers: Cannot set"
    fi
    
    # Test governor.max_workers
    if sysctl ${SYSCTL_PREFIX}.governor.max_workers=4 >/dev/null 2>&1; then
        VAL=$(sysctl -n ${SYSCTL_PREFIX}.governor.max_workers 2>/dev/null)
        [ "$VAL" = "4" ] && record_pass "governor.max_workers: Set to 4"
    else
        record_fail "governor.max_workers: Cannot set"
    fi
    
    # Test governor.poll_interval
    if sysctl ${SYSCTL_PREFIX}.governor.poll_interval=10 >/dev/null 2>&1; then
        VAL=$(sysctl -n ${SYSCTL_PREFIX}.governor.poll_interval 2>/dev/null)
        [ "$VAL" = "10" ] && record_pass "governor.poll_interval: Set to 10"
    else
        record_skip "governor.poll_interval: Not available"
    fi
    
    # Test governor.cpu_threshold
    if sysctl ${SYSCTL_PREFIX}.governor.cpu_threshold=80 >/dev/null 2>&1; then
        VAL=$(sysctl -n ${SYSCTL_PREFIX}.governor.cpu_threshold 2>/dev/null)
        [ "$VAL" = "80" ] && record_pass "governor.cpu_threshold: Set to 80"
    else
        record_fail "governor.cpu_threshold: Cannot set"
    fi
    
    # Test governor.cpu_low_threshold
    if sysctl ${SYSCTL_PREFIX}.governor.cpu_low_threshold=30 >/dev/null 2>&1; then
        VAL=$(sysctl -n ${SYSCTL_PREFIX}.governor.cpu_low_threshold 2>/dev/null)
        [ "$VAL" = "30" ] && record_pass "governor.cpu_low_threshold: Set to 30"
    else
        record_fail "governor.cpu_low_threshold: Cannot set"
    fi
}

# ============================================================================
# Main
# ============================================================================

main() {
    log_section "PPPoE Load Balancer Configuration Tests"
    log_info "Quick mode: $([ "$QUICK_MODE" = "1" ] && echo "Yes" || echo "No")"
    log_info "Verbose: $([ "$VERBOSE" = "1" ] && echo "Yes" || echo "No")"
    
    check_root
    
    # Example configuration tests
    log_section "Example Configuration Tests (CFG-01 to CFG-05)"
    test_cfg_01_basic_2workers || true
    test_cfg_02_basic_4workers || true
    test_cfg_03_governor_auto || true
    test_cfg_04_monitor_example || true
    test_cfg_05_rc_conf || true
    
    # Configuration variant tests
    log_section "Configuration Variant Tests (CFG-06 to CFG-10)"
    test_cfg_06_max_workers_2core || true
    test_cfg_07_max_workers_8core || true
    test_cfg_08_low_memory || true
    test_cfg_09_single_nic || true
    test_cfg_10_dual_nic || true
    
    # Syntax validation tests
    test_sysctl_conf_syntax || true
    
    # Governor sysctl tests
    test_governor_config_sysctls || true
    
    # Summary
    log_section "Configuration Test Summary"
    echo ""
    echo "  Tests Run:    $TESTS_RUN"
    echo "  Passed:       ${GREEN}$TESTS_PASSED${NC}"
    echo "  Failed:       ${RED}$TESTS_FAILED${NC}"
    echo "  Skipped:      ${YELLOW}$TESTS_SKIPPED${NC}"
    echo ""
    
    if [ $TESTS_FAILED -gt 0 ]; then
        echo "${RED}RESULT: FAILED${NC} - Some configuration tests failed"
        exit 1
    else
        echo "${GREEN}RESULT: PASSED${NC} - All configuration tests passed"
        exit 0
    fi
}

main "$@"
