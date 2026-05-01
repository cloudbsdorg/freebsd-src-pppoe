#!/bin/sh
#
# PPPoE Load Balancer Performance Monitor
#
# This script monitors PPPoE load balancer metrics and can output
# data in various formats for integration with monitoring systems.
#
# Usage:
#   ./pppoe_lb_monitor.sh [--format console|json|statsd] [--interval SECONDS]
#
# Options:
#   --format      Output format: console (default), json, statsd
#   --interval    Polling interval in seconds (default: 5)
#   --host        StatsD host (default: localhost)
#   --port        StatsD port (default: 8125)

# Configuration
FORMAT=${FORMAT:-console}
INTERVAL=${INTERVAL:-5}
STATSD_HOST=${STATSD_HOST:-localhost}
STATSD_PORT=${STATSD_PORT:-8125}

# Sysctl prefix
SYSCTL_PREFIX="net.graph.pppoe_lb"

# ============================================================================
# Helper Functions
# ============================================================================

# Read a sysctl value
read_sysctl() {
    sysctl -n "${SYSCTL_PREFIX}.$1" 2>/dev/null || echo "0"
}

# Get all metrics
get_metrics() {
    # Governor metrics
    GOVERNOR_ENABLED=$(read_sysctl governor.enabled)
    GOVERNOR_MODE=$(read_sysctl governor.mode)
    GOVERNOR_CURRENT_WORKERS=$(read_sysctl governor.current_workers)
    GOVERNOR_ACTIVE_WORKERS=$(read_sysctl governor.active_workers)
    GOVERNOR_DRAINING_WORKERS=$(read_sysctl governor.draining_workers)
    GOVERNOR_PENDING_REMOVALS=$(read_sysctl governor.pending_removals)
    GOVERNOR_CPU=$(read_sysctl governor.cpu_usage)
    GOVERNOR_CPU_AVG=$(read_sysctl governor.cpu_avg)
    GOVERNOR_SESSIONS=$(read_sysctl governor.sessions)
    GOVERNOR_LAST_DECISION=$(read_sysctl governor.last_decision)
    GOVERNOR_LAST_REASON=$(read_sysctl governor.last_reason)
    GOVERNOR_LAST_DECISION_TIME=$(read_sysctl governor.last_decision_time)
    
    # Algorithm and config
    ALGORITHM=$(read_sysctl algorithm)
    MAX_WORKERS=$(read_sysctl governor.max_workers)
    MIN_WORKERS=$(read_sysctl governor.min_workers)
    CPU_THRESHOLD=$(read_sysctl governor.cpu_threshold)
    CPU_LOW_THRESHOLD=$(read_sysctl governor.cpu_low_threshold)
    SESSIONS_PER_WORKER=$(read_sysctl governor.sessions_per_worker)
    
    # Statistics
    STATS_ERRORS=$(read_sysctl stats.errors)
    STATS_BYTES_IN=$(read_sysctl stats.bytes_in)
    STATS_BYTES_OUT=$(read_sysctl stats.bytes_out)
    STATS_PACKETS_IN=$(read_sysctl stats.packets_in)
    STATS_PACKETS_OUT=$(read_sysctl stats.packets_out)
    
    # Timestamps
    TIMESTAMP=$(date +%s)
    TIMESTAMP_HUMAN=$(date '+%Y-%m-%d %H:%M:%S')
}

# ============================================================================
# Output Formatters
# ============================================================================

# Console output
output_console() {
    get_metrics
    
    echo ""
    echo "=========================================="
    echo "PPPoE Load Balancer Status"
    echo "=========================================="
    echo "Time: $TIMESTAMP_HUMAN"
    echo ""
    
    echo "Governor Configuration:"
    echo "  Enabled:       $GOVERNOR_ENABLED ($([ "$GOVERNOR_ENABLED" = "1" ] && echo "active" || echo "inactive"))"
    echo "  Mode:          $GOVERNOR_MODE ($([ "$GOVERNOR_MODE" = "1" ] && echo "auto" || echo "manual"))"
    echo "  Min Workers:   $MIN_WORKERS"
    echo "  Max Workers:   ${MAX_WORKERS:-auto (mp_ncpus)}"
    echo "  CPU Threshold: $CPU_THRESHOLD%"
    echo "  CPU Low:       $CPU_LOW_THRESHOLD%"
    echo ""
    
    echo "Governor State:"
    echo "  Current Workers:   $GOVERNOR_CURRENT_WORKERS"
    echo "  Active Workers:    $GOVERNOR_ACTIVE_WORKERS"
    echo "  Draining Workers:  $GOVERNOR_DRAINING_WORKERS"
    echo "  Pending Removals:  $GOVERNOR_PENDING_REMOVALS"
    echo "  Sessions:          $GOVERNOR_SESSIONS"
    echo "  CPU Usage:         $GOVERNOR_CPU% (avg: $GOVERNOR_CPU_AVG%)"
    echo ""
    
    echo "Governor Decision:"
    echo "  Last Decision: ${GOVERNOR_LAST_DECISION} (${GOVERNOR_LAST_REASON})"
    echo "  Decision Time:  $(date -r "$GOVERNOR_LAST_DECISION_TIME" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo "$GOVERNOR_LAST_DECISION_TIME")"
    echo ""
    
    echo "Statistics:"
    echo "  Errors:      $STATS_ERRORS"
    echo "  Bytes In:    $STATS_BYTES_IN"
    echo "  Bytes Out:   $STATS_BYTES_OUT"
    echo "  Packets In:  $STATS_PACKETS_IN"
    echo "  Packets Out: $STATS_PACKETS_OUT"
    echo ""
    
    echo "Algorithm:    $ALGORITHM"
    echo "  0 = round-robin"
    echo "  1 = hash-based"
    echo "  2 = least-loaded"
    echo ""
}

# JSON output
output_json() {
    get_metrics
    
    cat << EOF
{
    "timestamp": $TIMESTAMP,
    "timestamp_human": "$TIMESTAMP_HUMAN",
    "governor": {
        "enabled": $GOVERNOR_ENABLED,
        "mode": $GOVERNOR_MODE,
        "min_workers": $MIN_WORKERS,
        "max_workers": ${MAX_WORKERS:-0},
        "cpu_threshold": $CPU_THRESHOLD,
        "cpu_low_threshold": $CPU_LOW_THRESHOLD,
        "current_workers": $GOVERNOR_CURRENT_WORKERS,
        "active_workers": $GOVERNOR_ACTIVE_WORKERS,
        "draining_workers": $GOVERNOR_DRAINING_WORKERS,
        "pending_removals": $GOVERNOR_PENDING_REMOVALS,
        "sessions": $GOVERNOR_SESSIONS,
        "cpu_usage": $GOVERNOR_CPU,
        "cpu_avg": $GOVERNOR_CPU_AVG,
        "last_decision": $GOVERNOR_LAST_DECISION,
        "last_reason": $GOVERNOR_LAST_REASON,
        "last_decision_time": $GOVERNOR_LAST_DECISION_TIME
    },
    "algorithm": $ALGORITHM,
    "stats": {
        "errors": $STATS_ERRORS,
        "bytes_in": $STATS_BYTES_IN,
        "bytes_out": $STATS_BYTES_OUT,
        "packets_in": $STATS_PACKETS_IN,
        "packets_out": $STATS_PACKETS_OUT
    }
}
EOF
}

# StatsD output (for graphite, influxdb, etc.)
output_statsd() {
    get_metrics
    
    # Send metrics via UDP to StatsD
    METRICS=""
    METRICS="${METRICS}pppoe_lb.governor.enabled:$GOVERNOR_ENABLED|g\n"
    METRICS="${METRICS}pppoe_lb.governor.mode:$GOVERNOR_MODE|g\n"
    METRICS="${METRICS}pppoe_lb.governor.current_workers:$GOVERNOR_CURRENT_WORKERS|g\n"
    METRICS="${METRICS}pppoe_lb.governor.active_workers:$GOVERNOR_ACTIVE_WORKERS|g\n"
    METRICS="${METRICS}pppoe_lb.governor.draining_workers:$GOVERNOR_DRAINING_WORKERS|g\n"
    METRICS="${METRICS}pppoe_lb.governor.pending_removals:$GOVERNOR_PENDING_REMOVALS|g\n"
    METRICS="${METRICS}pppoe_lb.governor.sessions:$GOVERNOR_SESSIONS|g\n"
    METRICS="${METRICS}pppoe_lb.governor.cpu_usage:$GOVERNOR_CPU|g\n"
    METRICS="${METRICS}pppoe_lb.governor.cpu_avg:$GOVERNOR_CPU_AVG|g\n"
    METRICS="${METRICS}pppoe_lb.stats.errors:$STATS_ERRORS|g\n"
    METRICS="${METRICS}pppoe_lb.stats.bytes_in:$STATS_BYTES_IN|g\n"
    METRICS="${METRICS}pppoe_lb.stats.bytes_out:$STATS_BYTES_OUT|g\n"
    METRICS="${METRICS}pppoe_lb.stats.packets_in:$STATS_PACKETS_IN|g\n"
    METRICS="${METRICS}pppoe_lb.stats.packets_out:$STATS_PACKETS_OUT|g\n"
    METRICS="${METRICS}pppoe_lb.algorithm:$ALGORITHM|g\n"
    
    # Send to StatsD
    echo -e "$METRICS" | nc -u -w1 "$STATSD_HOST" "$STATSD_PORT" 2>/dev/null || \
        echo "Warning: Could not send to StatsD at $STATSD_HOST:$STATSD_PORT"
}

# ============================================================================
# Main Loop
# ============================================================================

usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  --format FORMAT    Output format: console, json, statsd (default: console)"
    echo "  --interval SECONDS Polling interval in seconds (default: 5)"
    echo "  --host HOST        StatsD host (default: localhost)"
    echo "  --port PORT        StatsD port (default: 8125)"
    echo "  --once             Run once and exit (default: continuous)"
    echo "  --help             Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 --format console     # Continuous console output"
    echo "  $0 --format json       # JSON output for scripting"
    echo "  $0 --format statsd      # Send to StatsD"
    echo "  $0 --format json --once  # Single JSON output"
    exit 0
}

main() {
    ONCE=0
    
    # Parse arguments
    while [ $# -gt 0 ]; do
        case "$1" in
            --format)
                FORMAT="$2"
                shift 2
                ;;
            --interval)
                INTERVAL="$2"
                shift 2
                ;;
            --host)
                STATSD_HOST="$2"
                shift 2
                ;;
            --port)
                STATSD_PORT="$2"
                shift 2
                ;;
            --once)
                ONCE=1
                shift
                ;;
            --help)
                usage
                ;;
            *)
                echo "Unknown option: $1"
                usage
                ;;
        esac
    done
    
    # Validate format
    case "$FORMAT" in
        console|json|statsd)
            ;;
        *)
            echo "Unknown format: $FORMAT"
            usage
            ;;
    esac
    
    # Run loop
    while true; do
        case "$FORMAT" in
            console)
                output_console
                ;;
            json)
                output_json
                ;;
            statsd)
                output_statsd
                ;;
        esac
        
        if [ "$ONCE" -eq 1 ]; then
            exit 0
        fi
        
        sleep "$INTERVAL"
    done
}

# Run main
main "$@"
