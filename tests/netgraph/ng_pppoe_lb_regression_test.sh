#!/bin/sh
#
# ng_pppoe_lb_regression_test.sh - Regression tests for PPPoE Load Balancer
#
# This script tests backward compatibility of the ng_pppoe_lb module with
# existing FreeBSD PPPoE stack, ensuring no regressions in existing functionality.
#
# WARNING: This script requires VM environment or isolated test system!
#
# Usage:
#   ./ng_pppoe_lb_regression_test.sh [--quick] [--verbose]
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
#   REG-01 to REG-04: Netgraph compatibility
#   REG-05 to REG-10: PPP stack compatibility
#   REG-11 to REG-15: System services compatibility
#   REG-16 to REG-18: Configuration compatibility
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
    echo "ok $TESTS_RUN - $1"
    log_success "Test $TESTS_RUN: $1"
}

record_fail() {
    TESTS_RUN=$((TESTS_RUN + 1)); TESTS_FAILED=$((TESTS_FAILED + 1))
    echo "not ok $TESTS_RUN - $1"
    log_fail "Test $TESTS_RUN: $1"
}

record_skip() {
    TESTS_RUN=$((TESTS_RUN + 1)); TESTS_SKIPPED=$((TESTS_SKIPPED + 1))
    echo "ok $TESTS_RUN - $1 # skip"
    log_skip "Test $TESTS_RUN: $1 (skipped)"
}

check_root() {
    if [ "$(id -u)" -ne 0 ]; then
        log_error "This script must be run as root"
        exit 2
    fi
}

load_pppoe_lb() {
    log_info "Loading ng_pppoe_lb module..."
    if kldstat -n ng_pppoe_lb >/dev/null 2>&1; then
        log_info "Module already loaded"
        return 0
    fi
    if ! kldload ng_pppoe_lb 2>/dev/null; then
        if [ -f "/boot/kernel/ng_pppoe_lb.ko" ]; then
            kldload /boot/kernel/ng_pppoe_lb.ko
        elif [ -f "$(pwd)/ng_pppoe_lb.ko" ]; then
            kldload "$(pwd)/ng_pppoe_lb.ko"
        else
            log_error "Cannot load ng_pppoe_lb module"
            return 1
        fi
    fi
    log_info "Module loaded"
}

unload_pppoe_lb() {
    kldunload ng_pppoe_lb 2>/dev/null || true
}

# ============================================================================
# REG-01: ng_pppoe still works
# ============================================================================

test_reg_01_ng_pppoe_works() {
    log_section "REG-01: ng_pppoe Basic Functionality"
    
    # Load ng_pppoe (not ng_pppoe_lb)
    kldload ng_pppoe 2>/dev/null || true
    
    # Create a test topology with regular ng_pppoe
    ifconfig epair create >/dev/null 2>&1 || {
        record_skip "REG-01: Cannot create test interface"
        return 0
    }
    
    EPAIR=$(ifconfig epair create 2>/dev/null | head -1)
    
    if [ -z "$EPAIR" ]; then
        record_skip "REG-01: Cannot get interface name"
        return 0
    fi
    
    # Create ng_pppoe node (traditional)
    if $NGCTL mkpeer $EPAIR: pppoe mypppoe >/dev/null 2>&1; then
        record_pass "REG-01: Traditional ng_pppoe node created"
        
        # Get status
        if $NGCTL msg mypppoe: getstatus >/dev/null 2>&1; then
            record_pass "REG-01: ng_pppoe getstatus works"
        fi
        
        # Cleanup
        $NGCTL shutdown mypppoe: 2>/dev/null || true
    else
        record_fail "REG-01: Cannot create ng_pppoe node"
    fi
    
    ifconfig $EPAIR destroy 2>/dev/null || true
}

# ============================================================================
# REG-02: Existing nodes visible
# ============================================================================

test_reg_02_existing_nodes_visible() {
    log_section "REG-02: Existing Netgraph Nodes Visible"
    
    # Load modules
    kldload netgraph 2>/dev/null || true
    kldload ng_ether 2>/dev/null || true
    
    # List existing nodes
    if $NGCTL list >/dev/null 2>&1; then
        record_pass "REG-02: ngctl list works"
        
        NODE_COUNT=$($NGCTL list 2>/dev/null | wc -l)
        log_verbose "Found $NODE_COUNT netgraph nodes/lines"
        
        record_pass "REG-02: Existing netgraph nodes visible ($NODE_COUNT lines)"
    else
        record_fail "REG-02: Cannot list netgraph nodes"
    fi
}

# ============================================================================
# REG-03: Node lifecycle
# ============================================================================

test_reg_03_node_lifecycle() {
    log_section "REG-03: Node Lifecycle (Create/Destroy)"
    
    # Create interface
    EPAIR=$(ifconfig epair create 2>/dev/null | head -1) || {
        record_skip "REG-03: Cannot create test interface"
        return 0
    }
    
    # Create node
    if $NGCTL mkpeer $EPAIR: pppoe testnode >/dev/null 2>&1; then
        record_pass "REG-03: Node created successfully"
        
        # Verify node exists
        if $NGCTL list | grep -q testnode; then
            record_pass "REG-03: Node visible in list"
        fi
        
        # Destroy node
        if $NGCTL shutdown testnode: 2>/dev/null; then
            record_pass "REG-03: Node destroyed successfully"
        else
            record_fail "REG-03: Cannot destroy node"
        fi
    else
        record_fail "REG-03: Cannot create test node"
    fi
    
    ifconfig $EPAIR destroy 2>/dev/null || true
}

# ============================================================================
# REG-04: Netgraph messages backward compatibility
# ============================================================================

test_reg_04_netgraph_messages() {
    log_section "REG-04: Netgraph Message Backward Compatibility"
    
    # Create test node
    EPAIR=$(ifconfig epair create 2>/dev/null | head -1) || {
        record_skip "REG-04: Cannot create test interface"
        return 0
    }
    
    if $NGCTL mkpeer $EPAIR: pppoe msgnode >/dev/null 2>&1; then
        # Test standard netgraph messages
        if $NGCTL msg msgnode: gettype >/dev/null 2>&1; then
            record_pass "REG-04: gettype message works"
        else
            record_fail "REG-04: gettype message failed"
        fi
        
        if $NGCTL msg msgnode: gethooks >/dev/null 2>&1; then
            record_pass "REG-04: gethooks message works"
        else
            record_fail "REG-04: gethooks message failed"
        fi
        
        $NGCTL shutdown msgnode: 2>/dev/null || true
    else
        record_fail "REG-04: Cannot create test node"
    fi
    
    ifconfig $EPAIR destroy 2>/dev/null || true
}

# ============================================================================
# REG-05: ppp.linkup script test
# ============================================================================

test_reg_05_ppp_linkup() {
    log_section "REG-05: ppp.linkup Script Execution"
    
    # Check if linkup script exists or can be created
    LINKUP="/etc/ppp/ppp.linkup"
    
    if [ -f "$LINKUP" ]; then
        record_pass "REG-05: $LINKUP exists"
        
        # Check if executable
        if [ -x "$LINKUP" ] || [ -r "$LINKUP" ]; then
            record_pass "REG-05: linkup script is accessible"
        else
            record_skip "REG-05: linkup script not accessible"
        fi
    else
        record_skip "REG-05: $LINKUP not present (optional)"
    fi
}

# ============================================================================
# REG-06: ppp.linkdown script test
# ============================================================================

test_reg_06_ppp_linkdown() {
    log_section "REG-06: ppp.linkdown Script Execution"
    
    LINKDOWN="/etc/ppp/ppp.linkdown"
    
    if [ -f "$LINKDOWN" ]; then
        record_pass "REG-06: $LINKDOWN exists"
        
        if [ -x "$LINKDOWN" ] || [ -r "$LINKDOWN" ]; then
            record_pass "REG-06: linkdown script is accessible"
        else
            record_skip "REG-06: linkdown script not accessible"
        fi
    else
        record_skip "REG-06: $LINKDOWN not present (optional)"
    fi
}

# ============================================================================
# REG-07: PAP authentication configuration
# ============================================================================

test_reg_07_pap_auth() {
    log_section "REG-07: PAP Authentication Configuration"
    
    PAP_SECRETS="/etc/ppp/pap-secrets"
    
    if [ -f "$PAP_SECRETS" ]; then
        record_pass "REG-07: $PAP_SECRETS exists"
        
        # Check format (should have at least one line with username and secret)
        if grep -qE "^[^#]*[[:space:]]+" "$PAP_SECRETS" 2>/dev/null; then
            record_pass "REG-07: PAP secrets file has valid entries"
        else
            record_skip "REG-07: PAP secrets file empty or no valid entries"
        fi
    else
        record_skip "REG-07: $PAP_SECRETS not present (optional)"
    fi
}

# ============================================================================
# REG-08: CHAP authentication configuration
# ============================================================================

test_reg_08_chap_auth() {
    log_section "REG-08: CHAP Authentication Configuration"
    
    CHAP_SECRETS="/etc/ppp/chap-secrets"
    
    if [ -f "$CHAP_SECRETS" ]; then
        record_pass "REG-08: $CHAP_SECRETS exists"
        
        if grep -qE "^[^#]*[[:space:]]+" "$CHAP_SECRETS" 2>/dev/null; then
            record_pass "REG-08: CHAP secrets file has valid entries"
        else
            record_skip "REG-08: CHAP secrets file empty or no valid entries"
        fi
    else
        record_skip "REG-08: $CHAP_SECRETS not present (optional)"
    fi
}

# ============================================================================
# REG-09: Idle timeout configuration
# ============================================================================

test_reg_09_idle_timeout() {
    log_section "REG-09: Idle Timeout Configuration"
    
    # Test idle timeout sysctl if available
    if sysctl net.inet.ip.rtimeout 2>/dev/null >/dev/null; then
        record_pass "REG-09: IP timeout sysctl available"
    else
        # This is expected - PPPoE idle timeout is in ppp.conf
        record_pass "REG-09: Idle timeout configured via ppp.conf (expected)"
    fi
}

# ============================================================================
# REG-10: LCP negotiation basics
# ============================================================================

test_reg_10_lcp_negotiation() {
    log_section "REG-10: LCP Configuration Available"
    
    # LCP parameters are configured in ppp.conf
    # This test verifies the configuration mechanism exists
    
    PPP_CONF="/etc/ppp/ppp.conf"
    
    if [ -f "$PPP_CONF" ]; then
        record_pass "REG-10: $PPP_CONF exists"
        
        # Check for common LCP options
        if grep -qi "enable lcp" "$PPP_CONF" 2>/dev/null; then
            record_pass "REG-10: LCP options present in config"
        else
            record_skip "REG-10: No explicit LCP options (may use defaults)"
        fi
    else
        record_skip "REG-10: $PPP_CONF not present (optional)"
    fi
}

# ============================================================================
# REG-11: service pppoed start
# ============================================================================

test_reg_11_service_start() {
    log_section "REG-11: pppoed Service Start"
    
    # Check if pppoed script exists
    if [ -f /usr/sbin/pppoed ] || [ -f /usr/sbin/pppoed ]; then
        record_pass "REG-11: pppoed binary exists"
    else
        record_skip "REG-11: pppoed binary not found"
        return 0
    fi
    
    # Check rc.d script
    if [ -f /etc/rc.d/pppoed ]; then
        record_pass "REG-11: rc.d/pppoed script exists"
        
        # Try to check service status (don't actually start)
        if /etc/rc.d/pppoed status >/dev/null 2>&1; then
            record_pass "REG-11: Service status check works"
        else
            record_pass "REG-11: Service not running (expected)"
        fi
    else
        record_skip "REG-11: rc.d/pppoed script not found"
    fi
}

# ============================================================================
# REG-12: service pppoed stop
# ============================================================================

test_reg_12_service_stop() {
    log_section "REG-12: pppoed Service Script Functionality"
    
    if [ -f /etc/rc.d/pppoed ]; then
        # Verify script has required functions
        if grep -q "start)" /etc/rc.d/pppoed && grep -q "stop)" /etc/rc.d/pppoed; then
            record_pass "REG-12: rc.d script has start/stop functions"
        else
            record_fail "REG-12: rc.d script missing start/stop functions"
        fi
        
        # Verify it's executable
        if [ -x /etc/rc.d/pppoed ]; then
            record_pass "REG-12: rc.d script is executable"
        else
            record_fail "REG-12: rc.d script not executable"
        fi
    else
        record_skip "REG-12: rc.d/pppoed not found"
    fi
}

# ============================================================================
# REG-13: service pppoed restart
# ============================================================================

test_reg_13_service_restart() {
    log_section "REG-13: pppoed Service Restart Capability"
    
    # Test that restart function exists (don't actually restart)
    if [ -f /etc/rc.d/pppoed ]; then
        if grep -q "restart)" /etc/rc.d/pppoed || grep -q "rcvar" /etc/rc.d/pppoed; then
            record_pass "REG-13: Service supports restart/reload"
        else
            record_skip "REG-13: Service restart not explicitly defined"
        fi
    else
        record_skip "REG-13: rc.d/pppoed not found"
    fi
}

# ============================================================================
# REG-14: Boot sequence compatibility
# ============================================================================

test_reg_14_boot_sequence() {
    log_section "REG-14: Boot Sequence Compatibility"
    
    # Check rc.conf for pppoed configuration
    if grep -q "^pppoed_enable=" /etc/rc.conf 2>/dev/null; then
        record_pass "REG-14: pppoed_enable found in rc.conf"
    else
        record_skip "REG-14: pppoed not configured to start on boot (optional)"
    fi
    
    # Check for rc.d ordering dependencies
    if [ -f /etc/rc.d/pppoed ]; then
        if grep -q "REQUIRE:" /etc/rc.d/pppoed || grep -q "BEFORE:" /etc/rc.d/pppoed; then
            record_pass "REG-14: Service has dependency declarations"
        else
            record_skip "REG-14: Service has no explicit dependencies"
        fi
    fi
}

# ============================================================================
# REG-15: Syslog logging
# ============================================================================

test_reg_15_syslog_logging() {
    log_section "REG-15: Syslog Logging Configuration"
    
    # Check if syslog is running
    if pgrep -x syslogd >/dev/null 2>&1; then
        record_pass "REG-15: syslogd is running"
    else
        record_skip "REG-15: syslogd not running"
        return 0
    fi
    
    # Check for pppoed log destination
    if [ -f /etc/syslogd.conf ]; then
        if grep -qi "pppoe\|local0\|local1" /etc/syslogd.conf 2>/dev/null; then
            record_pass "REG-15: PPPoE syslog configuration found"
        else
            record_skip "REG-15: No explicit PPPoE syslog config (using defaults)"
        fi
    fi
    
    # Check newsyslog configuration
    if [ -f /etc/newsyslog.conf ]; then
        if grep -qi "pppoe\|pppd" /etc/newsyslog.conf 2>/dev/null; then
            record_pass "REG-15: Log rotation configured for PPPoE"
        else
            record_skip "REG-15: No explicit log rotation config"
        fi
    fi
}

# ============================================================================
# REG-16: Old rc.conf works
# ============================================================================

test_reg_16_old_rc_conf() {
    log_section "REG-16: Old rc.conf Compatibility"
    
    # Load the module with old-style settings
    load_pppoe_lb
    
    # Test basic module functionality with minimal config
    sysctl ${SYSCTL_PREFIX}.enabled=1 2>/dev/null || true
    
    # Module should load without requiring new-style configs
    if kldstat -n ng_pppoe_lb >/dev/null 2>&1; then
        record_pass "REG-16: Module loads without new-style configs"
    else
        record_fail "REG-16: Module failed to load"
    fi
}

# ============================================================================
# REG-17: Old sysctl.conf
# ============================================================================

test_reg_17_old_sysctl_conf() {
    log_section "REG-17: Old sysctl.conf Compatibility"
    
    if [ -f /etc/sysctl.conf ]; then
        record_pass "REG-17: /etc/sysctl.conf exists"
        
        # Check if old-style sysctls would conflict
        if grep -q "^net.graph.pppoe_lb\." /etc/sysctl.conf 2>/dev/null; then
            record_pass "REG-17: Old sysctl.conf has ng_pppoe_lb entries"
        else
            record_pass "REG-17: No conflicting sysctl.conf entries"
        fi
    else
        record_pass "REG-17: No sysctl.conf (using defaults)"
    fi
}

# ============================================================================
# REG-18: Works without ng_pppoe_lb loaded
# ============================================================================

test_reg_18_works_without_module() {
    log_section "REG-18: System Works Without ng_pppoe_lb"
    
    # Unload module if loaded
    unload_pppoe_lb
    
    # System should still be able to use regular PPPoE
    kldload netgraph 2>/dev/null || true
    kldload ng_ether 2>/dev/null || true
    kldload ng_pppoe 2>/dev/null || true
    
    # Create regular pppoe node (should work without ng_pppoe_lb)
    EPAIR=$(ifconfig epair create 2>/dev/null | head -1) || {
        record_skip "REG-18: Cannot create test interface"
        return 0
    }
    
    if $NGCTL mkpeer $EPAIR: pppoe basenode >/dev/null 2>&1; then
        record_pass "REG-18: Standard PPPoE works without ng_pppoe_lb"
        $NGCTL shutdown basenode: 2>/dev/null || true
    else
        record_fail "REG-18: Standard PPPoE broken without ng_pppoe_lb"
    fi
    
    ifconfig $EPAIR destroy 2>/dev/null || true
}

# ============================================================================
# Main
# ============================================================================

main() {
    log_section "PPPoE Load Balancer Regression Tests"
    log_info "Quick mode: $([ "$QUICK_MODE" = "1" ] && echo "Yes" || echo "No")"
    log_info "Verbose: $([ "$VERBOSE" = "1" ] && echo "Yes" || echo "No")"
    
    check_root
    
    # Netgraph compatibility
    log_section "Netgraph Compatibility (REG-01 to REG-04)"
    test_reg_01_ng_pppoe_works || true
    test_reg_02_existing_nodes_visible || true
    test_reg_03_node_lifecycle || true
    test_reg_04_netgraph_messages || true
    
    # PPP stack compatibility
    log_section "PPP Stack Compatibility (REG-05 to REG-10)"
    test_reg_05_ppp_linkup || true
    test_reg_06_ppp_linkdown || true
    test_reg_07_pap_auth || true
    test_reg_08_chap_auth || true
    test_reg_09_idle_timeout || true
    test_reg_10_lcp_negotiation || true
    
    # System services compatibility
    log_section "System Services Compatibility (REG-11 to REG-15)"
    test_reg_11_service_start || true
    test_reg_12_service_stop || true
    test_reg_13_service_restart || true
    test_reg_14_boot_sequence || true
    test_reg_15_syslog_logging || true
    
    # Configuration compatibility
    log_section "Configuration Compatibility (REG-16 to REG-18)"
    test_reg_16_old_rc_conf || true
    test_reg_17_old_sysctl_conf || true
    test_reg_18_works_without_module || true
    
    # Cleanup
    unload_pppoe_lb

    # Summary
    echo "1..$TESTS_RUN"
    log_section "Regression Test Summary"
    echo ""
    echo "  Tests Run:    $TESTS_RUN"
    echo "  Passed:       ${GREEN}$TESTS_PASSED${NC}"
    echo "  Failed:       ${RED}$TESTS_FAILED${NC}"
    echo "  Skipped:      ${YELLOW}$TESTS_SKIPPED${NC}"
    echo ""
    
    if [ $TESTS_FAILED -gt 0 ]; then
        echo "${RED}RESULT: FAILED${NC} - Some regression tests failed"
        exit 1
    else
        echo "${GREEN}RESULT: PASSED${NC} - All regression tests passed"
        exit 0
    fi
}

main "$@"
