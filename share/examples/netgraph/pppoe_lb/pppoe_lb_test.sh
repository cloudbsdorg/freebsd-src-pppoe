#!/bin/sh
#
# PPPoE Load Balancer Test Script
#
# A comprehensive script for testing multithreaded PPPoE load balancing.
# Supports both server (PPPoE concentrator) and client (PPPoE initiator) modes.
#
# Usage:
#   ./pppoe_lb_test.sh server [-i iface] [-n workers] [-a algorithm] [-d]
#   ./pppoe_lb_test.sh client [-i iface] [-s server_mac] [-u username] [-p password]
#   ./pppoe_lb_test.sh cleanup
#   ./pppoe_lb_test.sh status
#   ./pppoe_lb_test.sh -h
#
# Options:
#   server     Run in server mode (PPPoE concentrator with load balancer)
#   client     Run in client mode (initiate PPPoE session)
#   cleanup    Remove all PPPoE test nodes and interfaces
#   status     Show current PPPoE and netgraph status
#   -i iface   Network interface (default: em0)
#   -n workers Number of worker nodes (default: 2)
#   -a algo    Algorithm: 0=round-robin, 1=hash, 2=least-loaded (default: 0)
#   -d         Enable debug output
#   -g         Enable CPU governor auto-scaling
#   -m max     Maximum workers for governor (default: 4)
#   -h         Show this help message
#
# Examples:
#   # Start a 4-worker server on em0
#   ./pppoe_lb_test.sh server -i em0 -n 4
#
#   # Start server with hash-based session affinity
#   ./pppoe_lb_test.sh server -i em0 -a 1
#
#   # Start server with CPU governor auto-scaling
#   ./pppoe_lb_test.sh server -i em0 -g -m 8
#
#   # Connect a client
#   ./pppoe_lb_test.sh client -i ng0 -s 00:11:22:33:44:55
#
#   # View status
#   ./pppoe_lb_test.sh status
#
#   # Clean up all test resources
#   ./pppoe_lb_test.sh cleanup
#

set -e

# Default configuration
MODE=""
INTERFACE="em0"
WORKERS=2
ALGORITHM=0
DEBUG=0
GOVERNOR=0
GOVERNOR_MAX=4
USERNAME="testuser"
PASSWORD="testpass"
SERVER_MAC=""
LB_NODE="pppoe_lb0"
PPPOED_PID="/var/run/pppoed_test.pid"

# Colors for output
if [ -t 1 ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    BLUE='\033[0;34m'
    NC='\033[0m' # No Color
else
    RED=''
    GREEN=''
    YELLOW=''
    BLUE=''
    NC=''
fi

# Required kernel modules
REQUIRED_MODULES="netgraph ng_ether ng_pppoe ng_pppoe_lb"

#######################################
# Utility Functions
#######################################

log_info() {
    printf "${BLUE}[INFO]${NC} %s\n" "$*"
}

log_success() {
    printf "${GREEN}[OK]${NC} %s\n" "$*"
}

log_warn() {
    printf "${YELLOW}[WARN]${NC} %s\n" "$*"
}

log_error() {
    printf "${RED}[ERROR]${NC} %s\n" "$*" >&2
}

debug() {
    if [ "$DEBUG" -eq 1 ]; then
        printf "${BLUE}[DEBUG]${NC} %s\n" "$*"
    fi
}

show_help() {
    grep "^#" "$0" | tail -n +2 | cut -c 3-
}

#######################################
# Kernel Module Management
#######################################

check_kernel_modules() {
    log_info "Checking kernel modules..."

    local missing=""
    local loaded=""

    for mod in $REQUIRED_MODULES; do
        if kldstat -m "$mod" >/dev/null 2>&1; then
            debug "Module $mod is already loaded"
            loaded="$loaded $mod"
        else
            debug "Module $mod is not loaded"
            missing="$missing $mod"
        fi
    done

    if [ -z "$missing" ]; then
        log_success "All required modules are loaded ($REQUIRED_MODULES)"
        return 0
    fi

    log_warn "Missing modules:$missing"
    return 1
}

load_kernel_modules() {
    log_info "Loading kernel modules..."

    for mod in $REQUIRED_MODULES; do
        if kldstat -m "$mod" >/dev/null 2>&1; then
            debug "Module $mod already loaded, skipping"
            continue
        fi

        log_info "Loading $mod..."
        if kldload "$mod" 2>/dev/null; then
            log_success "Loaded $mod"
        else
            # Try with path for ng_pppoe_lb
            if [ -f "/boot/modules/ng_pppoe_lb.ko" ]; then
                if kldload -v "/boot/modules/ng_pppoe_lb.ko" 2>/dev/null; then
                    log_success "Loaded $mod from /boot/modules/"
                else
                    log_error "Failed to load $mod"
                    return 1
                fi
            else
                log_error "Failed to load $mod"
                return 1
            fi
        fi
    done

    log_success "All required modules loaded"
    return 0
}

ensure_kernel_modules() {
    if ! check_kernel_modules; then
        log_info "Attempting to load missing modules..."
        if ! load_kernel_modules; then
            log_error "Could not load kernel modules. Build them first."
            log_error "Try: cd sys/modules/netgraph/pppoe_lb && make && make install"
            return 1
        fi
    fi
    return 0
}

#######################################
# Network Interface Management
#######################################

check_interface() {
    if ifconfig "$INTERFACE" >/dev/null 2>&1; then
        debug "Interface $INTERFACE exists"
        return 0
    else
        log_error "Interface $INTERFACE does not exist"
        return 1
    fi
}

get_interface_mac() {
    ifconfig "$INTERFACE" | grep 'ether' | awk '{print $2}'
}

#######################################
# PPPoE Load Balancer (Server) Functions
#######################################

check_lb_exists() {
    ngctl show "$LB_NODE:" >/dev/null 2>&1
}

remove_lb_node() {
    if ngctl show "$LB_NODE:" >/dev/null 2>&1; then
        log_info "Removing existing load balancer node..."
        ngctl shutdown "$LB_NODE:" 2>/dev/null || true
        sleep 0.5
    fi
}

create_server_topology() {
    log_info "Creating PPPoE load balancer with $WORKERS workers (algorithm: $ALGORITHM)..."

    # Create load balancer node
    debug "Creating load balancer node..."
    ngctl mkpeer "$INTERFACE": pppoe_lb lower link0
    ngctl name "$INTERFACE":lower "$LB_NODE"

    # Create worker nodes
    local i=0
    while [ $i -lt "$WORKERS" ]; do
        debug "Creating worker $i..."
        ngctl mkpeer "$LB_NODE": pppoe worker"$i"
        ngctl mkpeer worker"$i": eiface link"$i" pppoe
        i=$((i + 1))
    done

    # Configure load balancer
    ngctl msg "$LB_NODE": setconfig "{ algorithm=$ALGORITHM debug=$DEBUG }"

    # Configure governor if enabled
    if [ "$GOVERNOR" -eq 1 ]; then
        log_info "Enabling CPU governor auto-scaling (max: $GOVERNOR_MAX workers)..."
        sysctl net.graph.pppoe_lb.governor.enabled=1 >/dev/null 2>&1 || true
        sysctl net.graph.pppoe_lb.governor.max_workers="$GOVERNOR_MAX" >/dev/null 2>&1 || true
        sysctl net.graph.pppoe_lb.governor.cpu_threshold=80 >/dev/null 2>&1 || true
        sysctl net.graph.pppoe_lb.governor.cpu_low_threshold=20 >/dev/null 2>&1 || true
    fi

    log_success "Load balancer created: $LB_NODE with $WORKERS workers"
}

start_pppoed_daemon() {
    log_info "Starting pppoed daemon..."

    # Stop existing daemon if running
    if [ -f "$PPPOED_PID" ]; then
        local old_pid=$(cat "$PPPOED_PID")
        if [ -n "$old_pid" ] && kill -0 "$old_pid" 2>/dev/null; then
            log_info "Stopping existing pppoed (PID: $old_pid)..."
            kill "$old_pid" 2>/dev/null || true
            sleep 1
        fi
        rm -f "$PPPOED_PID"
    fi

    # Start pppoed with load balancer support
    local pppoed_args="-L -w $WORKERS"

    if [ "$GOVERNOR" -eq 1 ]; then
        pppoed_args="$pppoed_args -G"
    fi

    debug "Running: pppoed $pppoed_args $INTERFACE"

    # Start pppoed in background
    pppoed $pppoed_args "$INTERFACE" &
    local pid=$!

    echo "$pid" > "$PPPOED_PID"
    sleep 1

    if kill -0 "$pid" 2>/dev/null; then
        log_success "pppoed started (PID: $pid)"
    else
        log_error "pppoed failed to start"
        return 1
    fi
}

run_server() {
    # Check prerequisites
    if ! ensure_kernel_modules; then
        return 1
    fi

    if ! check_interface; then
        return 1
    fi

    # Clean up any existing test resources
    cleanup_lb

    # Create topology
    create_server_topology

    # Optionally start daemon
    if [ "$WORKERS" -gt 2 ] || [ "$GOVERNOR" -eq 1 ]; then
        start_pppoed_daemon
    fi

    log_success "Server mode ready"
    show_status
}

#######################################
# PPPoE Client Functions
#######################################

create_pppoe_client() {
    log_info "Creating PPPoE client session..."

    local client_if="$INTERFACE"
    local server_hwaddr="${SERVER_MAC:-auto}"

    # Check if interface exists, create if not
    if ! ifconfig "$client_if" >/dev/null 2>&1; then
        debug "Creating interface $client_if..."
        ifconfig tap create >/dev/null 2>&1 || true
    fi

    # Create PPPoE node
    ngctl mkpeer "$client_if": pppoe upper "$LB_NODE"
    ngctl name "$client_if":upper "pppoe_client"

    # Connect to server if MAC specified
    if [ "$server_hwaddr" != "auto" ]; then
        ngctl msg "pppoe_client": setaddress "$server_hwaddr"
    fi

    # Configure authentication
    ngctl msg "pppoe_client": setconfig "{ authproto=1 debug=$DEBUG }"

    # Create upper interface
    ngctl mkpeer "pppoe_client": eiface link0 pppoe

    log_success "Client session created on $client_if"
}

run_client() {
    # Check prerequisites
    if ! ensure_kernel_modules; then
        return 1
    fi

    create_pppoe_client

    log_success "Client mode ready"
    show_status
}

#######################################
# Status and Cleanup Functions
#######################################

show_status() {
    echo ""
    echo "=========================================="
    echo "PPPoE Load Balancer Status"
    echo "=========================================="
    echo ""

    echo "--- Loaded Kernel Modules ---"
    for mod in $REQUIRED_MODULES; do
        if kldstat -m "$mod" >/dev/null 2>&1; then
            printf "  ${GREEN}✓${NC} $mod\n"
        else
            printf "  ${RED}✗${NC} $mod\n"
        fi
    done
    echo ""

    echo "--- PPPoE-related Netgraph Nodes ---"
    ngctl list 2>/dev/null | grep -E '(pppoe|pppoe_lb)' || echo "  (none)"
    echo ""

    echo "--- Load Balancer Statistics ---"
    if ngctl show "$LB_NODE:" >/dev/null 2>&1; then
        ngctl msg "$LB_NODE": getstats 2>/dev/null || echo "  (stats unavailable)"
        echo ""
        ngctl msg "$LB_NODE": getmap 2>/dev/null || echo "  (map unavailable)"
    else
        echo "  Load balancer not configured"
    fi
    echo ""

    echo "--- PPPoE Interfaces ---"
    ifconfig | grep -E '^(tun|ppp)[0-9]' || echo "  (none)"
    echo ""

    echo "--- PPPoED Daemon ---"
    if [ -f "$PPPOED_PID" ]; then
        local pid=$(cat "$PPPOED_PID")
        if kill -0 "$pid" 2>/dev/null; then
            printf "  ${GREEN}✓${NC} Running (PID: $pid)\n"
        else
            printf "  ${RED}✗${NC} Not running (stale PID file)\n"
        fi
    else
        echo "  Not running"
    fi
    echo ""

    echo "--- Governor Status ---"
    sysctl net.graph.pppoe_lb.governor 2>/dev/null || echo "  (unavailable)"
    echo ""

    echo "=========================================="
}

cleanup_lb() {
    log_info "Cleaning up PPPoE load balancer resources..."

    # Stop daemon
    if [ -f "$PPPOED_PID" ]; then
        local pid=$(cat "$PPPOED_PID")
        if kill -0 "$pid" 2>/dev/null; then
            log_info "Stopping pppoed (PID: $pid)..."
            kill "$pid" 2>/dev/null || true
        fi
        rm -f "$PPPOED_PID"
    fi

    # Shutdown netgraph nodes
    for node in "$LB_NODE" worker0 worker1 worker2 worker3 worker4 worker5 worker6 worker7; do
        ngctl shutdown "$node:" 2>/dev/null || true
    done

    # Also try generic cleanup of pppoe nodes
    ngctl list 2>/dev/null | grep -E 'pppoe' | awk '{print $2}' | tr -d ':' | while read node; do
        ngctl shutdown "$node:" 2>/dev/null || true
    done

    # Clean up interfaces
    for iface in tun ppp tap; do
        ifconfig "$iface" 2>/dev/null | grep 'flags=' | awk -F: '{print $1}' | while read name; do
            ifconfig "$name" destroy 2>/dev/null || true
        done
    done

    log_success "Cleanup complete"
}

#######################################
# Parse Arguments
#######################################

parse_args() {
    # Check for help flag first
    for arg in "$@"; do
        case "$arg" in
            -h|--help)
                show_help
                exit 0
                ;;
        esac
    done

    if [ $# -eq 0 ]; then
        show_help
        exit 0
    fi

    MODE="$1"
    shift

    while [ $# -gt 0 ]; do
        case "$1" in
            -i)
                INTERFACE="$2"
                shift 2
                ;;
            -n)
                WORKERS="$2"
                shift 2
                ;;
            -a)
                ALGORITHM="$2"
                shift 2
                ;;
            -d)
                DEBUG=1
                shift
                ;;
            -g)
                GOVERNOR=1
                shift
                ;;
            -m)
                GOVERNOR_MAX="$2"
                shift 2
                ;;
            -u)
                USERNAME="$2"
                shift 2
                ;;
            -p)
                PASSWORD="$2"
                shift 2
                ;;
            -s)
                SERVER_MAC="$2"
                shift 2
                ;;
            *)
                log_error "Unknown option: $1"
                show_help
                exit 1
                ;;
        esac
    done

    # Validate arguments
    case "$MODE" in
        server|client|cleanup|status)
            ;;
        *)
            log_error "Invalid mode: $MODE"
            log_error "Use: server, client, cleanup, or status"
            exit 1
            ;;
    esac

    case "$ALGORITHM" in
        0|1|2)
            ;;
        *)
            log_error "Invalid algorithm: $ALGORITHM (use 0, 1, or 2)"
            exit 1
            ;;
    esac

    if [ "$WORKERS" -lt 1 ] || [ "$WORKERS" -gt 16 ]; then
        log_error "Invalid worker count: $WORKERS (must be 1-16)"
        exit 1
    fi
}

#######################################
# Main
#######################################

main() {
    parse_args "$@"

    case "$MODE" in
        server)
            run_server
            ;;
        client)
            run_client
            ;;
        cleanup)
            cleanup_lb
            ;;
        status)
            show_status
            ;;
    esac
}

main "$@"
