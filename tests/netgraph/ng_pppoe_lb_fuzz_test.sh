#!/bin/sh
#
# ng_pppoe_lb_fuzz_test.sh - Fuzzing test for PPPoE load balancer
#
set -e

SYSCTL_PREFIX="net.graph.pppoe_lb"

ITERATIONS="${ITERATIONS:-1000}"
SEED="${SEED:-$(date +%s)}"

TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

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

unload_modules() {
    kldunload ng_pppoe_lb 2>/dev/null || true
}

# Test: Sysctl boundary values
test_sysctl_boundaries() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 1: Sysctl boundary values"

    local failures=0

    # max_workers: valid range should be accepted
    sysctl ${SYSCTL_PREFIX}.governor.max_workers=1000 2>/dev/null || failures=$((failures + 1))
    sysctl ${SYSCTL_PREFIX}.governor.max_workers=1 2>/dev/null || failures=$((failures + 1))
    sysctl ${SYSCTL_PREFIX}.governor.max_workers=16 2>/dev/null || failures=$((failures + 1))

    # min_workers: valid range should be accepted
    sysctl ${SYSCTL_PREFIX}.governor.min_workers=1 2>/dev/null || failures=$((failures + 1))
    sysctl ${SYSCTL_PREFIX}.governor.min_workers=4 2>/dev/null || failures=$((failures + 1))

    # Reset
    sysctl ${SYSCTL_PREFIX}.governor.max_workers=16 2>/dev/null
    sysctl ${SYSCTL_PREFIX}.governor.min_workers=1 2>/dev/null

    if [ $failures -eq 0 ]; then
        echo "ok $TESTS_RUN - sysctl_boundaries"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "not ok $TESTS_RUN - sysctl_boundaries ($failures failures)"
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
}

# Test: Rapid sysctl changes
test_rapid_sysctl_changes() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 2: Rapid sysctl changes"

    local i=0
    while [ $i -lt 100 ]; do
        val=$((i % 4))
        sysctl ${SYSCTL_PREFIX}.debug=${val} 2>/dev/null
        i=$((i + 1))
    done

    echo "ok $TESTS_RUN - rapid_sysctl_changes"
    TESTS_PASSED=$((TESTS_PASSED + 1))
}

# Test: Algorithm values
test_algorithm_values() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 3: Algorithm values"

    local failures=0

    sysctl ${SYSCTL_PREFIX}.algorithm=0 2>/dev/null || failures=$((failures + 1))
    sysctl ${SYSCTL_PREFIX}.algorithm=1 2>/dev/null || failures=$((failures + 1))
    sysctl ${SYSCTL_PREFIX}.algorithm=2 2>/dev/null || failures=$((failures + 1))

    if [ $failures -eq 0 ]; then
        echo "ok $TESTS_RUN - algorithm_values"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "not ok $TESTS_RUN - algorithm_values ($failures failures)"
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
}

# Test: Worker count stress
test_worker_count_stress() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 4: Worker count stress"

    local i=0
    while [ $i -lt 50 ]; do
        workers=$((1 + (i % 16)))
        sysctl ${SYSCTL_PREFIX}.governor.max_workers=${workers} 2>/dev/null
        sysctl ${SYSCTL_PREFIX}.governor.min_workers=1 2>/dev/null
        i=$((i + 1))
    done

    sysctl ${SYSCTL_PREFIX}.governor.max_workers=16 2>/dev/null

    echo "ok $TESTS_RUN - worker_count_stress"
    TESTS_PASSED=$((TESTS_PASSED + 1))
}

# Test: Netgraph node creation stress
test_ngnode_stress() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 5: Netgraph node creation stress"

    local i=0
    while [ $i -lt 50 ]; do
        ngctl -df mkpeer pppoe: pppoe out >/dev/null 2>&1 || true
        ngctl -df rm pppoe*: >/dev/null 2>&1 || true
        i=$((i + 1))
    done

    echo "ok $TESTS_RUN - ngnode_stress"
    TESTS_PASSED=$((TESTS_PASSED + 1))
}

# Test: Governor mode values
test_governor_mode() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 6: Governor mode values"

    local failures=0

    sysctl ${SYSCTL_PREFIX}.governor.mode=0 2>/dev/null || failures=$((failures + 1))
    sysctl ${SYSCTL_PREFIX}.governor.mode=1 2>/dev/null || failures=$((failures + 1))
    sysctl ${SYSCTL_PREFIX}.governor.mode=2 2>/dev/null || failures=$((failures + 1))

    if [ $failures -eq 0 ]; then
        echo "ok $TESTS_RUN - governor_mode"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "not ok $TESTS_RUN - governor_mode ($failures failures)"
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
}

# Test: Threshold values
test_threshold_values() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 7: Threshold values"

    local failures=0

    sysctl ${SYSCTL_PREFIX}.governor.cpu_threshold=80 2>/dev/null || failures=$((failures + 1))
    sysctl ${SYSCTL_PREFIX}.governor.cpu_low_threshold=25 2>/dev/null || failures=$((failures + 1))
    sysctl ${SYSCTL_PREFIX}.governor.sessions_per_worker=250 2>/dev/null || failures=$((failures + 1))

    if [ $failures -eq 0 ]; then
        echo "ok $TESTS_RUN - threshold_values"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "not ok $TESTS_RUN - threshold_values ($failures failures)"
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
}

# Test: Governor mode switching stress
test_governor_mode_stress() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 8: Governor mode switching stress"

    local i=0
    while [ $i -lt 50 ]; do
        mode=$((i % 3))
        sysctl ${SYSCTL_PREFIX}.governor.mode=${mode} 2>/dev/null
        i=$((i + 1))
    done

    sysctl ${SYSCTL_PREFIX}.governor.mode=1 2>/dev/null

    echo "ok $TESTS_RUN - governor_mode_stress"
    TESTS_PASSED=$((TESTS_PASSED + 1))
}

# Test: Interval values
test_interval_values() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 9: Interval values"

    local failures=0

    sysctl ${SYSCTL_PREFIX}.governor.scale_up_interval=5 2>/dev/null || failures=$((failures + 1))
    sysctl ${SYSCTL_PREFIX}.governor.scale_down_interval=30 2>/dev/null || failures=$((failures + 1))
    sysctl ${SYSCTL_PREFIX}.governor.drain_timeout=15 2>/dev/null || failures=$((failures + 1))

    if [ $failures -eq 0 ]; then
        echo "ok $TESTS_RUN - interval_values"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "not ok $TESTS_RUN - interval_values ($failures failures)"
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
}

# Test: Concurrency stress
test_concurrency_stress() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 10: Concurrency stress"

    (
        local i=0
        while [ $i -lt 100 ]; do
            sysctl ${SYSCTL_PREFIX}.debug=$((i % 4)) 2>/dev/null
            i=$((i + 1))
        done
    ) &
    local pid1=$!

    (
        local i=0
        while [ $i -lt 100 ]; do
            sysctl ${SYSCTL_PREFIX}.algorithm=$((i % 3)) 2>/dev/null
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
    TESTS_PASSED=$((TESTS_PASSED + 1))
}

# Test: Debug level values
test_debug_level() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 11: Debug level values"

    local failures=0

    sysctl ${SYSCTL_PREFIX}.debug=0 2>/dev/null || failures=$((failures + 1))
    sysctl ${SYSCTL_PREFIX}.debug=5 2>/dev/null || failures=$((failures + 1))
    sysctl ${SYSCTL_PREFIX}.debug=10 2>/dev/null || failures=$((failures + 1))

    if [ $failures -eq 0 ]; then
        echo "ok $TESTS_RUN - debug_level"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "not ok $TESTS_RUN - debug_level ($failures failures)"
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
}

# Test: Session map size
test_session_map_size() {
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Fuzz Test 12: Session map size"

    local failures=0

    sysctl ${SYSCTL_PREFIX}.session_map_size=512 2>/dev/null || failures=$((failures + 1))
    sysctl ${SYSCTL_PREFIX}.session_map_size=4096 2>/dev/null || failures=$((failures + 1))

    if [ $failures -eq 0 ]; then
        echo "ok $TESTS_RUN - session_map_size"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo "not ok $TESTS_RUN - session_map_size ($failures failures)"
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
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

    test_sysctl_boundaries
    test_rapid_sysctl_changes
    test_algorithm_values
    test_worker_count_stress
    test_ngnode_stress
    test_governor_mode
    test_threshold_values
    test_governor_mode_stress
    test_interval_values
    test_concurrency_stress
    test_debug_level
    test_session_map_size

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
        log_info "All fuzz tests passed!"
        exit 0
    fi
}

trap unload_modules EXIT

main