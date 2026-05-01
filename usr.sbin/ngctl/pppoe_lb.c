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

#include <err.h>
#include <netgraph.h>
#include <sys/sysctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "ngctl.h"

/* PPPoE Load Balancer constants and structures (userland copy) */
#define NGM_PPPOE_LB_COOKIE		1089893073

#define NG_PPPOE_LB_NODE_TYPE		"pppoe_lb"

/* Worker states */
#define NG_PPPOE_LB_WORKER_ACTIVE		0
#define NG_PPPOE_LB_WORKER_DRAINING		1
#define NG_PPPOE_LB_WORKER_PENDING_REMOVAL	2

/* Governor decisions */
#define NG_PPPOE_LB_GOV_DECISION_NONE		0
#define NG_PPPOE_LB_GOV_DECISION_SCALE_UP	1
#define NG_PPPOE_LB_GOV_DECISION_SCALE_DOWN	2
#define NG_PPPOE_LB_GOV_DECISION_CANCEL_DOWN	3

/* Governor reasons */
#define NG_PPPOE_LB_GOV_REASON_NONE		0
#define NG_PPPOE_LB_GOV_REASON_CPU_HIGH		1
#define NG_PPPOE_LB_GOV_REASON_CPU_LOW		2
#define NG_PPPOE_LB_GOV_REASON_SESS_HIGH	3
#define NG_PPPOE_LB_GOV_REASON_SESS_LOW		4

/* Message types */
enum {
	NGM_PPPOE_LB_ADD_WORKER = 1,
	NGM_PPPOE_LB_REMOVE_WORKER,
	NGM_PPPOE_LB_SET_CONFIG,
	NGM_PPPOE_LB_GET_STATS,
	NGM_PPPOE_LB_GET_MAP,
	NGM_PPPOE_LB_SET_WORKER_STATE,
	NGM_PPPOE_LB_GET_WORKER_INFO,
	NGM_PPPOE_LB_TRIGGER_SCALE,
};

/* Load balancing algorithms */
#define NG_PPPOE_LB_ALGO_ROUND_ROBIN	0
#define NG_PPPOE_LB_ALGO_HASH		1
#define NG_PPPOE_LB_ALGO_LEAST_LOADED	2

/* Structures */
struct ng_pppoe_lb_config {
	uint32_t	algorithm;
	uint32_t	max_workers;
	uint32_t	debug_level;
};

struct ng_pppoe_lb_stats {
	uint64_t	packets_in;
	uint64_t	packets_out;
	uint64_t	sessions_created;
	uint64_t	sessions_destroyed;
	uint32_t	num_workers;
	uint32_t	num_workers_active;
	uint32_t	algorithm;
	uint32_t	map_count;
};

struct ng_pppoe_lb_map_entry {
	uint16_t	session_id;
	uint16_t	worker_index;
	uint32_t	last_activity;
};

struct ng_pppoe_lb_map {
	uint32_t	count;
	uint32_t	max_entries;
	struct ng_pppoe_lb_map_entry entries[];
};

struct ng_pppoe_lb_worker_info {
	int32_t		worker_id;
	uint32_t	state;
	uint32_t	sessions;
	uint32_t	last_activity;
	uint32_t	uptime;
	uint64_t	packets_in;
	uint64_t	packets_out;
	uint64_t	bytes_in;
	uint64_t	bytes_out;
};

struct ng_pppoe_lb_set_worker_state {
	int32_t		worker_id;
	uint32_t	state;
};

struct ng_pppoe_lb_get_worker_info {
	int32_t		worker_id;
};

struct ng_pppoe_lb_trigger_scale {
	uint32_t	direction;
};

static int PppoeLbShowCmd(int ac, char **av);
static int PppoeLbConfigCmd(int ac, char **av);
static int PppoeLbStatsCmd(int ac, char **av);
static int PppoeLbMapCmd(int ac, char **av);
static int PppoeLbWorkersCmd(int ac, char **av);
static int PppoeLbWorkerCmd(int ac, char **av);
static int PppoeLbGovernorCmd(int ac, char **av);
static int PppoeLbTriggerScaleCmd(int ac, char **av);

const struct ngcmd pppoe_lb_show_cmd = {
	PppoeLbShowCmd,
	"pppoe_lb show <path>",
	"Show PPPoE load balancer information",
	"Displays statistics, worker count, algorithm, and session map for the specified pppoe_lb node.",
	{ "pppoe_lb info" }
};

const struct ngcmd pppoe_lb_config_cmd = {
	PppoeLbConfigCmd,
	"pppoe_lb config <path> [algorithm <0|1|2>] [max_workers <n>] [debug <level>]",
	"Configure PPPoE load balancer",
	"Sets the load balancing algorithm (0=round-robin, 1=hash, 2=least-loaded),\n"
	"maximum workers for governor, and debug level.",
	{ "pppoe_lb set" }
};

const struct ngcmd pppoe_lb_stats_cmd = {
	PppoeLbStatsCmd,
	"pppoe_lb stats <path>",
	"Show PPPoE load balancer statistics",
	"Displays packet counts, session counts, and worker statistics.",
	{ "pppoe_lb statistics" }
};

const struct ngcmd pppoe_lb_map_cmd = {
	PppoeLbMapCmd,
	"pppoe_lb map <path>",
	"Show PPPoE session-to-worker mapping",
	"Displays the current session ID to worker node assignments.",
	{ "pppoe_lb sessions" }
};

const struct ngcmd pppoe_lb_workers_cmd = {
	PppoeLbWorkersCmd,
	"pppoe_lb workers <path>",
	"Show all workers and their states",
	"Displays detailed information for all workers including state, sessions, and uptime.",
	{ "pppoe_lb showworkers" }
};

const struct ngcmd pppoe_lb_worker_cmd = {
	PppoeLbWorkerCmd,
	"pppoe_lb worker <path> <id> [state <0|1|2>]",
	"Get or set worker state",
	"Get worker info or set worker state: 0=ACTIVE, 1=DRAINING, 2=PENDING_REMOVAL.",
	{ "pppoe_lb wstate" }
};

const struct ngcmd pppoe_lb_governor_cmd = {
	PppoeLbGovernorCmd,
	"pppoe_lb governor <path> [enable|disable]",
	"Enable or disable the CPU governor",
	"Enable or disable automatic worker scaling based on CPU load.",
	{ "pppoe_lb gov" }
};

const struct ngcmd pppoe_lb_trigger_scale_cmd = {
	PppoeLbTriggerScaleCmd,
	"pppoe_lb trigger <path> [up|down]",
	"Trigger a scale event for testing",
	"Forces a scale up or scale down decision (for testing governor logic).",
	{ "pppoe_lb scale" }
};

static int
PppoeLbShowCmd(int ac, char **av)
{
	char *path;
	struct ng_mesg *resp = NULL;
	u_char rbuf[sizeof(struct ng_mesg) + sizeof(struct ng_pppoe_lb_stats)];
	int ch;

	/* Get options */
	optreset = 1;
	optind = 1;
	while ((ch = getopt(ac, av, "")) != -1) {
		switch (ch) {
		default:
			return (CMDRTN_USAGE);
		}
	}
	ac -= optind;
	av += optind;

	/* Get arguments */
	switch (ac) {
	case 1:
		path = av[0];
		break;
	default:
		return (CMDRTN_USAGE);
	}

	/* Get statistics */
	if (NgSendMsg(csock, path, NGM_PPPOE_LB_COOKIE,
	    NGM_PPPOE_LB_GET_STATS, NULL, 0) < 0) {
		warn("send stats msg");
		return (CMDRTN_ERROR);
	}

	if (NgRecvMsg(csock, resp, sizeof(rbuf), NULL) < 0) {
		warn("recv stats msg");
		return (CMDRTN_ERROR);
	}

	struct ng_pppoe_lb_stats *stats = (struct ng_pppoe_lb_stats *)resp->data;

	printf("PPPoE Load Balancer Statistics:\n");
	printf("  Workers:          %u (active: %u)\n",
	    stats->num_workers, stats->num_workers_active);
	printf("  Algorithm:        ");
	switch (stats->algorithm) {
	case NG_PPPOE_LB_ALGO_ROUND_ROBIN:
		printf("round-robin\n");
		break;
	case NG_PPPOE_LB_ALGO_HASH:
		printf("hash-based\n");
		break;
	case NG_PPPOE_LB_ALGO_LEAST_LOADED:
		printf("least-loaded\n");
		break;
	default:
		printf("unknown (%u)\n", stats->algorithm);
	}
	printf("  Packets in:       %lu\n", (u_long)stats->packets_in);
	printf("  Packets out:      %lu\n", (u_long)stats->packets_out);
	printf("  Sessions created: %lu\n", (u_long)stats->sessions_created);
	printf("  Sessions destroyed: %lu\n", (u_long)stats->sessions_destroyed);
	printf("  Map entries:      %u\n", stats->map_count);

	free(resp);
	return (CMDRTN_OK);
}

static int
PppoeLbConfigCmd(int ac, char **av)
{
	char *path;
	struct ng_pppoe_lb_config cfg;
	int ch;
	int set_algorithm = 0, set_max_workers = 0, set_debug = 0;

	/* Initialize with defaults */
	memset(&cfg, 0, sizeof(cfg));

	/* Get options */
	optreset = 1;
	optind = 1;
	while ((ch = getopt(ac, av, "")) != -1) {
		switch (ch) {
		default:
			return (CMDRTN_USAGE);
		}
	}
	ac -= optind;
	av += optind;

	/* Get arguments */
	if (ac < 1)
		return (CMDRTN_USAGE);

	path = av[0];
	ac--;
	av++;

	/* Parse optional parameters */
	while (ac >= 2) {
		if (strcmp(av[0], "algorithm") == 0) {
			cfg.algorithm = atoi(av[1]);
			set_algorithm = 1;
			ac -= 2;
			av += 2;
		} else if (strcmp(av[0], "max_workers") == 0) {
			cfg.max_workers = atoi(av[1]);
			set_max_workers = 1;
			ac -= 2;
			av += 2;
		} else if (strcmp(av[0], "debug") == 0) {
			cfg.debug_level = atoi(av[1]);
			set_debug = 1;
			ac -= 2;
			av += 2;
		} else {
			return (CMDRTN_USAGE);
		}
	}

	if (!set_algorithm && !set_max_workers && !set_debug) {
		warnx("No configuration parameters specified");
		return (CMDRTN_USAGE);
	}

	/* Send configuration message */
	if (NgSendMsg(csock, path, NGM_PPPOE_LB_COOKIE,
	    NGM_PPPOE_LB_SET_CONFIG, &cfg, sizeof(cfg)) < 0) {
		warn("send config msg");
		return (CMDRTN_ERROR);
	}

	printf("Configuration updated successfully\n");
	return (CMDRTN_OK);
}

static int
PppoeLbStatsCmd(int ac, char **av)
{
	char *path;
	struct ng_mesg *resp = NULL;
	u_char rbuf[sizeof(struct ng_mesg) + sizeof(struct ng_pppoe_lb_stats)];
	int ch;

	/* Get options */
	optreset = 1;
	optind = 1;
	while ((ch = getopt(ac, av, "")) != -1) {
		switch (ch) {
		default:
			return (CMDRTN_USAGE);
		}
	}
	ac -= optind;
	av += optind;

	/* Get arguments */
	switch (ac) {
	case 1:
		path = av[0];
		break;
	default:
		return (CMDRTN_USAGE);
	}

	/* Get statistics */
	if (NgSendMsg(csock, path, NGM_PPPOE_LB_COOKIE,
	    NGM_PPPOE_LB_GET_STATS, NULL, 0) < 0) {
		warn("send stats msg");
		return (CMDRTN_ERROR);
	}

	if (NgRecvMsg(csock, resp, sizeof(rbuf), NULL) < 0) {
		warn("recv stats msg");
		return (CMDRTN_ERROR);
	}

	struct ng_pppoe_lb_stats *stats = (struct ng_pppoe_lb_stats *)resp->data;

	printf("PPPoE Load Balancer Statistics:\n");
	printf("  Workers:          %u (active: %u)\n",
	    stats->num_workers, stats->num_workers_active);
	printf("  Algorithm:        ");
	switch (stats->algorithm) {
	case NG_PPPOE_LB_ALGO_ROUND_ROBIN:
		printf("round-robin\n");
		break;
	case NG_PPPOE_LB_ALGO_HASH:
		printf("hash-based\n");
		break;
	case NG_PPPOE_LB_ALGO_LEAST_LOADED:
		printf("least-loaded\n");
		break;
	default:
		printf("unknown (%u)\n", stats->algorithm);
	}
	printf("  Packets in:       %lu\n", (u_long)stats->packets_in);
	printf("  Packets out:      %lu\n", (u_long)stats->packets_out);
	printf("  Sessions created: %lu\n", (u_long)stats->sessions_created);
	printf("  Sessions destroyed: %lu\n", (u_long)stats->sessions_destroyed);
	printf("  Map entries:      %u\n", stats->map_count);

	free(resp);
	return (CMDRTN_OK);
}

static int
PppoeLbMapCmd(int ac, char **av)
{
	char *path;
	struct ng_mesg *resp = NULL;
	struct ng_pppoe_lb_map *map;
	u_char rbuf[4096];  /* Buffer for up to ~256 session entries */
	int ch;
	u_int i;

	/* Get options */
	optreset = 1;
	optind = 1;
	while ((ch = getopt(ac, av, "")) != -1) {
		switch (ch) {
		default:
			return (CMDRTN_USAGE);
		}
	}
	ac -= optind;
	av += optind;

	/* Get arguments */
	switch (ac) {
	case 1:
		path = av[0];
		break;
	default:
		return (CMDRTN_USAGE);
	}

	/* Prepare map request */
	map = (struct ng_pppoe_lb_map *)rbuf;
	map->max_entries = (sizeof(rbuf) - sizeof(struct ng_pppoe_lb_map)) /
	    sizeof(struct ng_pppoe_lb_map_entry);

	/* Get session map */
	if (NgSendMsg(csock, path, NGM_PPPOE_LB_COOKIE,
	    NGM_PPPOE_LB_GET_MAP, map, sizeof(struct ng_pppoe_lb_map)) < 0) {
		warn("send map msg");
		return (CMDRTN_ERROR);
	}

	if (NgRecvMsg(csock, resp, sizeof(rbuf), NULL) < 0) {
		warn("recv map msg");
		return (CMDRTN_ERROR);
	}

	map = (struct ng_pppoe_lb_map *)resp->data;

	printf("PPPoE Session-to-Worker Mapping:\n");
	printf("  %-10s %-10s %-15s\n", "Session ID", "Worker", "Last Activity");
	printf("  %-10s %-10s %-15s\n", "----------", "------", "-------------");

	if (map->count == 0) {
		printf("  (no active sessions)\n");
	} else {
		for (i = 0; i < map->count; i++) {
			struct ng_pppoe_lb_map_entry *entry = &map->entries[i];
			printf("  %-10u %-10u %-15u\n",
			    entry->session_id,
			    entry->worker_index,
			    entry->last_activity);
		}
	}

	printf("\n  Total sessions: %u\n", map->count);

	free(resp);
	return (CMDRTN_OK);
}

static const char *
worker_state_str(uint32_t state)
{
	switch (state) {
	case NG_PPPOE_LB_WORKER_ACTIVE:
		return "ACTIVE";
	case NG_PPPOE_LB_WORKER_DRAINING:
		return "DRAINING";
	case NG_PPPOE_LB_WORKER_PENDING_REMOVAL:
		return "PENDING_REMOVAL";
	default:
		return "UNKNOWN";
	}
}

static const char *
governor_decision_str(int decision)
{
	switch (decision) {
	case NG_PPPOE_LB_GOV_DECISION_NONE:
		return "none";
	case NG_PPPOE_LB_GOV_DECISION_SCALE_UP:
		return "scale_up";
	case NG_PPPOE_LB_GOV_DECISION_SCALE_DOWN:
		return "scale_down";
	case NG_PPPOE_LB_GOV_DECISION_CANCEL_DOWN:
		return "cancel_down";
	default:
		return "unknown";
	}
}

static const char *
governor_reason_str(int reason)
{
	switch (reason) {
	case NG_PPPOE_LB_GOV_REASON_NONE:
		return "none";
	case NG_PPPOE_LB_GOV_REASON_CPU_HIGH:
		return "cpu_high";
	case NG_PPPOE_LB_GOV_REASON_CPU_LOW:
		return "cpu_low";
	case NG_PPPOE_LB_GOV_REASON_SESS_HIGH:
		return "sessions_high";
	case NG_PPPOE_LB_GOV_REASON_SESS_LOW:
		return "sessions_low";
	default:
		return "unknown";
	}
}

/* Helper to read sysctl values in userland */
static int
read_sysctl_int(const char *name, int *value)
{
	size_t len = sizeof(*value);
	return sysctlbyname(name, value, &len, NULL, 0);
}

static int
PppoeLbWorkersCmd(int ac, char **av)
{
	char *path;
	struct ng_mesg *resp = NULL;
	struct ng_pppoe_lb_get_worker_info req;
	struct ng_pppoe_lb_worker_info *info;
	u_char rbuf[sizeof(struct ng_mesg) + sizeof(struct ng_pppoe_lb_worker_info)];
	int ch;
	int i, num_workers;

	/* Get options */
	optreset = 1;
	optind = 1;
	while ((ch = getopt(ac, av, "")) != -1) {
		switch (ch) {
		default:
			return (CMDRTN_USAGE);
		}
	}
	ac -= optind;
	av += optind;

	/* Get arguments */
	switch (ac) {
	case 1:
		path = av[0];
		break;
	default:
		return (CMDRTN_USAGE);
	}

	/* First get stats to find number of workers */
	if (NgSendMsg(csock, path, NGM_PPPOE_LB_COOKIE,
	    NGM_PPPOE_LB_GET_STATS, NULL, 0) < 0) {
		warn("send stats msg");
		return (CMDRTN_ERROR);
	}

	if (NgRecvMsg(csock, resp, sizeof(rbuf), NULL) < 0) {
		warn("recv stats msg");
		return (CMDRTN_ERROR);
	}

	struct ng_pppoe_lb_stats *stats = (struct ng_pppoe_lb_stats *)resp->data;
	num_workers = stats->num_workers;
	free(resp);

	if (num_workers == 0) {
		printf("PPPoE Load Balancer Workers:\n");
		printf("  No workers configured\n");
		return (CMDRTN_OK);
	}

	printf("PPPoE Load Balancer Workers:\n");
	printf("\n");
	printf("  %-6s %-15s %-10s %-10s %-10s %-15s\n",
	    "ID", "State", "Sessions", "Bytes In", "Bytes Out", "Last Activity");
	printf("  %-6s %-15s %-10s %-10s %-10s %-15s\n",
	    "------", "---------------", "----------", "----------", "----------", "---------------");

	for (i = 0; i < num_workers; i++) {
		req.worker_id = i;

		if (NgSendMsg(csock, path, NGM_PPPOE_LB_COOKIE,
		    NGM_PPPOE_LB_GET_WORKER_INFO, &req, sizeof(req)) < 0) {
			warn("send worker info msg for worker %d", i);
			continue;
		}

		if (NgRecvMsg(csock, resp, sizeof(rbuf), NULL) < 0) {
			warn("recv worker info msg");
			continue;
		}

		info = (struct ng_pppoe_lb_worker_info *)resp->data;
		printf("  %-6d %-15s %-10u %-10lu %-10lu %-15u\n",
		    info->worker_id,
		    worker_state_str(info->state),
		    info->sessions,
		    (u_long)info->bytes_in,
		    (u_long)info->bytes_out,
		    info->last_activity);

		free(resp);
	}

	return (CMDRTN_OK);
}

static int
PppoeLbWorkerCmd(int ac, char **av)
{
	char *path;
	int worker_id = -1;
	int new_state = -1;
	struct ng_pppoe_lb_set_worker_state req;
	int ch;

	/* Get options */
	optreset = 1;
	optind = 1;
	while ((ch = getopt(ac, av, "")) != -1) {
		switch (ch) {
		default:
			return (CMDRTN_USAGE);
		}
	}
	ac -= optind;
	av += optind;

	/* Get arguments */
	if (ac < 2)
		return (CMDRTN_USAGE);

	path = av[0];
	worker_id = atoi(av[1]);
	ac -= 2;
	av += 2;

	/* Check for state change */
	if (ac >= 2 && strcmp(av[0], "state") == 0) {
		new_state = atoi(av[1]);
		if (new_state < 0 || new_state > 2) {
			warnx("Invalid state: %d (valid: 0=ACTIVE, 1=DRAINING, 2=PENDING_REMOVAL)", new_state);
			return (CMDRTN_USAGE);
		}
	}

	if (new_state >= 0) {
		/* Set worker state */
		req.worker_id = worker_id;
		req.state = new_state;

		if (NgSendMsg(csock, path, NGM_PPPOE_LB_COOKIE,
		    NGM_PPPOE_LB_SET_WORKER_STATE, &req, sizeof(req)) < 0) {
			warn("send set worker state msg");
			return (CMDRTN_ERROR);
		}

		printf("Worker %d state set to %s\n", worker_id, worker_state_str(new_state));
		return (CMDRTN_OK);
	}

	/* Get worker info */
	struct ng_mesg *resp = NULL;
	struct ng_pppoe_lb_get_worker_info greq;
	struct ng_pppoe_lb_worker_info *info;
	u_char rbuf[sizeof(struct ng_mesg) + sizeof(struct ng_pppoe_lb_worker_info)];

	greq.worker_id = worker_id;

	if (NgSendMsg(csock, path, NGM_PPPOE_LB_COOKIE,
	    NGM_PPPOE_LB_GET_WORKER_INFO, &greq, sizeof(greq)) < 0) {
		warn("send worker info msg");
		return (CMDRTN_ERROR);
	}

	if (NgRecvMsg(csock, resp, sizeof(rbuf), NULL) < 0) {
		warn("recv worker info msg");
		return (CMDRTN_ERROR);
	}

	info = (struct ng_pppoe_lb_worker_info *)resp->data;

	printf("Worker %d Information:\n", worker_id);
	printf("  State:        %s (%u)\n", worker_state_str(info->state), info->state);
	printf("  Sessions:     %u\n", info->sessions);
	printf("  Last Activity: %u\n", info->last_activity);
	printf("  Uptime:       %u seconds\n", info->uptime);
	printf("  Bytes In:     %lu\n", (u_long)info->bytes_in);
	printf("  Bytes Out:    %lu\n", (u_long)info->bytes_out);

	free(resp);
	return (CMDRTN_OK);
}

static int
PppoeLbGovernorCmd(int ac, char **av)
{
	char *path;
	int ch;
	int enabled = 0, mode = 0, min_workers = 1, max_workers = 0, cpu_threshold = 80, cpu_low_threshold = 30;
	int current_workers = 0, active_workers = 0, draining_workers = 0, pending_removals = 0;
	int last_decision = 0, last_reason = 0;

	/* Get options */
	optreset = 1;
	optind = 1;
	while ((ch = getopt(ac, av, "")) != -1) {
		switch (ch) {
		default:
			return (CMDRTN_USAGE);
		}
	}
	ac -= optind;
	av += optind;

	/* Get arguments */
	if (ac < 1)
		return (CMDRTN_USAGE);

	path = av[0];
	(void)path;  /* Path is informational, sysctls are global */

	/* Get governor status via sysctl */
	read_sysctl_int("net.graph.pppoe_lb.governor.enabled", &enabled);
	read_sysctl_int("net.graph.pppoe_lb.governor.mode", &mode);
	read_sysctl_int("net.graph.pppoe_lb.governor.min_workers", &min_workers);
	read_sysctl_int("net.graph.pppoe_lb.governor.max_workers", &max_workers);
	read_sysctl_int("net.graph.pppoe_lb.governor.cpu_threshold", &cpu_threshold);
	read_sysctl_int("net.graph.pppoe_lb.governor.cpu_low_threshold", &cpu_low_threshold);
	read_sysctl_int("net.graph.pppoe_lb.governor.current_workers", &current_workers);
	read_sysctl_int("net.graph.pppoe_lb.governor.active_workers", &active_workers);
	read_sysctl_int("net.graph.pppoe_lb.governor.draining_workers", &draining_workers);
	read_sysctl_int("net.graph.pppoe_lb.governor.pending_removals", &pending_removals);
	read_sysctl_int("net.graph.pppoe_lb.governor.last_decision", &last_decision);
	read_sysctl_int("net.graph.pppoe_lb.governor.last_reason", &last_reason);

	printf("PPPoE Governor Status:\n");
	printf("\n");
	printf("  Configuration:\n");
	printf("    Enabled:        %s\n", enabled ? "Yes" : "No");
	printf("    Mode:           %s\n", mode ? "Auto" : "Manual");
	printf("    Min Workers:    %d\n", min_workers);
	printf("    Max Workers:    %s\n", max_workers == 0 ? "auto (mp_ncpus)" : "manual");
	printf("    CPU Threshold:  %d%%\n", cpu_threshold);
	printf("    CPU Low:        %d%%\n", cpu_low_threshold);
	printf("\n");
	printf("  Current State:\n");
	printf("    Current Workers:  %d\n", current_workers);
	printf("    Active Workers:   %d\n", active_workers);
	printf("    Draining Workers: %d\n", draining_workers);
	printf("    Pending Removals: %d\n", pending_removals);
	printf("\n");
	printf("  Last Decision:    %s (%s)\n",
	    governor_decision_str(last_decision),
	    governor_reason_str(last_reason));
	printf("\n");
	printf("  To configure, use sysctl:\n");
	printf("    sysctl net.graph.pppoe_lb.governor.enabled=1    # Enable\n");
	printf("    sysctl net.graph.pppoe_lb.governor.enabled=0    # Disable\n");
	printf("    sysctl net.graph.pppoe_lb.governor.cpu_threshold=80\n");

	return (CMDRTN_OK);
}

static int
PppoeLbTriggerScaleCmd(int ac, char **av)
{
	char *path;
	char *direction_str = NULL;
	struct ng_pppoe_lb_trigger_scale req;
	int ch;

	/* Get options */
	optreset = 1;
	optind = 1;
	while ((ch = getopt(ac, av, "")) != -1) {
		switch (ch) {
		default:
			return (CMDRTN_USAGE);
		}
	}
	ac -= optind;
	av += optind;

	/* Get arguments */
	if (ac < 2)
		return (CMDRTN_USAGE);

	path = av[0];
	direction_str = av[1];

	if (strcmp(direction_str, "up") == 0) {
		req.direction = 0;
	} else if (strcmp(direction_str, "down") == 0) {
		req.direction = 1;
	} else {
		warnx("Invalid direction: %s (use 'up' or 'down')", direction_str);
		return (CMDRTN_USAGE);
	}

	if (NgSendMsg(csock, path, NGM_PPPOE_LB_COOKIE,
	    NGM_PPPOE_LB_TRIGGER_SCALE, &req, sizeof(req)) < 0) {
		warn("send trigger scale msg");
		return (CMDRTN_ERROR);
	}

	printf("Scale %s triggered\n", direction_str);
	return (CMDRTN_OK);
}
