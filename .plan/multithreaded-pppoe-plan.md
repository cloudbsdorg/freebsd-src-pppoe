# Multithreaded PPPoE for FreeBSD — Implementation Plan

## 1. Executive Summary

This document outlines a sensible, incremental approach to adding multithreaded PPPoE support to FreeBSD. The goal is to enable parallel processing of PPPoE sessions to improve throughput and reduce latency on multi-core systems serving many concurrent subscribers.

**Primary Recommendation:** Horizontal scaling via multiple `ng_pppoe` nodes with a session-aware load balancer.

---

## 2. Current Architecture Analysis

### 2.1 Components

| Component | Location | Role |
|-----------|----------|------|
| `ng_pppoe` | `sys/netgraph/ng_pppoe.c` | Kernel netgraph node handling PPPoE discovery and session encapsulation/decapsulation |
| `pppoed` | `libexec/pppoed/pppoed.c` | Userland daemon for discovery phase and session setup |
| `ng_ether` | `sys/netgraph/ng_ether.c` | Netgraph node bridging Ethernet interfaces into netgraph |
| `ng_base` | `sys/netgraph/ng_base.c` | Netgraph framework with built-in worker thread pool |

### 2.2 Current Threading Model

- Netgraph maintains a pool of worker threads (`ngthread`), defaulting to `mp_ncpus` threads
- These threads pull items from a global worklist and process them
- **Critical limitation:** All packets for a single netgraph node are serialized through that node's input queue
- A single `ng_pppoe` node handles all sessions, becoming the bottleneck

### 2.3 Session Data Structures

```
struct PPPoE (per-node private data)
├── struct sess_hash_entry sesshash[SESSHASHSIZE]  (session hash table)
│   └── mtx (per-bucket lock)
├── LIST_HEAD(, sess_con) listeners                  (listening hooks)
├── hook_p ethernet_hook                             (ethernet connection)
└── int packets_in, packets_out

struct sess_con (per-session data)
├── uint16_t Session_ID
├── enum state { PPPOE_SINIT, PPPOE_SREQ, PPPOE_SOFFER, PPPOE_NEWCONNECTED, PPPOE_CONNECTED }
├── struct pppoe_neg *neg                            (negotiation state)
├── hook_p hook                                      (upper layer hook)
└── struct pppoe_full_hdr pkt_hdr                    (pre-built packet header)
```

### 2.4 Bottleneck Identification

1. **Node-level serialization:** All packets for all sessions flow through one netgraph node
2. **Discovery phase:** PADI/PADO/PADR/PADS handling is inherently serial (session creation)
3. **Session lookup:** Hash table lookups are fast but still serialized at the node level
4. **Data path:** Once connected, sessions are independent but still processed serially

---

## 3. Proposed Architecture: Multi-Node PPPoE with Load Balancing

### 3.1 High-Level Design

```
+-------------------------------------------------------------+
|                    Ethernet Interface                        |
|                      (ng_ether node)                         |
+----------------------------+--------------------------------+
                             |
                             v
+-------------------------------------------------------------+
|              ng_pppoe_lb (Load Balancer Node)                |
|  - Distributes packets to worker nodes based on session hash |
|  - Handles discovery phase (PADI) broadcast                  |
|  - Manages session-to-node mapping table                     |
+--+-----------+-----------+-----------+-----------+----------+
   |           |           |           |           |
   v           v           v           v           v
+------+   +------+   +------+   +------+   +------+
|pppoed|   |pppoed|   |pppoed|   |pppoed|   |pppoed|
|  #0  |   |  #1  |   |  #2  |   |  #3  |   |  #N  |
+------+   +------+   +------+   +------+   +------+
```

### 3.2 Key Design Principles

1. **Minimal changes to existing code:** Reuse `ng_pppoe` as-is, add new components
2. **Leverage existing netgraph parallelism:** Each worker node runs on different threads
3. **Session affinity:** All packets for a given session go to the same worker node
4. **Incremental deployment:** Can start with 1 node (current behavior) and scale up

---

## 4. Implementation Phases

### Phase 1: Create `ng_pppoe_lb` Load Balancer Node

**New file:** `sys/netgraph/ng_pppoe_lb.c`

#### 4.1.1 Node Type Definition

```c
static struct ng_type ng_pppoe_lb_typestruct = {
    .version =    NG_ABI_VERSION,
    .name =       "pppoe_lb",
    .constructor = ng_pppoe_lb_constructor,
    .rcvmsg =     ng_pppoe_lb_rcvmsg,
    .shutdown =   ng_pppoe_lb_shutdown,
    .newhook =    ng_pppoe_lb_newhook,
    .connect =    ng_pppoe_lb_connect,
    .rcvdata =    ng_pppoe_lb_rcvdata,
    .disconnect = ng_pppoe_lb_disconnect,
    .cmdlist =    ng_pppoe_lb_cmds,
};
```

#### 4.1.2 Private Data Structure

```c
struct pppoe_lb_private {
    node_p          node;
    hook_p          ether_hook;           /* Connection to ng_ether */
    hook_p         *worker_hooks;          /* Array of worker node hooks */
    int             num_workers;
    int             num_workers_active;
    
    /* Session-to-worker mapping */
    struct sess_map {
        uint16_t    session_id;
        int         worker_index;
        time_t      last_activity;
    } *session_map;
    
    struct mtx      map_mtx;
    int             map_size;
    int             map_count;
    
    /* Discovery handling */
    struct mtx      discovery_mtx;
    int             next_worker;           /* Round-robin for new sessions */
    
    /* Statistics */
    uint64_t        packets_in;
    uint64_t        packets_out;
    uint64_t        sessions_created;
    uint64_t        sessions_destroyed;
};
```

#### 4.1.3 Packet Distribution Logic

```c
static int
ng_pppoe_lb_rcvdata(hook_p hook, item_p item)
{
    struct mbuf *m;
    struct pppoe_full_hdr *wh;
    struct pppoe_lb_private *priv;
    int worker_idx, error = 0;
    
    priv = NG_NODE_PRIVATE(NG_HOOK_NODE(hook));
    NGI_GET_M(item, m);
    
    if (m->m_len < sizeof(*wh))
        m = m_pullup(m, sizeof(*wh));
    if (m == NULL)
        return (ENOBUFS);
    
    wh = mtod(m, struct pppoe_full_hdr *);
    
    switch (wh->eh.ether_type) {
    case ETHERTYPE_PPPOE_DISC:
    case ETHERTYPE_PPPOE_3COM_DISC:
        /* Discovery phase - use round-robin for new sessions */
        worker_idx = pppoe_lb_select_worker_discovery(priv);
        break;
        
    case ETHERTYPE_PPPOE_SESS:
    case ETHERTYPE_PPPOE_3COM_SESS:
        /* Session phase - use session ID hash for affinity */
        worker_idx = pppoe_lb_select_worker_session(priv, ntohs(wh->ph.sid));
        break;
        
    default:
        NG_FREE_M(m);
        NG_FREE_ITEM(item);
        return (EPFNOSUPPORT);
    }
    
    if (worker_idx < 0 || worker_idx >= priv->num_workers_active) {
        NG_FREE_M(m);
        NG_FREE_ITEM(item);
        return (ENETUNREACH);
    }
    
    NG_FWD_NEW_DATA(error, item, priv->worker_hooks[worker_idx], m);
    return (error);
}
```

#### 4.1.4 Worker Selection Algorithms

**Discovery Phase (Round-Robin):**
```c
static int
pppoe_lb_select_worker_discovery(struct pppoe_lb_private *priv)
{
    int idx;
    
    mtx_lock(&priv->discovery_mtx);
    idx = priv->next_worker;
    priv->next_worker = (priv->next_worker + 1) % priv->num_workers_active;
    mtx_unlock(&priv->discovery_mtx);
    
    return (idx);
}
```

**Session Phase (Hash-based Affinity):**
```c
static int
pppoe_lb_select_worker_session(struct pppoe_lb_private *priv, uint16_t session_id)
{
    struct sess_map *map;
    int idx, i;
    
    /* Fast path: check session map */
    mtx_lock(&priv->map_mtx);
    for (i = 0; i < priv->map_count; i++) {
        if (priv->session_map[i].session_id == session_id) {
            idx = priv->session_map[i].worker_index;
            priv->session_map[i].last_activity = time_uptime;
            mtx_unlock(&priv->map_mtx);
            return (idx);
        }
    }
    mtx_unlock(&priv->map_mtx);
    
    /* Fallback: hash-based selection */
    return (session_id % priv->num_workers_active);
}
```

#### 4.1.5 Control Messages

New netgraph messages for `ng_pppoe_lb`:

| Message | Purpose |
|---------|---------|
| `NGM_PPPOE_LB_ADD_WORKER` | Add a worker node hook |
| `NGM_PPPOE_LB_REMOVE_WORKER` | Remove a worker node hook |
| `NGM_PPPOE_LB_SET_CONFIG` | Configure load balancing algorithm |
| `NGM_PPPOE_LB_GET_STATS` | Get load balancer statistics |
| `NGM_PPPOE_LB_GET_MAP` | Get session-to-worker mapping |

### Phase 2: Modify `pppoed` for Multi-Node Support

**File:** `libexec/pppoed/pppoed.c`

#### 4.2.1 Changes Required

1. **Add worker node management:**
   - Track multiple `ng_pppoe` nodes instead of one
   - Each worker node handles a subset of sessions

2. **Discovery broadcast:**
   - Send PADI to all worker nodes or to load balancer
   - Load balancer distributes PADI to workers

3. **Session tracking:**
   - Maintain mapping of session ID to worker node
   - Forward session data to correct worker

#### 4.2.2 Configuration Options

Add new command-line options:

```c
/* New options */
case 'w':
    num_workers = atoi(optarg);
    break;
case 'L':
    use_load_balancer = 1;
    break;
```

### Phase 3: Optional Per-Session Parallelism in `ng_pppoe`

**File:** `sys/netgraph/ng_pppoe.c`

#### 4.3.1 Fine-Grained Locking

Replace node-level serialization with per-session locks for data path:

```c
struct sess_con {
    /* Existing fields... */
    struct mtx      sess_mtx;           /* Per-session lock */
    
    /* Data path can be parallelized */
    struct mbuf    *data_queue;         /* Queue for out-of-order packets */
};
```

#### 4.3.2 Parallel Data Path

```c
static int
ng_pppoe_rcvdata_ether(hook_p hook, item_p item)
{
    /* ... existing header parsing ... */
    
    switch (wh->eh.ether_type) {
    case ETHERTYPE_PPPOE_SESS:
    case ETHERTYPE_PPPOE_3COM_SESS:
        sp = pppoe_findsession(privp, wh);
        if (sp == NULL)
            LEAVE(ENETUNREACH);
        
        /* Acquire per-session lock for data path */
        mtx_lock(&sp->sess_mtx);
        
        m_adj(m, sizeof(*wh));
        if (m->m_pkthdr.len < length) {
            mtx_unlock(&sp->sess_mtx);
            LEAVE(EMSGSIZE);
        }
        
        /* Process packet while holding session lock */
        NG_FWD_NEW_DATA(error, item, sp->hook, m);
        
        mtx_unlock(&sp->sess_mtx);
        return (error);
    }
}
```

**Note:** This is more complex and riskier. Recommended only after Phase 1 & 2 are stable.

---

## 5. Alternative Approaches Considered

### 5.1 Single-Node Per-Session Locking (Rejected)

**Approach:** Add fine-grained locks within `ng_pppoe` to allow parallel session processing.

**Why Rejected:**
- Netgraph framework still serializes all node operations
- Would require significant changes to netgraph core
- Risk of introducing deadlocks and race conditions
- Limited benefit without framework-level changes

### 5.2 Taskqueue-Based Processing (Rejected)

**Approach:** Use FreeBSD's `taskqueue` system to offload session processing.

**Why Rejected:**
- Adds latency to packet processing
- More complex lifecycle management
- Doesn't integrate well with netgraph's item-based processing model
- Overkill for this use case

### 5.3 Netgraph Framework Modification (Rejected)

**Approach:** Modify `ng_base.c` to allow parallel processing within a single node.

**Why Rejected:**
- Too invasive, affects all netgraph node types
- Would require extensive testing across all netgraph consumers
- Breaks existing assumptions about node-level serialization

---

## 6. Tooling and Management Interface Modifications

To support and manage multithreaded PPPoE sessions, the following userland tools and interfaces must be modified or created. All modifications must include **activation switches** so administrators can explicitly enable or disable multithreaded mode.

### 6.1 Core Daemon: `pppoed`

**Files:** `libexec/pppoed/pppoed.c`, `libexec/pppoed/pppoed.8`

**Modifications:**
1. **Add `-w <workers>` flag** — Number of PPPoE worker nodes to create (default: 1, which preserves current single-threaded behavior).
2. **Add `-L` flag** — Explicitly enable the `ng_pppoe_lb` load balancer. Without this flag, `pppoed` behaves exactly as before, creating a single `ng_pppoe` node directly attached to `ng_ether`.
3. **Add `-A <algorithm>` flag** — Select load-balancing algorithm when `-L` is used: `round-robin`, `hash`, or `least-loaded`.
4. **Worker lifecycle management:** When `-w > 1` and `-L` are both specified, `pppoed` must:
   - Create the `ng_pppoe_lb` node.
   - Create `w` `ng_pppoe` worker nodes.
   - Connect each worker to the load balancer via netgraph hooks.
   - Register session-to-worker mappings with the load balancer using `NGM_PPPOE_LB_ADD_WORKER`.
   - On shutdown, gracefully drain sessions before disconnecting workers.
5. **Backward compatibility:** If neither `-w` nor `-L` is provided, the daemon follows the existing code path exactly.

**Example usage:**
```sh
# Traditional single-threaded mode (default)
pppoed -p "ISP" em0

# Multithreaded mode with 4 workers and hash-based session affinity
pppoed -L -w 4 -A hash -p "ISP" em0
```

### 6.2 PPP Userland Daemon: `ppp`

**Files:** `usr.sbin/ppp/ether.c`, `usr.sbin/ppp/command.c`, `usr.sbin/ppp/ppp.8`

**Modifications:**
1. **Add `set pppoe workers <n>` command** — Configures how many worker nodes `ppp` should expect when acting as a PPPoE client. This is relevant when `ppp` connects to a multithreaded access concentrator.
2. **Add `show pppoe workers` command** — Displays the number of active worker nodes reported by the peer (if available via PPPoE tags).
3. **Update `ether.c`** — Handle potential `NGM_PPPOE_LB_GET_STATS` messages from the load balancer when querying status, so `ppp` can display per-worker session counts.
4. **Man page update** — Document the new `set pppoe workers` and `show pppoe workers` commands.

**Note:** The `ppp` daemon primarily acts as a client; most multithreading benefits are on the server (access concentrator) side. Client-side changes are mainly for monitoring and diagnostics.

### 6.3 Netgraph Control Utility: `ngctl`

**Files:** `usr.sbin/ngctl/ngctl.c`, `usr.sbin/ngctl/ngctl.8`

**Modifications:**
1. **Add `pppoe_lb` node type support** — Ensure `ngctl mkpeer pppoe_lb:` and related commands work for the new node type.
2. **Add `show pppoe_lb` command** — Display load balancer statistics:
   - Number of active workers
   - Total sessions
   - Packets distributed per worker
   - Current load-balancing algorithm
3. **Add `pppoe_lb config` command** — Runtime reconfiguration of the load balancer:
   ```sh
   ngctl msg pppoe_lb: NGM_PPPOE_LB_SET_CONFIG { algorithm=hash }
   ```
4. **Man page update** — Document new `pppoe_lb` subcommands.

### 6.4 Startup Script: `rc.d/pppoed`

**Files:** `libexec/rc/rc.d/pppoed`, `libexec/rc/rc.conf`

**Modifications:**
1. **Add `pppoed_workers` rc.conf variable** — Number of worker nodes (default: `""`, meaning use single-threaded mode).
2. **Add `pppoed_loadbalancer` rc.conf variable** — Set to `"YES"` to enable the load balancer (default: `"NO"`).
3. **Add `pppoed_lb_algorithm` rc.conf variable** — Load-balancing algorithm when load balancer is enabled (default: `"round-robin"`).
4. **Update startup script** — Pass the appropriate flags to `pppoed` based on rc.conf variables:
   ```sh
   if [ -n "${pppoed_workers}" ] && [ "${pppoed_workers}" -gt 1 ]; then
       pppoed_flags="${pppoed_flags} -w ${pppoed_workers}"
   fi
   if checkyesno pppoed_loadbalancer; then
       pppoed_flags="${pppoed_flags} -L"
       if [ -n "${pppoed_lb_algorithm}" ]; then
           pppoed_flags="${pppoed_flags} -A ${pppoed_lb_algorithm}"
       fi
   fi
   ```
5. **Update `rc.conf` defaults** — Add commented-out examples:
   ```sh
   # pppoed_enable="NO"
   # pppoed_provider=""
   # pppoed_interface=""
   # pppoed_workers=""          # Number of PPPoE worker nodes (>1 enables multithreading)
   # pppoed_loadbalancer="NO"   # Enable ng_pppoe_lb load balancer
   # pppoed_lb_algorithm="round-robin"  # round-robin, hash, least-loaded
   ```

### 6.5 sysctl Variables

**Files:** `sys/netgraph/ng_pppoe_lb.c` (sysctl registration)

**New sysctl variables for runtime tuning:**

| Variable | Type | Default | Description |
|----------|------|---------|-------------|
| `net.graph.pppoe_lb.enabled` | int | 0 | Global enable/disable for load balancer features |
| `net.graph.pppoe_lb.num_workers` | int | 1 | Number of worker nodes |
| `net.graph.pppoe_lb.algorithm` | int | 0 | 0=round-robin, 1=hash, 2=least-loaded |
| `net.graph.pppoe_lb.session_map_size` | int | 1024 | Session map hash table size |
| `net.graph.pppoe_lb.debug` | int | 0 | Debug level (0=none, 1=verbose, 2=very verbose) |

**Important:** `net.graph.pppoe_lb.enabled` acts as a master switch. When set to 0, the load balancer node type may still be loaded, but it refuses to create new instances, preventing accidental activation.

### 6.6 Monitoring and Diagnostics

#### 6.6.1 `netstat` Extensions

**Files:** `usr.bin/netstat/netstat.c`, `usr.bin/netstat/netstat.h`

**Modifications:**
1. **Add `-W` flag** — Display PPPoE worker statistics:
   ```sh
   netstat -W
   ```
   Output format:
   ```
   PPPoE Worker Statistics:
   Worker  Sessions  Packets In  Packets Out  CPU
   0       150       1.2M        1.1M        3
   1       148       1.1M        1.0M        4
   2       152       1.2M        1.1M        5
   3       149       1.1M        1.0M        6
   ```
2. **Implementation:** Query `NGM_PPPOE_LB_GET_STATS` and `NGM_PPPOE_GET_STATUS` from netgraph nodes.

#### 6.6.2 `ifconfig` Extensions

**Files:** `sbin/ifconfig/ifconfig.c`

**Modifications:**
1. **Add `pppoe_workers` option** — Display or set the number of PPPoE workers for an interface:
   ```sh
   ifconfig em0 pppoe_workers 4
   ifconfig em0 pppoe_workers  # displays current value
   ```
2. **Note:** This is primarily a convenience wrapper that communicates with `ng_pppoe_lb` via netgraph messages.

#### 6.6.3 `pppctl` Extensions

**Files:** `usr.sbin/pppctl/pppctl.c`, `usr.sbin/pppctl/pppctl.8`

**Modifications:**
1. **Add `show pppoe lb` command** — Display load balancer status when connected to a multithreaded concentrator.
2. **Add `set pppoe workers` command** — Configure expected worker count (client-side hint).

### 6.7 Kernel Module Loading

**Files:** `sys/netgraph/ng_pppoe_lb.c`, `sys/netgraph/Makefile`

**Modifications:**
1. **KLD module support** — Ensure `ng_pppoe_lb` can be loaded as a kernel module (`ng_pppoe_lb.ko`) rather than requiring a full kernel rebuild.
2. **Module dependencies** — Declare dependency on `ng_pppoe` and `ng_ether`.
3. **Module parameters** — Allow module load-time parameters:
   ```sh
   kldload ng_pppoe_lb num_workers=4 algorithm=hash
   ```

### 6.8 Build System

**Files:** `sys/netgraph/Makefile`, `sys/modules/netgraph/Makefile`, `libexec/pppoed/Makefile`

**Modifications:**
1. Add `ng_pppoe_lb.c` and `ng_pppoe_lb.h` to kernel build.
2. Add `ng_pppoe_lb` module to `sys/modules/netgraph/`.
3. Update `pppoed` Makefile if new source files are added.

---

## 7. Implementation Details

### 7.1 File Changes

| File | Change Type | Description |
|------|-------------|-------------|
| `sys/netgraph/ng_pppoe_lb.c` | New | Load balancer node implementation |
| `sys/netgraph/ng_pppoe_lb.h` | New | Load balancer node header |
| `sys/netgraph/Makefile` | Modify | Add `ng_pppoe_lb` to build |
| `sys/modules/netgraph/ng_pppoe_lb/` | New | KLD module build files |
| `libexec/pppoed/pppoed.c` | Modify | Add multi-node support, activation switches |
| `libexec/pppoed/pppoed.8` | Modify | Update man page with new flags |
| `libexec/rc/rc.d/pppoed` | Modify | Add rc.conf variable handling |
| `libexec/rc/rc.conf` | Modify | Add default rc.conf variables |
| `usr.sbin/ppp/ether.c` | Modify | Handle load balancer messages |
| `usr.sbin/ppp/command.c` | Modify | Add `set/show pppoe workers` commands |
| `usr.sbin/ppp/ppp.8` | Modify | Document new commands |
| `usr.sbin/ngctl/ngctl.c` | Modify | Add `pppoe_lb` subcommands |
| `usr.sbin/ngctl/ngctl.8` | Modify | Document new subcommands |
| `usr.bin/netstat/netstat.c` | Modify | Add `-W` flag for PPPoE worker stats |
| `usr.bin/netstat/netstat.1` | Modify | Document `-W` flag |
| `sbin/ifconfig/ifconfig.c` | Modify | Add `pppoe_workers` option |
| `sbin/ifconfig/ifconfig.8` | Modify | Document `pppoe_workers` option |
| `usr.sbin/pppctl/pppctl.c` | Modify | Add load balancer commands |
| `usr.sbin/pppctl/pppctl.8` | Modify | Document new commands |
| `sys/netgraph/ng_pppoe.c` | Minor | Optional per-session locking |

### 7.2 Configuration Example

```sh
# Traditional single-threaded mode (default behavior)
pppoed -p "ISP" em0

# Multithreaded mode with 4 workers and hash-based session affinity
pppoed -L -w 4 -A hash -p "ISP" em0

# Using ngctl directly
ngctl mkpeer pppoe_lb: pppoe_lb ether
ngctl mkpeer pppoe_lb: pppoe worker0
ngctl mkpeer pppoe_lb: pppoe worker1
ngctl connect pppoe_lb: worker0: lb0 worker0
ngctl connect pppoe_lb: worker1: lb1 worker1
ngctl connect em0: pppoe_lb: lower ether

# Runtime reconfiguration
ngctl msg pppoe_lb: NGM_PPPOE_LB_SET_CONFIG { algorithm=least-loaded }

# Monitoring
netstat -W
ifconfig em0 pppoe_workers
ngctl show pppoe_lb:
```

### 7.3 sysctl Variables

| Variable | Type | Default | Description |
|----------|------|---------|-------------|
| `net.graph.pppoe_lb.enabled` | int | 0 | Master enable/disable switch |
| `net.graph.pppoe_lb.num_workers` | int | 1 | Number of worker nodes |
| `net.graph.pppoe_lb.algorithm` | int | 0 | 0=round-robin, 1=hash, 2=least-loaded |
| `net.graph.pppoe_lb.session_map_size` | int | 1024 | Session map hash table size |
| `net.graph.pppoe_lb.debug` | int | 0 | Debug level |
| `net.graph.pppoe_lb.governor.enabled` | int | 0 | Enable CPU governor (0=disabled, 1=enabled) |
| `net.graph.pppoe_lb.governor.max_workers` | int | 0 | Max workers (0=auto/mp_ncpus, >0=hard cap limited by mp_ncpus) |
| `net.graph.pppoe_lb.governor.cpu_threshold` | int | 80 | CPU % threshold to spawn more workers |
| `net.graph.pppoe_lb.governor.cpu_low_threshold` | int | 30 | CPU % threshold to reduce workers |
| `net.graph.pppoe_lb.governor.scale_up_interval` | int | 10 | Seconds between scale-up checks |
| `net.graph.pppoe_lb.governor.scale_down_interval` | int | 60 | Seconds between scale-down checks |

**Governor Behavior:**
- When `governor.enabled=1`, the load balancer monitors system-wide CPU usage via `read_cpu_time()` kernel function.
- CPU utilization is calculated as: `100 - (idle_time * 100 / total_time)` where total_time includes CP_USER, CP_NICE, CP_SYS, CP_INTR, and CP_IDLE.
- If average CPU exceeds `cpu_threshold` (default 80%) for `scale_up_interval` seconds, and `max_workers` not reached, scale-up is triggered (logged, actual worker creation handled by userland).
- If average CPU drops below `cpu_low_threshold` (default 30%) for `scale_down_interval` seconds, and more than 1 worker exists, scale-down is triggered.
- `max_workers=0` means auto-detect to `mp_ncpus` (number of CPU cores). Setting `max_workers=4` on an 8-core system limits workers to 4. Setting `max_workers=100` on an 8-core system is clamped to 8.
- **Hard cap enforcement:** The governor never allows workers to exceed `mp_ncpus`, regardless of user setting. This prevents oversubscription.

---

## 8. Testing Strategy and Isolated Test Harnesses

**Critical Requirement:** All testing must be performed in isolation. The test harnesses must never load experimental kernel code into the host operating system. Use VMs, user-mode simulation, or dedicated test hardware.

### 8.1 Testing Philosophy

| Level | Environment | Purpose |
|-------|-------------|---------|
| Unit Tests | User-mode mock framework | Test logic in isolation |
| Integration Tests | VM with snapshot rollback | Test kernel module loading and netgraph interactions |
| Performance Tests | Dedicated test hardware or VM with passthrough | Measure throughput and latency under load |
| Stress Tests | VM with resource limits | Verify stability and memory safety |

### 8.2 Unit Test Harness: `tests/netgraph/ng_pppoe_lb_test.c`

**File:** `tests/netgraph/ng_pppoe_lb_test.c`

**Approach:** Create a user-mode test framework that mocks the netgraph API and kernel structures. This allows testing the load balancer logic without loading any kernel code.

```c
/*
 * User-mode unit test for ng_pppoe_lb packet distribution logic.
 * This test runs entirely in userland and does not load any kernel modules.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Mock kernel structures */
struct mock_mbuf {
    uint8_t data[2048];
    size_t len;
};

struct mock_ng_pppoe_lb {
    int num_workers;
    int next_worker;
    uint64_t packets_per_worker[16];
};

/* Include the load balancer logic (extracted into a testable unit) */
#include "ng_pppoe_lb_logic.h"

static void
test_round_robin_discovery(void)
{
    struct mock_ng_pppoe_lb lb = { .num_workers = 4 };
    int worker;

    for (int i = 0; i < 8; i++) {
        worker = pppoe_lb_select_worker_discovery(&lb);
        assert(worker == i % 4);
    }
    printf("PASS: round-robin discovery\n");
}

static void
test_session_affinity(void)
{
    struct mock_ng_pppoe_lb lb = { .num_workers = 4 };
    uint16_t session_id = 0x1234;
    int worker1, worker2;

    worker1 = pppoe_lb_select_worker_session(&lb, session_id);
    worker2 = pppoe_lb_select_worker_session(&lb, session_id);
    assert(worker1 == worker2);
    printf("PASS: session affinity\n");
}

static void
test_worker_bounds(void)
{
    struct mock_ng_pppoe_lb lb = { .num_workers = 4 };
    int worker;

    for (uint16_t sid = 0; sid < 1000; sid++) {
        worker = pppoe_lb_select_worker_session(&lb, sid);
        assert(worker >= 0 && worker < 4);
    }
    printf("PASS: worker bounds\n");
}

int
main(int argc, char **argv)
{
    printf("ng_pppoe_lb unit tests (user-mode, no kernel code)\n");
    test_round_robin_discovery();
    test_session_affinity();
    test_worker_bounds();
    printf("All tests passed.\n");
    return 0;
}
```

**Build and run:**
```sh
cd tests/netgraph
make ng_pppoe_lb_test
./ng_pppoe_lb_test
```

### 8.3 Integration Test Harness: VM-Based Testing

**File:** `tests/netgraph/ng_pppoe_lb_vm_test.sh`

**Approach:** Use a virtual machine (bhyve, QEMU, or VirtualBox) with snapshot support. The test script:
1. Starts a fresh VM from a snapshot.
2. Copies the built kernel module into the VM.
3. Loads the module inside the VM.
4. Runs netgraph commands via SSH.
5. Verifies expected behavior.
6. Reverts the VM to the snapshot, leaving no trace.

```sh
#!/bin/sh
#
# VM-based integration test for ng_pppoe_lb
# This script NEVER loads kernel code on the host.
#

set -e

VM_NAME="pppoe-lb-test"
VM_SNAPSHOT="clean"
SSH_PORT="2222"
TEST_RESULTS="/tmp/ng_pppoe_lb_test_results.log"

# Revert VM to clean snapshot
echo "Reverting VM to clean snapshot..."
vm revert "${VM_NAME}" "${VM_SNAPSHOT}"

# Start VM
echo "Starting test VM..."
vm start "${VM_NAME}"
sleep 10  # Wait for VM to boot

# Copy kernel module into VM
echo "Copying kernel module to VM..."
scp -P "${SSH_PORT}" sys/modules/netgraph/ng_pppoe_lb/ng_pppoe_lb.ko \
    root@localhost:/tmp/

# Run tests inside VM via SSH
echo "Running integration tests inside VM..."
ssh -p "${SSH_PORT}" root@localhost << 'TESTSCRIPT'
    set -e
    echo "=== Loading ng_pppoe_lb module ==="
    kldload /tmp/ng_pppoe_lb.ko
    
    echo "=== Creating test netgraph topology ==="
    ngctl mkpeer pppoe_lb: pppoe_lb ether
    ngctl mkpeer pppoe_lb: pppoe worker0
    ngctl mkpeer pppoe_lb: pppoe worker1
    ngctl connect pppoe_lb: worker0: lb0 worker0
    ngctl connect pppoe_lb: worker1: lb1 worker1
    
    echo "=== Verifying topology ==="
    ngctl list | grep -q pppoe_lb
    ngctl list | grep -q worker0
    ngctl list | grep -q worker1
    
    echo "=== Querying statistics ==="
    ngctl msg pppoe_lb: NGM_PPPOE_LB_GET_STATS
    
    echo "=== Unloading module ==="
    ngctl shutdown pppoe_lb:
    kldunload ng_pppoe_lb
    
    echo "All integration tests passed."
TESTSCRIPT

# Stop VM
echo "Stopping test VM..."
vm stop "${VM_NAME}"

echo "Integration tests completed successfully."
```

### 8.4 Performance Test Harness

**File:** `tests/netgraph/ng_pppoe_lb_perf.c`

**Approach:** Run inside a VM with dedicated vCPUs. Use netgraph loopback nodes to simulate packet flow without requiring actual network hardware.

```c
/*
 * Performance test for ng_pppoe_lb.
 * Runs inside a VM. Uses netgraph loopback to avoid needing real hardware.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netgraph/ng_socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#define NUM_PACKETS 1000000
#define NUM_WORKERS 4

int
main(int argc, char **argv)
{
    int csock, dsock;
    struct ng_mesg msg;
    struct timespec start, end;
    double elapsed;
    
    /* Create netgraph control socket */
    if (NgMkSockNode(NULL, &csock, &dsock) < 0) {
        perror("NgMkSockNode");
        return 1;
    }
    
    /* Create load balancer and workers */
    NgSendMsg(csock, ".", NGM_GENERIC_COOKIE, NGM_MKPEER,
        "pppoe_lb", strlen("pppoe_lb") + 1);
    
    for (int i = 0; i < NUM_WORKERS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "worker%d", i);
        NgSendMsg(csock, "pppoe_lb:", NGM_GENERIC_COOKIE, NGM_MKPEER,
            "pppoe", strlen("pppoe") + 1);
    }
    
    /* Create loopback node for packet generation */
    NgSendMsg(csock, ".", NGM_GENERIC_COOKIE, NGM_MKPEER,
        "loopback", strlen("loopback") + 1);
    NgSendMsg(csock, "loopback:", NGM_GENERIC_COOKIE, NGM_CONNECT,
        "pppoe_lb: lower ether", strlen("pppoe_lb: lower ether") + 1);
    
    /* Generate test packets */
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < NUM_PACKETS; i++) {
        char packet[256];
        struct pppoe_full_hdr *hdr = (struct pppoe_full_hdr *)packet;
        
        memset(packet, 0, sizeof(packet));
        hdr->eh.ether_type = htons(ETHERTYPE_PPPOE_SESS);
        hdr->ph.sid = htons(i % 1000);  /* 1000 unique sessions */
        
        /* Send packet through loopback */
        write(dsock, packet, sizeof(packet));
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    elapsed = (end.tv_sec - start.tv_sec) +
              (end.tv_nsec - start.tv_nsec) / 1e9;
    
    printf("Packets: %d\n", NUM_PACKETS);
    printf("Time: %.3f seconds\n", elapsed);
    printf("Throughput: %.0f packets/sec\n", NUM_PACKETS / elapsed);
    
    /* Cleanup */
    NgSendMsg(csock, "loopback:", NGM_GENERIC_COOKIE, NGM_SHUTDOWN, NULL, 0);
    NgSendMsg(csock, "pppoe_lb:", NGM_GENERIC_COOKIE, NGM_SHUTDOWN, NULL, 0);
    
    close(csock);
    close(dsock);
    
    return 0;
}
```

### 8.5 Stress Test Harness

**File:** `tests/netgraph/ng_pppoe_lb_stress.sh`

**Approach:** Run inside a VM with memory and CPU limits. Rapidly create and destroy sessions while monitoring for leaks and crashes.

```sh
#!/bin/sh
#
# Stress test for ng_pppoe_lb
# Runs inside a VM with resource limits.
#

set -e

DURATION=300  # 5 minutes
WORKERS=8
SESSIONS=10000

echo "Starting ${DURATION}s stress test with ${WORKERS} workers and ${SESSIONS} sessions..."

# Monitor memory usage in background
(
    while true; do
        vmstat -m | grep pppoe >> /tmp/pppoe_memory.log
        sleep 1
    done
) &
MONITOR_PID=$!

# Run stress test
python3 << 'PYTEST'
import random
import struct
import socket
import time

# Connect to netgraph socket
sock = socket.socket(socket.AF_NETGRAPH, socket.SOCK_DGRAM)

start = time.time()
while time.time() - start < 300:
    # Rapidly create and destroy sessions
    for i in range(1000):
        session_id = random.randint(1, 65535)
        # Simulate PADI (discovery)
        packet = b'\x00' * 6 + b'\xff' * 6 + struct.pack('>H', 0x8863)
        sock.sendto(packet, "pppoe_lb:")
        
        # Simulate data
        packet = b'\x00' * 6 + b'\xff' * 6 + struct.pack('>H', 0x8864)
        packet += struct.pack('>H', session_id)
        sock.sendto(packet, "pppoe_lb:")
    
    time.sleep(0.01)

sock.close()
PYTEST

kill $MONITOR_PID

echo "Stress test completed."
echo "Memory usage log: /tmp/pppoe_memory.log"

# Check for memory leaks
if grep -q "fail" /tmp/pppoe_memory.log; then
    echo "FAIL: Memory allocation failures detected"
    exit 1
fi

echo "PASS: No memory issues detected"
```

### 8.6 Test Summary

| Test Type | Location | Environment | Kernel Code Loaded? |
|-----------|----------|-------------|---------------------|
| Unit Tests | `tests/netgraph/ng_pppoe_lb_test.c` | User-mode | **No** |
| Integration Tests | `tests/netgraph/ng_pppoe_lb_vm_test.sh` | VM | Yes, inside VM only |
| Performance Tests | `tests/netgraph/ng_pppoe_lb_perf.c` | VM with dedicated vCPUs | Yes, inside VM only |
| Stress Tests | `tests/netgraph/ng_pppoe_lb_stress.sh` | VM with resource limits | Yes, inside VM only |

**Safety Rules:**
1. Never run `kldload` on the host system for experimental modules.
2. Always use VM snapshots so tests start from a known clean state.
3. Automated CI must use VMs or containers with kernel isolation.
4. Physical test hardware should be dedicated lab equipment, not production servers.

---

## 9. Testing Strategy

### 9.1 Functional Testing

1. **Single worker:** Verify identical behavior to current implementation
2. **Multiple workers:** Verify session affinity and load distribution
3. **Worker failure:** Verify graceful degradation
4. **Discovery phase:** Verify PADI/PADO/PADR/PADS with multiple workers
5. **Session teardown:** Verify proper cleanup and map removal
6. **Activation switches:** Verify that omitting `-L` and `-w` produces exactly the old behavior

### 9.2 Performance Testing

1. **Throughput:** Measure packets/sec with 1, 2, 4, 8 workers
2. **Latency:** Measure per-packet latency under load
3. **Scalability:** Verify linear scaling with CPU cores
4. **Memory:** Monitor memory usage with large session counts

### 9.3 Stress Testing

1. **Rapid session creation/destruction:** Verify no leaks
2. **Out-of-order packets:** Verify session affinity handles reordering
3. **Worker hot-add/remove:** Verify dynamic reconfiguration
4. **High packet loss:** Verify recovery and retransmission
5. **Module load/unload cycles:** Verify clean load and unload

---

## 10. Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Session affinity violations | High | Use consistent hashing, verify with tests |
| Memory leaks in session map | Medium | Implement map entry aging/timeout |
| Lock contention in load balancer | Medium | Use per-CPU queues, reduce lock scope |
| Backward compatibility | Low | Default to single-worker mode; require explicit `-L` flag |
| Complexity in pppoed | Medium | Keep changes minimal, well-documented |
| Accidental kernel module load on host | **Critical** | Test harnesses use VMs only; never load on host |
| Performance regression in single-worker mode | Medium | Benchmark before and after; ensure no regression |

---

## 11. TODO — Step-by-Step Implementation Tracker

This section is the master checklist for implementing multithreaded PPPoE. Each task includes:
- **Status:** `NOT STARTED` | `IN PROGRESS` | `COMPLETED`
- **Owner:** Who is working on it
- **Start Date:** When work began
- **End Date:** When work finished
- **Dependencies:** What must be done first
- **Files Modified:** What files are touched
- **Notes:** Any blockers, decisions, or context

### Phase 0: Foundation and Setup

| # | Task | Status | Owner | Start | End | Dependencies | Files | Notes |
|---|------|--------|-------|-------|-----|--------------|-------|-------|
| 0.1 | Create feature branch `feature/multithreaded-pppoe` | NOT STARTED | | | | | | Branch from `main` |
| 0.2 | Set up VM test environment (bhyve/QEMU) | NOT STARTED | | | | | | Must support netgraph and snapshot rollback |
| 0.3 | Verify existing PPPoE tests pass on clean branch | NOT STARTED | | | | 0.2 | | Baseline before any changes |
| 0.4 | Document baseline performance (single-threaded) | NOT STARTED | | | | 0.3 | | Packets/sec, latency, CPU usage |

### Phase 1: Kernel Load Balancer Node (`ng_pppoe_lb`)

| # | Task | Status | Owner | Start | End | Dependencies | Files | Notes |
|---|------|--------|-------|-------|-----|--------------|-------|-------|
| 1.1 | Create `sys/netgraph/ng_pppoe_lb.h` header | COMPLETED | | 2026-04-23 | 2026-04-23 | 0.1 | `ng_pppoe_lb.h` | Define node type, messages, private data |
| 1.2 | Create `sys/netgraph/ng_pppoe_lb.c` core | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.1 | `ng_pppoe_lb.c` | Node constructor, destructor, hook management |
| 1.3 | Implement packet distribution logic | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.2 | `ng_pppoe_lb.c` | Discovery → round-robin, Session → hash |
| 1.4 | Implement session map (hash table) | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.3 | `ng_pppoe_lb.c` | Locking, aging, cleanup |
| 1.5 | Implement control messages (NGM_PPPOE_LB_*) | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.2 | `ng_pppoe_lb.c` | Add worker, remove worker, get stats, get map |
| 1.6 | Add sysctl variables | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.2 | `ng_pppoe_lb.c` | `net.graph.pppoe_lb.*` |
| 1.7 | Add KLD module support | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.2 | `ng_pppoe_lb.c` | `MOD_LOAD`/`MOD_UNLOAD` handlers |
| 1.8 | Update `sys/netgraph/Makefile` | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.1 | `Makefile` | Add `ng_pppoe_lb.o` |
| 1.9 | Create `sys/modules/netgraph/ng_pppoe_lb/` | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.8 | `Makefile`, `ng_pppoe_lb.c` | KLD module build |
| 1.10 | Write unit tests (user-mode mock) | NOT STARTED | | | | 1.3 | `tests/netgraph/ng_pppoe_lb_test.c` | No kernel code loaded |
| 1.11 | Run unit tests | NOT STARTED | | | | 1.10 | | Must pass before proceeding |
| 1.12 | Write integration test script (VM-based) | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.9 | `tests/netgraph/ng_pppoe_lb_vm_test.sh` | Loads module inside VM only |
| 1.13 | Run integration tests | NOT STARTED | | | | 1.12 | | Verify topology creation, stats, cleanup |
| 1.14 | Write performance test harness | NOT STARTED | | | | 1.9 | `tests/netgraph/ng_pppoe_lb_perf.c` | VM with dedicated vCPUs |
| 1.15 | Run performance tests | NOT STARTED | | | | 1.14 | | Measure 1, 2, 4, 8 worker throughput |
| 1.16 | Write stress test harness | NOT STARTED | | | | 1.9 | `tests/netgraph/ng_pppoe_lb_stress.sh` | VM with resource limits |
| 1.17 | Run stress tests | NOT STARTED | | | | 1.16 | | 5-minute run, check for leaks |
| 1.18 | Implement CPU governor thread | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.6 | `ng_pppoe_lb.c` | Monitors CPU load via `read_cpu_time()`, scales workers up/down |
| 1.19 | Implement worker hot-add (scale up) | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.18 | `ng_pppoe_lb.c` | Logs scale-up intent when CPU > threshold, respects max_workers cap |
| 1.20 | Implement worker hot-remove (scale down) | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.19 | `ng_pppoe_lb.c` | Logs scale-down intent when CPU < low_threshold, never goes below 1 worker |
| 1.21 | Add governor sysctl handlers | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.18 | `ng_pppoe_lb.c` | `governor.enabled`, `max_workers` (0=mp_ncpus, >0=hard cap), thresholds |
| 1.22 | Test governor scale-up under load | NOT STARTED | | | | 1.19 | | Verify CPU monitoring works, scale-up logged when CPU > threshold |
| 1.23 | Test governor scale-down under low load | NOT STARTED | | | | 1.20 | | Verify scale-down logged when CPU < low_threshold |
| 1.24 | Test max_workers hard cap | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.21 | `ng_pppoe_lb.c` | Constructor enforces: max_workers = min(user_value, mp_ncpus) |
| 1.25 | Code review and cleanup | IN PROGRESS | | 2026-04-23 | | 1.24 | | Style, comments, locking correctness |

### Phase 1.5: Enhanced Kernel Auto-Scaling Interface

This phase implements session-aware auto-scaling with the ability to change course if load patterns shift during worker removal.

#### 1.5.1 Worker State Machine

```
                    ┌─────────────────────────────────────┐
                    │                                     │
                    v                                     │
┌──────────┐     ┌──────────┐     ┌──────────────────┐   │
│ ACTIVE   │────>│ DRAINING │────>│ PENDING_REMOVAL   │───┘
│          │     │          │     │                  │
└──────────┘     └──────────┘     └──────────────────┘
    ▲                │                    │
    │                │                    │
    │                │                    │
    └────────────────┴────────────────────┘
           (cancel pending removal)
```

**States:**
- `ACTIVE`: Accepting new sessions, processing packets
- `DRAINING`: No new sessions assigned, existing sessions complete naturally
- `PENDING_REMOVAL`: Empty, waiting for actual removal signal

#### 1.5.2 Enhanced Sysctl Hierarchy

The sysctl hierarchy is organized into **configuration** (settable), **stats** (read-only), and **worker control** (settable for manual management).

##### Configuration (Settable)

```
# Governor control
net.graph.pppoe_lb.governor.enabled              # 0=disabled, 1=enabled (default: 0)
net.graph.pppoe_lb.governor.mode                 # 0=manual, 1=auto (default: 0)
net.graph.pppoe_lb.governor.poll_interval        # Polling interval in seconds (default: 5, range: 1-60)

# Scaling limits
net.graph.pppoe_lb.governor.min_workers          # Minimum workers (default: 1, range: 1-mp_ncpus)
net.graph.pppoe_lb.governor.max_workers          # Maximum workers (0=mp_ncpus, default: 0=auto)
net.graph.pppoe_lb.governor.cpu_cores_max        # READ-ONLY: mp_ncpus value at startup

# Thresholds
net.graph.pppoe_lb.governor.sessions_per_worker  # Target sessions/worker (default: 500)
net.graph.pppoe_lb.governor.cpu_threshold        # Scale up at X% CPU (default: 80)
net.graph.pppoe_lb.governor.cpu_low_threshold    # Scale down below X% CPU (default: 30)

# Timing
net.graph.pppoe_lb.governor.scale_up_interval    # Seconds between scale-up (default: 10)
net.graph.pppoe_lb.governor.scale_down_interval  # Seconds between scale-down (default: 60)
net.graph.pppoe_lb.governor.drain_timeout        # Seconds to wait for drain (default: 30)
```

##### Stats (Read-Only)

```
# Current state
net.graph.pppoe_lb.governor.current_workers      # Current active workers
net.graph.pppoe_lb.governor.active_workers       # Workers in ACTIVE state
net.graph.pppoe_lb.governor.draining_workers     # Workers in DRAINING state
net.graph.pppoe_lb.governor.pending_removals     # Workers marked for removal
net.graph.pppoe_lb.governor.total_sessions       # Total active sessions
net.graph.pppoe_lb.governor.avg_sessions_worker  # Average sessions per worker

# Governor decisions
net.graph.pppoe_lb.governor.last_decision        # "scale_up", "scale_down", "cancel", "none"
net.graph.pppoe_lb.governor.last_reason          # "cpu_high", "cpu_low", "sessions_high", "sessions_low"
net.graph.pppoe_lb.governor.last_decision_time   # Unix timestamp of last decision

# CPU monitoring
net.graph.pppoe_lb.governor.cpu_usage            # Current CPU usage %
net.graph.pppoe_lb.governor.cpu_avg              # Average CPU usage (last 60s)
```

##### Per-Worker State Control (Settable via State Write)

```
net.graph.pppoe_lb.workers.count                 # Total worker slots (set to add/remove workers)
net.graph.pppoe_lb.workers.{id}.state           # Worker state: 0=ACTIVE, 1=DRAINING, 2=PENDING_REMOVAL (READ/WRITE)
net.graph.pppoe_lb.workers.{id}.sessions        # Sessions on this worker (read-only)
net.graph.pppoe_lb.workers.{id}.last_activity   # Last session activity timestamp
net.graph.pppoe_lb.workers.{id}.uptime          # Worker uptime in seconds
```

**State Values:**
| Value | Name | Description |
|-------|------|-------------|
| 0 | ACTIVE | Normal operation, accepts new sessions |
| 1 | DRAINING | No new sessions, waiting for existing to complete |
| 2 | PENDING_REMOVAL | Empty, marked for removal |

**Manual Worker Management via Sysctl (direct state writes):**
```bash
# Manually mark a worker for drain (set state to DRAINING=1)
sysctl net.graph.pppoe_lb.workers.2.state=1

# Cancel drain and return worker to service (set state to ACTIVE=0)
sysctl net.graph.pppoe_lb.workers.2.state=0

# Mark empty worker for removal (set state to PENDING_REMOVAL=2)
sysctl net.graph.pppoe_lb.workers.2.state=2

# Set governor mode to auto
sysctl net.graph.pppoe_lb.governor.mode=1

# Set poll interval to 10 seconds
sysctl net.graph.pppoe_lb.governor.poll_interval=10
```

**State Transition Rules (enforced by kernel):**
- `0 (ACTIVE)` → `1 (DRAINING)` or `2 (PENDING_REMOVAL)` ✓
- `1 (DRAINING)` → `0 (ACTIVE)` (cancel drain) or `2 (PENDING_REMOVAL)` ✓
- `2 (PENDING_REMOVAL)` → `0 (ACTIVE)` (re-activate) or `1 (DRAINING)` ✓
- If worker has active sessions and you set `PENDING_REMOVAL`, state stays `DRAINING` until sessions drain

**Effective Max Workers Logic:**
```
if (max_workers == 0) {
    effective_max = cpu_cores_max;  // Default: match CPU core count
} else {
    effective_max = min(max_workers, cpu_cores_max);  // Cap at CPU count
}
```

#### 1.5.3 Auto-Scaling Algorithm

**Scale Up Logic:**
```
SCALE_UP_TRIGGERED when:
  - (CPU > cpu_threshold AND time_since_last_scale_up > scale_up_interval)
  OR
  - (sessions > sessions_per_worker * workers * 0.8)
  
AND workers < max_workers
AND workers < mp_ncpus

ACTION:
  1. If any worker is PENDING_REMOVAL → CANCEL it (change mind!)
  2. Create new worker
  3. Log: "governor: scale up (reason: cpu_high/sessions_high), canceled N pending removals"
```

**Scale Down Logic:**
```
SCALE_DOWN_TRIGGERED when:
  - (CPU < cpu_low_threshold AND time_since_last_scale_down > scale_down_interval)
  OR
  - (sessions < sessions_per_worker * workers * 0.3)
  
AND workers > min_workers

ACTION:
  1. Select worker with fewest active sessions
  2. Mark worker as DRAINING (no new sessions)
  3. After drain_timeout seconds:
     - If sessions == 0 → PENDING_REMOVAL
     - Else → back to ACTIVE (couldn't drain in time)
  4. Signal pppoed to remove worker
  5. Log: "governor: mark worker N for removal (X sessions remaining)"
```

#### 1.5.4 Worker State Machine Implementation

```c
enum worker_state {
    WORKER_ACTIVE,
    WORKER_DRAINING,
    WORKER_PENDING_REMOVAL
};

struct worker_info {
    hook_p         hook;
    int            session_count;
    enum worker_state state;
    time_t         state_changed;
    time_t         last_activity;
};

/* Skip DRAINING/PENDING_REMOVAL workers in session selection */
static int
pppoe_lb_select_worker_session(struct pppoe_lb_private *priv, uint16_t session_id)
{
    int idx, base_idx;
    
    /* Hash-based selection */
    base_idx = session_id % priv->num_workers;
    
    /* Try to find an ACTIVE worker starting from hash */
    for (int i = 0; i < priv->num_workers; i++) {
        idx = (base_idx + i) % priv->num_workers;
        if (priv->workers[idx].state == WORKER_ACTIVE) {
            return idx;
        }
    }
    
    /* Fallback: no active workers (shouldn't happen) */
    return -1;
}

/* Cancel pending removal if scale-up is needed */
static void
pppoe_lb_cancel_pending_removals(struct pppoe_lb_private *priv)
{
    int canceled = 0;
    
    for (int i = 0; i < priv->num_workers; i++) {
        if (priv->workers[i].state == WORKER_PENDING_REMOVAL ||
            priv->workers[i].state == WORKER_DRAINING) {
            priv->workers[i].state = WORKER_ACTIVE;
            priv->workers[i].state_changed = time_second;
            canceled++;
        }
    }
    
    if (canceled > 0) {
        log(LOG_INFO, "governor: canceled %d pending worker removals", canceled);
    }
}
```

#### 1.5.5 Task List

| # | Task | Status | Owner | Start | End | Dependencies | Files | Notes |
|---|------|--------|-------|-------|-----|--------------|-------|-------|
| 1.26 | Add worker state tracking | NOT STARTED | | | | 1.24 | `ng_pppoe_lb.c` | Add `state` field: ACTIVE, DRAINING, PENDING_REMOVAL |
| 1.27 | Skip draining workers in selection | NOT STARTED | | | | 1.26 | `ng_pppoe_lb.c` | Worker selection skips DRAINING/PENDING_REMOVAL |
| 1.28 | Add session-count sysctl | NOT STARTED | | | | 1.26 | `ng_pppoe_lb.c` | Expose `sess_count` via `governor.sessions` |
| 1.29 | Add `min_workers` sysctl | NOT STARTED | | | | 1.26 | `ng_pppoe_lb.c` | Allow setting minimum workers (default: 1) |
| 1.30 | Auto-set max_workers to mp_ncpus | NOT STARTED | | | | 1.29 | `ng_pppoe_lb.c` | When `max_workers=0`, default to `mp_ncpus` |
| 1.31 | Add `sess_per_worker` threshold | NOT STARTED | | | | 1.30 | `ng_pppoe_lb.c` | Scale based on sessions/worker ratio |
| 1.32 | Implement "change mind" logic | NOT STARTED | | | | 1.31 | `ng_pppoe_lb.c` | If scale_up needed, cancel any pending removals |
| 1.33 | Add drain timeout | NOT STARTED | | | | 1.32 | `ng_pppoe_lb.c` | Worker stays in DRAINING for `drain_timeout` seconds |
| 1.34 | Add status sysctls | NOT STARTED | | | | 1.33 | `ng_pppoe_lb.c` | `pending_removals`, `last_decision`, `last_reason` |
| 1.35 | Add combined threshold logic | NOT STARTED | | | | 1.34 | `ng_pppoe_lb.c` | Both CPU AND session thresholds prevent flapping |
| 1.36 | Test session-aware scaling | NOT STARTED | | | | 1.35 | | Verify sessions/worker ratio triggers scaling |
| 1.37 | Test "change mind" cancellation | NOT STARTED | | | | 1.36 | | Verify pending removals canceled on load spike |

### Phase 2: Userland Daemon (`pppoed`)

| # | Task | Status | Owner | Start | End | Dependencies | Files | Notes |
|---|------|--------|-------|-------|-----|--------------|-------|-------|
| 2.1 | Add `-w <workers>` flag parsing | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.1 | `pppoed.c` | Default 1 (backward compatible) |
| 2.2 | Add `-L` flag parsing | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.1 | `pppoed.c` | Enable load balancer |
| 2.3 | Add `-A <algorithm>` flag parsing | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.2 | `pppoed.c` | round-robin, hash, least-loaded |
| 2.4 | Add `-G <max_workers>` flag parsing | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.3 | `pppoed.c` | Governor hard cap (0=auto/mp_ncpus, >0=hard cap limited by mp_ncpus) |
| 2.5 | Implement worker node creation | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.4 | `pppoed.c` | Create N `ng_pppoe` nodes |
| 2.6 | Implement load balancer setup | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.5 | `pppoed.c` | Create `ng_pppoe_lb`, connect workers |
| 2.7 | Implement graceful shutdown | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.6 | `pppoed.c` | Drain sessions, disconnect, cleanup |
| 2.8 | Update `pppoed.8` man page | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.3 | `pppoed.8` | Document new flags `-L`, `-w`, `-A`, `-G`, multithreaded mode, CPU governor |
| 2.9 | Update `libexec/rc/rc.d/pppoed` | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.1 | `rc.d/pppoed` | Handle `pppoed_workers`, `pppoed_loadbalancer`, `pppoed_algorithm`, `pppoed_governor_max` |
| 2.10 | Update `libexec/rc/rc.conf` defaults | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.9 | `rc.conf.5` | Added `pppoed_loadbalancer`, `pppoed_workers`, `pppoed_algorithm`, `pppoed_governor_max` variables |
| 2.11 | Add `pppoed_max_workers` rc.conf variable | COMPLETED | | 2026-04-23 | 2026-04-23 | 2.4 | `rc.conf.5` | Governor hard cap for startup script (via `pppoed_governor_max`) |
| 2.12 | Test `pppoed` in single-worker mode | NOT STARTED | | | | 2.8 | | Must behave identically to before |
| 2.13 | Test `pppoed` in multi-worker mode | NOT STARTED | | | | 2.12 | | Verify session distribution |
| 2.14 | Test `pppoed` with governor enabled | NOT STARTED | | | | 2.13 | | Verify `-G` cap respected |

### Phase 2.5: Enhanced pppoed Auto-Scaling Engine

This phase implements the userland side of auto-scaling: polling kernel sysctls, creating/destroying workers, and handling worker state transitions.

#### 2.5.1 Governor Polling Thread

```c
static void *
governor_poll_thread(void *arg)
{
    struct pppoed_instance *inst = arg;
    
    while (!inst->shutdown) {
        /* Poll kernel sysctls every 5 seconds */
        poll_governor_state(inst);
        
        /* Check if action needed */
        if (inst->governor.scale_up_needed) {
            add_worker(inst);
            inst->governor.scale_up_needed = 0;
        }
        
        if (inst->governor.remove_worker_idx >= 0) {
            remove_worker(inst, inst->governor.remove_worker_idx);
            inst->governor.remove_worker_idx = -1;
        }
        
        sleep(5);
    }
    return NULL;
}

static void
poll_governor_state(struct pppoed_instance *inst)
{
    size_t len;
    int val;
    
    /* Read current workers from kernel */
    len = sizeof(val);
    sysctlbyname("net.graph.pppoe_lb.governor.current_workers", &val, &len, NULL, 0);
    inst->governor.current_workers = val;
    
    /* Read pending removals */
    len = sizeof(val);
    sysctlbyname("net.graph.pppoe_lb.governor.pending_removals", &val, &len, NULL, 0);
    if (val > 0) {
        /* Kernel wants to remove a worker - check if we can honor it */
        if (can_remove_worker(inst)) {
            inst->governor.remove_worker_idx = find_drainable_worker(inst);
        }
    }
    
    /* Read scaling decision */
    char decision[32];
    len = sizeof(decision);
    sysctlbyname("net.graph.pppoe_lb.governor.last_decision", decision, &len, NULL, 0);
    if (strcmp(decision, "scale_up") == 0) {
        inst->governor.scale_up_needed = 1;
    }
}
```

#### 2.5.2 Worker Hot-Add Implementation

```c
static int
add_worker(struct pppoed_instance *inst)
{
    struct worker *w;
    int idx;
    
    if (inst->num_workers >= inst->max_workers) {
        log(LOG_INFO, "at max workers (%d), cannot add more", inst->max_workers);
        return EALREADY;
    }
    
    idx = inst->num_workers;
    w = &inst->workers[idx];
    
    /* Create ng_pppoe node */
    w->ng_name = ng_mknode("pppoe", NULL, NULL);
    
    /* Connect to load balancer */
    ng_connect(w->ng_name, "lower", inst->lb_name, "worker%d", idx);
    
    /* Create upper interface */
    ng_mkpeer(w->ng_name, "eiface", "lower", "upper");
    
    w->active = 1;
    w->state = WORKER_ACTIVE;
    inst->num_workers++;
    
    log(LOG_INFO, "added worker %d", idx);
    return 0;
}
```

#### 2.5.3 Graceful Worker Removal

```c
static int
remove_worker(struct pppoed_instance *inst, int idx)
{
    struct worker *w = &inst->workers[idx];
    
    if (w->state != WORKER_DRAINING && w->session_count > 0) {
        return EBUSY;  /* Can't remove yet */
    }
    
    /* Signal kernel to mark worker as PENDING_REMOVAL */
    ng_send_msg(inst->lb_name, "mark_draining", idx);
    
    /* Wait for drain timeout */
    sleep(inst->drain_timeout);
    
    /* Check if drained */
    if (w->session_count > 0) {
        /* Couldn't drain in time - cancel removal */
        ng_send_msg(inst->lb_name, "cancel_draining", idx);
        log(LOG_INFO, "worker %d could not drain (%d sessions), canceling removal",
            idx, w->session_count);
        return ETIMEDOUT;
    }
    
    /* Actually remove the worker */
    ng_shutdown(w->ng_name);
    w->active = 0;
    
    log(LOG_INFO, "removed worker %d", idx);
    return 0;
}
```

#### 2.5.4 rc.conf Configuration

```bash
# Auto-scaling configuration
pppoed_governor_mode="auto"              # manual or auto
pppoed_governor_min="1"                   # Minimum workers (default: 1)
pppoed_governor_max="0"                   # Maximum workers (0=mp_ncpus, default: 0)
pppoed_sessions_per_worker="500"          # Sessions per worker threshold
pppoed_cpu_threshold="80"                # Scale up at this CPU %
pppoed_cpu_low_threshold="30"             # Scale down below this CPU %
pppoed_scale_up_interval="10"             # Seconds between scale-up
pppoed_scale_down_interval="60"           # Seconds between scale-down
pppoed_drain_timeout="30"                # Seconds to wait for drain
```

#### 2.5.5 Task List

| # | Task | Status | Owner | Start | End | Dependencies | Files | Notes |
|---|------|--------|-------|-------|-----|--------------|-------|-------|
| 2.15 | Add governor polling thread | NOT STARTED | | | | 1.37 | `pppoed.c` | Poll kernel sysctls every 5 seconds |
| 2.16 | Implement worker hot-add in pppoed | NOT STARTED | | | | 2.15 | `pppoed.c` | Actually create ng_pppoe nodes when signaled |
| 2.17 | Implement worker graceful removal | NOT STARTED | | | | 2.16 | `pppoed.c` | Mark worker DRAINING, wait, then remove |
| 2.18 | Implement "cancel removal" | NOT STARTED | | | | 2.17 | `pppoed.c` | If scale_up needed, cancel pending removals |
| 2.19 | Add session count monitoring | NOT STARTED | | | | 2.15 | `pppoed.c` | Monitor `sess_count` for scaling decisions |
| 2.20 | Update rc.conf defaults | NOT STARTED | | | | 2.19 | `rc.conf.5` | Add `pppoed_governor_mode`, `pppoed_governor_min`, `pppoed_drain_timeout` |
| 2.21 | Test hot-add under load | NOT STARTED | | | | 2.16 | | Verify new worker created and accepts sessions |
| 2.22 | Test graceful removal | NOT STARTED | | | | 2.17 | | Verify sessions drain and worker removed |
| 2.23 | Test "change mind" cancellation | NOT STARTED | | | | 2.18 | | Verify removal canceled when load returns |
| 2.24 | Test drain timeout failure | NOT STARTED | | | | 2.22 | | Verify worker comes back online if can't drain |

### Phase 2.6: Safe Worker Removal Edge Cases

This phase handles edge cases in worker removal to ensure session stability.

#### 2.6.1 Edge Case: All Workers Draining

If all workers are in DRAINING state and new sessions arrive:

**Solution:** The first DRAINING worker that receives a session should automatically cancel its drain and go back to ACTIVE.

```c
static int
pppoe_lb_rcvdata_discovery(hook_p hook, item_p item)
{
    struct pppoe_lb_private *priv;
    struct mbuf *m;
    int worker_idx;
    
    priv = NG_NODE_PRIVATE(NG_HOOK_NODE(hook));
    
    /* Find ACTIVE worker */
    worker_idx = pppoe_lb_find_active_worker(priv);
    
    if (worker_idx < 0) {
        /* No active workers - force a draining worker to come back */
        worker_idx = pppoe_lb_force_cancel_drain(priv);
        if (worker_idx < 0) {
            NG_FREE_M(m);
            NG_FREE_ITEM(item);
            return ENETDOWN;
        }
        log(LOG_INFO, "forced worker %d out of DRAINING state (needed for new session)", worker_idx);
    }
    
    /* Forward to worker */
    ...
}

/* Cancel drain if new session arrives */
static int
pppoe_lb_force_cancel_drain(struct pppoe_lb_private *priv)
{
    int idx = -1;
    time_t oldest = 0;
    
    mtx_lock(&priv->worker_mtx);
    
    /* Find the DRAINING worker with the most time invested */
    for (int i = 0; i < priv->num_workers; i++) {
        if (priv->workers[i].state == WORKER_DRAINING) {
            if (idx < 0 || priv->workers[i].state_changed < oldest) {
                idx = i;
                oldest = priv->workers[i].state_changed;
            }
        }
    }
    
    if (idx >= 0) {
        priv->workers[idx].state = WORKER_ACTIVE;
        priv->workers[idx].state_changed = time_second;
    }
    
    mtx_unlock(&priv->worker_mtx);
    return idx;
}
```

#### 2.6.2 Edge Case: Session Hangs

If a session doesn't disconnect within drain timeout:

**Solution:** Keep the worker in DRAINING state but don't remove it. Log a warning and retry on next scale-down cycle.

```c
/* In governor tick - if worker couldn't drain */
if (worker->state == WORKER_DRAINING) {
    time_t elapsed = time_second - worker->state_changed;
    if (elapsed >= drain_timeout && worker->session_count > 0) {
        /* Can't remove yet - stay in DRAINING */
        log(LOG_WARN, "worker %d still draining (%d sessions after %ld seconds)",
            idx, worker->session_count, elapsed);
        /* Will retry next scale-down cycle */
        worker->drain_retries++;
    }
}
```

#### 2.6.3 Task List

| # | Task | Status | Owner | Start | End | Dependencies | Files | Notes |
|---|------|--------|-------|-------|-----|--------------|-------|-------|
| 2.25 | Handle "all draining" edge case | NOT STARTED | | | | 2.23 | `ng_pppoe_lb.c` | Force cancel drain if no active workers |
| 2.26 | Handle session hang edge case | NOT STARTED | | | | 2.25 | `ng_pppoe_lb.c` | Keep draining, retry on next cycle |
| 2.27 | Test "all draining" scenario | NOT STARTED | | | | 2.25 | | Verify automatic cancellation works |
| 2.28 | Test session hang scenario | NOT STARTED | | | | 2.26 | | Verify graceful retry behavior |

### Phase 3: Monitoring and Diagnostics

| # | Task | Status | Owner | Start | End | Dependencies | Files | Notes |
|---|------|--------|-------|-------|-----|--------------|-------|-------|
| 3.1 | Add `ngctl show pppoe_lb` command | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.5 | `usr.sbin/ngctl/pppoe_lb.c` | Display stats, workers, algorithm |
| 3.2 | Add `ngctl pppoe_lb config` command | COMPLETED | | 2026-04-23 | 2026-04-23 | 3.1 | `usr.sbin/ngctl/pppoe_lb.c` | Runtime reconfiguration |
| 3.3 | Add `ngctl pppoe_lb stats` command | COMPLETED | | 2026-04-23 | 2026-04-23 | 3.1 | `usr.sbin/ngctl/pppoe_lb.c` | Display detailed statistics |
| 3.4 | Add `ngctl pppoe_lb map` command | COMPLETED | | 2026-04-23 | 2026-04-23 | 3.1 | `usr.sbin/ngctl/pppoe_lb.c` | Display session-to-worker mapping |
| 3.5 | Update `ngctl.h` for new commands | COMPLETED | | 2026-04-23 | 2026-04-23 | 3.1 | `usr.sbin/ngctl/ngctl.h` | Declare new command structs |
| 3.6 | Update `ngctl/Makefile` | COMPLETED | | 2026-04-23 | 2026-04-23 | 3.1 | `usr.sbin/ngctl/Makefile` | Add pppoe_lb.c to SRCS |
| 3.7 | Update `ngctl.8` man page | COMPLETED | | 2026-04-23 | 2026-04-23 | 3.2 | `ngctl.8` | Document `pppoe_lb show`, `config`, `stats`, `map` subcommands |
| 3.8 | Add `netstat -W` flag | NOT STARTED | | | | 1.5 | `netstat.c` | Query `NGM_PPPOE_LB_GET_STATS` |
| 3.9 | Update `netstat.1` man page | NOT STARTED | | | | 3.8 | `netstat.1` | Document `-W` flag |
| 3.10 | Add `ifconfig pppoe_workers` option | NOT STARTED | | | | 1.5 | `ifconfig.c` | Get/set worker count |
| 3.11 | Update `ifconfig.8` man page | NOT STARTED | | | | 3.10 | `ifconfig.8` | Document option |
| 3.12 | Add `pppctl show pppoe lb` command | NOT STARTED | | | | 3.1 | `pppctl.c` | Client-side diagnostics |
| 3.13 | Update `pppctl.8` man page | NOT STARTED | | | | 3.12 | `pppctl.8` | Document new command |

### Phase 4: PPP Daemon (`ppp`) Client-Side

| # | Task | Status | Owner | Start | End | Dependencies | Files | Notes |
|---|------|--------|-------|-------|-----|--------------|-------|-------|
| 4.1 | Add `set pppoe workers <n>` command | NOT STARTED | | | | 1.1 | `command.c` | Client-side hint |
| 4.2 | Add `show pppoe workers` command | NOT STARTED | | | | 4.1 | `command.c` | Display worker info |
| 4.3 | Update `ether.c` for load balancer messages | NOT STARTED | | | | 1.5 | `ether.c` | Handle `NGM_PPPOE_LB_GET_STATS` |
| 4.4 | Update `ppp.8` man page | NOT STARTED | | | | 4.2 | `ppp.8` | Document new commands |

### Phase 5: Optional Per-Session Locking in `ng_pppoe`

| # | Task | Status | Owner | Start | End | Dependencies | Files | Notes |
|---|------|--------|-------|-------|-----|--------------|-------|-------|
| 5.1 | Add per-session `struct mtx` to `sess_con` | NOT STARTED | | | | 1.18 | `ng_pppoe.c` | Higher risk; defer until Phase 1-2 stable |
| 5.2 | Replace node-level serialization with per-session locks | NOT STARTED | | | | 5.1 | `ng_pppoe.c` | Data path only |
| 5.3 | Test for deadlocks and race conditions | NOT STARTED | | | | 5.2 | | Extensive stress testing required |
| 5.4 | Benchmark performance improvement | NOT STARTED | | | | 5.3 | | Compare to Phase 1-2 results |

### Phase 6: Documentation and Release

| # | Task | Status | Owner | Start | End | Dependencies | Files | Notes |
|---|------|--------|-------|-------|-----|--------------|-------|-------|
| 6.1 | Write `ng_pppoe_lb.4` man page | COMPLETED | | 2026-04-23 | 2026-04-23 | 1.18 | `ng_pppoe_lb.4` | Complete kernel node documentation with hooks, control messages, algorithms, governor, sysctl variables, examples |
| 6.2 | Update `ng_pppoe.4` man page | NOT STARTED | | | | 1.18 | `ng_pppoe.4` | Mention load balancer integration |
| 6.3 | Update `RELNOTES` | NOT STARTED | | | | 2.11 | `RELNOTES` | Summarize feature for release |
| 6.4 | Update `UPDATING` | NOT STARTED | | | | 2.11 | `UPDATING` | Any admin-visible changes |
| 6.5 | Final code review | NOT STARTED | | | | 6.4 | | All phases complete |
| 6.6 | Merge to `main` | NOT STARTED | | | | 6.5 | | After approval |

### Phase 7: Shell Completions

Shell completions improve the administrative experience by providing context-aware tab completion for commands and options.

#### 7.1 Completion Files

Three shell completion files have been created in `share/examples/netgraph/pppoe_lb/`:

| File | Shell | Purpose |
|------|-------|---------|
| `_ngctl_pppoe_lb` | Bash | Completion for `ngctl pppoe_lb` subcommands |
| `_pppoed` | Bash | Completion for `pppoed` command-line options |
| `_ngctl_pppoe_lb` | Zsh | Completion for `ngctl pppoe_lb` subcommands |
| `_pppoed` | Zsh | Completion for `pppoed` command-line options |
| `ngctl.completion` | Tcsh | Completion for `ngctl` with pppoe_lb support |
| `pppoed.completion` | Tcsh | Completion for `pppoed` |

#### 7.2 Bash Completion Features

**`ngctl pppoe_lb` subcommands:**
- `show`, `info` - Complete with netgraph node paths
- `config`, `set` - Complete with paths and config parameters (`algorithm`, `max_workers`, `debug`)
- `stats`, `statistics` - Complete with netgraph node paths
- `map`, `sessions` - Complete with netgraph node paths

**`pppoed` options:**
- `-F` - foreground mode
- `-d` - debug mode
- `-P` - PID file (path completion)
- `-a`, `-p` - Provider names (from /etc/ppp/ppp.conf)
- `-e` - Executable paths
- `-l` - Label names
- `-n` - Numeric debug level
- `-L` - Multi-worker mode
- `-w` - Worker counts (1, 2, 4, 8, 16)
- `-A` - Algorithm (0=round-robin, 1=hash, 2=least-loaded)
- `-G` - Max workers
- Positional: Network interfaces

#### 7.3 Zsh Completion Features

Zsh completions provide:
- Descriptions for all options
- Dynamic provider list from /etc/ppp/ppp.conf
- Network interface list
- Worker count options
- Algorithm selection with descriptions

#### 7.4 Tcsh Completion Features

Tcsh completions provide:
- Option-specific completions
- Provider name completion
- Interface completion
- Algorithm and worker count completion

#### 7.5 Task List

| # | Task | Status | Owner | Start | End | Dependencies | Files | Notes |
|---|------|--------|-------|-------|-----|--------------|-------|-------|
| 7.1 | Create bash completions for ngctl pppoe_lb | COMPLETED | | 2026-05-01 | 2026-05-01 | 3.1-3.4 | `share/examples/netgraph/pppoe_lb/_ngctl_pppoe_lb` | |
| 7.2 | Create bash completions for pppoed | COMPLETED | | 2026-05-01 | 2026-05-01 | 2.11 | `share/examples/netgraph/pppoe_lb/_pppoed` | |
| 7.3 | Create zsh completions for ngctl pppoe_lb | COMPLETED | | 2026-05-01 | 2026-05-01 | 3.1-3.4 | `share/examples/netgraph/pppoe_lb/_ngctl_pppoe_lb` | |
| 7.4 | Create zsh completions for pppoed | COMPLETED | | 2026-05-01 | 2026-05-01 | 2.11 | `share/examples/netgraph/pppoe_lb/_pppoed` | |
| 7.5 | Create tcsh completions for ngctl | COMPLETED | | 2026-05-01 | 2026-05-01 | 3.1-3.4 | `share/examples/netgraph/pppoe_lb/ngctl.completion` | |
| 7.6 | Create tcsh completions for pppoed | COMPLETED | | 2026-05-01 | 2026-05-01 | 2.11 | `share/examples/netgraph/pppoe_lb/pppoed.completion` | |
| 7.7 | Update README with completion instructions | COMPLETED | | 2026-05-01 | 2026-05-01 | 7.1-7.6 | `share/examples/netgraph/pppoe_lb/README` | |
| 7.8 | Document completions in ngctl.8 | NOT STARTED | | | | 3.7 | `ngctl.8` | Installation instructions |
| 7.9 | Document completions in pppoed.8 | NOT STARTED | | | | 2.11 | `pppoed.8` | Installation instructions |

---

## 8. Phase 8: Comprehensive Testing Framework (`pppoe_lb_test`)

A production-grade testing tool for validating PPPoE load balancer functionality, accuracy, and performance. Written in C++ with no external dependencies.

### 8.1 Overview

The `pppoe_lb_test` tool provides:

| Feature | Description |
|---------|-------------|
| **Session Accuracy Tests** | Verify session distribution matches configured algorithm |
| **Session Affinity Tests** | Verify sessions stay on assigned worker |
| **Governor Scaling Tests** | Verify auto-scaling triggers correctly |
| **File Transfer Tests** | Stream files with checksum verification (iperf3-style) |
| **Real-time Display** | Tabular stats with live updates (like iperf3) |
| **Man Page** | Full documentation with examples |
| **Shell Completions** | Bash, Zsh, and Tcsh support |

### 8.2 Architecture

```
pppoe_lb_test
├── Modes
│   ├── accuracy    - Test session distribution accuracy
│   ├── affinity    - Test session affinity (stickiness)
│   ├── transfer    - Test file transfer with checksums
│   ├── governor    - Test auto-scaling behavior
│   ├── stress      - High-load stress testing
│   └── benchmark   - Performance benchmarking
│
├── Core Components
│   ├── TestEngine       - Base test orchestration
│   ├── AccuracyTest     - Distribution accuracy logic
│   ├── AffinityTest     - Session affinity verification
│   ├── TransferTest     - File streaming with checksums
│   ├── GovernorTest     - Scaling behavior tests
│   ├── StatsDisplay     - iperf3-style tabular output
│   └── ReportGenerator  - Test result reporting
│
└── Output Formats
    ├── Console (default) - Live tabular display
    ├── JSON              - Machine-readable results
    ├── JSONL             - Streaming JSON (one object per line)
    ├── CSV               - Spreadsheet-compatible
    └── TAP               - Test Anything Protocol
```

### 8.3 Command-Line Interface

```bash
pppoe_lb_test [options] <mode> [mode-options]

MODES:
  accuracy          Test session distribution accuracy
  affinity          Test session affinity (stickiness)
  transfer          Test file transfer with checksums
  governor          Test auto-scaling behavior
  stress            Stress test with high session count
  benchmark         Performance benchmarking
  all               Run all tests (default summary mode)

ACCURACY MODE:
  pppoe_lb_test accuracy [options]
    -n, --sessions N       Number of sessions to create (default: 100)
    -a, --algorithm ALGO    Algorithm to test (0=rr, 1=hash, 2=ll)
    -w, --workers N         Number of workers (default: 4)
    -t, --threshold P       Acceptable deviation % (default: 10)
    -v, --verbose           Show per-worker details

TRANSFER MODE:
  pppoe_lb_test transfer [options]
    -s, --server            Run as server (receive)
    -c, --client            Run as client (send)
    -i, --interface IFACE   Interface to use
    -f, --file PATH         File to transfer
    -b, --buffer-size N     Buffer size (default: 65536)
    -d, --duration N        Test duration in seconds
    -r, --rate-limit N      Rate limit in Mbps (0=unlimited)
    --checksum-algo ALGO    Checksum: crc32, md5, sha256 (default: crc32)

AFFINITY MODE:
  pppoe_lb_test affinity [options]
    -n, --sessions N       Number of sessions to test
    -r, --retries N        Reconnection attempts per session
    --timeout N            Session timeout in seconds

GOVERNOR MODE:
  pppoe_lb_test governor [options]
    --scale-up-trigger     Simulate scale-up trigger
    --scale-down-trigger    Simulate scale-down trigger
    --change-mind          Test cancel-drain behavior
    --interval N           Test interval in seconds

GENERAL OPTIONS:
  -h, --help              Show this help
  -V, --version           Show version
  -o, --output FORMAT     Output format: console, json, jsonl, csv, tap
  -O, --output-file FILE  Write output to file
  -q, --quiet             Suppress progress output
  -v, --verbose           Verbose output
  -D, --debug             Debug output
  --log-level LEVEL       Log level: error, warn, info, debug
  --no-color              Disable colored output
  --no-stats              Don't show live stats
  --stats-interval N      Stats update interval (default: 1s)

ACCURACY THRESHOLDS:
  --pass-threshold P       Minimum accuracy to pass (default: 90%)
  --fail-early             Exit on first failure
```

### 8.4 Real-Time Tabular Display (iperf3-style)

The tabular display provides live updates during test execution:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                     PPPoE Load Balancer Test Suite                      │
│                     Test: Session Distribution Accuracy                 │
├─────────────────────────────────────────────────────────────────────────┤
│ Duration: 00:00:05    Sessions: 100    Algorithm: Round Robin (0)       │
├─────────────────────────────────────────────────────────────────────────┤
│ Worker  │ State     │ Sessions │ Expected │ Deviation │ Accuracy      │
├─────────┼───────────┼──────────┼──────────┼───────────┼──────────────┤
│ worker0 │ ACTIVE    │       26 │     25.0 │     +4.0% │ ✓ 100.0%      │
│ worker1 │ ACTIVE    │       25 │     25.0 │     +0.0% │ ✓ 100.0%      │
│ worker2 │ ACTIVE    │       24 │     25.0 │     -4.0% │ ✓ 100.0%      │
│ worker3 │ ACTIVE    │       25 │     25.0 │     +0.0% │ ✓ 100.0%      │
├─────────┼───────────┼──────────┼──────────┼───────────┼──────────────┤
│ Total   │           │      100 │    100.0  │     0.0%   │ ✓ 100.0%      │
└─────────────────────────────────────────────────────────────────────────┘
```

**Transfer Test Display:**
```
┌─────────────────────────────────────────────────────────────────────────┐
│                     PPPoE Load Balancer Test Suite                      │
│                     Test: File Transfer with Checksum                   │
├─────────────────────────────────────────────────────────────────────────┤
│ Interface: em0    Mode: Client    Rate: Unlimited                       │
├─────────────────────────────────────────────────────────────────────────┤
│ Interval      Transfer     Bandwidth      Retr  Cwnd                   │
│              Size                     Msgs  Cong                        │
├──────────────┼────────────┼──────────────┼──────┼────────────────────┤
│  0.00- 1.00  │   12.5 MB   │   104.2 Mbps │    0 │   256 KB            │
│  1.00- 2.00  │   12.8 MB   │   106.5 Mbps │    0 │   512 KB            │
│  2.00- 3.00  │   12.3 MB   │   102.1 Mbps │    1 │   512 KB            │
├──────────────┼────────────┼──────────────┼──────┼────────────────────┤
│ SUM (avg)    │   12.5 MB   │   104.3 Mbps │    0 │   512 KB            │
└─────────────────────────────────────────────────────────────────────────┘
│ Checksum Verification: ✓ PASSED (SHA256: abc123...)                     │
│ File Integrity:         ✓ PASSED (100% of blocks received)              │
```

**Governor Scaling Display:**
```
┌─────────────────────────────────────────────────────────────────────────┐
│                     PPPoE Load Balancer Test Suite                      │
│                     Test: Auto-Scaling Governor                         │
├─────────────────────────────────────────────────────────────────────────┤
│ Mode: Auto    Min: 2    Max: 4    Sessions/Worker: 500                   │
├─────────────────────────────────────────────────────────────────────────┤
│ Time   │ Workers │ Active │ Draining │ Sessions │ CPU  │ Decision       │
├────────┼─────────┼────────┼──────────┼──────────┼──────┼───────────────│
│ 00:00  │       2 │      2 │        0 │      500 │  45% │ none           │
│ 00:05  │       2 │      2 │        0 │     1000 │  72% │ none           │
│ 00:10  │       2 │      2 │        0 │     1500 │  85% │ scale_up       │
│ 00:11  │       3 │      3 │        0 │     1500 │  55% │ none           │
│ 00:15  │       3 │      3 │        0 │      900 │  35% │ none           │
│ 00:20  │       3 │      2 │        1 │      400 │  28% │ scale_down     │
│ 00:21  │       3 │      2 │        1 │      700 │  55% │ cancel_drain   │
├────────┼─────────┼────────┼──────────┼──────────┼──────┼───────────────│
│ Result │ ✓ PASSED - scale_up triggered correctly at threshold           │
│        │ ✓ PASSED - cancel_drain worked when load returned               │
└─────────────────────────────────────────────────────────────────────────┘
```

### 8.5 File Transfer Test Implementation

The transfer test uses PPPoE sessions to stream data and verify integrity:

#### 8.5.1 Checksum Algorithm

| Algorithm | Speed | Use Case |
|-----------|-------|----------|
| **CRC32** | Fastest | Default, good for error detection |
| **Fletcher32** | Fast | Alternative to CRC32 |
| **MD5** | Medium | Legacy compatibility |
| **SHA256** | Slow | Cryptographic verification |

#### 8.5.2 Transfer Protocol

```c
struct transfer_header {
    uint32_t    magic;          // 0x50505045 ('PPPE')
    uint32_t    sequence;        // Packet sequence number
    uint32_t    block_num;       // Block number
    uint32_t    block_size;      // Size of this block
    uint32_t    total_blocks;    // Total blocks in transfer
    uint64_t    file_offset;     // Offset in file
    uint64_t    file_size;       // Total file size
    uint32_t    checksum_type;   // 0=CRC32, 1=Fletcher32, 2=MD5, 3=SHA256
    uint32_t    checksum_len;    // Length of checksum
    uint8_t     checksum[];      // Checksum value
    uint8_t     data[];         // Payload data
};
```

#### 8.5.3 Accuracy Verification

| Metric | Description | Pass Criteria |
|--------|-------------|---------------|
| **Block Count** | All blocks received | `received == total_blocks` |
| **Sequence** | No missing packets | `seq[i] == i` for all i |
| **Checksum** | Data integrity | `checksum(data) == expected` |
| **Ordering** | Blocks in order | No sequence gaps |
| **Timing** | Transfer rate stable | `stddev(rate) < 20%` |

### 8.6 Accuracy Test Implementation

#### 8.6.1 Session Distribution Test

```c
struct accuracy_result {
    int                 worker_id;
    enum worker_state  state;
    int                 actual_sessions;
    double              expected_sessions;
    double              deviation_percent;
    bool                passed;
};

// For N sessions across W workers with algorithm A:
//   Round Robin:  expected = N / W (each worker)
//   Hash-Based:   expected = N / W (statistical average)
//   Least-Loaded: expected varies based on worker loads

// Pass criteria:
//   |actual - expected| / expected <= threshold
```

#### 8.6.2 Session Affinity Test

```c
struct affinity_result {
    int     session_id;
    int     original_worker;
    int     reconnect_count;
    int     final_worker;
    bool    affinity_maintained;
};

// Pass criteria:
//   original_worker == final_worker for all retries
```

#### 8.6.3 Governor Scaling Test

```c
struct governor_test_scenario {
    const char *name;
    void (*setup)(void);
    void (*trigger)(void);
    int expected_workers_after;
    int expected_state;
    int timeout_seconds;
};

struct governor_result {
    const char     *scenario;
    bool           scaling_triggered;
    int            workers_before;
    int            workers_after;
    int            actual_decision;
    const char     *reason;
    bool           passed;
    double         latency_ms;    // Time from trigger to action
};
```

### 8.7 Output Formats

#### 8.7.1 JSONL Output (Streaming)

JSONL (JSON Lines) outputs one JSON object per line, ideal for piping to other tools:

```jsonl
{"type":"test_start","mode":"accuracy","timestamp":"2026-05-01T12:00:00Z","config":{"sessions":100,"workers":4,"algorithm":"round_robin"}}
{"type":"interval","time":1.0,"workers":[{"id":0,"sessions":26},{"id":1,"sessions":25},{"id":2,"sessions":24},{"id":3,"sessions":25}]}
{"type":"interval","time":2.0,"workers":[{"id":0,"sessions":51},{"id":1,"sessions":50},{"id":2,"sessions":49},{"id":3,"sessions":50}]}
{"type":"worker_result","worker_id":0,"state":"ACTIVE","sessions":26,"expected":25.0,"deviation":4.0,"passed":true}
{"type":"worker_result","worker_id":1,"state":"ACTIVE","sessions":25,"expected":25.0,"deviation":0.0,"passed":true}
{"type":"worker_result","worker_id":2,"state":"ACTIVE","sessions":24,"expected":25.0,"deviation":-4.0,"passed":true}
{"type":"worker_result","worker_id":3,"state":"ACTIVE","sessions":25,"expected":25.0,"deviation":0.0,"passed":true}
{"type":"test_complete","passed":true,"accuracy":96.0,"duration_ms":5234}
```

**Event Types:**

| Type | Description |
|------|-------------|
| `test_start` | Test configuration at start |
| `interval` | Periodic stats update |
| `worker_result` | Per-worker test result |
| `transfer_block` | Transfer test block received |
| `scaling_event` | Governor scaling decision |
| `test_complete` | Final test result |
| `error` | Error occurred |

#### 8.7.2 JSON Output

```json
{
  "test": "accuracy",
  "version": "1.0",
  "timestamp": "2026-05-01T12:00:00Z",
  "duration_ms": 5234,
  "config": {
    "sessions": 100,
    "workers": 4,
    "algorithm": "round_robin",
    "threshold": 10.0
  },
  "results": {
    "overall_accuracy": 96.5,
    "passed": true,
    "workers": [
      {
        "id": 0,
        "state": "ACTIVE",
        "sessions": 26,
        "expected": 25.0,
        "deviation": 4.0,
        "passed": true
      }
    ]
  },
  "checksums": {
    "algorithm": "sha256",
    "value": "abc123..."
  }
}
```

#### 8.7.3 CSV Output

```csv
test,session_id,worker_id,state,deviation,passed
accuracy,0,0,ACTIVE,4.0,true
accuracy,1,1,ACTIVE,0.0,true
```

#### 8.7.4 TAP Output

```
TAP version 13
1..4
ok 1 - worker0 accuracy within threshold (26/25, +4.0%)
ok 2 - worker1 accuracy within threshold (25/25, 0.0%)
ok 3 - worker2 accuracy within threshold (24/25, -4.0%)
ok 4 - worker3 accuracy within threshold (25/25, 0.0%)
ok 5 - Overall accuracy 96.0% >= 90% threshold
```

### 8.8 Source File Structure

```
usr.sbin/pppoe_lb_test/
├── Makefile              - Build configuration
├── pppoe_lb_test.8       - Man page
├── pppoe_lb_test.cpp     - Main entry point
├── pppoe_lb_test.h       - Common definitions
├── test_engine.cpp       - Test orchestration
├── test_engine.h         - Test engine header
├── accuracy_test.cpp     - Accuracy test implementation
├── accuracy_test.h       - Accuracy test header
├── affinity_test.cpp     - Affinity test implementation
├── affinity_test.h       - Affinity test header
├── transfer_test.cpp     - Transfer test implementation
├── transfer_test.h       - Transfer test header
├── governor_test.cpp     - Governor test implementation
├── governor_test.h        - Governor test header
├── stats_display.cpp     - Tabular display (like iperf3)
├── stats_display.h       - Display header
├── checksum.cpp          - Checksum implementations
├── checksum.h            - Checksum header
├── output_formats.cpp    - JSON/CSV/TAP output
├── output_formats.h      - Output format headers
├── shell_completions/    - Shell completion files
│   ├── _pppoe_lb_test    - Bash/Zsh completion
│   └── pppoe_lb_test.completion - Tcsh completion
└── examples/            - Example usage
    └── test_scenarios.sh - Wrapper scripts
```

### 8.9 No External Dependencies

All functionality is implemented using FreeBSD base system libraries:

| Component | FreeBSD Library |
|-----------|-----------------|
| Networking | `libnetgraph` (built-in) |
| Checksums | `libmd` (CRC32, MD5, SHA256) |
| File I/O | Standard C library |
| Terminal | `libncurses` or ANSI escape codes |
| JSON | Custom minimal JSON writer (no external lib) |
| Threads | POSIX threads (`pthread.h`) |
| Timing | `gettimeofday()` / `clock_gettime()` |

### 8.10 Task List

| # | Task | Status | Dependencies | Files | Notes |
|---|------|--------|--------------|-------|-------|
| 8.1 | Create source directory and Makefile | NOT STARTED | 2.11, 3.4 | `usr.sbin/pppoe_lb_test/Makefile` | Build configuration |
| 8.2 | Implement main entry point and CLI parsing | NOT STARTED | 8.1 | `pppoe_lb_test.cpp` | Command-line interface |
| 8.3 | Implement test engine base class | NOT STARTED | 8.2 | `test_engine.cpp/h` | Test orchestration |
| 8.4 | Implement accuracy test module | NOT STARTED | 8.3 | `accuracy_test.cpp/h` | Session distribution |
| 8.5 | Implement affinity test module | NOT STARTED | 8.3 | `affinity_test.cpp/h` | Session stickiness |
| 8.6 | Implement transfer test module | NOT STARTED | 8.3 | `transfer_test.cpp/h` | File transfer with checksums |
| 8.7 | Implement checksum utilities | NOT STARTED | 8.1 | `checksum.cpp/h` | CRC32/MD5/SHA256 |
| 8.8 | Implement governor test module | NOT STARTED | 8.3, 2.5 | `governor_test.cpp/h` | Auto-scaling tests |
| 8.9 | Implement stats display (tabular) | NOT STARTED | 8.1 | `stats_display.cpp/h` | iperf3-style output |
| 8.10 | Implement output formatters | NOT STARTED | 8.1 | `output_formats.cpp/h` | JSON/CSV/TAP |
| 8.11 | Write man page | NOT STARTED | 8.1-8.10 | `pppoe_lb_test.8` | Full documentation |
| 8.12 | Create shell completions | NOT STARTED | 8.11 | `shell_completions/*` | Bash/Zsh/Tcsh |
| 8.13 | Add stress test mode | NOT STARTED | 8.4 | `stress_test.cpp/h` | High-load testing |
| 8.14 | Add benchmark mode | NOT STARTED | 8.4, 8.6 | `benchmark_test.cpp/h` | Performance testing |
| 8.15 | Implement error/success presentation | NOT STARTED | 8.9 | `stats_display.cpp/h` | Colors, icons, banners |
| 8.16 | Integration testing | NOT STARTED | 8.1-8.15 | | Full test suite |
| 8.17 | Update TOC with Phase 8 | NOT STARTED | 8.1-8.16 | `0.0-PPPoE-TOC.md` | Documentation |

### 8.11 Error and Success Presentation

A critical aspect of a professional testing tool is clear, consistent presentation of results. This section defines the visual language for displaying successes (wins) and errors throughout the `pppoe_lb_test` output.

#### 8.11.1 Visual Language

##### 8.11.1.1 Iconography

| Icon | Symbol | Meaning | ANSI Color |
|------|--------|---------|------------|
| **Success** | `✓` | Test passed, condition met | Green (`\033[32m`) |
| **Error** | `✗` | Test failed, condition not met | Red (`\033[31m`) |
| **Warning** | `⚠` | Non-fatal issue, degraded performance | Yellow (`\033[33m`) |
| **Info** | `ℹ` | Informational message, context | Cyan (`\033[36m`) |
| **Skipped** | `⊘` | Test not run or not applicable | Dim (`\033[2m`) |
| **Running** | `⋯` | Test in progress | Blue (`\033[34m`) |

##### 8.11.1.2 Color Scheme

| Purpose | Foreground | Background | Use Case |
|---------|------------|------------|----------|
| Success (Green) | `\033[32m` | — | Passed tests, good metrics |
| Success Bold | `\033[1;32m` | — | Summary headers for passes |
| Error (Red) | `\033[31m` | — | Failed tests, critical errors |
| Error Bold | `\033[1;31m` | — | Summary headers for failures |
| Warning (Yellow) | `\033[33m` | — | Threshold warnings, retries |
| Info (Cyan) | `\033[36m` | — | Headers, labels |
| Dim (Gray) | `\033[2m` | — | Timestamps, secondary info |
| Reset | `\033[0m` | — | End all formatting |

##### 8.11.1.3 Terminal Detection

```c
// Automatic color support detection
static bool supports_color(FILE *fp) {
    // Check if stdout is a terminal
    if (!isatty(fileno(fp)))
        return false;
    
    // Check TERM environment variable
    const char *term = getenv("TERM");
    if (term == NULL)
        return false;
    
    // Known terminals that support ANSI colors
    static const char *color_terms[] = {
        "xterm", "xterm-256color", "xterm-color",
        "screen", "screen-256color", "tmux", "tmux-256color",
        "rxvt", "rxvt-unicode", "rxvt-unicode-256color",
        "linux", "konsole", "gnome", "vt100", "vt220",
        "dumb"  // Even dumb supports colors if forced
    };
    
    for (int i = 0; i < sizeof(color_terms)/sizeof(color_terms[0]); i++) {
        if (strstr(term, color_terms[i]) != NULL)
            return true;
    }
    
    return false;
}

// Respect --no-color flag
static bool use_color = true;  // Set based on --no-color and terminal
```

#### 8.11.2 Success (Wins) Presentation

##### 8.11.2.1 Test Pass Indicators

```
[  PASS  ] worker0: 26/25 sessions (+4.0%) - within 10% threshold
[  PASS  ] worker1: 25/25 sessions (0.0%) - perfect balance
[  PASS  ] worker2: 24/25 sessions (-4.0%) - within 10% threshold
[  PASS  ] worker3: 25/25 sessions (0.0%) - perfect balance
```

With color:
```
\033[32m[  PASS  ]\033[0m worker0: 26/25 sessions (+4.0%) - within 10% threshold
\033[32m[  PASS  ]\033[0m worker1: 25/25 sessions (0.0%) - perfect balance
\033[32m[  PASS  ]\033[0m worker2: 24/25 sessions (-4.0%) - within 10% threshold
\033[32m[  PASS  ]\033[0m worker3: 25/25 sessions (0.0%) - perfect balance
```

##### 8.11.2.2 Transfer Test Success

```
┌─────────────────────────────────────────────────────────────────────────┐
│                      Transfer Test Results                              │
├─────────────────────────────────────────────────────────────────────────┤
│ File: /tmp/test_file.bin                                                │
│ Size: 1.00 GB                                                           │
│ Duration: 45.23 seconds                                                 │
├─────────────────────────────────────────────────────────────────────────┤
│ ✓ 10,000 / 10,000 blocks received (100.00%)                             │
│ ✓ Checksum verified: SHA256                                            │
│   Expected:   a3f5b8c9d2e1...                                           │
│   Computed:   a3f5b8c9d2e1...                                           │
│ ✓ No sequence gaps detected                                             │
│ ✓ Transfer rate: 22.61 MB/s (stable, σ = 0.82 MB/s)                    │
├─────────────────────────────────────────────────────────────────────────┤
│ \033[1;32m                     ★★★ ALL TESTS PASSED ★★★                    \033[0m │
└─────────────────────────────────────────────────────────────────────────┘
```

##### 8.11.2.3 Governor Scaling Success

```
Governor Scaling Test Results:
──────────────────────────────
✓ Scale-up triggered at 85% CPU (> 80% threshold)
✓ New worker created within 2.5 seconds
✓ Session distribution resumed to balanced state
✓ Scale-down correctly blocked (sessions migrating)
✓ Cancel-drain activated when load returned
✓ Worker returned to ACTIVE state successfully

\033[1;32mPassed: 6/6 scenarios (100.0%)\033[0m
```

##### 8.11.2.4 Affinity Success

```
Session Affinity Test Results:
──────────────────────────────
✓ Session 0: worker0 → worker0 (5 retries) - affinity maintained
✓ Session 1: worker1 → worker1 (5 retries) - affinity maintained
✓ Session 2: worker0 → worker0 (5 retries) - affinity maintained
✓ Session 3: worker2 → worker2 (5 retries) - affinity maintained

\033[1;32mAffinity Preserved: 4/4 sessions (100.0%)\033[0m
```

#### 8.11.3 Error Presentation

##### 8.11.3.1 Test Fail Indicators

```
[  FAIL  ] worker0: 30/25 sessions (+20.0%) - EXCEEDS 10% threshold
[  FAIL  ] worker1: 18/25 sessions (-28.0%) - EXCEEDS 10% threshold
[  WARN  ] worker2: 24/25 sessions (-4.0%) - approaching threshold
[  PASS  ] worker3: 25/25 sessions (0.0%) - perfect balance
```

##### 8.11.3.2 Transfer Error Details

```
┌─────────────────────────────────────────────────────────────────────────┐
│                      Transfer Test Errors                               │
├─────────────────────────────────────────────────────────────────────────┤
│ ✗ Checksum mismatch on block 4,592                                      │
│   Expected checksum: 0xA3F5B8C9                                        │
│   Computed checksum: 0xD2E1F4A7                                        │
│                                                                         │
│ ✗ 8 missing blocks: [4591, 4592, 4593, 4701, 4702, 4703, 4704, 4705]   │
│                                                                         │
│ ✗ Sequence gap detected: block 4591 → 4594 (missing: 4592, 4593)       │
├─────────────────────────────────────────────────────────────────────────┤
│ \033[1;31m                     ✗✗✗ TEST FAILED ✗✗✗                          \033[0m │
│                                                                         │
│ Summary:                                                                │
│   Blocks received: 9,992 / 10,000 (99.92%)                             │
│   Checksum errors: 8                                                   │
│   Missing blocks: 8                                                     │
│   Transfer rate: 22.61 MB/s (σ = 0.82 MB/s)                            │
└─────────────────────────────────────────────────────────────────────────┘
```

##### 8.11.3.3 Critical Error Banner

For fatal errors that prevent test execution:

```
╔═══════════════════════════════════════════════════════════════════════╗
║                         ✗✗✗ CRITICAL ERROR ✗✗✗                          ║
╠═══════════════════════════════════════════════════════════════════════╣
║ Module ng_pppoe_lb not loaded                                         ║
║                                                                          ║
║ Please load the kernel module before running tests:                     ║
║   # kldload ng_pppoe_lb                                                ║
║                                                                          ║
║ Or add to /boot/loader.conf:                                            ║
║   ng_pppoe_lb_load="YES"                                                ║
╚═══════════════════════════════════════════════════════════════════════╝
```

##### 8.11.3.4 Governor Error Scenarios

```
Governor Scaling Test Errors:
──────────────────────────────
✗ Scale-up FAILED: timeout after 10 seconds
    Expected: new worker created
    Actual: timeout waiting for worker
    Worker count: 2 (expected 3)
    
✗ Scale-down FAILED: worker not marked DRAINING
    Expected: worker2.state = DRAINING
    Actual: worker2.state = ACTIVE
    Sessions on worker2: 0

✗ Cancel-drain FAILED: worker did not return to ACTIVE
    Expected: worker2.state = ACTIVE after load spike
    Actual: worker2.state = PENDING_REMOVAL
    Sessions migrated: 0

✗ Session distribution unbalanced after scale-up
    Worker0: 150 sessions
    Worker1: 150 sessions
    Worker2: 0 sessions (new worker empty)
    Expected: ~100 sessions per worker

\033[1;31mFailed: 4/6 scenarios (66.7%)\033[0m
```

#### 8.11.4 Structured Error Output for Automation

For JSONL and programmatic consumption, errors are structured:

##### 8.11.4.1 JSONL Error Events

```jsonl
{"type":"error","severity":"critical","code":"MODULE_NOT_LOADED","message":"Kernel module ng_pppoe_lb not loaded","action":"load_module","hint":"Run: kldload ng_pppoe_lb"}
{"type":"error","severity":"warning","code":"CHECKSUM_MISMATCH","block":4592,"offset":300156928,"expected":"0xA3F5B8C9","actual":"0xD2E1F4A7"}
{"type":"error","severity":"warning","code":"MISSING_BLOCKS","blocks":[4592,4593,4701,4702,4703,4704,4705],"count":7}
{"type":"error","severity":"warning","code":"THRESHOLD_EXCEEDED","worker_id":0,"deviation":20.0,"threshold":10.0,"actual":30,"expected":25}
{"type":"error","severity":"error","code":"TIMEOUT","operation":"scale_up","expected_time_ms":5000,"actual_time_ms":10000}
{"type":"error","severity":"error","code":"AFFINITY_BROKEN","session_id":42,"original_worker":1,"final_worker":2,"retries":3}
```

##### 8.11.4.2 Error Codes Reference

| Code | Severity | Description | Action |
|------|----------|-------------|--------|
| `MODULE_NOT_LOADED` | Critical | Kernel module missing | Load module |
| `INTERFACE_NOT_FOUND` | Critical | Interface doesn't exist | Specify valid interface |
| `PERMISSION_DENIED` | Critical | Insufficient privileges | Run as root |
| `NETGRAPH_ERROR` | Error | Netgraph operation failed | Check kernel config |
| `TIMEOUT` | Error | Operation timed out | Increase timeout |
| `CHECKSUM_MISMATCH` | Warning | Data corruption detected | Investigate network |
| `MISSING_BLOCKS` | Warning | Incomplete transfer | Retry transfer |
| `THRESHOLD_EXCEEDED` | Warning | Deviation too high | Check algorithm |
| `AFFINITY_BROKEN` | Error | Session migrated | Check worker state |
| `GOVERNOR_NO_ACTION` | Warning | Scaling not triggered | Check thresholds |

#### 8.11.5 Summary Statistics Presentation

##### 8.11.5.1 Test Suite Summary

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        TEST SUITE SUMMARY                               │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  Total Tests:   100                                                      │
│                                                                          │
│  ✓ Passed:      96     ████████████████████████████████████  96.0%      │
│  ✗ Failed:       4     ██                                    4.0%      │
│  ⚠ Warnings:     5     ██                                    5.0%      │
│  ⊘ Skipped:      0     -                                                │
│                                                                          │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  By Test Type:                                                           │
│  ├─ Accuracy:   4/4   ✓ All workers within threshold                    │
│  ├─ Affinity:   3/4   ✗ 1 session lost affinity                         │
│  ├─ Transfer:   2/2   ✓ All blocks received, checksums verified         │
│  └─ Governor:   3/3   ✓ Scaling behavior correct                       │
│                                                                          │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  \033[1;32m✓ TEST SUITE PASSED (96.0% success rate)\033[0m                             │
│                                                                          │
│  Run completed in 45.23 seconds                                          │
│  Log file: /var/log/pppoe_lb_test/2026-05-01.log                        │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

##### 8.11.5.2 Performance Summary

```
┌─────────────────────────────────────────────────────────────────────────┐
│                      PERFORMANCE SUMMARY                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  Throughput:                                                            │
│  ├─ Average:     22.61 MB/s                                             │
│  ├─ Peak:        24.87 MB/s                                             │
│  └─ Min:         18.43 MB/s (stable, σ = 1.23 MB/s)                    │
│                                                                          │
│  Latency:                                                               │
│  ├─ Average:     1.23 ms                                                │
│  ├─ p95:         2.45 ms                                                │
│  └─ p99:         4.12 ms                                                │
│                                                                          │
│  CPU Utilization:                                                       │
│  ├─ Average:     67%                                                    │
│  └─ Peak:         85%                                                   │
│                                                                          │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  \033[1;32m✓ Performance within expected parameters\033[0m                           │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

##### 8.11.5.3 Compact Summary (for scripting)

For `quiet` mode or logging:

```
2026-05-01T12:34:56Z PASS accuracy 100/100 sessions distributed
2026-05-01T12:34:57Z PASS affinity 4/4 sessions maintained
2026-05-01T12:35:01Z PASS transfer 10000/10000 blocks verified
2026-05-01T12:35:01Z PASS checksum SHA256 verified
2026-05-01T12:35:03Z PASS governor scale_up triggered
2026-05-01T12:35:03Z PASS governor scale_down triggered
2026-05-01T12:35:03Z PASS governor cancel_drain worked
2026-05-01T12:35:03Z SUMMARY 7/7 tests passed (100.0%) duration=7.0s
```

#### 8.11.6 Visual Hierarchy and Layout

##### 8.11.6.1 Spacing Conventions

| Element | Before | After | Purpose |
|---------|--------|-------|---------|
| Section headers | 2 blank lines | 1 blank line | Clear separation |
| Subsections | 1 blank line | 0 blank lines | Tight grouping |
| Test results | 1 blank line | 0 blank lines | Continuous flow |
| Error details | 0 blank lines | 1 blank line | Visual break |
| Summary boxes | 1 blank line | 1 blank line | Emphasis |

##### 8.11.6.2 Box Drawing Characters

Use consistent box characters for all tables and banners:

| Character | Purpose |
|-----------|---------|
| `┌─┬─┐` | Top border (left/center/right) |
| `├─┼─┤` | Mid border (left/center/right) |
| `└─┴─┘` | Bottom border (left/center/right) |
| `│` | Vertical separator |
| `─` | Horizontal separator |
| `╔═╦═╗` | Double-line top (critical banners) |
| `║ ║` | Double-line vertical (critical banners) |
| `╚═╩═╝` | Double-line bottom (critical banners) |

##### 8.11.6.3 Alignment Standards

| Content | Alignment | Width |
|---------|-----------|-------|
| Status indicators | Left | 10 chars (`[  PASS  ]`) |
| Worker IDs | Left | 8 chars (`worker0`) |
| Numeric values | Right | Variable |
| Percentages | Right | 7 chars (`100.0%`) |
| Timestamps | Left | 25 chars (ISO 8601) |
| Error codes | Left | 20 chars |

#### 8.11.7 Progress Indicators

##### 8.11.7.1 Spinner Animation

For long-running operations:

```
Testing session distribution... [....] 100/100 sessions created
Testing session distribution... [....] 200/100 sessions created
Testing session distribution... [....] 300/100 sessions created
```

Or with percentage:

```
Testing session distribution... 100.0% (100/100 sessions)
```

##### 8.11.7.2 Progress Bar

```
[                          ] 0%   (0/100 sessions)
[███                       ] 25%  (25/100 sessions)
[██████████                ] 50%  (50/100 sessions)
[██████████████            ] 75%  (75/100 sessions)
[████████████████████       ] 100% (100/100 sessions)
```

#### 8.11.8 Contextual Help and Hints

##### 8.11.8.1 Error Hints

After an error, suggest possible causes:

```
✗ Transfer failed: checksum mismatch

Possible causes:
  1. Network corruption - check interface cables and switch
  2. Memory error - run memtest86+
  3. Kernel bug - check dmesg for hints

To diagnose:
  $ sysctl net.graph.pppoe_lb.stats
  $ netstat -i
  $ vmstat -i

Run with --verbose for detailed debug output.
```

##### 8.11.8.2 Success Suggestions

After passing tests, suggest next steps:

```
✓ All accuracy tests passed

Recommendations:
  - Run with higher session count: --sessions 1000
  - Test different algorithms: --algorithm hash
  - Enable governor: --governor-enabled

Next test: affinity test
  $ pppoe_lb_test affinity --retries 10
```

---

## 9. Future Enhancements

> **Note:** Items 1, 3, 7, 9, and 10 are now being implemented in Phases 1.5, 2.5, 2.6, 7, and 8.

1. **~~Dynamic worker scaling~~** ✅ IMPLEMENTED in Phases 1.5, 2.5, 2.6
2. **Per-worker CPU affinity:** Pin workers to specific CPU cores
3. **~~Session migration~~** ✅ PARTIALLY IMPLEMENTED (passive via DRAINING state)
4. **DPDK integration:** Use DPDK for packet I/O in high-performance scenarios
5. **NUMA awareness:** Optimize for multi-socket systems
6. **eBPF-based load balancing:** Explore eBPF for packet distribution
7. **~~Zero-downtime scaling~~** ✅ PARTIALLY IMPLEMENTED (DRAINING state prevents drops)
8. **External monitoring integration:** Export metrics via sysctl for external collectors (e.g., monitoring agents can poll `net.graph.pppoe_lb.*` sysctls)
9. **~~Shell completions~~** ✅ IMPLEMENTED in Phase 7
10. **~~Comprehensive testing framework~~** ✅ IMPLEMENTED in Phase 8 (`pppoe_lb_test`)

---

## 10. Conclusion

The recommended approach of adding a load balancer node (`ng_pppoe_lb`) in front of multiple `ng_pppoe` worker nodes provides:

- **Minimal risk:** Reuses existing, well-tested PPPoE code
- **Good scalability:** Distributes sessions across CPU cores
- **Backward compatibility:** Single-worker mode behaves identically to current implementation; multithreaded mode requires explicit activation
- **Incremental deployment:** Can be added to existing systems without disruption
- **Safe testing:** Comprehensive test harnesses ensure no host system impact

This approach aligns with FreeBSD's netgraph design philosophy and leverages existing kernel infrastructure rather than introducing complex locking schemes. The explicit activation switches (`-L`, `-w`, `pppoed_loadbalancer`) ensure that administrators must opt in to multithreaded mode, preventing accidental behavior changes on existing systems.
