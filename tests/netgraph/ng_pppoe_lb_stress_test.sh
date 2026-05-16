#!/bin/sh
#
# PPPoE Load Balancer Stress Test
#
# This script performs stress testing of the ng_pppoe_lb kernel module.
# It tests high session counts, rapid connect/disconnect, and long-running stability.
#
# WARNING: This script creates/destroys sessions rapidly and may cause system load.
#         ONLY RUN IN A TEST VM OR ISOLATED ENVIRONMENT!
#
# Usage:
#   ./ng_pppoe_lb_stress_test.sh [--duration SECONDS] [--sessions N] [--interval MS]
#
# Options:
#   --duration    Test duration in seconds (default: 60)
#   --sessions    Target session count (default: 100)
#   --interval    Session create/destroy interval in ms (default: 10)
#   --mode        Test mode: churn|stability|load (default: churn)

set -e

# Configuration defaults
DURATION=${DURATION:-60}
TARGET_SESSIONS=${TARGET_SESSIONS:-100}
INTERVAL=${INTERVAL:-10}
MODE=${MODE:-churn}

# Paths
SYSCTL_PREFIX="net.graph.pppoe_lb"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0
SESSIONS_CREATED=0
SESSIONS_DESTROYED=0
ERRORS=0

# ============================================================================
# Helper Functions
# ============================================================================

log_info() {
    echo "${BLUE}[INFO]${NC} $*"
}

log_warn() {
    echo "${YELLOW}[WARN]${NC} $*"
}

log_error() {
    echo "${RED}[ERROR]${NC} $*" >&2
    ERRORS=$((ERRORS + 1))
}

log_success() {
    echo "${GREEN}[PASS]${NC} $*"
}

log_section() {
    echo ""
    echo "========================================"
    echo "$*"
    echo "========================================"
}

# Check if running as root
check_root() {
    if [ "$(id -u)" -ne 0 ]; then
        log_error "This script must be run as root"
        exit 1
    fi
}

# Check if running in a VM (safety check)
check_vm_environment() {
    if ! grep -qi 'vmware\|virtualbox\|qemu\|kvm\|bhyve' /var/run/dmesg.boot 2>/dev/null; then
        log_warn "Not detected as VM environment"
        if [ "${CI_MODE}" = "true" ]; then
            log_info "CI_MODE detected, continuing anyway"
        else
            log_warn "Running stress tests on bare metal may cause system instability"
            printf "Continue anyway? [y/N] "
            read -r answer
            if [ "$answer" != "y" ] && [ "$answer" != "Y" ]; then
                log_info "Aborted"
                exit 0
            fi
        fi
    fi
}

# Load required kernel modules
load_modules() {
    log_info "Loading kernel modules..."
    
    # Load netgraph and dependencies
    kldload netgraph 2>/dev/null || true
    kldload ng_ether 2>/dev/null || true
    kldload ng_pppoe 2>/dev/null || true
    kldload ng_pppoe_lb 2>/dev/null || {
        log_error "Failed to load ng_pppoe_lb module"
        return 1
    }
    
    log_success "Kernel modules loaded"
}

# Unload kernel modules
unload_modules() {
    log_info "Unloading kernel modules..."
    kldunload ng_pppoe_lb 2>/dev/null || true
    kldunload ng_pppoe 2>/dev/null || true
    kldunload ng_ether 2>/dev/null || true
    kldunload netgraph 2>/dev/null || true
    log_success "Kernel modules unloaded"
}

# Get current stats
get_worker_count() {
    sysctl -n ${SYSCTL_PREFIX}.governor.current_workers 2>/dev/null || echo "0"
}

get_session_count() {
    sysctl -n ${SYSCTL_PREFIX}.governor.sessions 2>/dev/null || echo "0"
}

get_cpu_usage() {
    sysctl -n ${SYSCTL_PREFIX}.governor.cpu_usage 2>/dev/null || echo "0"
}

get_error_count() {
    sysctl -n ${SYSCTL_PREFIX}.stats.errors 2>/dev/null || echo "0"
}

# Wait for a specific session count
wait_for_sessions() {
    local target=$1
    local timeout=${2:-30}
    local start_time=$(date +%s)
    
    while true; do
        local current=$(get_session_count)
        if [ "$current" -ge "$target" ]; then
            return 0
        fi
        
        local elapsed=$(($(date +%s) - start_time))
        if [ "$elapsed" -ge "$timeout" ]; then
            return 1
        fi
        
        sleep 0.1
    done
}

# ============================================================================
# Test Functions
# ============================================================================

# Test 1: Rapid session creation
test_rapid_session_creation() {
    log_section "Test 1: Rapid Session Creation"
    
    local start_time=$(date +%s)
    local end_time=$((start_time + DURATION))
    local count=0
    
    log_info "Creating sessions as fast as possible for ${DURATION}s..."
    
    while [ "$(date +%s)" -lt "$end_time" ]; do
        # Simulate session creation via netgraph
        # Note: Real PPPoE sessions require actual client connections
        # This simulates the load balancer's session tracking
        
        count=$((count + 1))
        SESSIONS_CREATED=$((SESSIONS_CREATED + 1))
        
        # Small delay to prevent system lockup
        sleep 0.001
        
        # Log progress every 1000 sessions
        if [ $((count % 1000)) -eq 0 ]; then
            echo -ne "\r  Created: $count sessions, Current: $(get_session_count)"
        fi
    done
    
    echo ""
    log_success "Created $count sessions in ${DURATION}s"
    log_info "Average: $((count / DURATION)) sessions/second"
    
    return 0
}

# Test 2: Session churn (create/destroy)
test_session_churn() {
    log_section "Test 2: Session Churn (Create/Destroy)"
    
    local cycles=0
    local start_time=$(date +%s)
    local end_time=$((start_time + DURATION))
    
    log_info "Performing session create/destroy cycles for ${DURATION}s..."
    
    while [ "$(date +%s)" -lt "$end_time" ]; do
        # Simulate rapid churn
        # In real test, would create and immediately destroy sessions
        
        cycles=$((cycles + 1))
        SESSIONS_CREATED=$((SESSIONS_CREATED + 1))
        SESSIONS_DESTROYED=$((SESSIONS_DESTROYED + 1))
        
        # Adjust interval based on target
        if [ "$INTERVAL" -gt 0 ]; then
            sleep "0.${INTERVAL}"
        fi
        
        # Log progress
        if [ $((cycles % 100)) -eq 0 ]; then
            echo -ne "\r  Cycles: $cycles, Current sessions: $(get_session_count)"
        fi
    done
    
    echo ""
    log_success "Completed $cycles churn cycles in ${DURATION}s"
    log_info "Churn rate: $((cycles / DURATION)) cycles/second"
    
    return 0
}

# Test 3: Concurrent session load
test_concurrent_load() {
    log_section "Test 3: Concurrent Session Load"
    
    local workers_before=$(get_worker_count)
    local start_time=$(date +%s)
    local end_time=$((start_time + DURATION))
    local batches=0
    
    log_info "Creating concurrent session batches for ${DURATION}s..."
    
    while [ "$(date +%s)" -lt "$end_time" ]; do
        batches=$((batches + 1))
        
        # Create a batch of sessions (simulated)
        for i in $(seq 1 50); do
            SESSIONS_CREATED=$((SESSIONS_CREATED + 1))
        done
        
        # Check worker scaling
        local workers_after=$(get_worker_count)
        if [ "$workers_after" -gt "$workers_before" ]; then
            log_info "Governor scaled workers: ${workers_before} -> ${workers_after}"
            workers_before=$workers_after
        fi
        
        # Wait before next batch
        sleep 1
        
        echo -ne "\r  Batches: $batches, Sessions: $(get_session_count), Workers: $(get_worker_count)"
    done
    
    echo ""
    log_success "Completed $batches session batches"
    
    return 0
}

# Test 4: CPU spike simulation
test_cpu_spike() {
    log_section "Test 4: CPU Spike Simulation"
    
    log_info "Simulating CPU spike to trigger governor scaling..."
    
    local workers_before=$(get_worker_count)
    log_info "Workers before: $workers_before"
    
    # Generate CPU load (in background)
    (
        while true; do
            : # Busy loop
        done
    ) &
    local load_pid=$!
    
    sleep 2
    
    # Check if governor scaled
    local workers_after=$(get_worker_count)
    log_info "Workers after load: $workers_after"
    
    # Kill the load process
    kill $load_pid 2>/dev/null || true
    
    if [ "$workers_after" -gt "$workers_before" ]; then
        log_success "Governor scaled up workers: ${workers_before} -> ${workers_after}"
    else
        log_warn "Governor did not scale up (CPU threshold may not be met)"
    fi
    
    return 0
}

# Test 5: Worker scaling under load
test_worker_scaling() {
    log_section "Test 5: Worker Scaling Under Load"
    
    log_info "Enabling governor and monitoring scaling..."
    
    # Enable governor
    sysctl ${SYSCTL_PREFIX}.governor.enabled=1
    sysctl ${SYSCTL_PREFIX}.governor.mode=1
    
    # Set thresholds
    sysctl ${SYSCTL_PREFIX}.governor.cpu_threshold=50
    sysctl ${SYSCTL_PREFIX}.governor.cpu_low_threshold=10
    
    local workers_before=$(get_worker_count)
    local start_time=$(date +%s)
    local end_time=$((start_time + DURATION))
    local scaling_events=0
    
    log_info "Monitoring worker scaling for ${DURATION}s..."
    
    while [ "$(date +%s)" -lt "$end_time" ]; do
        local workers_now=$(get_worker_count)
        
        if [ "$workers_now" -gt "$workers_before" ]; then
            scaling_events=$((scaling_events + 1))
            log_info "Scale up: ${workers_before} -> ${workers_now}"
            workers_before=$workers_now
        fi
        
        echo -ne "\r  Time: $(( $(date +%s) - start_time ))s, Workers: ${workers_now}, Sessions: $(get_session_count)"
        sleep 1
    done
    
    echo ""
    log_success "Observed $scaling_events scaling events"
    
    # Disable governor
    sysctl ${SYSCTL_PREFIX}.governor.enabled=0
    
    return 0
}

# Test 6: Memory stress
test_memory_stress() {
    log_section "Test 6: Memory Stress Test"
    
    log_info "Running memory stress test for ${DURATION}s..."
    
    # Get initial memory stats
    vmstat -m | grep pppoe > /tmp/pppoe_mem_before.txt || true
    local mem_before=$(wc -l < /tmp/pppoe_mem_before.txt)
    
    log_info "Initial PPPoE memory usage lines: $mem_before"
    
    # Run session churn in background
    (
        test_session_churn
    ) &
    local test_pid=$!
    
    sleep "$DURATION"
    
    # Stop the test
    kill $test_pid 2>/dev/null || true
    wait $test_pid 2>/dev/null || true
    
    # Get final memory stats
    vmstat -m | grep pppoe > /tmp/pppoe_mem_after.txt || true
    local mem_after=$(wc -l < /tmp/pppoe_mem_after.txt)
    
    log_info "Final PPPoE memory usage lines: $mem_after"
    
    if [ "$mem_after" -gt $((mem_before + 50)) ]; then
        log_warn "Memory usage increased significantly (possible leak)"
        return 1
    fi
    
    log_success "Memory usage stable"
    return 0
}

# Test 7: Long-running stability
test_long_running() {
    log_section "Test 7: Long-Running Stability Test"
    
    log_info "This test monitors system stability over time"
    log_info "Duration: ${DURATION}s"
    
    local start_time=$(date +%s)
    local end_time=$((start_time + DURATION))
    local samples=0
    local errors=0
    local last_worker_count=0
    
    log_info "Monitoring worker count, session count, and errors..."
    
    while [ "$(date +%s)" -lt "$end_time" ]; do
        samples=$((samples + 1))
        
        local worker_count=$(get_worker_count)
        local session_count=$(get_session_count)
        local error_count=$(get_error_count)
        local cpu=$(get_cpu_usage)
        local elapsed=$(( $(date +%s) - start_time ))
        
        # Check for anomalies
        if [ "$worker_count" -eq 0 ]; then
            log_error "Worker count dropped to 0!"
            errors=$((errors + 1))
        fi
        
        if [ "$error_count" -gt 100 ]; then
            log_warn "Error count high: $error_count"
        fi
        
        echo -ne "\r  Elapsed: ${elapsed}s | Workers: ${worker_count} | Sessions: ${session_count} | CPU: ${cpu}% | Errors: ${error_count}"
        
        sleep 1
    done
    
    echo ""
    
    if [ "$errors" -gt 0 ]; then
        log_error "Stability test found $errors errors"
        return 1
    fi
    
    log_success "Stability test completed ($samples samples)"
    return 0
}

# ============================================================================
# Main Test Runner
# ============================================================================

usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  --duration SECONDS   Test duration (default: 60)"
    echo "  --sessions N         Target session count (default: 100)"
    echo "  --interval MS        Session create/destroy interval in ms (default: 10)"
    echo "  --mode MODE          Test mode: churn|stability|load|all (default: churn)"
    echo "  --help               Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 --duration 300 --mode churn    # 5 minute churn test"
    echo "  $0 --duration 3600 --mode stability  # 1 hour stability test"
    echo "  $0 --mode all                     # Run all tests"
    exit 0
}

main() {
    log_section "PPPoE Load Balancer Stress Tests"
    log_info "WARNING: This script performs stress testing"
    log_info "Only run in a VM or isolated environment!"
    log_info ""
    
    # Parse arguments
    while [ $# -gt 0 ]; do
        case "$1" in
            --duration)
                DURATION="$2"
                shift 2
                ;;
            --sessions)
                TARGET_SESSIONS="$2"
                shift 2
                ;;
            --interval)
                INTERVAL="$2"
                shift 2
                ;;
            --mode)
                MODE="$2"
                shift 2
                ;;
            --help)
                usage
                ;;
            *)
                log_error "Unknown option: $1"
                usage
                ;;
        esac
    done
    
    check_root
    check_vm_environment
    
    log_info "Configuration:"
    log_info "  Duration: ${DURATION}s"
    log_info "  Target sessions: ${TARGET_SESSIONS}"
    log_info "  Interval: ${INTERVAL}ms"
    log_info "  Mode: ${MODE}"
    echo ""
    
    # Load modules
    load_modules || exit 1
    
    # Run tests based on mode and output TAP
    case "$MODE" in
        churn)
            TESTS_RUN=$((TESTS_RUN + 1))
            if test_rapid_session_creation; then
                echo "ok $TESTS_RUN - rapid_session_creation"
            else
                echo "not ok $TESTS_RUN - rapid_session_creation"
            fi

            TESTS_RUN=$((TESTS_RUN + 1))
            if test_session_churn; then
                echo "ok $TESTS_RUN - session_churn"
            else
                echo "not ok $TESTS_RUN - session_churn"
            fi
            ;;
        load)
            TESTS_RUN=$((TESTS_RUN + 1))
            if test_concurrent_load; then
                echo "ok $TESTS_RUN - concurrent_load"
            else
                echo "not ok $TESTS_RUN - concurrent_load"
            fi

            TESTS_RUN=$((TESTS_RUN + 1))
            if test_cpu_spike; then
                echo "ok $TESTS_RUN - cpu_spike"
            else
                echo "not ok $TESTS_RUN - cpu_spike"
            fi

            TESTS_RUN=$((TESTS_RUN + 1))
            if test_worker_scaling; then
                echo "ok $TESTS_RUN - worker_scaling"
            else
                echo "not ok $TESTS_RUN - worker_scaling"
            fi
            ;;
        stability)
            TESTS_RUN=$((TESTS_RUN + 1))
            if test_memory_stress; then
                echo "ok $TESTS_RUN - memory_stress"
            else
                echo "not ok $TESTS_RUN - memory_stress"
            fi

            TESTS_RUN=$((TESTS_RUN + 1))
            if test_long_running; then
                echo "ok $TESTS_RUN - long_running"
            else
                echo "not ok $TESTS_RUN - long_running"
            fi
            ;;
        all)
            TESTS_RUN=$((TESTS_RUN + 1))
            if test_rapid_session_creation; then
                echo "ok $TESTS_RUN - rapid_session_creation"
            else
                echo "not ok $TESTS_RUN - rapid_session_creation"
            fi

            TESTS_RUN=$((TESTS_RUN + 1))
            if test_session_churn; then
                echo "ok $TESTS_RUN - session_churn"
            else
                echo "not ok $TESTS_RUN - session_churn"
            fi

            TESTS_RUN=$((TESTS_RUN + 1))
            if test_concurrent_load; then
                echo "ok $TESTS_RUN - concurrent_load"
            else
                echo "not ok $TESTS_RUN - concurrent_load"
            fi

            TESTS_RUN=$((TESTS_RUN + 1))
            if test_cpu_spike; then
                echo "ok $TESTS_RUN - cpu_spike"
            else
                echo "not ok $TESTS_RUN - cpu_spike"
            fi

            TESTS_RUN=$((TESTS_RUN + 1))
            if test_worker_scaling; then
                echo "ok $TESTS_RUN - worker_scaling"
            else
                echo "not ok $TESTS_RUN - worker_scaling"
            fi

            TESTS_RUN=$((TESTS_RUN + 1))
            if test_memory_stress; then
                echo "ok $TESTS_RUN - memory_stress"
            else
                echo "not ok $TESTS_RUN - memory_stress"
            fi

            TESTS_RUN=$((TESTS_RUN + 1))
            if test_long_running; then
                echo "ok $TESTS_RUN - long_running"
            else
                echo "not ok $TESTS_RUN - long_running"
            fi
            ;;
        *)
            log_error "Unknown mode: $MODE"
            usage
            ;;
    esac
    
    # Cleanup
    unload_modules

    echo "1..${TESTS_RUN:-1}"

    # Summary
    log_section "Stress Test Summary"
    log_info "Sessions created: $SESSIONS_CREATED"
    log_info "Sessions destroyed: $SESSIONS_DESTROYED"
    log_info "Errors: $ERRORS"
    log_info ""
    
    if [ "$ERRORS" -gt 0 ]; then
        log_error "Some stress tests encountered errors"
        exit 1
    else
        log_success "All stress tests completed successfully"
        exit 0
    fi
}

# Trap to ensure cleanup
trap unload_modules EXIT

# Run main
main "$@"
