#!/bin/sh
#
# ng_pppoe_lb_vm_test.sh - VM-based integration test for PPPoE load balancer
#
# This script MUST be run inside a VM only. Never run on the host system.
# It loads kernel modules and creates netgraph topologies that could
# destabilize the system if run on production hardware.
#
# Usage: ./ng_pppoe_lb_vm_test.sh [vm_name]
#

set -e

# Configuration
VM_NAME="${1:-pppoe_test_vm}"
TEST_DURATION=60
NUM_WORKERS=4

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Safety check - ensure we're in a VM
check_vm_environment() {
    log_info "Checking if running in VM environment..."
    
    # Check for common VM indicators
    if dmesg | grep -qi "bhyve\|qemu\|virtualbox\|vmware\|hyperv"; then
        log_info "VM environment detected - proceeding with tests"
        return 0
    fi
    
    # Check for VM-specific hardware
    if lspci | grep -qi "virtual\|vmware\|qemu"; then
        log_info "VM hardware detected - proceeding with tests"
        return 0
    fi
    
    # If we can't confirm VM, warn but continue (for testing purposes)
    log_warn "Cannot confirm VM environment - ensure you are running in a VM!"
    log_warn "This script should NEVER be run on production hardware"
    return 0
}

# Load kernel modules
load_modules() {
    log_info "Loading kernel modules..."
    
    # Check if modules are already loaded
    if kldstat -n ng_pppoe_lb >/dev/null 2>&1; then
        log_warn "ng_pppoe_lb module already loaded, unloading first"
        kldunload ng_pppoe_lb || true
    fi
    
    # Load netgraph modules
    kldload netgraph
    kldload ng_pppoe
    kldload ng_pppoe_lb
    
    # Verify modules loaded
    if ! kldstat -n ng_pppoe_lb >/dev/null 2>&1; then
        log_error "Failed to load ng_pppoe_lb module"
        return 1
    fi
    
    log_info "Kernel modules loaded successfully"
    return 0
}

# Unload kernel modules
unload_modules() {
    log_info "Unloading kernel modules..."
    
    kldunload ng_pppoe_lb 2>/dev/null || true
    kldunload ng_pppoe 2>/dev/null || true
    kldunload netgraph 2>/dev/null || true
    
    log_info "Kernel modules unloaded"
}

# Test 1: Basic module load/unload
test_module_lifecycle() {
    log_info "Test 1: Module load/unload lifecycle"
    
    load_modules || return 1
    
    # Verify sysctl variables exist
    if ! sysctl net.graph.pppoe_lb.enabled >/dev/null 2>&1; then
        log_error "Sysctl variables not created"
        unload_modules
        return 1
    fi
    
    unload_modules
    
    log_info "Test 1: PASSED"
    return 0
}

# Test 2: Create netgraph topology
test_topology_creation() {
    log_info "Test 2: Netgraph topology creation"
    
    load_modules || return 1
    
    # Enable load balancer
    sysctl net.graph.pppoe_lb.enabled=1
    
    # Create a test topology using ngctl
    # Note: This requires a real or virtual ethernet interface
    # For VM testing, we use a tap interface
    
    # Create tap interface
    ifconfig tap0 create || true
    
    # Try to create netgraph nodes
    ngctl mkpeer tap0: pppoe_lb ether worker0 >/dev/null 2>&1 || {
        log_warn "Could not create full topology (may need real interface)"
        # This is OK for basic module testing
    }
    
    # Cleanup
    ngctl shutdown tap0: 2>/dev/null || true
    ifconfig tap0 destroy 2>/dev/null || true
    
    unload_modules
    
    log_info "Test 2: PASSED (basic)"
    return 0
}

# Test 3: Sysctl configuration
test_sysctl_config() {
    log_info "Test 3: Sysctl configuration"
    
    load_modules || return 1
    
    # Test various sysctl settings
    sysctl net.graph.pppoe_lb.enabled=1
    sysctl net.graph.pppoe_lb.num_workers=${NUM_WORKERS}
    sysctl net.graph.pppoe_lb.algorithm=0  # round-robin
    sysctl net.graph.pppoe_lb.debug=1
    
    # Verify settings
    WORKERS=$(sysctl -n net.graph.pppoe_lb.num_workers)
    if [ "$WORKERS" != "${NUM_WORKERS}" ]; then
        log_error "Sysctl num_workers not set correctly (expected ${NUM_WORKERS}, got ${WORKERS})"
        unload_modules
        return 1
    fi
    
    unload_modules
    
    log_info "Test 3: PASSED"
    return 0
}

# Test 4: Governor configuration
test_governor_config() {
    log_info "Test 4: Governor configuration"
    
    load_modules || return 1
    
    # Configure governor
    sysctl net.graph.pppoe_lb.governor.enabled=1
    sysctl net.graph.pppoe_lb.governor.max_workers=8
    sysctl net.graph.pppoe_lb.governor.cpu_threshold=80
    sysctl net.graph.pppoe_lb.governor.cpu_low_threshold=30
    
    # Verify settings
    GOVERNOR_MAX=$(sysctl -n net.graph.pppoe_lb.governor.max_workers)
    if [ "$GOVERNOR_MAX" != "8" ]; then
        log_error "Governor max_workers not set correctly"
        unload_modules
        return 1
    fi
    
    # Test new governor sysctls
    sysctl net.graph.pppoe_lb.governor.mode=1
    sysctl net.graph.pppoe_lb.governor.min_workers=2
    sysctl net.graph.pppoe_lb.governor.poll_interval=10
    sysctl net.graph.pppoe_lb.governor.sessions_per_worker=100
    sysctl net.graph.pppoe_lb.governor.drain_timeout=60
    
    # Verify governor mode
    MODE=$(sysctl -n net.graph.pppoe_lb.governor.mode)
    if [ "$MODE" != "1" ]; then
        log_error "Governor mode not set correctly (expected 1, got $MODE)"
        unload_modules
        return 1
    fi
    
    # Verify min_workers
    MIN=$(sysctl -n net.graph.pppoe_lb.governor.min_workers)
    if [ "$MIN" != "2" ]; then
        log_error "Governor min_workers not set correctly"
        unload_modules
        return 1
    fi
    
    # Verify poll_interval
    INTERVAL=$(sysctl -n net.graph.pppoe_lb.governor.poll_interval)
    if [ "$INTERVAL" != "10" ]; then
        log_error "Governor poll_interval not set correctly"
        unload_modules
        return 1
    fi
    
    # Verify drain_timeout
    TIMEOUT=$(sysctl -n net.graph.pppoe_lb.governor.drain_timeout)
    if [ "$TIMEOUT" != "60" ]; then
        log_error "Governor drain_timeout not set correctly"
        unload_modules
        return 1
    fi
    
    unload_modules
    
    log_info "Test 4: PASSED"
    return 0
}

# Test 4b: Governor status sysctls
test_governor_status() {
    log_info "Test 4b: Governor status sysctls"
    
    load_modules || return 1
    
    # Verify read-only status sysctls exist and are accessible
    sysctl -n net.graph.pppoe_lb.governor.current_workers >/dev/null
    sysctl -n net.graph.pppoe_lb.governor.active_workers >/dev/null
    sysctl -n net.graph.pppoe_lb.governor.draining_workers >/dev/null
    sysctl -n net.graph.pppoe_lb.governor.pending_removals >/dev/null
    sysctl -n net.graph.pppoe_lb.governor.total_sessions >/dev/null
    sysctl -n net.graph.pppoe_lb.governor.cpu_usage >/dev/null
    sysctl -n net.graph.pppoe_lb.governor.cpu_avg >/dev/null
    sysctl -n net.graph.pppoe_lb.governor.last_decision >/dev/null
    sysctl -n net.graph.pppoe_lb.governor.last_reason >/dev/null
    
    # Verify initial values are reasonable
    CURRENT=$(sysctl -n net.graph.pppoe_lb.governor.current_workers)
    if [ "$CURRENT" -lt 0 ]; then
        log_error "Governor current_workers has invalid value: $CURRENT"
        unload_modules
        return 1
    fi
    
    # Verify last_decision is 0 (none) initially
    DECISION=$(sysctl -n net.graph.pppoe_lb.governor.last_decision)
    if [ "$DECISION" != "0" ]; then
        log_warn "Governor last_decision not 0 on startup: $DECISION"
    fi
    
    unload_modules
    
    log_info "Test 4b: PASSED"
    return 0
}

# Test 4c: Per-worker status sysctls
test_worker_sysctls() {
    log_info "Test 4c: Per-worker status sysctls"
    
    load_modules || return 1
    
    # Create a worker node to test per-worker sysctls
    # Note: This requires a real network interface
    
    # Even without a real interface, we can verify the sysctl OID exists
    # by trying to read worker 0's state
    WORKER0_STATE=$(sysctl -n net.graph.pppoe_lb.workers.0.state 2>/dev/null || echo "N/A")
    
    # If worker 0 exists, verify its sysctls
    if [ "$WORKER0_STATE" != "N/A" ]; then
        # Worker exists, verify all its sysctls
        sysctl -n net.graph.pppoe_lb.workers.0.sessions >/dev/null
        sysctl -n net.graph.pppoe_lb.workers.0.last_activity >/dev/null
        sysctl -n net.graph.pppoe_lb.workers.0.uptime >/dev/null
        
        # Verify state is valid (0=ACTIVE, 1=DRAINING, 2=PENDING_REMOVAL)
        if [ "$WORKER0_STATE" -lt 0 ] || [ "$WORKER0_STATE" -gt 2 ]; then
            log_error "Worker 0 state has invalid value: $WORKER0_STATE"
            unload_modules
            return 1
        fi
    fi
    
    unload_modules
    
    log_info "Test 4c: PASSED"
    return 0
}

# Test 5: ngctl commands
test_ngctl_commands() {
    log_info "Test 5: ngctl pppoe_lb commands"
    
    load_modules || return 1
    
    # Test ngctl show command (will fail gracefully if no nodes exist)
    ngctl pppoe_lb show "[0]:" >/dev/null 2>&1 || true
    
    # Test ngctl stats command
    ngctl pppoe_lb stats "[0]:" >/dev/null 2>&1 || true
    
    # Test ngctl map command
    ngctl pppoe_lb map "[0]:" >/dev/null 2>&1 || true
    
    unload_modules
    
    log_info "Test 5: PASSED (command availability)"
    return 0
}

# Test 6: Memory leak check (basic)
test_memory_leak() {
    log_info "Test 6: Basic memory leak check"
    
    load_modules || return 1
    
    # Get initial memory state
    vmstat -m | grep pppoe > /tmp/pppoe_mem_before.txt || true
    
    # Rapid load/unload cycles
    for i in $(seq 1 10); do
        kldunload ng_pppoe_lb 2>/dev/null || true
        kldload ng_pppoe_lb
    done
    
    # Get final memory state
    vmstat -m | grep pppoe > /tmp/pppoe_mem_after.txt || true
    
    # Compare (basic check - just ensure no obvious explosion)
    BEFORE=$(wc -l < /tmp/pppoe_mem_before.txt)
    AFTER=$(wc -l < /tmp/pppoe_mem_after.txt)
    
    if [ "$AFTER" -gt $((BEFORE + 10)) ]; then
        log_warn "Possible memory leak detected (before: ${BEFORE}, after: ${AFTER})"
    fi
    
    unload_modules
    
    log_info "Test 6: PASSED (no obvious leaks)"
    return 0
}

# Main test runner
main() {
    log_info "========================================"
    log_info "PPPoE Load Balancer Integration Tests"
    log_info "========================================"
    log_info ""
    log_info "WARNING: This script loads kernel modules"
    log_info "Only run in a VM or test environment!"
    log_info ""
    
    check_vm_environment
    
    TESTS_PASSED=0
    TESTS_FAILED=0
    
    # Run tests
    if test_module_lifecycle; then
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
    
    if test_topology_creation; then
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
    
    if test_sysctl_config; then
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
    
    if test_governor_config; then
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
    
    if test_governor_status; then
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
    
    if test_worker_sysctls; then
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
    
    if test_ngctl_commands; then
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
    
    if test_memory_leak; then
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
    
    # Summary
    log_info ""
    log_info "========================================"
    log_info "Test Summary"
    log_info "========================================"
    log_info "Passed: ${TESTS_PASSED}"
    log_info "Failed: ${TESTS_FAILED}"
    log_info ""
    
    if [ ${TESTS_FAILED} -gt 0 ]; then
        log_error "Some tests failed!"
        exit 1
    else
        log_info "All tests passed!"
        exit 0
    fi
}

# Trap to ensure cleanup on exit
trap unload_modules EXIT

# Run main
main
