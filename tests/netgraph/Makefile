# Makefile for PPPoE Load Balancer Tests
#
# This Makefile provides convenient targets for running the various test suites.
#
# Usage:
#   make test              Run all quick tests
#   make unit_test        Run unit tests only
#   make regression_test  Run regression tests only
#   make config_test      Run configuration tests only
#   make full_test        Run all tests (including slow ones)
#   make clean            Clean up test artifacts
#

# Test scripts
UNIT_TEST=	ng_pppoe_lb_unit_test.sh
REGRESSION_TEST=	ng_pppoe_lb_regression_test.sh
CONFIG_TEST=	ng_pppoe_lb_config_test.sh
INTEGRATION_TEST=	ng_pppoe_lb_integration_test.sh
STRESS_TEST=	ng_pppoe_lb_stress_test.sh
PERF_TEST=	ng_pppoe_lb_perf_baseline.sh

# Test directories
TEST_DIR=	${TESTSDIR}
TESTS?=	unit_test regression_test config_test

# Options
VERBOSE=	0
STOP_ON_FAIL=	0
QUICK_MODE=	1

# Export for test scripts
export VERBOSE STOP_ON_FAIL QUICK_MODE

# Canned test sequence
.define run_test
	@echo "Running $(1)..."
	@if $(2) $(3); then \
		echo "$(1): PASSED"; \
	else \
		echo "$(1): FAILED (exit code $$?)"; \
		$(4) \
	fi
.endef

# Default target
.PHONY: test
test: unit_test regression_test config_test
	@echo ""
	@echo "========================================"
	@echo "All quick tests completed"
	@echo "========================================"

# Unit tests (KERN-01 to KERN-15, SYS-01 to SYS-08, NGCTL-01 to NGCTL-10)
.PHONY: unit_test
unit_test:
	@echo ""
	@echo "========================================"
	@echo "Running Unit Tests"
	@echo "========================================"
	@./$(UNIT_TEST)$(TEST_ARGS)
	@echo "Unit tests completed"

# Regression tests (REG-01 to REG-18)
.PHONY: regression_test
regression_test:
	@echo ""
	@echo "========================================"
	@echo "Running Regression Tests"
	@echo "========================================"
	@./$(REGRESSION_TEST)$(TEST_ARGS)
	@echo "Regression tests completed"

# Configuration tests (CFG-01 to CFG-10)
.PHONY: config_test
config_test:
	@echo ""
	@echo "========================================"
	@echo "Running Configuration Tests"
	@echo "========================================"
	@./$(CONFIG_TEST)$(TEST_ARGS)
	@echo "Configuration tests completed"

# Integration tests (INT-01 to INT-14)
.PHONY: integration_test
integration_test:
	@echo ""
	@echo "========================================"
	@echo "Running Integration Tests"
	@echo "========================================"
	@./$(INTEGRATION_TEST)$(TEST_ARGS)
	@echo "Integration tests completed"

# Stress tests (STRESS-01 to STRESS-08)
.PHONY: stress_test
stress_test:
	@echo ""
	@echo "========================================"
	@echo "Running Stress Tests"
	@echo "========================================"
	@./$(STRESS_TEST)$(TEST_ARGS)
	@echo "Stress tests completed"

# Performance baseline tests
.PHONY: perf_test
perf_test:
	@echo ""
	@echo "========================================"
	@echo "Running Performance Baseline Tests"
	@echo "========================================"
	@./$(PERF_TEST)$(TEST_ARGS)
	@echo "Performance tests completed"

# Full test suite (all tests including slow ones)
.PHONY: full_test
full_test: unit_test regression_test config_test integration_test stress_test
	@echo ""
	@echo "========================================"
	@echo "Full test suite completed"
	@echo "========================================"

# Run with verbose output
.PHONY: verbose
verbose:
	VERBOSE=1 $(MAKE) test

# Run with stop-on-fail
.PHONY: strict
strict:
	STOP_ON_FAIL=1 $(MAKE) test

# Clean up test artifacts
.PHONY: clean
clean:
	@echo "Cleaning up test artifacts..."
	@rm -rf /tmp/pppoe_lb_*
	@rm -rf /tmp/ng_pppoe_*
	@echo "Clean completed"

# Show help
.PHONY: help
help:
	@echo "PPPoE Load Balancer Test Suite"
	@echo ""
	@echo "Targets:"
	@echo "  test              Run quick tests (unit, regression, config)"
	@echo "  unit_test         Run unit tests only"
	@echo "  regression_test   Run regression tests only"
	@echo "  config_test       Run configuration tests only"
	@echo "  integration_test  Run integration tests only"
	@echo "  stress_test       Run stress tests only"
	@echo "  perf_test         Run performance baseline tests"
	@echo "  full_test         Run all tests (including slow)"
	@echo "  verbose           Run with verbose output"
	@echo "  strict            Stop on first failure"
	@echo "  clean             Clean up test artifacts"
	@echo "  help              Show this help"
	@echo ""
	@echo "Options (via environment):"
	@echo "  VERBOSE=1         Enable verbose output"
	@echo "  STOP_ON_FAIL=1    Stop on first failure"
	@echo "  QUICK_MODE=0      Include slow tests"

# Check syntax of all test scripts
.PHONY: check
check:
	@echo "Checking test script syntax..."
	@sh -n $(UNIT_TEST) && echo "  $(UNIT_TEST): OK" || echo "  $(UNIT_TEST): SYNTAX ERROR"
	@sh -n $(REGRESSION_TEST) && echo "  $(REGRESSION_TEST): OK" || echo "  $(REGRESSION_TEST): SYNTAX ERROR"
	@sh -n $(CONFIG_TEST) && echo "  $(CONFIG_TEST): OK" || echo "  $(CONFIG_TEST): SYNTAX ERROR"
	@sh -n $(INTEGRATION_TEST) && echo "  $(INTEGRATION_TEST): OK" || echo "  $(INTEGRATION_TEST): NOT FOUND"
	@sh -n $(STRESS_TEST) && echo "  $(STRESS_TEST): OK" || echo "  $(STRESS_TEST): NOT FOUND"
	@echo "Syntax check completed"
