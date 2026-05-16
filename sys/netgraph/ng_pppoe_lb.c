/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2025 CloudBSD Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

/*
 * ng_pppoe_lb.c - PPPoE Load Balancer Node
 *
 * This netgraph node distributes PPPoE sessions across multiple worker
 * ng_pppoe nodes to enable parallel processing on multi-core systems.
 *
 * Key features:
 * - Session affinity: packets for the same session always go to the same worker
 * - Discovery phase load balancing: round-robin distribution of PADI packets
 * - Dynamic worker management: add/remove workers at runtime
 * - CPU governor: automatic scaling based on CPU load (optional)
 * - Backward compatibility: single-worker mode behaves identically to legacy
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/smp.h>
#include <sys/limits.h>

#include <net/ethernet.h>
#include <netgraph/ng_message.h>
#include <netgraph/netgraph.h>
#include <netgraph/ng_parse.h>
#include <netgraph/ng_pppoe.h>
#include <netgraph/ng_ether.h>

#include "ng_pppoe_lb.h"

/* Memory type */
#ifdef NG_SEPARATE_MALLOC
static MALLOC_DEFINE(M_NETGRAPH_PPPOE_LB, "netgraph_pppoe_lb", "netgraph pppoe_lb node");
#else
#define M_NETGRAPH_PPPOE_LB M_NETGRAPH
#endif

/* Parse types for control messages */
static const struct ng_parse_struct_field ng_pppoe_lb_add_worker_type_fields[]
	= NG_PPPOE_LB_ADD_WORKER_TYPE_INFO;
static const struct ng_parse_type ng_pppoe_lb_add_worker_type = {
	&ng_parse_struct_type,
	&ng_pppoe_lb_add_worker_type_fields
};

static const struct ng_parse_struct_field ng_pppoe_lb_remove_worker_type_fields[]
	= { { "worker_index", &ng_parse_int32_type }, { NULL } };
static const struct ng_parse_type ng_pppoe_lb_remove_worker_type = {
	&ng_parse_struct_type,
	&ng_pppoe_lb_remove_worker_type_fields
};

static const struct ng_parse_struct_field ng_pppoe_lb_config_type_fields[]
	= NG_PPPOE_LB_CONFIG_TYPE_INFO;
static const struct ng_parse_type ng_pppoe_lb_config_type = {
	&ng_parse_struct_type,
	&ng_pppoe_lb_config_type_fields
};

static const struct ng_parse_struct_field ng_pppoe_lb_stats_type_fields[]
	= NG_PPPOE_LB_STATS_TYPE_INFO;
static const struct ng_parse_type ng_pppoe_lb_stats_type = {
	&ng_parse_struct_type,
	&ng_pppoe_lb_stats_type_fields
};

static const struct ng_parse_struct_field ng_pppoe_lb_map_entry_type_fields[]
	= NG_PPPOE_LB_MAP_ENTRY_TYPE_INFO;
static const struct ng_parse_type ng_pppoe_lb_map_entry_type = {
	&ng_parse_struct_type,
	&ng_pppoe_lb_map_entry_type_fields
};

static const struct ng_parse_array_info ng_pppoe_lb_map_entry_array_info = {
	&ng_pppoe_lb_map_entry_type,
	NULL	/* fixed-size array, no getLength needed */
};
static const struct ng_parse_type ng_pppoe_lb_map_entry_array_type = {
	&ng_parse_array_type,
	&ng_pppoe_lb_map_entry_array_info
};

static const struct ng_parse_struct_field ng_pppoe_lb_map_type_fields[]
	= NG_PPPOE_LB_MAP_TYPE_INFO;
static const struct ng_parse_type ng_pppoe_lb_map_type = {
	&ng_parse_struct_type,
	&ng_pppoe_lb_map_type_fields
};

static const struct ng_parse_struct_field ng_pppoe_lb_worker_info_type_fields[]
	= NG_PPPOE_LB_WORKER_INFO_TYPE_INFO;
static const struct ng_parse_type ng_pppoe_lb_worker_info_type = {
	&ng_parse_struct_type,
	&ng_pppoe_lb_worker_info_type_fields
};

static const struct ng_parse_struct_field ng_pppoe_lb_set_worker_state_type_fields[]
	= NG_PPPOE_LB_SET_WORKER_STATE_TYPE_INFO;
static const struct ng_parse_type ng_pppoe_lb_set_worker_state_type = {
	&ng_parse_struct_type,
	&ng_pppoe_lb_set_worker_state_type_fields
};

static const struct ng_parse_struct_field ng_pppoe_lb_get_worker_info_type_fields[]
	= NG_PPPOE_LB_GET_WORKER_INFO_TYPE_INFO;
static const struct ng_parse_type ng_pppoe_lb_get_worker_info_type = {
	&ng_parse_struct_type,
	&ng_pppoe_lb_get_worker_info_type_fields
};

static const struct ng_parse_struct_field ng_pppoe_lb_trigger_scale_type_fields[]
	= NG_PPPOE_LB_TRIGGER_SCALE_TYPE_INFO;
static const struct ng_parse_type ng_pppoe_lb_trigger_scale_type = {
	&ng_parse_struct_type,
	&ng_pppoe_lb_trigger_scale_type_fields
};

/* Sysctl variables */
static int ng_pppoe_lb_enabled = 0;
static int ng_pppoe_lb_num_workers = 1;
static int ng_pppoe_lb_algorithm = NG_PPPOE_LB_ALGO_ROUND_ROBIN;
static int ng_pppoe_lb_session_map_size = 1024;
static int ng_pppoe_lb_debug = 0;

/* Governor sysctl variables */
static int ng_pppoe_lb_governor_enabled = 0;
static int ng_pppoe_lb_governor_mode = 0;           /* 0=manual, 1=auto */
static int ng_pppoe_lb_governor_min_workers = 1;     /* Minimum workers */
static int ng_pppoe_lb_governor_max_workers = 0;    /* 0 = auto (mp_ncpus) */
static int ng_pppoe_lb_governor_cpu_threshold = 80;
static int ng_pppoe_lb_governor_cpu_low_threshold = 30;
static int ng_pppoe_lb_governor_sessions_per_worker = 500;
static int ng_pppoe_lb_governor_scale_up_interval = 10;
static int ng_pppoe_lb_governor_scale_down_interval = 60;
static int ng_pppoe_lb_governor_drain_timeout = 30;

/* Governor status (read-only) */
static int ng_pppoe_lb_governor_current_workers = 0;
static int ng_pppoe_lb_governor_active_workers = 0;
static int ng_pppoe_lb_governor_draining_workers = 0;
static int ng_pppoe_lb_governor_pending_removals = 0;
static int ng_pppoe_lb_governor_sessions = 0;
static int ng_pppoe_lb_governor_avg_sessions_worker = 0;
static int ng_pppoe_lb_governor_cpu_usage = 0;
static int ng_pppoe_lb_governor_cpu_avg = 0;
static int ng_pppoe_lb_governor_last_decision = 0;
static int ng_pppoe_lb_governor_last_reason = 0;
static int ng_pppoe_lb_governor_cpu_cores_max = 0;

/* Include for CPU statistics */
#include <sys/resource.h>

SYSCTL_NODE(_net_graph, OID_AUTO, pppoe_lb, CTLFLAG_RW, 0, "PPPoE Load Balancer");
SYSCTL_INT(_net_graph_pppoe_lb, OID_AUTO, enabled, CTLFLAG_RW, &ng_pppoe_lb_enabled, 0,
    "Enable PPPoE load balancer (0=disabled, 1=enabled)");
SYSCTL_INT(_net_graph_pppoe_lb, OID_AUTO, num_workers, CTLFLAG_RW, &ng_pppoe_lb_num_workers, 0,
    "Number of worker nodes");
SYSCTL_INT(_net_graph_pppoe_lb, OID_AUTO, algorithm, CTLFLAG_RW, &ng_pppoe_lb_algorithm, 0,
    "Load balancing algorithm (0=round-robin, 1=hash, 2=least-loaded)");
SYSCTL_INT(_net_graph_pppoe_lb, OID_AUTO, session_map_size, CTLFLAG_RW, &ng_pppoe_lb_session_map_size, 0,
    "Session map hash table size");
SYSCTL_INT(_net_graph_pppoe_lb, OID_AUTO, debug, CTLFLAG_RW, &ng_pppoe_lb_debug, 0,
    "Debug level (0=none, 1=verbose, 2=very verbose)");

SYSCTL_NODE(_net_graph_pppoe_lb, OID_AUTO, governor, CTLFLAG_RW, 0, "CPU Governor");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, enabled, CTLFLAG_RW, &ng_pppoe_lb_governor_enabled, 0,
    "Enable CPU governor (0=disabled, 1=enabled)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, mode, CTLFLAG_RW, &ng_pppoe_lb_governor_mode, 0,
    "Governor mode (0=manual, 1=auto)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, min_workers, CTLFLAG_RW, &ng_pppoe_lb_governor_min_workers, 0,
    "Minimum workers (default: 1)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, max_workers, CTLFLAG_RW, &ng_pppoe_lb_governor_max_workers, 0,
    "Maximum workers (0=auto/mp_ncpus, >0=hard cap)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, sessions_per_worker, CTLFLAG_RW, &ng_pppoe_lb_governor_sessions_per_worker, 0,
    "Target sessions per worker before scaling (default: 500)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, cpu_threshold, CTLFLAG_RW, &ng_pppoe_lb_governor_cpu_threshold, 0,
    "CPU % threshold to spawn more workers (default: 80)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, cpu_low_threshold, CTLFLAG_RW, &ng_pppoe_lb_governor_cpu_low_threshold, 0,
    "CPU % threshold to reduce workers (default: 30)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, scale_up_interval, CTLFLAG_RW, &ng_pppoe_lb_governor_scale_up_interval, 0,
    "Seconds between scale-up checks (default: 10)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, scale_down_interval, CTLFLAG_RW, &ng_pppoe_lb_governor_scale_down_interval, 0,
    "Seconds between scale-down checks (default: 60)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, drain_timeout, CTLFLAG_RW, &ng_pppoe_lb_governor_drain_timeout, 0,
    "Seconds to wait for worker drain (default: 30)");
/* Read-only status sysctls */
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, cpu_cores_max, CTLFLAG_RD, &ng_pppoe_lb_governor_cpu_cores_max, 0,
    "Maximum workers based on CPU cores (read-only)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, current_workers, CTLFLAG_RD, &ng_pppoe_lb_governor_current_workers, 0,
    "Current number of workers (read-only)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, active_workers, CTLFLAG_RD, &ng_pppoe_lb_governor_active_workers, 0,
    "Number of active workers (read-only)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, draining_workers, CTLFLAG_RD, &ng_pppoe_lb_governor_draining_workers, 0,
    "Number of draining workers (read-only)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, pending_removals, CTLFLAG_RD, &ng_pppoe_lb_governor_pending_removals, 0,
    "Workers marked for removal (read-only)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, sessions, CTLFLAG_RD, &ng_pppoe_lb_governor_sessions, 0,
    "Total active sessions (read-only)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, avg_sessions_worker, CTLFLAG_RD, &ng_pppoe_lb_governor_avg_sessions_worker, 0,
    "Average sessions per worker (read-only)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, cpu_usage, CTLFLAG_RD, &ng_pppoe_lb_governor_cpu_usage, 0,
    "Current CPU usage % (read-only)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, cpu_avg, CTLFLAG_RD, &ng_pppoe_lb_governor_cpu_avg, 0,
    "Average CPU usage % (read-only)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, last_decision, CTLFLAG_RD, &ng_pppoe_lb_governor_last_decision, 0,
    "Last governor decision: 0=none, 1=scale_up, 2=scale_down, 3=cancel_down (read-only)");
SYSCTL_INT(_net_graph_pppoe_lb_governor, OID_AUTO, last_reason, CTLFLAG_RD, &ng_pppoe_lb_governor_last_reason, 0,
    "Last governor reason: 0=none, 1=cpu_high, 2=cpu_low, 3=sess_high, 4=sess_low (read-only)");

/* Session map entry */
struct ng_pppoe_lb_sess_entry {
	LIST_ENTRY(ng_pppoe_lb_sess_entry)	next;
	uint16_t				session_id;
	int					worker_index;
	time_t					last_activity;
};

/* Worker state tracking */
struct ng_pppoe_lb_worker {
	int					worker_id;
	uint32_t				state;		/* ACTIVE/DRAINING/PENDING_REMOVAL */
	uint32_t				sessions;
	time_t					last_activity;
	time_t					start_time;
	uint64_t				packets_in;
	uint64_t				packets_out;
	uint64_t				bytes_in;
	uint64_t				bytes_out;
	time_t					drain_start_time;	/* When drain was initiated */
};

/* Private data for each node */
struct ng_pppoe_lb_private {
	struct mtx				mtx;
	hook_p					ether_hook;
	hook_p					*worker_hooks;
	struct ng_pppoe_lb_worker			*workers;
	uint32_t					*worker_states;
	int					*worker_id_to_idx;	/* Maps worker_id to current array index */
	int					num_workers;
	int					num_workers_active;
	int					num_workers_draining;
	int					num_pending_removals;
	int					max_workers;
	
	/* Session-to-worker mapping */
	LIST_HEAD(, ng_pppoe_lb_sess_entry)	sess_list;
	int					sess_count;
	
	/* Round-robin counter for discovery */
	int					next_worker;
	
	/* Configuration */
	int					algorithm;
	int					debug_level;
	
	/* Statistics */
	uint64_t				packets_in;
	uint64_t				packets_out;
	uint64_t				sessions_created;
	uint64_t				sessions_destroyed;
	
	/* Governor state */
	struct callout				governor_callout;
	time_t					last_scale_up;
	time_t					last_scale_down;
	int					pending_removal_id;	/* Worker marked for removal */
	int					scale_event_cpu;	/* CPU at last scale event */
};

#define	GET_PRIV(hook)	((struct ng_pppoe_lb_private *)NG_HOOK_PRIVATE(hook))
#define	GET_NODE_PRIV(node)	((struct ng_pppoe_lb_private *)NG_NODE_PRIVATE(node))

/* Forward declarations */
static int ng_pppoe_lb_constructor(node_p node);
static int ng_pppoe_lb_rcvmsg(node_p node, item_p item, hook_p lasthook);
static int ng_pppoe_lb_shutdown(node_p node);
static int ng_pppoe_lb_newhook(node_p node, hook_p hook, const char *name);
static int ng_pppoe_lb_connect(hook_p hook);
static int ng_pppoe_lb_rcvdata(hook_p hook, item_p item);
static int ng_pppoe_lb_disconnect(hook_p hook);

/* Internal functions */
static int ng_pppoe_lb_select_worker_discovery(struct ng_pppoe_lb_private *priv);
static int ng_pppoe_lb_select_worker_session(struct ng_pppoe_lb_private *priv, uint16_t session_id);
static struct ng_pppoe_lb_sess_entry *ng_pppoe_lb_find_session(struct ng_pppoe_lb_private *priv, uint16_t session_id);
static int ng_pppoe_lb_add_session(struct ng_pppoe_lb_private *priv, uint16_t session_id, int worker_index);
static int ng_pppoe_lb_remove_session(struct ng_pppoe_lb_private *priv, uint16_t session_id);
static void ng_pppoe_lb_governor_tick(void *arg);

/* Command list */
static const struct ng_cmdlist ng_pppoe_lb_cmds[] = {
	{
		NGM_PPPOE_LB_COOKIE,
		NGM_PPPOE_LB_ADD_WORKER,
		"add_worker",
		&ng_pppoe_lb_add_worker_type,
		NULL,
	},
	{
		NGM_PPPOE_LB_COOKIE,
		NGM_PPPOE_LB_REMOVE_WORKER,
		"remove_worker",
		&ng_pppoe_lb_remove_worker_type,
		NULL,
	},
	{
		NGM_PPPOE_LB_COOKIE,
		NGM_PPPOE_LB_SET_CONFIG,
		"set_config",
		&ng_pppoe_lb_config_type,
		NULL,
	},
	{
		NGM_PPPOE_LB_COOKIE,
		NGM_PPPOE_LB_GET_STATS,
		"get_stats",
		NULL,
		&ng_pppoe_lb_stats_type,
	},
	{
		NGM_PPPOE_LB_COOKIE,
		NGM_PPPOE_LB_GET_MAP,
		"get_map",
		NULL,
		&ng_pppoe_lb_map_type,
	},
	{
		NGM_PPPOE_LB_COOKIE,
		NGM_PPPOE_LB_SET_WORKER_STATE,
		"set_worker_state",
		&ng_pppoe_lb_set_worker_state_type,
		NULL,
	},
	{
		NGM_PPPOE_LB_COOKIE,
		NGM_PPPOE_LB_GET_WORKER_INFO,
		"get_worker_info",
		&ng_pppoe_lb_get_worker_info_type,
		&ng_pppoe_lb_worker_info_type,
	},
	{
		NGM_PPPOE_LB_COOKIE,
		NGM_PPPOE_LB_TRIGGER_SCALE,
		"trigger_scale",
		&ng_pppoe_lb_trigger_scale_type,
		NULL,
	},
	{ 0 }
};

static struct ng_type ng_pppoe_lb_typestruct = {
	.version =	NG_ABI_VERSION,
	.name =		NG_PPPOE_LB_NODE_TYPE,
	.constructor =	ng_pppoe_lb_constructor,
	.rcvmsg =	ng_pppoe_lb_rcvmsg,
	.shutdown =	ng_pppoe_lb_shutdown,
	.newhook =	ng_pppoe_lb_newhook,
	.connect =	ng_pppoe_lb_connect,
	.rcvdata =	ng_pppoe_lb_rcvdata,
	.disconnect =	ng_pppoe_lb_disconnect,
	.cmdlist =	ng_pppoe_lb_cmds,
};

NETGRAPH_INIT(pppoe_lb, &ng_pppoe_lb_typestruct);

static char ng_pppoe_lb_version[] = "ng_pppoe_lb v2 - pppoe- hook support";

/* Node constructor */
static int
ng_pppoe_lb_constructor(node_p node)
{
	struct ng_pppoe_lb_private *priv;
	int max_workers_alloc;

	priv = malloc(sizeof(*priv), M_NETGRAPH, M_NOWAIT | M_ZERO);
	if (priv == NULL)
		return (ENOMEM);

	mtx_init(&priv->mtx, "pppoe_lb", NULL, MTX_DEF);
	LIST_INIT(&priv->sess_list);
	priv->num_workers = 0;
	priv->num_workers_active = 0;
	priv->num_workers_draining = 0;
	priv->num_pending_removals = 0;
	priv->debug_level = ng_pppoe_lb_debug;
	if (priv->debug_level >= 1)
		printf("%s: loaded\n", ng_pppoe_lb_version);

	/* Initialize max_workers: 0 means auto-detect to mp_ncpus */
	ng_pppoe_lb_governor_cpu_cores_max = mp_ncpus;
	if (ng_pppoe_lb_governor_max_workers <= 0) {
		priv->max_workers = mp_ncpus;
	} else {
		/* Hard cap: never exceed mp_ncpus even if user sets higher */
		priv->max_workers = min(ng_pppoe_lb_governor_max_workers, mp_ncpus);
	}

	/* Allocate worker info array */
	max_workers_alloc = max(priv->max_workers, 1);
	priv->workers = malloc(max_workers_alloc * sizeof(*priv->workers), M_NETGRAPH, M_NOWAIT | M_ZERO);
	if (priv->workers == NULL) {
		mtx_destroy(&priv->mtx);
		free(priv, M_NETGRAPH);
		return (ENOMEM);
	}
	priv->worker_states = malloc(max_workers_alloc * sizeof(*priv->worker_states), M_NETGRAPH, M_NOWAIT | M_ZERO);
	if (priv->worker_states == NULL) {
		mtx_destroy(&priv->mtx);
		free(priv->workers, M_NETGRAPH);
		free(priv, M_NETGRAPH);
		return (ENOMEM);
	}
	priv->worker_id_to_idx = malloc(max_workers_alloc * sizeof(*priv->worker_id_to_idx), M_NETGRAPH, M_NOWAIT | M_ZERO);
	if (priv->worker_id_to_idx == NULL) {
		mtx_destroy(&priv->mtx);
		free(priv->workers, M_NETGRAPH);
		free(priv->worker_states, M_NETGRAPH);
		free(priv, M_NETGRAPH);
		return (ENOMEM);
	}
	for (int i = 0; i < max_workers_alloc; i++)
		priv->worker_id_to_idx[i] = -1;

	priv->algorithm = ng_pppoe_lb_algorithm;
	priv->debug_level = ng_pppoe_lb_debug;
	priv->next_worker = 0;
	priv->packets_in = 0;
	priv->packets_out = 0;
	priv->sessions_created = 0;
	priv->sessions_destroyed = 0;
	priv->last_scale_up = 0;
	priv->last_scale_down = 0;
	priv->pending_removal_id = -1;
	priv->scale_event_cpu = 0;

	callout_init(&priv->governor_callout, CALLOUT_MPSAFE);
	callout_reset(&priv->governor_callout, hz, ng_pppoe_lb_governor_tick, priv);

	NG_NODE_SET_PRIVATE(node, priv);
	NG_NODE_REF(node);  /* Reference for private data */

	return (0);
}

/* Receive control message */
static int
ng_pppoe_lb_rcvmsg(node_p node, item_p item, hook_p lasthook)
{
	struct ng_pppoe_lb_private *priv;
	struct ng_mesg *msg, *resp = NULL;
	int error = 0;

	priv = GET_NODE_PRIV(node);
	NGI_GET_MSG(item, msg);

	switch (msg->header.typecookie) {
	case NGM_PPPOE_LB_COOKIE:
		switch (msg->header.cmd) {
		case NGM_PPPOE_LB_ADD_WORKER:
			/* Handled via newhook/connect */
			break;

		case NGM_PPPOE_LB_REMOVE_WORKER:
			/* Handled via disconnect */
			break;

		case NGM_PPPOE_LB_REMOVE_WORKER_BY_ID: {
			struct ng_pppoe_lb_worker_id *wid;
			int widx;

			if (msg->header.arglen != sizeof(*wid)) {
				error = EINVAL;
				break;
			}
			wid = (struct ng_pppoe_lb_worker_id *)msg->data;
			mtx_lock(&priv->mtx);
			if (wid->worker_id < 0 || wid->worker_id >= priv->num_workers) {
				mtx_unlock(&priv->mtx);
				error = EINVAL;
				break;
			}
			widx = wid->worker_id;
			if (priv->worker_states[widx] == NG_PPPOE_LB_WORKER_PENDING_REMOVAL) {
				priv->worker_states[widx] = NG_PPPOE_LB_WORKER_REMOVED;
				priv->num_pending_removals--;
			}
			mtx_unlock(&priv->mtx);
			error = EINVAL;
			break;
		}

		case NGM_PPPOE_LB_SET_CONFIG: {
			struct ng_pppoe_lb_config *cfg;
			if (msg->header.arglen != sizeof(*cfg)) {
				error = EINVAL;
				break;
			}
			cfg = (struct ng_pppoe_lb_config *)msg->data;
			mtx_lock(&priv->mtx);
			priv->algorithm = cfg->algorithm;
			priv->max_workers = cfg->max_workers;
			priv->debug_level = cfg->debug_level;
			mtx_unlock(&priv->mtx);
			break;
		}

		case NGM_PPPOE_LB_GET_STATS: {
			struct ng_pppoe_lb_stats *stats;
			resp = malloc(sizeof(*resp) + sizeof(*stats), M_NETGRAPH_PPPOE_LB, M_NOWAIT | M_ZERO);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}
			resp->header.typecookie = NGM_PPPOE_LB_COOKIE;
			resp->header.cmd = NGM_PPPOE_LB_GET_STATS;
			resp->header.arglen = sizeof(*stats);
			stats = (struct ng_pppoe_lb_stats *)resp->data;
			mtx_lock(&priv->mtx);
			stats->packets_in = priv->packets_in;
			stats->packets_out = priv->packets_out;
			stats->sessions_created = priv->sessions_created;
			stats->sessions_destroyed = priv->sessions_destroyed;
			stats->num_workers = priv->num_workers;
			stats->num_workers_active = priv->num_workers_active;
			stats->algorithm = priv->algorithm;
			stats->map_count = priv->sess_count;
			mtx_unlock(&priv->mtx);
			break;
		}

		case NGM_PPPOE_LB_GET_MAP: {
			struct ng_pppoe_lb_map *map_req, *map_resp;
			struct ng_pppoe_lb_sess_entry *entry;
			int count = 0, max_entries;

			if (msg->header.arglen < sizeof(*map_req)) {
				error = EINVAL;
				break;
			}
			map_req = (struct ng_pppoe_lb_map *)msg->data;
			max_entries = map_req->max_entries;
			if (max_entries > 1024)
				max_entries = 1024;

			resp = malloc(sizeof(*resp) + sizeof(*map_resp) +
			    max_entries * sizeof(struct ng_pppoe_lb_map_entry),
			    M_NETGRAPH_PPPOE_LB, M_NOWAIT | M_ZERO);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}
			resp->header.typecookie = NGM_PPPOE_LB_COOKIE;
			resp->header.cmd = NGM_PPPOE_LB_GET_MAP;
			map_resp = (struct ng_pppoe_lb_map *)resp->data;

			mtx_lock(&priv->mtx);
			LIST_FOREACH(entry, &priv->sess_list, next) {
				if (count >= max_entries)
					break;
				map_resp->entries[count].session_id = entry->session_id;
				map_resp->entries[count].worker_index = entry->worker_index;
				map_resp->entries[count].last_activity = entry->last_activity;
				count++;
			}
			map_resp->count = count;
			resp->header.arglen = sizeof(*map_resp) +
			    count * sizeof(struct ng_pppoe_lb_map_entry);
			mtx_unlock(&priv->mtx);
			break;
		}

		case NGM_PPPOE_LB_SET_WORKER_STATE: {
			struct ng_pppoe_lb_set_worker_state *req;
			struct ng_pppoe_lb_worker *worker;
			int widx = -1;
			if (msg->header.arglen != sizeof(*req)) {
				error = EINVAL;
				break;
			}
			req = (struct ng_pppoe_lb_set_worker_state *)msg->data;

			mtx_lock(&priv->mtx);

			/* Find worker by worker_id */
			for (int i = 0; i < priv->num_workers; i++) {
				if (priv->workers[i].worker_id == req->worker_id) {
					widx = i;
					break;
				}
			}

			/* Validate worker ID */
			if (widx < 0 || widx >= priv->num_workers) {
				mtx_unlock(&priv->mtx);
				error = ENOENT;
				break;
			}

			/* Validate state value */
			if (req->state > NG_PPPOE_LB_WORKER_PENDING_REMOVAL) {
				mtx_unlock(&priv->mtx);
				error = EINVAL;
				break;
			}

			worker = &priv->workers[widx];

			/* Can't set PENDING_REMOVAL if worker has active sessions */
			if (req->state == NG_PPPOE_LB_WORKER_PENDING_REMOVAL && worker->sessions > 0) {
				mtx_unlock(&priv->mtx);
				error = EBUSY;
				break;
			}

			/* Update state in worker_states array */
			priv->worker_states[req->worker_id] = req->state;
			worker->last_activity = time_uptime;

			/* Update counters - only increment, don't decrement (decrement handled by disconnect) */
			if (req->state == NG_PPPOE_LB_WORKER_ACTIVE)
				priv->num_workers_active++;
			else if (req->state == NG_PPPOE_LB_WORKER_DRAINING)
				priv->num_workers_draining++;
			else if (req->state == NG_PPPOE_LB_WORKER_PENDING_REMOVAL)
				priv->num_pending_removals++;

			/* Track pending removal for "change mind" feature */
			if (req->state == NG_PPPOE_LB_WORKER_DRAINING) {
				worker->drain_start_time = time_uptime;
				priv->pending_removal_id = widx;
			} else if (req->state == NG_PPPOE_LB_WORKER_ACTIVE) {
				/* Cancel any pending removal for this worker */
				if (priv->pending_removal_id == widx) {
					priv->pending_removal_id = -1;
					ng_pppoe_lb_governor_last_decision = NG_PPPOE_LB_GOV_DECISION_CANCEL_DOWN;
					ng_pppoe_lb_governor_last_reason = NG_PPPOE_LB_GOV_REASON_CPU_HIGH;
				}
			}

			mtx_unlock(&priv->mtx);
			break;
		}

		case NGM_PPPOE_LB_GET_WORKER_INFO: {
			struct ng_pppoe_lb_get_worker_info *req;
			struct ng_pppoe_lb_worker_info *info;

			if (msg->header.arglen != sizeof(*req)) {
				error = EINVAL;
				break;
			}
			req = (struct ng_pppoe_lb_get_worker_info *)msg->data;

			resp = malloc(sizeof(*resp) + sizeof(*info), M_NETGRAPH_PPPOE_LB, M_NOWAIT | M_ZERO);
			if (resp == NULL) {
				error = ENOMEM;
				break;
			}
			resp->header.typecookie = NGM_PPPOE_LB_COOKIE;
			resp->header.cmd = NGM_PPPOE_LB_GET_WORKER_INFO;
			resp->header.arglen = sizeof(*info);
			info = (struct ng_pppoe_lb_worker_info *)resp->data;

			mtx_lock(&priv->mtx);

			/*
			 * Special case: -1 means return info for first worker.
			 * Use a temporary variable to avoid modifying req->worker_id
			 * before validation.
			 */
			int wid = (req->worker_id == -1) ? 0 : req->worker_id;
			int widx = -1;

			for (int i = 0; i < priv->num_workers; i++) {
				if (priv->workers[i].worker_id == wid) {
					widx = i;
					break;
				}
			}

			/* Validate worker ID */
			if (widx < 0 || widx >= priv->num_workers) {
				mtx_unlock(&priv->mtx);
				free(resp, M_NETGRAPH);
				error = ENOENT;
				break;
			}

			info->worker_id = wid;
			info->state = priv->worker_states[wid];
			info->sessions = priv->workers[widx].sessions;
			info->last_activity = priv->workers[widx].last_activity;
			info->uptime = time_uptime - priv->workers[widx].start_time;
			info->packets_in = priv->workers[widx].packets_in;
			info->packets_out = priv->workers[widx].packets_out;
			info->bytes_in = priv->workers[widx].bytes_in;
			info->bytes_out = priv->workers[widx].bytes_out;
			if (priv->worker_hooks[widx] != NULL)
				strlcpy(info->hook_name, priv->worker_hooks[widx]->hk_name,
				    sizeof(info->hook_name));
			else
				info->hook_name[0] = '\0';

			mtx_unlock(&priv->mtx);
			break;
		}

		case NGM_PPPOE_LB_GET_WORKERS_BY_STATE: {
			struct ng_pppoe_lb_get_workers_by_state *req;
			int32_t *ids;
			int count, i;

			if (msg->header.arglen != sizeof(*req)) {
				error = EINVAL;
				break;
			}
			req = (struct ng_pppoe_lb_get_workers_by_state *)msg->data;

			mtx_lock(&priv->mtx);
			count = 0;
			for (i = 0; i < priv->num_workers && count < req->max_count; i++) {
				if (priv->worker_states[priv->workers[i].worker_id] == req->state)
					count++;
			}
			resp = malloc(sizeof(*resp) + count * sizeof(int32_t),
			    M_NETGRAPH_PPPOE_LB, M_NOWAIT | M_ZERO);
			if (resp == NULL) {
				mtx_unlock(&priv->mtx);
				error = ENOMEM;
				break;
			}
			resp->header.typecookie = NGM_PPPOE_LB_COOKIE;
			resp->header.cmd = NGM_PPPOE_LB_GET_WORKERS_BY_STATE;
			resp->header.arglen = sizeof(*resp) + count * sizeof(int32_t);
			ids = (int32_t *)resp->data;
			count = 0;
			for (i = 0; i < priv->num_workers && count < req->max_count; i++) {
				if (priv->worker_states[priv->workers[i].worker_id] == req->state)
					ids[count++] = priv->workers[i].worker_id;
			}
			mtx_unlock(&priv->mtx);
			break;
		}

		case NGM_PPPOE_LB_TRIGGER_SCALE: {
			struct ng_pppoe_lb_trigger_scale *req;

			if (msg->header.arglen != sizeof(*req)) {
				error = EINVAL;
				break;
			}
			req = (struct ng_pppoe_lb_trigger_scale *)msg->data;

			mtx_lock(&priv->mtx);

			if (req->direction == 0) {
				/* Trigger scale up */
				priv->last_scale_up = 0;  /* Force immediate scale */
				ng_pppoe_lb_governor_last_decision = NG_PPPOE_LB_GOV_DECISION_SCALE_UP;
				ng_pppoe_lb_governor_last_reason = NG_PPPOE_LB_GOV_REASON_CPU_HIGH;
				if (priv->debug_level >= 1) {
					printf("ng_pppoe_lb: scale up triggered manually\n");
				}
			} else {
				/* Trigger scale down */
				priv->last_scale_down = 0;  /* Force immediate scale */
				ng_pppoe_lb_governor_last_decision = NG_PPPOE_LB_GOV_DECISION_SCALE_DOWN;
				ng_pppoe_lb_governor_last_reason = NG_PPPOE_LB_GOV_REASON_CPU_LOW;
				if (priv->debug_level >= 1) {
					printf("ng_pppoe_lb: scale down triggered manually\n");
				}
			}

			mtx_unlock(&priv->mtx);
			break;
		}

		default:
			error = EINVAL;
			break;
		}
		break;

	case NGM_PPPOE_COOKIE:
		switch (msg->header.cmd) {
		case NGM_PPPOE_LISTEN:
			/* LISTEN - just acknowledge, session handling is via workers */
			break;
		default:
			error = EINVAL;
			break;
		}
		break;

	case NGM_ETHER_COOKIE:
		if (msg->header.cmd == NGM_ETHER_GET_ENADDR) {
			error = ENOTCONN;
			break;
		}
		error = EINVAL;
		break;

	default:
		error = EINVAL;
		break;
	}

	if (error != 0) {
		NG_FREE_MSG(msg);
		NG_FREE_ITEM(item);
	} else {
		if (resp != NULL) {
			NG_RESPOND_MSG(error, node, item, resp);
			NG_FREE_MSG(msg);
		} else {
			NG_FREE_MSG(msg);
			NG_FREE_ITEM(item);
		}
	}

	return (error);
}

/* Node shutdown */
static int
ng_pppoe_lb_shutdown(node_p node)
{
	struct ng_pppoe_lb_private *priv;
	struct ng_pppoe_lb_sess_entry *entry, *tmp;

	priv = GET_NODE_PRIV(node);

	/* Stop governor callout */
	callout_stop(&priv->governor_callout);

	/* Free all session entries */
	mtx_lock(&priv->mtx);
	LIST_FOREACH_SAFE(entry, &priv->sess_list, next, tmp) {
		LIST_REMOVE(entry, next);
		free(entry, M_NETGRAPH);
	}
	mtx_unlock(&priv->mtx);

	if (priv->worker_hooks != NULL)
		free(priv->worker_hooks, M_NETGRAPH);

	free(priv->workers, M_NETGRAPH);
	free(priv->worker_states, M_NETGRAPH);
	free(priv->worker_id_to_idx, M_NETGRAPH);

	mtx_destroy(&priv->mtx);
	free(priv, M_NETGRAPH);

	NG_NODE_SET_PRIVATE(node, NULL);
	NG_NODE_UNREF(node);

	return (0);
}

/* New hook creation */
static int
ng_pppoe_lb_newhook(node_p node, hook_p hook, const char *name)
{
	struct ng_pppoe_lb_private *priv;

	priv = GET_NODE_PRIV(node);
	if (priv->debug_level >= 2)
		printf("ng_pppoe_lb_newhook: START node=%p hook=%p name='%s'\n",
		    node, hook, name);

	if (strcmp(name, NG_PPPOE_LB_HOOK_ETHER) == 0) {
		if (priv->ether_hook != NULL)
			return (EEXIST);
		priv->ether_hook = hook;
		NG_HOOK_SET_PRIVATE(hook, priv);
	} else if (strncmp(name, "pppoe-", 6) == 0) {
		NG_HOOK_SET_PRIVATE(hook, priv);
	} else if (strncmp(name, NG_PPPOE_LB_HOOK_WORKER_BASE,
	    strlen(NG_PPPOE_LB_HOOK_WORKER_BASE)) == 0) {
		NG_HOOK_SET_PRIVATE(hook, priv);
	} else {
		if (priv->debug_level >= 1)
			printf("ng_pppoe_lb_newhook: rejecting invalid hook name '%s'\n", name);
		return (EINVAL);
	}

	return (0);
}

/* Hook connection */
static int
ng_pppoe_lb_connect(hook_p hook)
{
	struct ng_pppoe_lb_private *priv;
	hook_p *new_hooks;
	int new_size;

	priv = GET_PRIV(hook);
	if (priv == NULL) {
		printf("ng_pppoe_lb_connect: priv is NULL, returning EINVAL\n");
		return (EINVAL);
	}
	if (priv->debug_level >= 2)
		printf("ng_pppoe_lb_connect: hook=%p\n", hook);

	if (hook == priv->ether_hook) {
		/* Ethernet hook connected */
		return (0);
	}

	/* Check for PPPoE control hook from socket */
	if (strncmp(NG_HOOK_NAME(hook), "pppoe-", 6) == 0) {
		/* Control hook - just acknowledge, don't add to workers */
		return (0);
	}

	/* Worker hook - add to array */
	mtx_lock(&priv->mtx);
	new_size = (priv->num_workers + 1) * sizeof(hook_p);
	new_hooks = realloc(priv->worker_hooks, new_size, M_NETGRAPH, M_NOWAIT);
	if (new_hooks == NULL) {
		mtx_unlock(&priv->mtx);
		return (ENOMEM);
	}
	int idx = priv->num_workers;
	priv->worker_hooks = new_hooks;
	priv->worker_hooks[idx] = hook;
	priv->workers[idx].worker_id = idx;
	priv->workers[idx].state = NG_PPPOE_LB_WORKER_ACTIVE;
	priv->worker_states[idx] = NG_PPPOE_LB_WORKER_ACTIVE;
	priv->worker_id_to_idx[idx] = idx;
	priv->num_workers++;
	priv->num_workers_active++;
	mtx_unlock(&priv->mtx);

	return (0);
}

/* Receive data packet */
static int
ng_pppoe_lb_rcvdata(hook_p hook, item_p item)
{
	struct ng_pppoe_lb_private *priv;
	struct mbuf *m;
	struct pppoe_full_hdr *wh;
	int worker_idx, error = 0;
	uint16_t session_id;

	priv = GET_PRIV(hook);
	NGI_GET_M(item, m);

	if (m->m_len < sizeof(*wh))
		m = m_pullup(m, sizeof(*wh));
	if (m == NULL) {
		NG_FREE_ITEM(item);
		return (ENOBUFS);
	}

	wh = mtod(m, struct pppoe_full_hdr *);

	mtx_lock(&priv->mtx);
	priv->packets_in++;

	switch (ntohs(wh->eh.ether_type)) {
	case ETHERTYPE_PPPOE_DISC:
	case ETHERTYPE_PPPOE_3COM_DISC:
		/* Discovery phase - use round-robin for new sessions */
		worker_idx = ng_pppoe_lb_select_worker_discovery(priv);
		break;

	case ETHERTYPE_PPPOE_SESS:
	case ETHERTYPE_PPPOE_3COM_SESS:
		/* Session phase - use session ID for affinity */
		session_id = ntohs(wh->ph.sid);
		worker_idx = ng_pppoe_lb_select_worker_session(priv, session_id);
		break;

	default:
		mtx_unlock(&priv->mtx);
		NG_FREE_M(m);
		NG_FREE_ITEM(item);
		return (EPFNOSUPPORT);
	}

	if (worker_idx < 0 || worker_idx >= priv->num_workers) {
		mtx_unlock(&priv->mtx);
		NG_FREE_M(m);
		NG_FREE_ITEM(item);
		return (ENETUNREACH);
	}

	priv->packets_out++;
	mtx_unlock(&priv->mtx);

	NG_FWD_NEW_DATA(error, item, priv->worker_hooks[worker_idx], m);
	return (error);
}

/* Hook disconnection */
static int
ng_pppoe_lb_disconnect(hook_p hook)
{
	struct ng_pppoe_lb_private *priv;
	int i, idx;

	priv = GET_PRIV(hook);
	if (priv == NULL)
		return (EINVAL);

	if (hook == priv->ether_hook) {
		priv->ether_hook = NULL;
		return (0);
	}

	/* Worker hook - remove from array */
	mtx_lock(&priv->mtx);
	idx = -1;
	for (i = 0; i < priv->num_workers; i++) {
		if (priv->worker_hooks[i] == hook) {
			idx = i;
			break;
		}
	}

	if (idx >= 0) {
		struct ng_pppoe_lb_sess_entry *sess_entry, *sess_tmp;
		uint32_t removed_worker_id = priv->workers[idx].worker_id;
		int worker_state = priv->worker_states[removed_worker_id];

		/*
		 * Clean up sessions mapped to this worker before removal.
		 * We must remove sessions that were mapped to idx, since
		 * after the shift they'll have stale indices.
		 */
		LIST_FOREACH_SAFE(sess_entry, &priv->sess_list, next, sess_tmp) {
			if (sess_entry->worker_index == idx) {
				LIST_REMOVE(sess_entry, next);
				free(sess_entry, M_NETGRAPH);
				priv->sess_count--;
				priv->sessions_destroyed++;
			} else if (sess_entry->worker_index > idx) {
				/*
				 * Adjust indices for sessions that will be
				 * shifted left after worker removal.
				 */
				sess_entry->worker_index--;
			}
		}

		for (i = idx; i < priv->num_workers - 1; i++)
			priv->worker_hooks[i] = priv->worker_hooks[i + 1];
		priv->worker_hooks[priv->num_workers - 1] = NULL;

		for (i = idx; i < priv->num_workers - 1; i++)
			priv->workers[i] = priv->workers[i + 1];
		bzero(&priv->workers[priv->num_workers - 1], sizeof(priv->workers[priv->num_workers - 1]));

		for (i = idx; i < priv->num_workers - 1; i++)
			priv->worker_id_to_idx[priv->workers[i].worker_id] = i;
		priv->worker_id_to_idx[removed_worker_id] = -1;

		priv->num_workers--;
		if (worker_state == NG_PPPOE_LB_WORKER_ACTIVE)
			priv->num_workers_active--;
		else if (worker_state == NG_PPPOE_LB_WORKER_DRAINING)
			priv->num_workers_draining--;
		else if (worker_state == NG_PPPOE_LB_WORKER_PENDING_REMOVAL)
			priv->num_pending_removals--;

		priv->worker_states[removed_worker_id] = NG_PPPOE_LB_WORKER_REMOVED;
	}
	mtx_unlock(&priv->mtx);

	return (0);
}

/* Select worker for discovery (round-robin) - skips DRAINING/PENDING_REMOVAL workers */
static int
ng_pppoe_lb_select_worker_discovery(struct ng_pppoe_lb_private *priv)
{
	int idx, start_idx, count;
	int active_count = 0;

	if (priv->num_workers_active == 0)
		return (-1);

	idx = priv->next_worker;
	start_idx = idx;

	/* Find next ACTIVE worker */
	count = 0;
	while (count < priv->num_workers) {
		if (priv->worker_states[priv->workers[idx].worker_id] == NG_PPPOE_LB_WORKER_ACTIVE) {
			active_count++;
			if (active_count > 1)
				break;  /* Found the next active worker */
		}
		count++;
		idx = (idx + 1) % priv->num_workers;
		if (idx == start_idx)
			break;
	}

	/* Advance next_worker to the next position for next call */
	priv->next_worker = (priv->next_worker + 1) % priv->num_workers;

	/* Find the ACTIVE worker starting from next_worker */
	count = 0;
	idx = priv->next_worker;
	while (count < priv->num_workers) {
		if (priv->worker_states[priv->workers[idx].worker_id] == NG_PPPOE_LB_WORKER_ACTIVE)
			break;
		count++;
		idx = (idx + 1) % priv->num_workers;
	}

	priv->next_worker = idx;

	return (idx);
}

/* Select worker for session (hash-based affinity) - skips DRAINING/PENDING_REMOVAL workers */
static int
ng_pppoe_lb_select_worker_session(struct ng_pppoe_lb_private *priv, uint16_t session_id)
{
	struct ng_pppoe_lb_sess_entry *entry;
	int idx;

	/* Check session map first - if existing worker is still active, use it */
	entry = ng_pppoe_lb_find_session(priv, session_id);
	if (entry != NULL) {
		idx = entry->worker_index;
		if (idx >= 0 && idx < priv->num_workers &&
		    priv->worker_states[priv->workers[idx].worker_id] == NG_PPPOE_LB_WORKER_ACTIVE) {
			entry->last_activity = time_uptime;
			return (idx);
		}
		/* Worker no longer active - remove from map, will reassign */
		ng_pppoe_lb_remove_session(priv, session_id);
	}

	/* No existing mapping or worker inactive - find an ACTIVE worker */
	if (priv->num_workers_active == 0)
		return (-1);

	/* Use hash to select a candidate, then find nearest active worker */
	idx = session_id % priv->num_workers;

	/* Check if this worker is active */
	if (priv->worker_states[priv->workers[idx].worker_id] != NG_PPPOE_LB_WORKER_ACTIVE) {
		/* Find the nearest active worker */
		int i, found_idx = -1;
		for (i = 0; i < priv->num_workers; i++) {
			if (priv->worker_states[priv->workers[i].worker_id] == NG_PPPOE_LB_WORKER_ACTIVE) {
				found_idx = i;
				break;
			}
		}
		if (found_idx == -1)
			return (-1);
		idx = found_idx;
	}

	/* Add to session map */
	ng_pppoe_lb_add_session(priv, session_id, idx);

	return (idx);
}

/* Find session in map */
static struct ng_pppoe_lb_sess_entry *
ng_pppoe_lb_find_session(struct ng_pppoe_lb_private *priv, uint16_t session_id)
{
	struct ng_pppoe_lb_sess_entry *entry;

	LIST_FOREACH(entry, &priv->sess_list, next) {
		if (entry->session_id == session_id)
			return (entry);
	}
	return (NULL);
}

/* Add session to map */
static int
ng_pppoe_lb_add_session(struct ng_pppoe_lb_private *priv, uint16_t session_id, int worker_index)
{
	struct ng_pppoe_lb_sess_entry *entry;

	entry = malloc(sizeof(*entry), M_NETGRAPH, M_NOWAIT);
	if (entry == NULL)
		return (ENOMEM);

	entry->session_id = session_id;
	entry->worker_index = worker_index;
	entry->last_activity = time_uptime;
	LIST_INSERT_HEAD(&priv->sess_list, entry, next);
	priv->sess_count++;
	priv->sessions_created++;

	/* Update worker session count */
	if (worker_index >= 0 && worker_index < priv->num_workers) {
		priv->workers[worker_index].sessions++;
		priv->workers[worker_index].last_activity = time_uptime;
	}

	/* Update global sysctls */
	ng_pppoe_lb_governor_sessions = priv->sess_count;
	if (priv->num_workers_active > 0)
		ng_pppoe_lb_governor_avg_sessions_worker = priv->sess_count / priv->num_workers_active;

	return (0);
}

/* Remove session from map */
static int
ng_pppoe_lb_remove_session(struct ng_pppoe_lb_private *priv, uint16_t session_id)
{
	struct ng_pppoe_lb_sess_entry *entry;
	int worker_index;

	entry = ng_pppoe_lb_find_session(priv, session_id);
	if (entry == NULL)
		return (ENOENT);

	worker_index = entry->worker_index;
	LIST_REMOVE(entry, next);
	free(entry, M_NETGRAPH);
	priv->sess_count--;
	priv->sessions_destroyed++;

	/* Update worker session count */
	if (worker_index >= 0 && worker_index < priv->num_workers) {
		if (priv->workers[worker_index].sessions > 0)
			priv->workers[worker_index].sessions--;
		priv->workers[worker_index].last_activity = time_uptime;
	}

	/* Update global sysctls */
	ng_pppoe_lb_governor_sessions = priv->sess_count;
	if (priv->num_workers_active > 0)
		ng_pppoe_lb_governor_avg_sessions_worker = priv->sess_count / priv->num_workers_active;

	return (0);
}

/* Governor tick function - monitors CPU load and scales workers with "change mind" support */
static void
ng_pppoe_lb_governor_tick(void *arg)
{
	struct ng_pppoe_lb_private *priv;
	time_t now;
	int avg_cpu, target_workers;
	long cp_time[CPUSTATES];
	long total_time, idle_time;
	int sessions_per_worker;
	int i;

	priv = (struct ng_pppoe_lb_private *)arg;

	if (!ng_pppoe_lb_governor_enabled)
		goto out;

	mtx_lock(&priv->mtx);
	now = time_uptime;

	/* Read CPU statistics */
	read_cpu_time(cp_time);

	/* Calculate CPU utilization percentage */
	total_time = cp_time[CP_USER] + cp_time[CP_NICE] + cp_time[CP_SYS] +
	    cp_time[CP_INTR] + cp_time[CP_IDLE];
	idle_time = cp_time[CP_IDLE];

	if (total_time > 0) {
		avg_cpu = 100 - (int)((idle_time * 100) / total_time);
	} else {
		avg_cpu = 0;
	}

	/* Update global sysctls */
	ng_pppoe_lb_governor_cpu_usage = avg_cpu;
	ng_pppoe_lb_governor_cpu_avg = avg_cpu;  /* Could be rolling average */
	ng_pppoe_lb_governor_current_workers = priv->num_workers;
	ng_pppoe_lb_governor_active_workers = priv->num_workers_active;
	ng_pppoe_lb_governor_draining_workers = priv->num_workers_draining;
	ng_pppoe_lb_governor_pending_removals = priv->num_pending_removals;
	ng_pppoe_lb_governor_sessions = priv->sess_count;

	/* Check for pending removal timeout - promote to PENDING_REMOVAL if drained */
	if (priv->pending_removal_id >= 0) {
		int widx = priv->worker_id_to_idx[priv->pending_removal_id];
		if (widx >= 0 && widx < priv->num_workers) {
			struct ng_pppoe_lb_worker *worker = &priv->workers[widx];
			if (worker->state == NG_PPPOE_LB_WORKER_DRAINING &&
			    now - worker->drain_start_time >= ng_pppoe_lb_governor_drain_timeout) {
				if (worker->sessions == 0) {
					worker->state = NG_PPPOE_LB_WORKER_PENDING_REMOVAL;
					priv->worker_states[worker->worker_id] = NG_PPPOE_LB_WORKER_PENDING_REMOVAL;
					priv->num_workers_draining--;
					priv->num_pending_removals++;
					priv->pending_removal_id = -1;
					ng_pppoe_lb_governor_last_decision = NG_PPPOE_LB_GOV_DECISION_SCALE_DOWN;
					ng_pppoe_lb_governor_last_reason = NG_PPPOE_LB_GOV_REASON_CPU_LOW;
					if (priv->debug_level >= 1) {
						printf("ng_pppoe_lb: worker %d drained successfully, "
						    "marked for removal\n", worker->worker_id);
					}
				} else {
					worker->state = NG_PPPOE_LB_WORKER_ACTIVE;
					priv->worker_states[worker->worker_id] = NG_PPPOE_LB_WORKER_ACTIVE;
					priv->num_workers_draining--;
					priv->num_workers_active++;
					priv->pending_removal_id = -1;
					ng_pppoe_lb_governor_last_decision = NG_PPPOE_LB_GOV_DECISION_CANCEL_DOWN;
					ng_pppoe_lb_governor_last_reason = NG_PPPOE_LB_GOV_REASON_SESS_HIGH;
					if (priv->debug_level >= 1) {
						printf("ng_pppoe_lb: worker %d drain timeout, "
						    "returning to ACTIVE (%u sessions remaining)\n",
						    worker->worker_id, worker->sessions);
					}
				}
			}
		}
	}

	/* Check if any DRAINING worker received a new session - cancel its drain */
	for (i = 0; i < priv->num_workers; i++) {
		struct ng_pppoe_lb_worker *worker = &priv->workers[i];
		if (priv->worker_states[worker->worker_id] == NG_PPPOE_LB_WORKER_DRAINING && worker->sessions > 0) {
			worker->state = NG_PPPOE_LB_WORKER_ACTIVE;
			priv->worker_states[worker->worker_id] = NG_PPPOE_LB_WORKER_ACTIVE;
			priv->num_workers_draining--;
			priv->num_workers_active++;
			if (priv->pending_removal_id == worker->worker_id)
				priv->pending_removal_id = -1;
			ng_pppoe_lb_governor_last_decision = NG_PPPOE_LB_GOV_DECISION_CANCEL_DOWN;
			ng_pppoe_lb_governor_last_reason = NG_PPPOE_LB_GOV_REASON_SESS_HIGH;
			if (priv->debug_level >= 1) {
				printf("ng_pppoe_lb: worker %d received new session, "
				    "canceling drain\n", worker->worker_id);
			}
		}
	}

	/* Calculate effective max workers considering min_workers setting */
	target_workers = priv->max_workers;
	if (ng_pppoe_lb_governor_min_workers > 0)
		target_workers = max(target_workers, ng_pppoe_lb_governor_min_workers);

	/* Calculate sessions per worker */
	sessions_per_worker = ng_pppoe_lb_governor_sessions_per_worker;
	if (priv->num_workers_active > 0)
		sessions_per_worker = priv->sess_count / priv->num_workers_active;
	ng_pppoe_lb_governor_avg_sessions_worker = sessions_per_worker;

	/* SCALE UP DECISION
	 * Scale up if:
	 *   - CPU > threshold AND interval elapsed
	 *   - OR sessions/worker > threshold AND interval elapsed
	 *   - AND workers < max_workers
	 *   - AND there are no workers marked for removal (we can "change mind")
	 */
	if ((avg_cpu > ng_pppoe_lb_governor_cpu_threshold ||
	    (ng_pppoe_lb_governor_sessions_per_worker > 0 &&
	     sessions_per_worker > ng_pppoe_lb_governor_sessions_per_worker)) &&
	    now - priv->last_scale_up >= ng_pppoe_lb_governor_scale_up_interval) {

		if (priv->num_workers_active < target_workers) {

			/* "CHANGE MIND" LOGIC: Cancel any pending removal if we need to scale up */
			if (priv->pending_removal_id >= 0) {
				int widx = priv->worker_id_to_idx[priv->pending_removal_id];
				if (widx >= 0 && widx < priv->num_workers) {
					struct ng_pppoe_lb_worker *worker = &priv->workers[widx];
					worker->state = NG_PPPOE_LB_WORKER_ACTIVE;
					priv->worker_states[worker->worker_id] = NG_PPPOE_LB_WORKER_ACTIVE;
					priv->num_workers_draining--;
					priv->num_workers_active++;
					priv->pending_removal_id = -1;
					ng_pppoe_lb_governor_last_decision = NG_PPPOE_LB_GOV_DECISION_CANCEL_DOWN;
					ng_pppoe_lb_governor_last_reason = NG_PPPOE_LB_GOV_REASON_CPU_HIGH;
					if (priv->debug_level >= 1) {
						printf("ng_pppoe_lb: governor canceled pending removal "
						    "of worker %d (change mind - scale up needed)\n",
						    worker->worker_id);
					}
				}
			}

			/* Signal scale up */
			priv->last_scale_up = now;
			ng_pppoe_lb_governor_last_decision = NG_PPPOE_LB_GOV_DECISION_SCALE_UP;
			ng_pppoe_lb_governor_last_reason =
			    (avg_cpu > ng_pppoe_lb_governor_cpu_threshold) ?
			    NG_PPPOE_LB_GOV_REASON_CPU_HIGH : NG_PPPOE_LB_GOV_REASON_SESS_HIGH;
			if (priv->debug_level >= 1) {
				printf("ng_pppoe_lb: governor wants to scale up "
				    "(CPU=%d%%, sess/worker=%d, workers=%d/%d)\n",
				    avg_cpu, sessions_per_worker,
				    priv->num_workers_active, target_workers);
			}
		}
	}

	/* SCALE DOWN DECISION
	 * Scale down if:
	 *   - CPU < low threshold AND interval elapsed
	 *   - AND workers > min_workers
	 *   - AND no pending removal already
	 */
	if (avg_cpu < ng_pppoe_lb_governor_cpu_low_threshold &&
	    now - priv->last_scale_down >= ng_pppoe_lb_governor_scale_down_interval &&
	    priv->num_workers_active > ng_pppoe_lb_governor_min_workers &&
	    priv->pending_removal_id < 0) {

		/* Find worker with fewest sessions to drain */
		int min_sess = INT_MAX;
		int drain_worker = -1;
		for (i = 0; i < priv->num_workers; i++) {
			if (priv->worker_states[priv->workers[i].worker_id] == NG_PPPOE_LB_WORKER_ACTIVE &&
			    (int)priv->workers[i].sessions < min_sess) {
				min_sess = priv->workers[i].sessions;
				drain_worker = i;
			}
		}

		if (drain_worker >= 0) {
			struct ng_pppoe_lb_worker *worker = &priv->workers[drain_worker];
			priv->worker_states[worker->worker_id] = NG_PPPOE_LB_WORKER_DRAINING;
			worker->drain_start_time = now;
			priv->num_workers_active--;
			priv->num_workers_draining++;
			priv->pending_removal_id = worker->worker_id;
			priv->last_scale_down = now;
			ng_pppoe_lb_governor_last_decision = NG_PPPOE_LB_GOV_DECISION_SCALE_DOWN;
			ng_pppoe_lb_governor_last_reason = NG_PPPOE_LB_GOV_REASON_CPU_LOW;
			if (priv->debug_level >= 1) {
				printf("ng_pppoe_lb: governor marking worker %d for drain "
				    "(CPU=%d%%, sess=%u, drain_timeout=%ds)\n",
				    worker->worker_id, avg_cpu, worker->sessions,
				    ng_pppoe_lb_governor_drain_timeout);
			}
		}
	}

	mtx_unlock(&priv->mtx);

out:
	/* Schedule next tick */
	callout_reset(&priv->governor_callout, hz, ng_pppoe_lb_governor_tick, priv);
}
