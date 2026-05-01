#!/bin/sh
#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2024 Mark LaPointe <mark@cloudbsd.org>
#
# CI Integration Script for PPPoE Load Balancer Tests
#
# This script runs the PPPoE Load Balancer test suite in a CI environment.
# It is designed to be called by CI systems like GitHub Actions, Jenkins,
# or manual test runs.
#
# Usage:
#   ./run_tests.sh [--suite suite_name] [--quick] [--verbose] [--junit]
#
# Options:
#   --suite      Run specific test suite (unit|regression|config|integration|stress|all)
#   --quick      Run only quick tests (skip long-running tests)
#   --verbose    Show detailed output
#   --junit      Generate JUnit XML output
#   --help       Show this help message
#
# Environment Variables:
#   CI_MODE              Set to "true" for CI mode (non-interactive)
#   TEST_TIMEOUT         Timeout for each test (default: 300 seconds)
#   TEST_LOG_DIR         Directory for test logs (default: /tmp/pppoe_lb_test_logs)

set -e

# ============================================================================
# Configuration
# ============================================================================

# Default values
SUITE="all"
QUICK_MODE=""
VERBOSE=""
JUNIT_OUTPUT=""
CI_MODE="${CI_MODE:-false}"
TEST_TIMEOUT="${TEST_TIMEOUT:-300}"
TEST_LOG_DIR="${TEST_LOG_DIR:-/tmp/pppoe_lb_test_logs}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Exit codes
EXIT_SUCCESS=0
EXIT_FAILURE=1
EXIT_SKIP=2

# ============================================================================
# Helper Functions
# ============================================================================

log_info() {
    echo "[INFO] $*"
}

log_error() {
    echo "[ERROR] $*" >&2
}

log_warn() {
    echo "[WARN] $*" >&2
}

# Print section header
section() {
    echo ""
    echo "============================================================================"
    echo "  $*"
    echo "============================================================================"
}

# Check if running as root
check_root() {
    if [ "$(id -u)" -ne 0 ]; then
        log_error "This script must be run as root"
        exit $EXIT_FAILURE
    fi
}

# Create log directory
setup_logging() {
    mkdir -p "$TEST_LOG_DIR"
    TEST_LOG_DIR="$TEST_LOG_DIR"  # Export for child scripts
    export TEST_LOG_DIR
}

# Check for VM environment
check_vm_environment() {
    if ! grep -qi 'vmware\|virtualbox\|qemu\|kvm\|bhyve' /var/run/dmesg.boot 2>/dev/null; then
        if [ "$CI_MODE" = "true" ]; then
            log_warn "Not detected as VM environment, tests may fail"
        else
            log_warn "Not detected as VM environment"
            printf "Continue anyway? [y/N] "
            read -r answer
            if [ "$answer" != "y" ] && [ "$answer" != "Y" ]; then
                log_info "Aborted"
                exit $EXIT_SKIP
            fi
        fi
    fi
}

# Load kernel modules
load_modules() {
    section "Loading Kernel Modules"
    
    log_info "Loading netgraph modules..."
    
    kldload netgraph 2>/dev/null || true
    kldload ng_ether 2>/dev/null || true
    kldload ng_pppoe 2>/dev/null || true
    
    if ! kldload ng_pppoe_lb 2>/dev/null; then
        log_error "Failed to load ng_pppoe_lb module"
        log_error "Ensure the module has been built"
        exit $EXIT_FAILURE
    fi
    
    log_info "All modules loaded successfully"
}

# Run a test script and capture results
run_test_script() {
    local script="$1"
    local name="$(basename "$script")"
    local log_file="${TEST_LOG_DIR}/${name}.log"
    local start_time
    local end_time
    local duration
    local result=0
    
    if [ ! -f "$script" ]; then
        log_error "Test script not found: $script"
        return 1
    fi
    
    start_time=$(date +%s)
    
    log_info "Running: $name"
    log_info "Log file: $log_file"
    
    # Run the test with timeout
    if timeout "$TEST_TIMEOUT" "$script" $QUICK_MODE $VERBOSE >> "$log_file" 2>&1; then
        result=0
    else
        result=$?
    fi
    
    end_time=$(date +%s)
    duration=$((end_time - start_time))
    
    # Update log with summary
    {
        echo ""
        echo "============================================================================"
        echo "Test completed at: $(date)"
        echo "Duration: ${duration}s"
        echo "Exit code: $result"
        echo "============================================================================"
    } >> "$log_file"
    
    if [ $result -eq 0 ]; then
        log_info "PASS: $name (${duration}s)"
        return 0
    elif [ $result -eq 124 ]; then
        log_error "TIMEOUT: $name (${duration}s > ${TEST_TIMEOUT}s)"
        return 1
    else
        log_error "FAIL: $name (${duration}s, exit code: $result)"
        log_error "See log: $log_file"
        return 1
    fi
}

# Generate JUnit XML report
generate_junit_report() {
    local output_file="${TEST_LOG_DIR}/junit-report.xml"
    
    log_info "Generating JUnit XML report: $output_file"
    
    # Count results
    local passed=0
    local failed=0
    local skipped=0
    
    for log in "${TEST_LOG_DIR}"/*.log; do
        if [ -f "$log" ]; then
            if grep -q "Tests Passed:" "$log" 2>/dev/null; then
                local script_passed
                local script_failed
                local script_skipped
                script_passed=$(grep "Tests Passed:" "$log" | awk '{print $3}')
                script_failed=$(grep "Tests Failed:" "$log" | awk '{print $3}')
                script_skipped=$(grep "Tests Skipped:" "$log" | awk '{print $3}')
                passed=$((passed + script_passed))
                failed=$((failed + script_failed))
                skipped=$((skipped + script_skipped))
            fi
        fi
    done
    
    # Generate XML
    cat > "$output_file" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<testsuite name="PPPoE Load Balancer Test Suite" 
           tests="$((passed + failed + skipped))" 
           failures="$failed" 
           skipped="$skipped" 
           timestamp="$(date -Iseconds)"
           hostname="$(hostname)">
EOF

    # Add test cases
    for log in "${TEST_LOG_DIR}"/*.log; do
        if [ -f "$log" ]; then
            local name
            name=$(basename "$log" .log)
            local duration
            duration=$(grep "Duration:" "$log" | awk '{print $2}' | tr -d 's')
            local classname="pppoe_lb.$(dirname "$log" | xargs basename)"
            
            if grep -q "Exit code: 0" "$log"; then
                cat >> "$output_file" << EOF
  <testcase name="$name" classname="$classname" time="$duration"/>
EOF
            else
                cat >> "$output_file" << EOF
  <testcase name="$name" classname="$classname" time="$duration">
    <failure message="Test failed" type="TestFailure">
$(tail -50 "$log" | sed 's/^/      /')
    </failure>
  </testcase>
EOF
            fi
        fi
    done
    
    cat >> "$output_file" << EOF
</testsuite>
EOF

    log_info "JUnit report generated: $output_file"
}

# ============================================================================
# Test Suites
# ============================================================================

run_unit_tests() {
    section "Running Unit Tests"
    
    local tests=(
        "${SCRIPT_DIR}/ng_pppoe_lb_unit_test.sh"
    )
    
    local failed=0
    for test in "${tests[@]}"; do
        if [ -f "$test" ]; then
            if ! run_test_script "$test"; then
                failed=$((failed + 1))
            fi
        fi
    done
    
    return $failed
}

run_regression_tests() {
    section "Running Regression Tests"
    
    local tests=(
        "${SCRIPT_DIR}/ng_pppoe_lb_regression_test.sh"
    )
    
    local failed=0
    for test in "${tests[@]}"; do
        if [ -f "$test" ]; then
            if ! run_test_script "$test"; then
                failed=$((failed + 1))
            fi
        fi
    done
    
    return $failed
}

run_config_tests() {
    section "Running Configuration Tests"
    
    local tests=(
        "${SCRIPT_DIR}/ng_pppoe_lb_config_test.sh"
    )
    
    local failed=0
    for test in "${tests[@]}"; do
        if [ -f "$test" ]; then
            if ! run_test_script "$test"; then
                failed=$((failed + 1))
            fi
        fi
    done
    
    return $failed
}

run_integration_tests() {
    section "Running Integration Tests"
    
    local tests=(
        "${SCRIPT_DIR}/ng_pppoe_lb_integration_test.sh"
    )
    
    local failed=0
    for test in "${tests[@]}"; do
        if [ -f "$test" ]; then
            if ! run_test_script "$test"; then
                failed=$((failed + 1))
            fi
        fi
    done
    
    return $failed
}

run_stress_tests() {
    section "Running Stress Tests"
    
    # Skip stress tests in quick mode or non-CI mode without confirmation
    if [ -n "$QUICK_MODE" ]; then
        log_info "Skipping stress tests (quick mode)"
        return 0
    fi
    
    local tests=(
        "${SCRIPT_DIR}/ng_pppoe_lb_stress_test.sh"
    )
    
    local failed=0
    for test in "${tests[@]}"; do
        if [ -f "$test" ]; then
            if ! run_test_script "$test"; then
                failed=$((failed + 1))
            fi
        fi
    done
    
    return $failed
}

# ============================================================================
# Main Entry Point
# ============================================================================

print_usage() {
    cat << EOF
Usage: $0 [options]

Options:
  --suite SUITE      Run specific test suite (unit|regression|config|integration|stress|all)
  --quick            Run only quick tests (skip long-running tests)
  --verbose          Show detailed output
  --junit            Generate JUnit XML output
  --help             Show this help message

Environment Variables:
  CI_MODE            Set to "true" for CI mode (non-interactive)
  TEST_TIMEOUT       Timeout for each test (default: 300 seconds)
  TEST_LOG_DIR       Directory for test logs (default: /tmp/pppoe_lb_test_logs)

Examples:
  $0 --suite all --quick           # Run all quick tests
  $0 --suite integration --verbose # Run integration tests with verbose output
  CI_MODE=true $0 --junit         # Run all tests in CI mode with JUnit output

EOF
}

main() {
    # Parse arguments
    while [ $# -gt 0 ]; do
        case "$1" in
            --suite)
                SUITE="$2"
                shift 2
                ;;
            --quick)
                QUICK_MODE="--quick"
                ;;
            --verbose)
                VERBOSE="--verbose"
                ;;
            --junit)
                JUNIT_OUTPUT="yes"
                ;;
            --help|-h)
                print_usage
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                print_usage
                exit 1
                ;;
        esac
    done
    
    # Print banner
    echo ""
    echo "============================================================================"
    echo "  PPPoE Load Balancer CI Test Runner"
    echo "============================================================================"
    echo ""
    echo "Date:       $(date)"
    echo "Suite:      $SUITE"
    echo "Quick:      ${QUICK_MODE:-no}"
    echo "Verbose:    ${VERBOSE:-no}"
    echo "Log Dir:    $TEST_LOG_DIR"
    echo "Timeout:    ${TEST_TIMEOUT}s"
    echo "CI Mode:    $CI_MODE"
    echo ""
    
    # Setup
    check_root
    setup_logging
    check_vm_environment
    load_modules
    
    # Cleanup on exit
    trap 'log_info "Cleaning up..."; rm -f /tmp/pppoe_lb_test_*.socket 2>/dev/null; true' EXIT
    
    # Run test suites
    local failed=0
    
    case "$SUITE" in
        unit)
            run_unit_tests || failed=$?
            ;;
        regression)
            run_regression_tests || failed=$?
            ;;
        config)
            run_config_tests || failed=$?
            ;;
        integration)
            run_integration_tests || failed=$?
            ;;
        stress)
            run_stress_tests || failed=$?
            ;;
        all)
            run_unit_tests || failed=$((failed + $?))
            run_regression_tests || failed=$((failed + $?))
            run_config_tests || failed=$((failed + $?))
            run_integration_tests || failed=$((failed + $?))
            run_stress_tests || failed=$((failed + $?))
            ;;
        *)
            log_error "Unknown test suite: $SUITE"
            print_usage
            exit 1
            ;;
    esac
    
    # Generate JUnit report if requested
    if [ "$JUNIT_OUTPUT" = "yes" ]; then
        generate_junit_report
    fi
    
    # Print summary
    section "Test Summary"
    echo ""
    echo "Log directory: $TEST_LOG_DIR"
    echo ""
    
    if [ $failed -eq 0 ]; then
        echo "ALL TESTS PASSED"
        exit $EXIT_SUCCESS
    else
        echo "SOME TESTS FAILED ($failed suites failed)"
        echo ""
        echo "Failed test logs:"
        for log in "${TEST_LOG_DIR}"/*.log; do
            if [ -f "$log" ] && grep -q "Exit code: [^0]" "$log" 2>/dev/null; then
                echo "  - $(basename "$log")"
            fi
        done
        exit $EXIT_FAILURE
    fi
}

main "$@"
