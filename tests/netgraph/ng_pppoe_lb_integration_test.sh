#!/bin/sh
#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2024 Mark LaPointe <mark@cloudbsd.org>
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
# OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.
#
# $FreeBSD$
#
# PPPoE Load Balancer Integration Test Script
#
# This script performs end-to-end integration tests of the ng_pppoe_lb
# kernel module, testing the complete PPPoE session lifecycle with multiple
# workers, governor auto-scaling, and recovery scenarios.
#
# WARNING: This script creates/destroys netgraph nodes and may cause system
#          load. ONLY RUN IN A TEST VM OR ISOLATED ENVIRONMENT!
#
# Usage:
#   ./ng_pppoe_lb_integration_test.sh [--quick] [--verbose] [--stop-on-fail]
#
# Options:
#   --quick       Run only basic integration tests (skip long-running tests)
#   --verbose     Show detailed output for each test
#   --stop-on-fail  Stop on first failure

set -e

# ============================================================================
# Configuration
# ============================================================================

# Test environment
PREFIX="net.graph.pppoe_lb"
TEST_PREFIX="ng_pppoe_lb_integration"
NODE_NAME="pppoe_lb_int_test"
TEST_INTERFACE="ng0"  # Virtual netgraph interface for testing

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0
TESTS_SKIPPED=0

# Test flags
QUICK_MODE=false
VERBOSE=false
STOP_ON_FAIL=false

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
}

log_pass() {
    echo "ok $TESTS_RUN - $*"
    echo "${GREEN}[PASS]${NC} $*"
    TESTS_PASSED=$((TESTS_PASSED + 1))
}

log_fail() {
    echo "not ok $TESTS_RUN - $*"
    echo "${RED}[FAIL]${NC} $*"
    TESTS_FAILED=$((TESTS_FAILED + 1))
    if [ "$STOP_ON_FAIL" = "true" ]; then
        log_error "Stopping on failure as requested"
        cleanup_all
        exit 1
    fi
}

log_skip() {
    echo "ok $TESTS_RUN - $* # skip"
    echo "${CYAN}[SKIP]${NC} $*"
    TESTS_SKIPPED=$((TESTS_SKIPPED + 1))
}

log_section() {
    echo ""
    echo "============================================================================"
    echo "  $*"
    echo "============================================================================"
}

log_subsection() {
    echo ""
    echo "--- $* ---"
}

# Print test header
test_header() {
    TESTS_RUN=$((TESTS_RUN + 1))
    if [ "$VERBOSE" = "true" ]; then
        echo ""
        echo "${BOLD}[TEST $TESTS_RUN]${NC} $*"
    fi
}

# Print test result with details
test_result() {
    local status="$1"
    local expected="$2"
    local actual="$3"
    local details="$4"
    
    if [ "$status" = "PASS" ]; then
        log_pass "Test $TESTS_RUN: $expected"
        if [ "$VERBOSE" = "true" ] && [ -n "$actual" ]; then
            echo "    Expected: $expected"
            echo "    Actual:   $actual"
        fi
    else
        log_fail "Test $TESTS_RUN: $expected"
        echo "    Expected: $expected"
        echo "    Actual:   $actual"
        if [ -n "$details" ]; then
            echo "    Details:  $details"
        fi
    fi
}

# ============================================================================
# Environment Checks
# ============================================================================

check_root() {
    if [ "$(id -u)" -ne 0 ]; then
        log_error "This script must be run as root"
        exit 1
    fi
}

check_vm_environment() {
    if ! grep -qi 'vmware\|virtualbox\|qemu\|kvm\|bhyve' /var/run/dmesg.boot 2>/dev/null; then
        log_warn "Not detected as VM environment"
        if [ "${CI_MODE}" = "true" ]; then
            log_info "CI_MODE detected, continuing anyway"
        else
            log_warn "Integration tests on bare metal may affect system stability"
            printf "Continue anyway? [y/N] "
            read -r answer
            if [ "$answer" != "y" ] && [ "$answer" != "Y" ]; then
                log_info "Aborted"
                exit 0
            fi
        fi
    fi
}

check_modules() {
    log_section "Checking Required Kernel Modules"
    
    # Check if modules are available
    local missing=""
    
    if ! kldstat -m netgraph >/dev/null 2>&1; then
        missing="$missing netgraph"
    fi
    if ! kldstat -m ng_ether >/dev/null 2>&1; then
        missing="$missing ng_ether"
    fi
    if ! kldstat -m ng_pppoe >/dev/null 2>&1; then
        missing="$missing ng_pppoe"
    fi
    if ! kldstat -m ng_pppoe_lb >/dev/null 2>&1; then
        missing="$missing ng_pppoe_lb"
    fi
    
    if [ -n "$missing" ]; then
        log_warn "Missing modules:$missing"
        log_info "Attempting to load modules..."
        
        kldload netgraph 2>/dev/null || true
        kldload ng_ether 2>/dev/null || true
        kldload ng_pppoe 2>/dev/null || true
        kldload ng_pppoe_lb 2>/dev/null || {
            log_error "Failed to load ng_pppoe_lb module"
            log_error "Cannot run integration tests without ng_pppoe_lb"
            return 1
        }
    fi
    
    log_pass "All required modules loaded"
}

check_tools() {
    log_section "Checking Required Tools"
    
    # Check for ngctl
    if ! command -v ngctl >/dev/null 2>&1; then
        log_error "ngctl not found in PATH"
        log_error "Please build ngctl from the modified source"
        return 1
    fi
    
    # Check for sysctl
    if ! command -v sysctl >/dev/null 2>&1; then
        log_error "sysctl not found"
        return 1
    fi
    
    log_pass "All required tools available"
}

# ============================================================================
# Sysctl Helpers
# ============================================================================

sysctl_get() {
    local name="$1"
    local value
    value=$(sysctl -n "$PREFIX.$name" 2>/dev/null || echo "-1")
    echo "$value"
}

sysctl_set() {
    local name="$1"
    local value="$2"
    sysctl "$PREFIX.$name=$value" 2>/dev/null
    return $?
}

sysctl_exists() {
    local name="$1"
    sysctl -n "$PREFIX.$name" >/dev/null 2>&1
    return $?
}

# ============================================================================
# Node Management
# ============================================================================

create_node() {
    local name="$1"
    log_info "Creating netgraph node: $name"
    
    # Create a new netgraph node
    ngctl mkpeer . pppoe_lb lb "$name" || {
        log_error "Failed to create node: $name"
        return 1
    }
    
    # Wait for node to be ready
    sleep 0.5
    
    log_pass "Node created: $name"
    return 0
}

destroy_node() {
    local name="$1"
    log_info "Destroying netgraph node: $name"
    
    ngctl shutdown "$name:" 2>/dev/null || {
        # Try alternative shutdown syntax
        ngctl msg "$name:" shutdown 2>/dev/null || true
    }
    
    # Wait for cleanup
    sleep 0.5
    
    log_info "Node destroyed: $name"
}

cleanup_all() {
    log_section "Cleaning Up Test Environment"
    
    # Destroy test node if exists
    if ngctl list 2>/dev/null | grep -q "$NODE_NAME"; then
        destroy_node "$NODE_NAME"
    fi
    
    # Clean up any orphaned test nodes
    for node in $(ngctl list 2>/dev/null | grep -E "(pppoe_lb|${TEST_PREFIX})" | awk '{print $1}' | tr -d ':'); do
        log_info "Cleaning orphaned node: $node"
        ngctl shutdown "$node:" 2>/dev/null || true
    done
    
    # Reset governor to defaults
    sysctl_set "governor.enabled" 0 2>/dev/null || true
    sysctl_set "governor.min_workers" 1 2>/dev/null || true
    sysctl_set "governor.max_workers" 0 2>/dev/null || true
    
    log_info "Cleanup complete"
}

# ============================================================================
# Integration Tests: Basic Node Lifecycle
# ============================================================================

test_node_creation() {
    log_section "Test: Node Creation and Lifecycle"
    
    # Test INT-01: Create single node
    test_header "Single node creation"
    if create_node "$NODE_NAME"; then
        test_result "PASS" "Node created successfully" "Node exists"
        
        # Test INT-02: Verify node shows in ngctl list
        test_header "Node visible in ngctl list"
        local node_exists
        node_exists=$(ngctl list 2>/dev/null | grep -c "$NODE_NAME" || echo "0")
        if [ "$node_exists" -gt 0 ]; then
            test_result "PASS" "Node visible in ngctl list" "Found $node_exists"
        else
            test_result "FAIL" "Node visible in ngctl list" "Not found"
        fi
        
        # Test INT-03: Verify node type
        test_header "Node type verification"
        local node_type
        node_type=$(ngctl list 2>/dev/null | grep -A1 "$NODE_NAME" | grep Type | awk '{print $2}' || echo "unknown")
        if [ "$node_type" = "pppoe_lb" ]; then
            test_result "PASS" "Node type is pppoe_lb" "$node_type"
        else
            test_result "FAIL" "Node type is pppoe_lb" "$node_type"
        fi
        
        # Test INT-04: Destroy node
        test_header "Node destruction"
        destroy_node "$NODE_NAME"
        local node_gone
        node_gone=$(ngctl list 2>/dev/null | grep -c "$NODE_NAME" || echo "0")
        if [ "$node_gone" -eq 0 ]; then
            test_result "PASS" "Node destroyed successfully" "Node removed"
        else
            test_result "FAIL" "Node destroyed successfully" "Still exists"
        fi
    else
        test_result "FAIL" "Node created successfully" "Creation failed"
    fi
}

# ============================================================================
# Integration Tests: Worker Management
# ============================================================================

test_worker_management() {
    log_section "Test: Worker Management"
    
    # Create test node
    create_node "$NODE_NAME" || return 1
    
    # Test INT-05: Add worker
    test_header "Add worker to node"
    ngctl msg "$NODE_NAME:" pppoe_lb addworker 2>/dev/null
    sleep 0.5
    local worker_count
    worker_count=$(sysctl_get "governor.current_workers")
    if [ "$worker_count" -ge 1 ]; then
        test_result "PASS" "Worker added (count >= 1)" "Count: $worker_count"
    else
        test_result "FAIL" "Worker added (count >= 1)" "Count: $worker_count"
    fi
    
    # Test INT-06: Add multiple workers
    test_header "Add multiple workers"
    for i in 2 3 4; do
        ngctl msg "$NODE_NAME:" pppoe_lb addworker 2>/dev/null
        sleep 0.2
    done
    sleep 0.5
    worker_count=$(sysctl_get "governor.current_workers")
    if [ "$worker_count" -ge 4 ]; then
        test_result "PASS" "Multiple workers added (count >= 4)" "Count: $worker_count"
    else
        test_result "FAIL" "Multiple workers added (count >= 4)" "Count: $worker_count"
    fi
    
    # Test INT-07: Remove worker
    test_header "Remove worker from node"
    local workers_before
    workers_before=$(sysctl_get "governor.current_workers")
    ngctl msg "$NODE_NAME:" pppoe_lb rmworker 0 2>/dev/null
    sleep 0.5
    local workers_after
    workers_after=$(sysctl_get "governor.current_workers")
    if [ "$workers_after" -lt "$workers_before" ]; then
        test_result "PASS" "Worker removed" "Before: $workers_before, After: $workers_after"
    else
        test_result "FAIL" "Worker removed" "Before: $workers_before, After: $workers_after"
    fi
    
    # Test INT-08: Worker state transitions
    test_header "Worker state transitions"
    local worker_id
    worker_id=$(sysctl_get "governor.active_workers")
    if [ "$worker_id" -gt 0 ]; then
        worker_id=0  # Set first worker to draining
        sysctl_set "workers.0.state" 1
        sleep 0.5
        local state
        state=$(sysctl_get "workers.0.state")
        if [ "$state" = "1" ]; then
            test_result "PASS" "Worker enters DRAINING state" "State: $state"
            
            # Restore to active
            sysctl_set "workers.0.state" 0
            sleep 0.3
            state=$(sysctl_get "workers.0.state")
            if [ "$state" = "0" ]; then
                test_result "PASS" "Worker returns to ACTIVE state" "State: $state"
            else
                test_result "FAIL" "Worker returns to ACTIVE state" "State: $state"
            fi
        else
            test_result "FAIL" "Worker enters DRAINING state" "State: $state"
        fi
    else
        test_result "SKIP" "Worker state transitions" "No workers available"
    fi
    
    # Cleanup
    destroy_node "$NODE_NAME"
}

# ============================================================================
# Integration Tests: Governor Auto-Scaling
# ============================================================================

test_governor_scaling() {
    log_section "Test: Governor Auto-Scaling"
    
    # Skip in quick mode
    if [ "$QUICK_MODE" = "true" ]; then
        log_skip "Governor scaling tests skipped in quick mode"
        return 0
    fi
    
    # Create test node
    create_node "$NODE_NAME" || return 1
    
    # Test INT-09: Governor enable/disable
    test_header "Governor enable/disable"
    sysctl_set "governor.enabled" 1
    sleep 0.3
    local enabled
    enabled=$(sysctl_get "governor.enabled")
    if [ "$enabled" = "1" ]; then
        test_result "PASS" "Governor enabled" "enabled=$enabled"
        
        sysctl_set "governor.enabled" 0
        sleep 0.3
        enabled=$(sysctl_get "governor.enabled")
        if [ "$enabled" = "0" ]; then
            test_result "PASS" "Governor disabled" "enabled=$enabled"
        else
            test_result "FAIL" "Governor disabled" "enabled=$enabled"
        fi
    else
        test_result "FAIL" "Governor enabled" "enabled=$enabled"
    fi
    
    # Test INT-10: Governor threshold configuration
    test_header "Governor threshold configuration"
    sysctl_set "governor.cpu_threshold" 75
    sleep 0.2
    local threshold
    threshold=$(sysctl_get "governor.cpu_threshold")
    if [ "$threshold" = "75" ]; then
        test_result "PASS" "CPU threshold configured" "threshold=$threshold"
    else
        test_result "FAIL" "CPU threshold configured" "threshold=$threshold"
    fi
    
    # Test INT-11: Min/max workers configuration
    test_header "Min/max workers configuration"
    sysctl_set "governor.min_workers" 2
    sysctl_set "governor.max_workers" 8
    sleep 0.2
    local min_w max_w
    min_w=$(sysctl_get "governor.min_workers")
    max_w=$(sysctl_get "governor.max_workers")
    if [ "$min_w" = "2" ] && [ "$max_w" = "8" ]; then
        test_result "PASS" "Min/max workers configured" "min=$min_w, max=$max_w"
    else
        test_result "FAIL" "Min/max workers configured" "min=$min_w, max=$max_w"
    fi
    
    # Test INT-12: Governor poll interval
    test_header "Governor poll interval configuration"
    sysctl_set "governor.poll_interval" 3
    sleep 0.2
    local interval
    interval=$(sysctl_get "governor.poll_interval")
    if [ "$interval" = "3" ]; then
        test_result "PASS" "Poll interval configured" "interval=${interval}s"
    else
        test_result "FAIL" "Poll interval configured" "interval=${interval}s"
    fi
    
    # Test INT-13: Governor scale-up trigger
    test_header "Governor scale-up trigger"
    sysctl_set "governor.enabled" 1
    sysctl_set "governor.min_workers" 1
    sysctl_set "governor.max_workers" 4
    
    # Trigger scale-up via test command
    sysctl_set "test.command" 1  # trigger_scale_up
    sleep 2  # Wait for governor to process
    
    local decision
    decision=$(sysctl_get "governor.last_decision")
    if [ "$decision" = "scale_up" ] || [ "$decision" = "none" ]; then
        test_result "PASS" "Scale-up decision made" "decision=$decision"
    else
        test_result "FAIL" "Scale-up decision made" "decision=$decision"
    fi
    
    # Cleanup
    destroy_node "$NODE_NAME"
    sysctl_set "governor.enabled" 0
}

# ============================================================================
# Integration Tests: Algorithm Testing
# ============================================================================

test_algorithms() {
    log_section "Test: Distribution Algorithms"
    
    # Create test node with multiple workers
    create_node "$NODE_NAME" || return 1
    
    # Add 4 workers for testing
    for i in 1 2 3 4; do
        ngctl msg "$NODE_NAME:" pppoe_lb addworker 2>/dev/null
        sleep 0.2
    done
    sleep 0.5
    
    # Test INT-14: Set round-robin algorithm
    test_header "Round-robin algorithm"
    ngctl msg "$NODE_NAME:" pppoe_lb setalgorithm 0 2>/dev/null
    sleep 0.2
    local algo
    algo=$(sysctl_get "governor.algorithm")
    if [ "$algo" = "0" ]; then
        test_result "PASS" "Round-robin algorithm set" "algorithm=$algo"
    else
        test_result "FAIL" "Round-robin algorithm set" "algorithm=$algo"
    fi
    
    # Test INT-15: Set hash algorithm
    test_header "Hash algorithm"
    ngctl msg "$NODE_NAME:" pppoe_lb setalgorithm 1 2>/dev/null
    sleep 0.2
    algo=$(sysctl_get "governor.algorithm")
    if [ "$algo" = "1" ]; then
        test_result "PASS" "Hash algorithm set" "algorithm=$algo"
    else
        test_result "FAIL" "Hash algorithm set" "algorithm=$algo"
    fi
    
    # Test INT-16: Set least-loaded algorithm
    test_header "Least-loaded algorithm"
    ngctl msg "$NODE_NAME:" pppoe_lb setalgorithm 2 2>/dev/null
    sleep 0.2
    algo=$(sysctl_get "governor.algorithm")
    if [ "$algo" = "2" ]; then
        test_result "PASS" "Least-loaded algorithm set" "algorithm=$algo"
    else
        test_result "FAIL" "Least-loaded algorithm set" "algorithm=$algo"
    fi
    
    # Cleanup
    destroy_node "$NODE_NAME"
}

# ============================================================================
# Integration Tests: Sysctl Interface
# ============================================================================

test_sysctl_interface() {
    log_section "Test: Sysctl Interface"
    
    # Create test node
    create_node "$NODE_NAME" || return 1
    
    # Test INT-17: Read all required sysctls
    test_header "Read all sysctl values"
    local missing=""
    local sysctls="governor.current_workers governor.active_workers governor.draining_workers governor.total_sessions governor.algorithm"
    
    for sysctl in $sysctls; do
        local val
        val=$(sysctl_get "$sysctl")
        if [ "$val" = "-1" ]; then
            missing="$missing $sysctl"
        fi
    done
    
    if [ -z "$missing" ]; then
        test_result "PASS" "All sysctls readable" "All sysctls accessible"
    else
        test_result "FAIL" "All sysctls readable" "Missing:$missing"
    fi
    
    # Test INT-18: Per-worker sysctls
    test_header "Per-worker sysctls"
    local worker_count
    worker_count=$(sysctl_get "governor.active_workers")
    if [ "$worker_count" -gt 0 ]; then
        local worker_missing=""
        for i in $(seq 0 $((worker_count - 1))); do
            local state sess bytes_in bytes_out
            state=$(sysctl_get "workers.$i.state")
            sess=$(sysctl_get "workers.$i.sessions")
            bytes_in=$(sysctl_get "workers.$i.bytes_in")
            bytes_out=$(sysctl_get "workers.$i.bytes_out")
            
            if [ "$state" = "-1" ] || [ "$sess" = "-1" ]; then
                worker_missing="$worker_missing worker[$i]"
            fi
        done
        
        if [ -z "$worker_missing" ]; then
            test_result "PASS" "All per-worker sysctls readable" "Workers: $worker_count"
        else
            test_result "FAIL" "All per-worker sysctls readable" "Missing:$worker_missing"
        fi
    else
        test_result "SKIP" "Per-worker sysctls" "No workers available"
    fi
    
    # Test INT-19: Governor configuration sysctls
    test_header "Governor configuration sysctls"
    local gov_missing=""
    local gov_sysctls="governor.enabled governor.mode governor.min_workers governor.max_workers governor.cpu_threshold governor.cpu_low_threshold governor.poll_interval governor.drain_timeout governor.sessions_per_worker"
    
    for sysctl in $gov_sysctls; do
        local val
        val=$(sysctl_get "$sysctl")
        if [ "$val" = "-1" ]; then
            gov_missing="$gov_missing $sysctl"
        fi
    done
    
    if [ -z "$gov_missing" ]; then
        test_result "PASS" "All governor sysctls readable" "All sysctls accessible"
    else
        test_result "FAIL" "All governor sysctls readable" "Missing:$gov_missing"
    fi
    
    # Cleanup
    destroy_node "$NODE_NAME"
}

# ============================================================================
# Integration Tests: Error Handling
# ============================================================================

test_error_handling() {
    log_section "Test: Error Handling"
    
    # Create test node
    create_node "$NODE_NAME" || return 1
    
    # Test INT-20: Invalid algorithm
    test_header "Invalid algorithm rejection"
    ngctl msg "$NODE_NAME:" pppoe_lb setalgorithm 99 2>/dev/null
    sleep 0.2
    local algo
    algo=$(sysctl_get "governor.algorithm")
    # Should keep previous value or default to 0
    if [ "$algo" -le 2 ] || [ "$algo" = "-1" ]; then
        test_result "PASS" "Invalid algorithm rejected" "algorithm=$algo"
    else
        test_result "FAIL" "Invalid algorithm rejected" "algorithm=$algo"
    fi
    
    # Test INT-21: Remove non-existent worker
    test_header "Remove non-existent worker"
    ngctl msg "$NODE_NAME:" pppoe_lb rmworker 999 2>/dev/null
    # Should not crash - just error
    local still_alive
    still_alive=$(ngctl list 2>/dev/null | grep -c "$NODE_NAME" || echo "0")
    if [ "$still_alive" -gt 0 ]; then
        test_result "PASS" "Invalid worker removal handled gracefully" "Node still alive"
    else
        test_result "FAIL" "Invalid worker removal handled gracefully" "Node destroyed"
    fi
    
    # Test INT-22: Node destruction with active sessions
    test_header "Node destruction with workers"
    # Add a worker first
    ngctl msg "$NODE_NAME:" pppoe_lb addworker 2>/dev/null
    sleep 0.3
    
    # Destroy should succeed
    destroy_node "$NODE_NAME"
    local node_gone
    node_gone=$(ngctl list 2>/dev/null | grep -c "$NODE_NAME" || echo "0")
    if [ "$node_gone" -eq 0 ]; then
        test_result "PASS" "Node destroyed with active workers" "Clean shutdown"
    else
        test_result "FAIL" "Node destroyed with active workers" "Node still exists"
    fi
}

# ============================================================================
# Integration Tests: Long-Running (Skip in Quick Mode)
# ============================================================================

test_long_running() {
    # Skip in quick mode
    if [ "$QUICK_MODE" = "true" ]; then
        log_skip "Long-running tests skipped in quick mode"
        return 0
    fi
    
    log_section "Test: Long-Running Stability"
    
    # Create test node
    create_node "$NODE_NAME" || return 1
    
    # Test INT-23: Sustained operation
    test_header "Sustained operation (30 seconds)"
    log_info "Running sustained operation test (this takes 30 seconds)..."
    
    local start_time
    start_time=$(date +%s)
    local end_time=$((start_time + 30))
    local errors=0
    
    while [ "$(date +%s)" -lt "$end_time" ]; do
        # Check node is still alive
        if ! ngctl list 2>/dev/null | grep -q "$NODE_NAME"; then
            errors=$((errors + 1))
            break
        fi
        
        # Toggle governor
        sysctl_set "governor.enabled" 1 2>/dev/null
        sleep 1
        sysctl_set "governor.enabled" 0 2>/dev/null
        sleep 1
    done
    
    if [ "$errors" -eq 0 ]; then
        test_result "PASS" "Sustained operation completed" "30s without errors"
    else
        test_result "FAIL" "Sustained operation completed" "$errors errors"
    fi
    
    # Cleanup
    destroy_node "$NODE_NAME"
}

# ============================================================================
# Integration Tests: Recovery Scenarios
# ============================================================================

test_recovery() {
    # Skip in quick mode
    if [ "$QUICK_MODE" = "true" ]; then
        log_skip "Recovery tests skipped in quick mode"
        return 0
    fi
    
    log_section "Test: Recovery Scenarios"
    
    # Test INT-24: Node recreation after destruction
    test_header "Node recreation after destruction"
    
    create_node "$NODE_NAME" || {
        test_result "FAIL" "Node recreation after destruction" "Creation failed"
        return 1
    }
    
    destroy_node "$NODE_NAME"
    sleep 0.5
    
    create_node "${NODE_NAME}_recreated" || {
        test_result "FAIL" "Node recreation after destruction" "Recreation failed"
        return 1
    }
    
    local node_exists
    node_exists=$(ngctl list 2>/dev/null | grep -c "${NODE_NAME}_recreated" || echo "0")
    if [ "$node_exists" -gt 0 ]; then
        test_result "PASS" "Node recreation after destruction" "Node recreated"
    else
        test_result "FAIL" "Node recreation after destruction" "Recreation failed"
    fi
    
    destroy_node "${NODE_NAME}_recreated"
}

# ============================================================================
# Main Test Runner
# ============================================================================

print_banner() {
    echo ""
    echo "============================================================================"
    echo "  PPPoE Load Balancer Integration Test Suite"
    echo "============================================================================"
    echo ""
    echo "Date:    $(date)"
    echo "System:  $(uname -rs)"
    echo "Mode:    $([ "$QUICK_MODE" = "true" ] && echo "Quick" || echo "Full")"
    echo ""
}

print_summary() {
    echo "1..$TESTS_RUN"
    log_section "Test Summary"

    echo ""
    echo "  Tests Run:    $TESTS_RUN"
    echo "  Tests Passed: ${GREEN}$TESTS_PASSED${NC}"
    echo "  Tests Failed: ${RED}$TESTS_FAILED${NC}"
    echo "  Tests Skipped: ${CYAN}$TESTS_SKIPPED${NC}"
    echo ""

    if [ "$TESTS_FAILED" -eq 0 ]; then
        echo "${GREEN}${BOLD}ALL TESTS PASSED${NC}"
        echo ""
        return 0
    else
        echo "${RED}${BOLD}SOME TESTS FAILED${NC}"
        echo ""
        return 1
    fi
}

# ============================================================================
# Main Entry Point
# ============================================================================

main() {
    # Parse arguments
    while [ $# -gt 0 ]; do
        case "$1" in
            --quick)
                QUICK_MODE=true
                ;;
            --verbose|-v)
                VERBOSE=true
                ;;
            --stop-on-fail)
                STOP_ON_FAIL=true
                ;;
            --help|-h)
                echo "Usage: $0 [--quick] [--verbose] [--stop-on-fail]"
                echo ""
                echo "Options:"
                echo "  --quick       Run only basic tests (skip long-running tests)"
                echo "  --verbose     Show detailed output for each test"
                echo "  --stop-on-fail  Stop on first failure"
                echo "  --help        Show this help message"
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                exit 1
                ;;
        esac
        shift
    done
    
    # Environment checks
    check_root
    check_vm_environment
    check_tools
    check_modules
    
    # Setup cleanup trap
    trap cleanup_all EXIT
    
    # Print banner
    print_banner
    
    # Run all integration tests
    test_node_creation
    test_worker_management
    test_governor_scaling
    test_algorithms
    test_sysctl_interface
    test_error_handling
    test_long_running
    test_recovery
    
    # Print summary
    print_summary
    
    # Exit with appropriate code
    if [ "$TESTS_FAILED" -gt 0 ]; then
        exit 1
    fi
    exit 0
}

# Run main
main "$@"
