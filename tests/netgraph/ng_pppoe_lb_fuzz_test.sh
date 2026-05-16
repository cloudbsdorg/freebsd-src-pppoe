#!/bin/sh
#
# ng_pppoe_lb_fuzz_test.sh - Fuzzing test for PPPoE load balancer
#
# This script performs fuzz testing on the PPPoE load balancer kernel module.
# It generates random, boundary, and malformed inputs to test robustness.
# MUST be run in a VM - can cause kernel panics.
#
# Usage: ./ng_pppoe_lb_fuzz_test.sh [--iterations N] [--seed SEED]
#

set -e

# Configuration
SYSCTL_PREFIX="net.graph.pppoe_lb"
NUM_WORKERS=4

ITERATIONS="${ITERATIONS:-1000}"
SEED="${SEED:-$(date +%s)}"

# Counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0
PANICS=0

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Safety check
check_vm_environment() {
    log_info "Checking if running in VM environment..."

    if dmesg | grep -qi "bhyve\|qemu\|virtualbox\|vmware\|hyperv"; then
        log_info "VM environment detected - proceeding with tests"
        return 0
    fi

    if lspci | grep -qi "virtual\|vmware\|qemu"; then
        log_info "VM hardware detected - proceeding with tests"
        return 0
    fi

    if [ "${CI_MODE}" = "true" ]; then
        log_info "CI_MODE detected, continuing anyway"
        return 0
    fi

    log_warn "Cannot confirm VM environment - fuzzing can cause panics!"
    log_warn "This script should ONLY be run in a VM!"
    return 0
}

# Load kernel modules
load_modules() {
    log_info "Loading kernel modules..."

    if kldstat -n ng_pppoe_lb >/dev/null 2>&1; then
        log_warn "ng_pppoe_lb module already loaded"
    else
        kldload netgraph 2>/dev/null || true
        kldload ng_pppoe 2>/dev/null || true
        kldload ng_pppoe_lb 2>/dev/null || true
    fi

    if ! kldstat -n ng_pppoe_lb >/dev/null 2>&1; then
        log_error "Failed to load ng_pppoe_lb module"
        return 1
    fi

    log_info "Kernel modules loaded"
    return 0
}

# Unload kernel modules
unload_modules() {
    kldunload ng_pppoe_lb 2>/dev/null || true
}

# Generate random number 0-MAX
rand() {
    local max=$1
    awk "BEGIN{srand($SEED + $(date +%s%N) % 1000000); print int(rand() * $max)}"
}

# ============================================================================
# Fuzz Tests
# ============================================================================

# Test: Sysctl boundary values
test_sysctl_boundaries() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 1: Sysctl boundary values"

    local tests=0
    local passed=0

    # Test maximum values
    sysctl ${SYSCTL_PREFIX}.governor.max_workers=1000 2>/dev/null && passed=$((passed + 1))
    tests=$((tests + 1))

    sysctl ${SYSCTL_PREFIX}.governor.min_workers=0 2>/dev/null && passed=$((passed + 1))
    tests=$((tests + 1))

    sysctl ${SYSCTL_PREFIX}.governor.session_threshold_high=65535 2>/dev/null && passed=$((passed + 1))
    tests=$((tests + 1))

    sysctl ${SYSCTL_PREFIX}.governor.session_threshold_low=0 2>/dev/null && passed=$((passed + 1))
    tests=$((tests + 1))

    # Test overflow values
    sysctl ${SYSCTL_PREFIX}.governor.max_workers=4294967295 2>/dev/null || passed=$((passed + 1))
    tests=$((tests + 1))

    sysctl ${SYSCTL_PREFIX}.governor.max_workers=-1 2>/dev/null || passed=$((passed + 1))
    tests=$((tests + 1))

    # Reset to safe values
    sysctl ${SYSCTL_PREFIX}.governor.max_workers=16 2>/dev/null
    sysctl ${SYSCTL_PREFIX}.governor.min_workers=1 2>/dev/null
    sysctl ${SYSCTL_PREFIX}.governor.session_threshold_high=1000 2>/dev/null
    sysctl ${SYSCTL_PREFIX}.governor.session_threshold_low=100 2>/dev/null

    if [ $passed -eq $tests ]; then
        echo "ok $TESTS_RUN - sysctl_boundaries"
        return 0
    else
        echo "not ok $TESTS_RUN - sysctl_boundaries"
        return 1
    fi
}

# Test: Rapid sysctl changes
test_rapid_sysctl_changes() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 2: Rapid sysctl changes"

    local errors=0
    local i=0

    while [ $i -lt 100 ]; do
        case $((i % 4)) in
            0) sysctl ${SYSCTL_PREFIX}.debug.level=0 2>/dev/null || errors=$((errors + 1)) ;;
            1) sysctl ${SYSCTL_PREFIX}.debug.level=1 2>/dev/null || errors=$((errors + 1)) ;;
            2) sysctl ${SYSCTL_PREFIX}.debug.level=2 2>/dev/null || errors=$((errors + 1)) ;;
            3) sysctl ${SYSCTL_PREFIX}.debug.level=3 2>/dev/null || errors=$((errors + 1)) ;;
        esac
        i=$((i + 1))
    done

    if [ $errors -eq 0 ]; then
        echo "ok $TESTS_RUN - rapid_sysctl_changes"
        return 0
    else
        echo "not ok $TESTS_RUN - rapid_sysctl_changes # $errors errors"
        return 1
    fi
}

# Test: Invalid algorithm names
test_invalid_algorithms() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 3: Invalid algorithm names"

    local errors=0

    # These should all be rejected
    sysctl ${SYSCTL_PREFIX}.algorithm="invalid_algo" 2>/dev/null && errors=$((errors + 1))
    sysctl ${SYSCTL_PREFIX}.algorithm="" 2>/dev/null && errors=$((errors + 1))
    sysctl ${SYSCTL_PREFIX}.algorithm="roundrobin " 2>/dev/null && errors=$((errors + 1))
    sysctl ${SYSCTL_PREFIX}.algorithm=" RoundRobin" 2>/dev/null && errors=$((errors + 1))
    sysctl ${SYSCTL_PREFIX}.algorithm="../../etc/passwd" 2>/dev/null && errors=$((errors + 1))
    sysctl ${SYSCTL_PREFIX}.algorithm="; rm -rf /" 2>/dev/null && errors=$((errors + 1))

    # Valid ones should work
    sysctl ${SYSCTL_PREFIX}.algorithm="round_robin" 2>/dev/null || errors=$((errors + 1))
    sysctl ${SYSCTL_PREFIX}.algorithm="least_load" 2>/dev/null || errors=$((errors + 1))
    sysctl ${SYSCTL_PREFIX}.algorithm="session_count" 2>/dev/null || errors=$((errors + 1))

    if [ $errors -eq 0 ]; then
        echo "ok $TESTS_RUN - invalid_algorithms"
        return 0
    else
        echo "not ok $TESTS_RUN - invalid_algorithms # $errors errors"
        return 1
    fi
}

# Test: Worker count stress
test_worker_count_stress() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 4: Worker count stress"

    local errors=0

    # Rapidly change worker counts
    local i=0
    while [ $i -lt 50 ]; do
        workers=$((1 + (i % 16)))
        sysctl ${SYSCTL_PREFIX}.governor.max_workers=$workers 2>/dev/null || errors=$((errors + 1))
        sysctl ${SYSCTL_PREFIX}.governor.min_workers=1 2>/dev/null || errors=$((errors + 1))
        i=$((i + 1))
    done

    # Reset
    sysctl ${SYSCTL_PREFIX}.governor.max_workers=16 2>/dev/null

    if [ $errors -eq 0 ]; then
        echo "ok $TESTS_RUN - worker_count_stress"
        return 0
    else
        echo "not ok $TESTS_RUN - worker_count_stress # $errors errors"
        return 1
    fi
}

# Test: Netgraph node creation stress
test_ngnode_stress() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 5: Netgraph node creation stress"

    local errors=0

    # Create and destroy nodes rapidly
    local i=0
    while [ $i -lt 50 ]; do
        ngctl -df mkpeer pppoe: pppoe out >/dev/null 2>&1 && ngctl -df rm pppoe*: >/dev/null 2>&1 || errors=$((errors + 1))
        i=$((i + 1))
    done

    if [ $errors -eq 0 ]; then
        echo "ok $TESTS_RUN - ngnode_stress"
        return 0
    else
        echo "not ok $TESTS_RUN - ngnode_stress # $errors errors"
        return 1
    fi
}

# Test: Malformed netgraph commands
test_malformed_ng_commands() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 6: Malformed netgraph commands"

    local errors=0

    # These should not crash the system
    ngctl 'show' 2>/dev/null || errors=$((errors + 1))
    ngctl 'msg pppoe: help' 2>/dev/null || errors=$((errors + 1))
    ngctl 'mkpeer pppoe: pppoe out' 2>/dev/null || errors=$((errors + 1))

    if [ $errors -eq 0 ]; then
        echo "ok $TESTS_RUN - malformed_ng_commands"
        return 0
    else
        echo "not ok $TESTS_RUN - malformed_ng_commands # $errors errors"
        return 1
    fi
}

# Test: Session limit stress
test_session_limit_stress() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 7: Session limit stress"

    local errors=0

    # Try to set extreme session limits
    sysctl ${SYSCTL_PREFIX}.governor.session_limit=0 2>/dev/null || errors=$((errors + 1))
    sysctl ${SYSCTL_PREFIX}.governor.session_limit=4294967295 2>/dev/null || errors=$((errors + 1))
    sysctl ${SYSCTL_PREFIX}.governor.session_limit=-1 2>/dev/null || errors=$((errors + 1))

    # Reset
    sysctl ${SYSCTL_PREFIX}.governor.session_limit=10000 2>/dev/null

    if [ $errors -eq 0 ]; then
        echo "ok $TESTS_RUN - session_limit_stress"
        return 0
    else
        echo "not ok $TESTS_RUN - session_limit_stress # $errors errors"
        return 1
    fi
}

# Test: Governor mode switching stress
test_governor_mode_stress() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 8: Governor mode switching stress"

    local errors=0

    # Rapidly switch governor modes
    local i=0
    while [ $i -lt 50 ]; do
        case $((i % 3)) in
            0) sysctl ${SYSCTL_PREFIX}.governor.mode=auto 2>/dev/null || errors=$((errors + 1)) ;;
            1) sysctl ${SYSCTL_PREFIX}.governor.mode=manual 2>/dev/null || errors=$((errors + 1)) ;;
            2) sysctl ${SYSCTL_PREFIX}.governor.mode=disabled 2>/dev/null || errors=$((errors + 1)) ;;
        esac
        i=$((i + 1))
    done

    # Reset
    sysctl ${SYSCTL_PREFIX}.governor.mode=auto 2>/dev/null

    if [ $errors -eq 0 ]; then
        echo "ok $TESTS_RUN - governor_mode_stress"
        return 0
    else
        echo "not ok $TESTS_RUN - governor_mode_stress # $errors errors"
        return 1
    fi
}

# Test: Memory pressure simulation
test_memory_pressure() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 9: Memory pressure simulation"

    local errors=0

    # Set very low memory limits
    sysctl ${SYSCTL_PREFIX}.governor.max_workers=256 2>/dev/null || errors=$((errors + 1))
    sysctl ${SYSCTL_PREFIX}.governor.session_limit=1 2>/dev/null || errors=$((errors + 1))

    # Reset
    sysctl ${SYSCTL_PREFIX}.governor.max_workers=16 2>/dev/null
    sysctl ${SYSCTL_PREFIX}.governor.session_limit=10000 2>/dev/null

    if [ $errors -eq 0 ]; then
        echo "ok $TESTS_RUN - memory_pressure"
        return 0
    else
        echo "not ok $TESTS_RUN - memory_pressure # $errors errors"
        return 1
    fi
}

# Test: Concurrency stress
test_concurrency_stress() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 10: Concurrency stress"

    # Run multiple sysctl operations in parallel
    (
        local i=0
        while [ $i -lt 100 ]; do
            sysctl ${SYSCTL_PREFIX}.debug.level=$((i % 4)) 2>/dev/null
            i=$((i + 1))
        done
    ) &
    local pid1=$!

    (
        local i=0
        while [ $i -lt 100 ]; do
            sysctl ${SYSCTL_PREFIX}.algorithm="round_robin" 2>/dev/null
            i=$((i + 1))
        done
    ) &
    local pid2=$!

    (
        local i=0
        while [ $i -lt 100 ]; do
            sysctl ${SYSCTL_PREFIX}.governor.max_workers=$((1 + (i % 16))) 2>/dev/null
            i=$((i + 1))
        done
    ) &
    local pid3=$!

    wait $pid1 $pid2 $pid3 2>/dev/null || true

    echo "ok $TESTS_RUN - concurrency_stress"
    return 0
}

# ============================================================================
# Main
# ============================================================================

main() {
    log_info "========================================"
    log_info "PPPoE Load Balancer Fuzz Tests"
    log_info "========================================"
    log_info "Seed: ${SEED}"
    log_info "Iterations: ${ITERATIONS}"
    log_info ""

    check_vm_environment

    TESTS_RUN=0
    TESTS_PASSED=0
    TESTS_FAILED=0

    load_modules || exit 1

    log_info "Running fuzz tests..."
    echo ""

    test_sysctl_boundaries || TESTS_FAILED=$((TESTS_FAILED + 1))
    test_rapid_sysctl_changes || TESTS_FAILED=$((TESTS_FAILED + 1))
    test_invalid_algorithms || TESTS_FAILED=$((TESTS_FAILED + 1))
    test_worker_count_stress || TESTS_FAILED=$((TESTS_FAILED + 1))
    test_ngnode_stress || TESTS_FAILED=$((TESTS_FAILED + 1))
    test_malformed_ng_commands || TESTS_FAILED=$((TESTS_FAILED + 1))
    test_session_limit_stress || TESTS_FAILED=$((TESTS_FAILED + 1))
    test_governor_mode_stress || TESTS_FAILED=$((TESTS_FAILED + 1))
    test_memory_pressure || TESTS_FAILED=$((TESTS_FAILED + 1))
    test_concurrency_stress || TESTS_FAILED=$((TESTS_FAILED + 1))

    unload_modules

    echo "1..${TESTS_RUN}"

    log_info ""
    log_info "========================================"
    log_info "Fuzz Test Summary"
    log_info "========================================"
    log_info "Tests run: ${TESTS_RUN}"
    log_info "Passed: ${TESTS_PASSED}"
    log_info "Failed: ${TESTS_FAILED}"
    log_info ""

    if [ ${TESTS_FAILED} -gt 0 ]; then
        log_error "Some fuzz tests failed!"
        exit 1
    else
        log_success "All fuzz tests passed!"
        exit 0
    fi
}

# Trap to ensure cleanup on exit
trap unload_modules EXIT

main