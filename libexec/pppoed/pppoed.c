/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 1999-2001 Brian Somers <brian@Awfulhak.org>
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
 */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netgraph.h>
#include <net/ethernet.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netgraph/ng_ether.h>
#include <netgraph/ng_message.h>
#include <netgraph/ng_pppoe.h>
#include <netgraph/ng_pppoe_lb.h>
#include <netgraph/ng_socket.h>

#include <errno.h>
#include <paths.h>
#include <signal.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <sys/fcntl.h>
#ifndef NOKLDLOAD
#include <sys/linker.h>
#include <sys/module.h>
#endif
#include <sys/uio.h>
#include <sys/wait.h>
#include <syslog.h>
#include <termios.h>
#include <unistd.h>

/* Governor thread support */
#include <pthread.h>
#include <time.h>


#define	DEFAULT_EXEC_PREFIX	"exec /usr/sbin/ppp -direct "

/*
 * Local definitions for pppoe_lb structures (userland copy of kernel headers)
 * These mirror the kernel definitions in sys/netgraph/ng_pppoe_lb.h
 */

/* Worker states */
#define NG_PPPOE_LB_WORKER_ACTIVE		0
#define NG_PPPOE_LB_WORKER_DRAINING		1
#define NG_PPPOE_LB_WORKER_PENDING_REMOVAL	2

/* Control message cookie */
#define NGM_PPPOE_LB_COOKIE	1089893073

/* Set worker state message */
struct ng_pppoe_lb_set_worker_state {
	int32_t		worker_id;		/* Worker index */
	uint32_t	state;			/* New state */
};

/* Control messages */
enum {
	NGM_PPPOE_LB_SET_WORKER_STATE = 6,	/* Set individual worker state */
};

#define	HISMACADDR		"HISMACADDR"
#define	SESSION_ID		"SESSION_ID"

static void nglogx(const char *, ...) __printflike(1, 2);

static int ReceivedSignal;

/* Governor state */
struct governor_config {
	int	enabled;		/* Governor enabled */
	int	mode;			/* 0=manual, 1=auto */
	int	min_workers;		/* Minimum workers */
	int	max_workers;		/* Maximum workers (0=auto/mp_ncpus) */
	int	poll_interval;		/* Poll interval in seconds */
	int	cpu_threshold;		/* Scale up at CPU % */
	int	cpu_low_threshold;	/* Scale down at CPU % */
	int	sessions_per_worker;	/* Target sessions per worker */
	int	scale_up_interval;	/* Min seconds between scale up */
	int	scale_down_interval;	/* Min seconds between scale down */
	int	drain_timeout;		/* Seconds to wait for drain */
};

struct governor_state {
	int		current_workers;	/* Current worker count */
	int		active_workers;		/* Workers in ACTIVE state */
	int		draining_workers;	/* Workers in DRAINING state */
	int		pending_removals;	/* Workers pending removal */
	int		total_sessions;		/* Total active sessions */
	int		cpu_usage;		/* Current CPU usage % */
	int		last_decision;		/* Last scaling decision */
	int		last_reason;		/* Reason for last decision */
	time_t		last_scale_up;		/* Timestamp of last scale up */
	time_t		last_scale_down;	/* Timestamp of last scale down */
	time_t		last_poll;		/* Timestamp of last poll */
	pthread_mutex_t	lock;		/* Mutex for state access */
};

static struct governor_config governor_config = {
	.enabled = 0,
	.mode = 0,			/* Manual by default */
	.min_workers = 1,
	.max_workers = 0,		/* Auto (mp_ncpus) */
	.poll_interval = 5,
	.cpu_threshold = 80,
	.cpu_low_threshold = 30,
	.sessions_per_worker = 500,
	.scale_up_interval = 10,
	.scale_down_interval = 60,
	.drain_timeout = 30
};

static struct governor_state governor_state;
static pthread_t governor_thread;
static int governor_running = 0;
static int cs_fd = -1;		/* Netgraph socket for governor */
static char lb_path[64];	/* Path to load balancer node */

/* Decision/reason strings */
static const char *decision_str[] = {
	"none", "scale_up", "scale_down", "cancel_drain", "no_action"
};

static const char *reason_str[] = {
	"none", "cpu_high", "cpu_low", "sessions_high", "sessions_low"
};

static int
usage(const char *prog)
{
  fprintf(stderr, "usage: %s [-Fd] [-P pidfile] [-a name] [-e exec | -l label]"
          " [-n ngdebug] [-p provider] [-L] [-w workers] [-A algorithm]"
          " [-G max_workers] [-M min_workers] [-I poll_interval]"
          " [-c cpu_threshold] [-C cpu_low_threshold] interface\n", prog);
  fprintf(stderr, "  -L              Enable load balancer (multithreaded mode)\n");
  fprintf(stderr, "  -w workers      Number of worker nodes (default: 1)\n");
  fprintf(stderr, "  -A algorithm    Load balancing algorithm: 0=round-robin, 1=hash, 2=least-loaded\n");
  fprintf(stderr, "  -G max_workers  Maximum workers for governor (0=auto/mp_ncpus)\n");
  fprintf(stderr, "  -M min_workers  Minimum workers for governor (default: 1)\n");
  fprintf(stderr, "  -I poll_interval Governor poll interval in seconds (default: 5)\n");
  fprintf(stderr, "  -c cpu_threshold    CPU threshold for scale up (default: 80)\n");
  fprintf(stderr, "  -C cpu_low_threshold CPU low threshold for scale down (default: 30)\n");
  return EX_USAGE;
}

static void
Farewell(int sig)
{
  ReceivedSignal = sig;
}

/*
 * Governor thread functions
 */

/* Read sysctl integer value */
static int
read_sysctl_int(const char *name, int *value)
{
	size_t len = sizeof(*value);
	return sysctlbyname(name, value, &len, NULL, 0);
}

/* Read CPU core count for auto max_workers */
static int
get_cpu_count(void)
{
	int ncpu;
	size_t len = sizeof(ncpu);

	if (sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0) == -1)
		return (1);  /* Fallback to 1 */
	return (ncpu);
}

/* Get effective max_workers (resolve 0 to mp_ncpus) */
static int
get_max_workers(void)
{
	if (governor_config.max_workers == 0)
		return (get_cpu_count());
	return (governor_config.max_workers);
}

/* Update governor state from kernel sysctls */
static void
governor_update_state(void)
{
	pthread_mutex_lock(&governor_state.lock);

	read_sysctl_int("net.graph.pppoe_lb.governor.current_workers",
	    &governor_state.current_workers);
	read_sysctl_int("net.graph.pppoe_lb.governor.active_workers",
	    &governor_state.active_workers);
	read_sysctl_int("net.graph.pppoe_lb.governor.draining_workers",
	    &governor_state.draining_workers);
	read_sysctl_int("net.graph.pppoe_lb.governor.pending_removals",
	    &governor_state.pending_removals);
	read_sysctl_int("net.graph.pppoe_lb.governor.total_sessions",
	    &governor_state.total_sessions);
	read_sysctl_int("net.graph.pppoe_lb.governor.cpu_usage",
	    &governor_state.cpu_usage);

	governor_state.last_poll = time(NULL);

	pthread_mutex_unlock(&governor_state.lock);
}

/* Add a new worker via netgraph */
static int
governor_add_worker(int worker_id)
{
	struct ngm_mkpeer mkp;
	char worker_path[64];

	if (cs_fd < 0) {
		syslog(LOG_ERR, "governor: no netgraph socket");
		return (-1);
	}

	/* Create worker path */
	snprintf(worker_path, sizeof(worker_path), "%s%d", NG_PPPOE_LB_HOOK_WORKER_BASE, worker_id);

	/* Create worker peer node */
	snprintf(mkp.type, sizeof(mkp.type), "%s", NG_PPPOE_NODE_TYPE);
	snprintf(mkp.ourhook, sizeof(mkp.ourhook), "%s", worker_path);
	snprintf(mkp.peerhook, sizeof(mkp.peerhook), "%s", NG_PPPOE_HOOK_ETHERNET);

	if (NgSendMsg(cs_fd, lb_path, NGM_GENERIC_COOKIE,
	    NGM_MKPEER, &mkp, sizeof(mkp)) < 0) {
		syslog(LOG_ERR, "governor: failed to create worker %d: %m", worker_id);
		return (-1);
	}

	syslog(LOG_INFO, "governor: created worker %d", worker_id);
	return (0);
}

/* Remove a worker (mark as PENDING_REMOVAL, kernel handles actual removal) */
static int
governor_remove_worker(int worker_id)
{
	struct ng_pppoe_lb_set_worker_state msg;

	if (cs_fd < 0) {
		syslog(LOG_ERR, "governor: no netgraph socket");
		return (-1);
	}

	/* Send worker state change message - mark for draining */
	msg.worker_id = worker_id;
	msg.state = NG_PPPOE_LB_WORKER_DRAINING;

	if (NgSendMsg(cs_fd, lb_path, NGM_PPPOE_LB_COOKIE,
	    NGM_PPPOE_LB_SET_WORKER_STATE, &msg, sizeof(msg)) < 0) {
		syslog(LOG_ERR, "governor: failed to drain worker %d: %m", worker_id);
		return (-1);
	}

	syslog(LOG_INFO, "governor: marked worker %d for drain", worker_id);
	return (0);
}

/* Cancel drain for a worker (change mind) */
static int
governor_cancel_drain(int worker_id)
{
	struct ng_pppoe_lb_set_worker_state msg;

	if (cs_fd < 0) {
		syslog(LOG_ERR, "governor: no netgraph socket");
		return (-1);
	}

	/* Send cancel drain message - back to active */
	msg.worker_id = worker_id;
	msg.state = NG_PPPOE_LB_WORKER_ACTIVE;

	if (NgSendMsg(cs_fd, lb_path, NGM_PPPOE_LB_COOKIE,
	    NGM_PPPOE_LB_SET_WORKER_STATE, &msg, sizeof(msg)) < 0) {
		syslog(LOG_ERR, "governor: failed to cancel drain for worker %d: %m", worker_id);
		return (-1);
	}

	syslog(LOG_INFO, "governor: canceled drain for worker %d (change mind!)", worker_id);
	return (0);
}

/* Find worker with fewest sessions for removal */
static int
governor_find_worker_to_remove(void)
{
	int worker_id, min_sessions, sessions;
	int i, max_workers;

	/* First, check if there are pending removals we can cancel */
	pthread_mutex_lock(&governor_state.lock);
	max_workers = governor_state.current_workers;
	pthread_mutex_unlock(&governor_state.lock);

	/* Scan workers via sysctl for session counts */
	for (i = 0; i < max_workers; i++) {
		char name[64];
		int state = 0;

		snprintf(name, sizeof(name),
		    "net.graph.pppoe_lb.workers.%d.state", i);
		if (read_sysctl_int(name, &state) == -1)
			continue;

		/* Skip non-draining workers for now */
		if (state != NG_PPPOE_LB_WORKER_DRAINING)
			continue;

		/* Found a draining worker - return its ID */
		return (i);
	}

	return (-1);  /* No worker to remove */
}

/* Check if there are draining workers that can be cancelled */
static int
governor_find_draining_worker(void)
{
	int i, max_workers;

	pthread_mutex_lock(&governor_state.lock);
	max_workers = governor_state.current_workers;
	pthread_mutex_unlock(&governor_state.lock);

	for (i = 0; i < max_workers; i++) {
		char name[64];
		int state = 0;

		snprintf(name, sizeof(name),
		    "net.graph.pppoe_lb.workers.%d.state", i);
		if (read_sysctl_int(name, &state) == -1)
			continue;

		if (state == NG_PPPOE_LB_WORKER_DRAINING)
			return (i);
	}

	return (-1);
}

/* Main governor poll function - called periodically */
static void
governor_poll(void)
{
	time_t now;
	int cpu, sessions, workers;
	int max_workers, min_workers;
	int avg_sessions_per_worker;
	int should_scale_up = 0, should_scale_down = 0;
	int scale_reason = 0;  /* 1=cpu_high, 2=cpu_low, 3=sessions_high, 4=sessions_low */
	int draining_worker;

	pthread_mutex_lock(&governor_state.lock);
	now = time(NULL);

	/* Skip if not enough time since last scale action */
	if (governor_state.last_scale_up > 0 &&
	    (now - governor_state.last_scale_up) < governor_config.scale_up_interval) {
		pthread_mutex_unlock(&governor_state.lock);
		return;
	}

	if (governor_state.last_scale_down > 0 &&
	    (now - governor_state.last_scale_down) < governor_config.scale_down_interval) {
		pthread_mutex_unlock(&governor_state.lock);
		return;
	}

	pthread_mutex_unlock(&governor_state.lock);

	/* Update state from kernel */
	governor_update_state();

	pthread_mutex_lock(&governor_state.lock);
	cpu = governor_state.cpu_usage;
	sessions = governor_state.total_sessions;
	workers = governor_state.current_workers;
	max_workers = get_max_workers();
	min_workers = governor_config.min_workers;
	pthread_mutex_unlock(&governor_state.lock);

	avg_sessions_per_worker = (workers > 0) ?
	    (sessions / workers) : governor_config.sessions_per_worker;

	/* Scale up decision */
	if (workers < max_workers) {
		/* Check CPU threshold */
		if (cpu > governor_config.cpu_threshold) {
			should_scale_up = 1;
			scale_reason = 1;  /* cpu_high */
		}
		/* Check session threshold (80% of target) */
		else if (avg_sessions_per_worker >
		    (governor_config.sessions_per_worker * 8 / 10)) {
			should_scale_up = 1;
			scale_reason = 3;  /* sessions_high */
		}
	}

	/* Scale down decision */
	if (workers > min_workers) {
		/* Check CPU low threshold */
		if (cpu < governor_config.cpu_low_threshold) {
			should_scale_down = 1;
			scale_reason = 2;  /* cpu_low */
		}
		/* Check session threshold (30% of target) */
		else if (workers > 1 &&
		    avg_sessions_per_worker <
		    (governor_config.sessions_per_worker * 3 / 10)) {
			should_scale_down = 1;
			scale_reason = 4;  /* sessions_low */
		}
	}

	/* Execute scaling decisions */
	if (should_scale_up) {
		/* "Change mind" logic: cancel any pending drains first */
		draining_worker = governor_find_draining_worker();
		if (draining_worker >= 0) {
			pthread_mutex_lock(&governor_state.lock);
			governor_state.last_decision = 3;  /* cancel_drain */
			governor_state.last_reason = scale_reason;
			pthread_mutex_unlock(&governor_state.lock);

			if (governor_cancel_drain(draining_worker) == 0) {
				syslog(LOG_INFO,
				    "governor: scale up (%s) - canceled drain for worker %d",
				    reason_str[scale_reason], draining_worker);
			}
		}

		/* Add new worker (even if we had a draining worker, cancel-drain
		 * just makes it active again; we still need additional capacity) */
		pthread_mutex_lock(&governor_state.lock);
		governor_state.last_decision = 1;  /* scale_up */
		governor_state.last_reason = scale_reason;
		governor_state.last_scale_up = now;
		int new_wid = governor_state.current_workers;
		pthread_mutex_unlock(&governor_state.lock);

		if (governor_add_worker(new_wid) == 0) {
			syslog(LOG_INFO,
			    "governor: scale up (%s) - created worker %d",
			    reason_str[scale_reason], new_wid);
		}
	} else if (should_scale_down) {
		/* Find worker to drain */
		draining_worker = governor_find_worker_to_remove();
		if (draining_worker >= 0) {
			pthread_mutex_lock(&governor_state.lock);
			governor_state.last_decision = 2;  /* scale_down */
			governor_state.last_reason = scale_reason;
			governor_state.last_scale_down = now;
			pthread_mutex_unlock(&governor_state.lock);

			if (governor_remove_worker(draining_worker) == 0) {
				syslog(LOG_INFO,
				    "governor: scale down (%s) - marked worker %d for drain",
				    reason_str[scale_reason], draining_worker);
			}
		}
	} else {
		/* No action taken */
		pthread_mutex_lock(&governor_state.lock);
		governor_state.last_decision = 0;  /* none */
		governor_state.last_reason = 0;
		pthread_mutex_unlock(&governor_state.lock);
	}
}

/* Governor thread entry point */
static void *
governor_thread_main(void *arg)
{
	(void)arg;

	syslog(LOG_INFO, "governor: thread started, poll interval %ds",
	    governor_config.poll_interval);

	while (governor_running) {
		sleep(governor_config.poll_interval);

		if (!governor_running)
			break;

		/* Only poll if governor is enabled */
		pthread_mutex_lock(&governor_state.lock);
		if (!governor_config.enabled || governor_config.mode != 1) {
			pthread_mutex_unlock(&governor_state.lock);
			continue;
		}
		pthread_mutex_unlock(&governor_state.lock);

		governor_poll();
	}

	syslog(LOG_INFO, "governor: thread exiting");
	return (NULL);
}

/* Start the governor thread */
static int
governor_start(int netgraph_fd, const char *path)
{
	int ret;

	/* Save netgraph socket and path for governor thread */
	cs_fd = netgraph_fd;
	strlcpy(lb_path, path, sizeof(lb_path));

	/* Initialize state mutex */
	pthread_mutex_init(&governor_state.lock, NULL);

	/* Initialize timestamps */
	governor_state.last_scale_up = 0;
	governor_state.last_scale_down = 0;
	governor_state.last_poll = 0;
	governor_state.last_decision = 0;
	governor_state.last_reason = 0;

	/* Set max_workers to CPU count if not specified */
	if (governor_config.max_workers == 0) {
		governor_config.max_workers = get_cpu_count();
		syslog(LOG_INFO, "governor: max_workers auto-set to %d (CPU cores)",
		    governor_config.max_workers);
	}

	governor_running = 1;

	/* Create governor thread */
	ret = pthread_create(&governor_thread, NULL, governor_thread_main, NULL);
	if (ret != 0) {
		syslog(LOG_ERR, "governor: pthread_create failed: %m");
		return (-1);
	}

	syslog(LOG_INFO, "governor: started (mode=%s, min=%d, max=%d, poll=%ds)",
	    governor_config.mode ? "auto" : "manual",
	    governor_config.min_workers,
	    governor_config.max_workers,
	    governor_config.poll_interval);

	return (0);
}

/* Stop the governor thread */
static void
governor_stop(void)
{
	if (!governor_running)
		return;

	governor_running = 0;

	/* Cancel and join thread */
	pthread_join(governor_thread, NULL);

	pthread_mutex_destroy(&governor_state.lock);

	syslog(LOG_INFO, "governor: stopped");
}

/* Configure kernel governor via sysctl */
static void
governor_configure_kernel(void)
{
	char buf[64];

	/* Set governor enabled */
	snprintf(buf, sizeof(buf), "net.graph.pppoe_lb.governor.enabled=%d",
	    governor_config.enabled);
	sysctlbyname(buf, NULL, NULL, &governor_config.enabled, sizeof(governor_config.enabled));

	/* Set governor mode */
	snprintf(buf, sizeof(buf), "net.graph.pppoe_lb.governor.mode=%d",
	    governor_config.mode);
	sysctlbyname(buf, NULL, NULL, &governor_config.mode, sizeof(governor_config.mode));

	/* Set min_workers */
	snprintf(buf, sizeof(buf), "net.graph.pppoe_lb.governor.min_workers=%d",
	    governor_config.min_workers);
	sysctlbyname(buf, NULL, NULL, &governor_config.min_workers, sizeof(governor_config.min_workers));

	/* Set max_workers (0 = auto) */
	snprintf(buf, sizeof(buf), "net.graph.pppoe_lb.governor.max_workers=%d",
	    governor_config.max_workers);
	sysctlbyname(buf, NULL, NULL, &governor_config.max_workers, sizeof(governor_config.max_workers));

	/* Set poll_interval */
	snprintf(buf, sizeof(buf), "net.graph.pppoe_lb.governor.poll_interval=%d",
	    governor_config.poll_interval);
	sysctlbyname(buf, NULL, NULL, &governor_config.poll_interval, sizeof(governor_config.poll_interval));

	/* Set CPU thresholds */
	snprintf(buf, sizeof(buf), "net.graph.pppoe_lb.governor.cpu_threshold=%d",
	    governor_config.cpu_threshold);
	sysctlbyname(buf, NULL, NULL, &governor_config.cpu_threshold, sizeof(governor_config.cpu_threshold));

	snprintf(buf, sizeof(buf), "net.graph.pppoe_lb.governor.cpu_low_threshold=%d",
	    governor_config.cpu_low_threshold);
	sysctlbyname(buf, NULL, NULL, &governor_config.cpu_low_threshold, sizeof(governor_config.cpu_low_threshold));

	/* Set sessions_per_worker */
	snprintf(buf, sizeof(buf), "net.graph.pppoe_lb.governor.sessions_per_worker=%d",
	    governor_config.sessions_per_worker);
	sysctlbyname(buf, NULL, NULL, &governor_config.sessions_per_worker, sizeof(governor_config.sessions_per_worker));

	/* Set drain_timeout */
	snprintf(buf, sizeof(buf), "net.graph.pppoe_lb.governor.drain_timeout=%d",
	    governor_config.drain_timeout);
	sysctlbyname(buf, NULL, NULL, &governor_config.drain_timeout, sizeof(governor_config.drain_timeout));

	syslog(LOG_INFO, "governor: configured kernel governor");
}

static int
ConfigureNode(const char *prog, const char *iface, const char *provider,
              int cs, int ds, int debug, struct ngm_connect *ngc,
              int use_load_balancer, int num_workers, int algorithm, int max_workers)
{
  /*
   * Single-threaded mode (use_load_balancer=0):
   * .---------.
   * |  ether  |
   * | <iface> |
   * `---------'
   *  (orphan)                                     ds    cs
   *     |                                         |     |
   *     |                                         |     |
   * (ethernet)                                    |     |
   * .---------.                                .-----------.
   * |  pppoe  |                                |  socket   |
   * | <iface> |(pppoe-<pid>)<---->(pppoe-<pid>)| <unnamed> |
   * `---------                                 `-----------'
   *
   * Multi-threaded mode (use_load_balancer=1):
   * .---------.
   * |  ether  |
   * | <iface> |
   * `---------'
   *     |
   *     v
   * .-------------.
   * |  pppoe_lb   |  (load balancer)
   * `-------------'
   *   /    |    \
   *  v     v     v
   * w0    w1    w2 ... wN  (worker ng_pppoe nodes)
   *
   * where there are potentially many ppp processes running off of the
   * same PPPoE node.
   * The exec-<pid> hook isn't made 'till we Spawn().
   */

  char *epath, *spath, *lbpath, *workerpath;
  struct ngpppoe_init_data *data;
  const struct hooklist *hlist;
  const struct nodeinfo *ninfo;
  const struct linkinfo *nlink;
  struct ngm_mkpeer mkp;
  struct ng_mesg *resp;
  struct ng_pppoe_lb_config cfg;
  u_char rbuf[2048];
  int f, plen, i;

  /*
   * Ask for a list of hooks attached to the "ether" node.  This node should
   * magically exist as a way of hooking stuff onto an ethernet device
   */
  epath = (char *)alloca(strlen(iface) + 2);
  sprintf(epath, "%s:", iface);

  if (debug)
    fprintf(stderr, "Sending NGM_LISTHOOKS to %s\n", epath);

  if (NgSendMsg(cs, epath, NGM_GENERIC_COOKIE, NGM_LISTHOOKS, NULL, 0) < 0) {
    if (errno == ENOENT)
      fprintf(stderr, "%s Cannot send a netgraph message: Invalid interface\n",
              epath);
    else
      fprintf(stderr, "%s Cannot send a netgraph message: %s\n",
              epath, strerror(errno));
    return EX_UNAVAILABLE;
  }

  /* Get our list back */
  resp = (struct ng_mesg *)rbuf;
  if (NgRecvMsg(cs, resp, sizeof rbuf, NULL) <= 0) {
    perror("Cannot get netgraph response");
    return EX_UNAVAILABLE;
  }

  hlist = (const struct hooklist *)resp->data;
  ninfo = &hlist->nodeinfo;

  if (debug)
    fprintf(stderr, "Got reply from id [%x]: Type %s with %d hooks\n",
            ninfo->id, ninfo->type, ninfo->hooks);

  /* Make sure we've got the right type of node */
  if (strncmp(ninfo->type, NG_ETHER_NODE_TYPE, sizeof NG_ETHER_NODE_TYPE - 1)) {
    fprintf(stderr, "%s Unexpected node type ``%s'' (wanted ``"
            NG_ETHER_NODE_TYPE "'')\n", epath, ninfo->type);
    return EX_DATAERR;
  }

  if (use_load_balancer && num_workers > 1) {
    /*
     * Multi-threaded mode: Create load balancer and workers
     */
    if (debug)
      fprintf(stderr, "Creating load balancer with %d workers\n", num_workers);

    /* Create load balancer node */
    snprintf(mkp.type, sizeof mkp.type, "%s", "pppoe_lb");
    snprintf(mkp.ourhook, sizeof mkp.ourhook, "%s", NG_ETHER_HOOK_ORPHAN);
    snprintf(mkp.peerhook, sizeof mkp.peerhook, "%s", NG_PPPOE_LB_HOOK_ETHER);

    if (debug)
      fprintf(stderr, "Send MKPEER: %s%s -> [type %s]:%s\n", epath,
              mkp.ourhook, mkp.type, mkp.peerhook);

    if (NgSendMsg(cs, epath, NGM_GENERIC_COOKIE,
                  NGM_MKPEER, &mkp, sizeof mkp) < 0) {
      fprintf(stderr, "%s Cannot create a peer PPPoE load balancer node: %s\n",
              epath, strerror(errno));
      return EX_OSERR;
    }

    /* Create worker nodes */
    for (i = 0; i < num_workers; i++) {
      lbpath = (char *)alloca(strlen(iface) + 32);
      sprintf(lbpath, "%s%s:", epath, NG_ETHER_HOOK_ORPHAN);

      snprintf(mkp.type, sizeof mkp.type, "%s", NG_PPPOE_NODE_TYPE);
      snprintf(mkp.ourhook, sizeof mkp.ourhook, "%s%d", NG_PPPOE_LB_HOOK_WORKER_BASE, i);
      snprintf(mkp.peerhook, sizeof mkp.peerhook, "%s", NG_PPPOE_HOOK_ETHERNET);

      if (debug)
        fprintf(stderr, "Send MKPEER: %s%s -> [type %s]:%s\n", lbpath,
                mkp.ourhook, mkp.type, mkp.peerhook);

      if (NgSendMsg(cs, lbpath, NGM_GENERIC_COOKIE,
                    NGM_MKPEER, &mkp, sizeof mkp) < 0) {
        fprintf(stderr, "%s Cannot create worker %d PPPoE node: %s\n",
                lbpath, i, strerror(errno));
        return EX_OSERR;
      }
    }

    /* Configure load balancer */
    lbpath = (char *)alloca(strlen(iface) + 32);
    sprintf(lbpath, "%s%s:", epath, NG_ETHER_HOOK_ORPHAN);

    cfg.algorithm = algorithm;
    cfg.max_workers = max_workers;
    cfg.debug_level = debug ? 1 : 0;

    if (debug)
      fprintf(stderr, "Configuring load balancer: algorithm=%d, max_workers=%d\n",
              algorithm, max_workers);

    if (NgSendMsg(cs, lbpath, NGM_PPPOE_LB_COOKIE,
                  NGM_PPPOE_LB_SET_CONFIG, &cfg, sizeof cfg) < 0) {
      fprintf(stderr, "%s Cannot configure load balancer: %s\n",
              lbpath, strerror(errno));
      return EX_OSERR;
    }

    /* Connect socket to load balancer */
    snprintf(ngc->path, sizeof ngc->path, "%s%s", epath, NG_ETHER_HOOK_ORPHAN);
    snprintf(ngc->ourhook, sizeof ngc->ourhook, "pppoe-%ld", (long)getpid());
    memcpy(ngc->peerhook, ngc->ourhook, sizeof ngc->peerhook);

    if (NgSendMsg(cs, ".:", NGM_GENERIC_COOKIE,
                  NGM_CONNECT, ngc, sizeof *ngc) < 0) {
      perror("Cannot CONNECT PPPoE load balancer and socket nodes");
      return EX_OSERR;
    }

    plen = strlen(provider);
    data = (struct ngpppoe_init_data *)alloca(sizeof *data + plen);
    snprintf(data->hook, sizeof data->hook, "%s", ngc->peerhook);
    memcpy(data->data, provider, plen);
    data->data_len = plen;

    spath = (char *)alloca(strlen(ngc->peerhook) + 3);
    strcpy(spath, ".:");
    strcpy(spath + 2, ngc->ourhook);

    if (debug) {
      if (provider)
        fprintf(stderr, "Sending PPPOE_LISTEN to %s (load balancer), provider %s\n",
                spath, provider);
      else
        fprintf(stderr, "Sending PPPOE_LISTEN to %s (load balancer)\n", spath);
    }

    if (NgSendMsg(cs, spath, NGM_PPPOE_COOKIE, NGM_PPPOE_LISTEN,
                  data, sizeof *data + plen) == -1) {
      fprintf(stderr, "%s: Cannot LISTEN on netgraph load balancer node: %s\n",
              spath, strerror(errno));
      return EX_OSERR;
    }
  } else {
    /*
     * Single-threaded mode: Use existing behavior
     */
    /* look for a hook already attached.  */
    for (f = 0; f < ninfo->hooks; f++) {
      nlink = &hlist->link[f];

      if (debug)
        fprintf(stderr, "  Got [%x]:%s -> [%x]:%s\n", ninfo->id,
                nlink->ourhook, nlink->nodeinfo.id, nlink->peerhook);

      if (!strcmp(nlink->ourhook, NG_ETHER_HOOK_ORPHAN) ||
          !strcmp(nlink->ourhook, NG_ETHER_HOOK_DIVERT)) {
        /*
         * Something is using the data coming out of this `ether' node.
         * If it's a PPPoE node, we use that node, otherwise we complain that
         * someone else is using the node.
         */
        if (strcmp(nlink->nodeinfo.type, NG_PPPOE_NODE_TYPE)) {
          fprintf(stderr, "%s Node type %s is currently active\n",
                  epath, nlink->nodeinfo.type);
          return EX_UNAVAILABLE;
        }
        break;
      }
    }

    if (f == ninfo->hooks) {
      /*
       * Create a new PPPoE node connected to the `ether' node using
       * the magic `orphan' and `ethernet' hooks
       */
      snprintf(mkp.type, sizeof mkp.type, "%s", NG_PPPOE_NODE_TYPE);
      snprintf(mkp.ourhook, sizeof mkp.ourhook, "%s", NG_ETHER_HOOK_ORPHAN);
      snprintf(mkp.peerhook, sizeof mkp.peerhook, "%s", NG_PPPOE_HOOK_ETHERNET);

      if (debug)
        fprintf(stderr, "Send MKPEER: %s%s -> [type %s]:%s\n", epath,
                mkp.ourhook, mkp.type, mkp.peerhook);

      if (NgSendMsg(cs, epath, NGM_GENERIC_COOKIE,
                    NGM_MKPEER, &mkp, sizeof mkp) < 0) {
        fprintf(stderr, "%s Cannot create a peer PPPoE node: %s\n",
                epath, strerror(errno));
        return EX_OSERR;
      }
    }

    /* Connect the PPPoE node to our socket node.  */
    snprintf(ngc->path, sizeof ngc->path, "%s%s", epath, NG_ETHER_HOOK_ORPHAN);
    snprintf(ngc->ourhook, sizeof ngc->ourhook, "pppoe-%ld", (long)getpid());
    memcpy(ngc->peerhook, ngc->ourhook, sizeof ngc->peerhook);

    if (NgSendMsg(cs, ".:", NGM_GENERIC_COOKIE,
                  NGM_CONNECT, ngc, sizeof *ngc) < 0) {
      perror("Cannot CONNECT PPPoE and socket nodes");
      return EX_OSERR;
    }

    plen = strlen(provider);

    data = (struct ngpppoe_init_data *)alloca(sizeof *data + plen);
    snprintf(data->hook, sizeof data->hook, "%s", ngc->peerhook);
    memcpy(data->data, provider, plen);
    data->data_len = plen;

    spath = (char *)alloca(strlen(ngc->peerhook) + 3);
    strcpy(spath, ".:");
    strcpy(spath + 2, ngc->ourhook);

    if (debug) {
      if (provider)
        fprintf(stderr, "Sending PPPOE_LISTEN to %s, provider %s\n",
                spath, provider);
      else
        fprintf(stderr, "Sending PPPOE_LISTEN to %s\n", spath);
    }

    if (NgSendMsg(cs, spath, NGM_PPPOE_COOKIE, NGM_PPPOE_LISTEN,
                  data, sizeof *data + plen) == -1) {
      fprintf(stderr, "%s: Cannot LISTEN on netgraph node: %s\n",
              spath, strerror(errno));
      return EX_OSERR;
    }
  }

  return 0;
}

static void
Spawn(const char *prog, const char *acname, const char *provider,
      const char *exec, struct ngm_connect ngc, int cs, int ds, void *request,
      int sz, int debug)
{
  char msgbuf[sizeof(struct ng_mesg) + sizeof(struct ngpppoe_sts)];
  struct ng_mesg *rep = (struct ng_mesg *)msgbuf;
  struct ngpppoe_sts *sts = (struct ngpppoe_sts *)(msgbuf + sizeof *rep);
  struct ngpppoe_init_data *data;
  char env[18], unknown[14], sessionid[5], *path;
  unsigned char *macaddr;
  const char *msg;
  int ret, slen;

  switch ((ret = fork())) {
    case -1:
      syslog(LOG_ERR, "fork: %m");
      break;

    case 0:
      switch (fork()) {
        case 0:
          break;
        case -1:
          _exit(errno);
        default:
          _exit(0);
      }
      close(cs);
      close(ds);

      /* Create a new socket node */
      if (debug)
        syslog(LOG_INFO, "Creating a new socket node");

      if (NgMkSockNode(NULL, &cs, &ds) == -1) {
        syslog(LOG_ERR, "Cannot create netgraph socket node: %m");
        _exit(EX_CANTCREAT);
      }

      /* Connect the PPPoE node to our new socket node.  */
      snprintf(ngc.ourhook, sizeof ngc.ourhook, "exec-%ld", (long)getpid());
      memcpy(ngc.peerhook, ngc.ourhook, sizeof ngc.peerhook);

      if (debug)
        syslog(LOG_INFO, "Sending CONNECT from .:%s -> %s.%s",
               ngc.ourhook, ngc.path, ngc.peerhook);
      if (NgSendMsg(cs, ".:", NGM_GENERIC_COOKIE,
                    NGM_CONNECT, &ngc, sizeof ngc) < 0) {
        syslog(LOG_ERR, "Cannot CONNECT PPPoE and socket nodes: %m");
        _exit(EX_OSERR);
      }

      /*
       * If we tell the socket node not to LINGER, it will go away when
       * the last hook is removed.
       */
      if (debug)
        syslog(LOG_INFO, "Sending NGM_SOCK_CMD_NOLINGER to socket");
      if (NgSendMsg(cs, ".:", NGM_SOCKET_COOKIE,
                    NGM_SOCK_CMD_NOLINGER, NULL, 0) < 0) {
        syslog(LOG_ERR, "Cannot send NGM_SOCK_CMD_NOLINGER: %m");
        _exit(EX_OSERR);
      }

      /* Put the PPPoE node into OFFER mode */
      slen = strlen(acname);
      data = (struct ngpppoe_init_data *)alloca(sizeof *data + slen);
      snprintf(data->hook, sizeof data->hook, "%s", ngc.ourhook);
      memcpy(data->data, acname, slen);
      data->data_len = slen;

      path = (char *)alloca(strlen(ngc.ourhook) + 3);
      strcpy(path, ".:");
      strcpy(path + 2, ngc.ourhook);

      syslog(LOG_INFO, "Offering to %s as access concentrator %s",
             path, acname);
      if (NgSendMsg(cs, path, NGM_PPPOE_COOKIE, NGM_PPPOE_OFFER,
                    data, sizeof *data + slen) == -1) {
        syslog(LOG_INFO, "%s: Cannot OFFER on netgraph node: %m", path);
        _exit(EX_OSERR);
      }
      /* If we have a provider code, set it */
      if (provider) {
        slen = strlen(provider);
        data = (struct ngpppoe_init_data *)alloca(sizeof *data + slen);
        snprintf(data->hook, sizeof data->hook, "%s", ngc.ourhook);
        memcpy(data->data, provider, slen);
        data->data_len = slen;

        syslog(LOG_INFO, "adding to %s as offered service %s",
             path, acname);
        if (NgSendMsg(cs, path, NGM_PPPOE_COOKIE, NGM_PPPOE_SERVICE,
                    data, sizeof *data + slen) == -1) {
          syslog(LOG_INFO, "%s: Cannot add service on netgraph node: %m", path);
          _exit(EX_OSERR);
        }
      }

      /* Put the peer's MAC address in the environment */
      if (sz >= sizeof(struct ether_header)) {
        macaddr = ((struct ether_header *)request)->ether_shost;
        snprintf(env, sizeof(env), "%x:%x:%x:%x:%x:%x",
                 macaddr[0], macaddr[1], macaddr[2], macaddr[3], macaddr[4],
                 macaddr[5]);
        if (setenv(HISMACADDR, env, 1) != 0)
          syslog(LOG_INFO, "setenv: cannot set %s: %m", HISMACADDR);
      }

      /* And send our request data to the waiting node */
      if (debug)
        syslog(LOG_INFO, "Sending original request to %s (%d bytes)", path, sz);
      if (NgSendData(ds, ngc.ourhook, request, sz) == -1) {
        syslog(LOG_ERR, "Cannot send original request to %s: %m", path);
        _exit(EX_OSERR);
      }

      /* Then wait for a success indication */

      if (debug)
        syslog(LOG_INFO, "Waiting for a SUCCESS reply %s", path);

      do {
        if ((ret = NgRecvMsg(cs, rep, sizeof msgbuf, NULL)) < 0) {
          syslog(LOG_ERR, "%s: Cannot receive a message: %m", path);
          _exit(EX_OSERR);
        }

        if (ret == 0) {
          /* The socket has been closed */
          syslog(LOG_INFO, "%s: Client timed out", path);
          _exit(EX_TEMPFAIL);
        }

        if (rep->header.version != NG_VERSION) {
          syslog(LOG_ERR, "%ld: Unexpected netgraph version, expected %ld",
                 (long)rep->header.version, (long)NG_VERSION);
          _exit(EX_PROTOCOL);
        }

        if (rep->header.typecookie != NGM_PPPOE_COOKIE) {
          syslog(LOG_INFO, "%ld: Unexpected netgraph cookie, expected %ld",
                 (long)rep->header.typecookie, (long)NGM_PPPOE_COOKIE);
          continue;
        }

        switch (rep->header.cmd) {
          case NGM_PPPOE_SET_FLAG:	msg = "SET_FLAG";	break;
          case NGM_PPPOE_CONNECT:	msg = "CONNECT";	break;
          case NGM_PPPOE_LISTEN:	msg = "LISTEN";		break;
          case NGM_PPPOE_OFFER:		msg = "OFFER";		break;
          case NGM_PPPOE_SUCCESS:	msg = "SUCCESS";	break;
          case NGM_PPPOE_FAIL:		msg = "FAIL";		break;
          case NGM_PPPOE_CLOSE:		msg = "CLOSE";		break;
          case NGM_PPPOE_GET_STATUS:	msg = "GET_STATUS";	break;
          case NGM_PPPOE_ACNAME:
            msg = "ACNAME";
            if (setenv("ACNAME", sts->hook, 1) != 0)
              syslog(LOG_WARNING, "setenv: cannot set ACNAME=%s: %m",
                     sts->hook);
            break;
          case NGM_PPPOE_SESSIONID:
            msg = "SESSIONID";
            snprintf(sessionid, sizeof sessionid, "%04x", *(u_int16_t *)sts);
            if (setenv("SESSIONID", sessionid, 1) != 0)
              syslog(LOG_WARNING, "setenv: cannot set SESSIONID=%s: %m",
                     sessionid);
            break;
          default:
            snprintf(unknown, sizeof unknown, "<%d>", (int)rep->header.cmd);
            msg = unknown;
            break;
        }

        switch (rep->header.cmd) {
          case NGM_PPPOE_FAIL:
          case NGM_PPPOE_CLOSE:
            syslog(LOG_ERR, "Received NGM_PPPOE_%s (hook \"%s\")",
                   msg, sts->hook);
            _exit(0);
        }

        syslog(LOG_INFO, "Received NGM_PPPOE_%s (hook \"%s\")", msg, sts->hook);
      } while (rep->header.cmd != NGM_PPPOE_SUCCESS);

      dup2(ds, STDIN_FILENO);
      dup2(ds, STDOUT_FILENO);
      close(ds);
      close(cs);

      setsid();
      syslog(LOG_INFO, "Executing: %s", exec);
      execlp(_PATH_BSHELL, _PATH_BSHELL, "-c", exec, (char *)NULL);
      syslog(LOG_ERR, "execlp failed: %m");
      _exit(EX_OSFILE);

    default:
      wait(&ret);
      errno = ret;
      if (errno)
        syslog(LOG_ERR, "Second fork failed: %m");
      break;
  }
}

#ifndef NOKLDLOAD
static int
LoadModules(int use_load_balancer)
{
  const char *module[] = { "netgraph", "ng_socket", "ng_ether", "ng_pppoe" };
  int f, num_modules;

  num_modules = sizeof module / sizeof *module;

  for (f = 0; f < num_modules; f++)
    if (modfind(module[f]) == -1 && kldload(module[f]) == -1) {
      fprintf(stderr, "kldload: %s: %s\n", module[f], strerror(errno));
      return 0;
    }

  /* Load load balancer module if requested */
  if (use_load_balancer) {
    if (modfind("ng_pppoe_lb") == -1 && kldload("ng_pppoe_lb") == -1) {
      fprintf(stderr, "kldload: ng_pppoe_lb: %s\n", strerror(errno));
      return 0;
    }
  }

  return 1;
}
#endif

static void
nglog(const char *fmt, ...)
{
  char nfmt[256];
  va_list ap;

  snprintf(nfmt, sizeof nfmt, "%s: %s", fmt, strerror(errno));
  va_start(ap, fmt);
  vsyslog(LOG_INFO, nfmt, ap);
  va_end(ap);
}

static void
nglogx(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vsyslog(LOG_INFO, fmt, ap);
  va_end(ap);
}

int
main(int argc, char *argv[])
{
  char hostname[MAXHOSTNAMELEN], *exec, rhook[NG_HOOKSIZ];
  unsigned char response[1024];
  const char *label, *prog, *provider, *acname;
  struct ngm_connect ngc;
  struct sigaction act;
  int ch, cs, ds, ret, optF, optd, optn, sz, f;
  const char *pidfile;
  int optL, optw, optA, optG, optM;
  int num_workers, algorithm, max_workers, min_workers;
  int optI, optc, optC;
  char lbpath[64];  /* Path to load balancer node */

  prog = strrchr(argv[0], '/');
  prog = prog ? prog + 1 : argv[0];
  pidfile = NULL;
  exec = NULL;
  label = NULL;
  acname = NULL;
  provider = "";
  optF = optd = optn = 0;
  optL = optw = optA = optG = optM = optI = optc = optC = 0;
  num_workers = 1;
  algorithm = NG_PPPOE_LB_ALGO_ROUND_ROBIN;
  max_workers = 0;
  min_workers = 1;

  while ((ch = getopt(argc, argv, "FP:a:de:l:n:p:Lw:A:G:M:I:c:C:")) != -1) {
    switch (ch) {
      case 'F':
        optF = 1;
        break;

      case 'P':
        pidfile = optarg;
        break;

      case 'a':
        acname = optarg;
        break;

      case 'd':
        optd = 1;
        break;

      case 'e':
        exec = optarg;
        break;

      case 'l':
        label = optarg;
        break;

      case 'n':
        optn = 1;
        NgSetDebug(atoi(optarg));
        break;

      case 'p':
        provider = optarg;
        break;

      case 'L':
        optL = 1;
        break;

      case 'w':
        optw = 1;
        num_workers = atoi(optarg);
        if (num_workers < 1) {
          fprintf(stderr, "%s: Invalid number of workers: %s\n", prog, optarg);
          return usage(prog);
        }
        break;

      case 'A':
        optA = 1;
        algorithm = atoi(optarg);
        if (algorithm < 0 || algorithm > 2) {
          fprintf(stderr, "%s: Invalid algorithm: %s (must be 0, 1, or 2)\n", prog, optarg);
          return usage(prog);
        }
        break;

      case 'G':
        optG = 1;
        governor_config.max_workers = atoi(optarg);
        if (governor_config.max_workers < 0) {
          fprintf(stderr, "%s: Invalid max_workers: %s\n", prog, optarg);
          return usage(prog);
        }
        max_workers = governor_config.max_workers;
        break;

      case 'M':
        optM = 1;
        governor_config.min_workers = atoi(optarg);
        if (governor_config.min_workers < 1) {
          fprintf(stderr, "%s: Invalid min_workers: %s\n", prog, optarg);
          return usage(prog);
        }
        min_workers = governor_config.min_workers;
        break;

      case 'I':
        optI = 1;
        governor_config.poll_interval = atoi(optarg);
        if (governor_config.poll_interval < 1 || governor_config.poll_interval > 60) {
          fprintf(stderr, "%s: Invalid poll_interval: %s (must be 1-60)\n", prog, optarg);
          return usage(prog);
        }
        break;

      case 'c':
        optc = 1;
        governor_config.cpu_threshold = atoi(optarg);
        if (governor_config.cpu_threshold < 1 || governor_config.cpu_threshold > 100) {
          fprintf(stderr, "%s: Invalid cpu_threshold: %s (must be 1-100)\n", prog, optarg);
          return usage(prog);
        }
        break;

      case 'C':
        optC = 1;
        governor_config.cpu_low_threshold = atoi(optarg);
        if (governor_config.cpu_low_threshold < 1 || governor_config.cpu_low_threshold > 100) {
          fprintf(stderr, "%s: Invalid cpu_low_threshold: %s (must be 1-100)\n", prog, optarg);
          return usage(prog);
        }
        break;

      default:
        return usage(prog);
    }
  }

  /* Enable governor in auto mode if load balancer is used without explicit governor config */
  if (optL && !optG && !optM && !optI && !optc && !optC) {
    /* Auto-enable governor with defaults when using -L */
    governor_config.enabled = 1;
    governor_config.mode = 1;
  }

  if (optind >= argc || optind + 2 < argc)
    return usage(prog);

  if (exec != NULL && label != NULL)
    return usage(prog);

  if (exec == NULL) {
    if (label == NULL)
      label = provider;
    if (label == NULL) {
      fprintf(stderr, "%s: Either a provider, a label or an exec command"
              " must be given\n", prog);
      return usage(prog);
    }
    exec = (char *)alloca(sizeof DEFAULT_EXEC_PREFIX + strlen(label));
    if (exec == NULL) {
      fprintf(stderr, "%s: Cannot allocate %zu bytes\n", prog,
              sizeof DEFAULT_EXEC_PREFIX + strlen(label));
      return EX_OSERR;
    }
    strcpy(exec, DEFAULT_EXEC_PREFIX);
    strcpy(exec + sizeof DEFAULT_EXEC_PREFIX - 1, label);
  }

  if (acname == NULL) {
    char *dot;

    if (gethostname(hostname, sizeof hostname))
      strcpy(hostname, "localhost");
    else if ((dot = strchr(hostname, '.')))
      *dot = '\0';

    acname = hostname;
  }

#ifndef NOKLDLOAD
  if (!LoadModules(optL))
    return EX_UNAVAILABLE;
#endif

  /* Create a socket node */
  if (NgMkSockNode(NULL, &cs, &ds) == -1) {
    perror("Cannot create netgraph socket node");
    return EX_CANTCREAT;
  }

  /* Connect it up (and fill in `ngc') */
  if ((ret = ConfigureNode(prog, argv[optind], provider, cs, ds,
                           optd, &ngc, optL, num_workers, algorithm, max_workers)) != 0) {
    close(cs);
    close(ds);
    return ret;
  }

  /* Build load balancer path for governor */
  if (optL) {
    snprintf(lbpath, sizeof(lbpath), "%s%s:", argv[optind], NG_ETHER_HOOK_ORPHAN);
  }

  if (!optF && daemon(1, 0) == -1) {
    perror("daemon()");
    close(cs);
    close(ds);
    return EX_OSERR;
  }


  if (pidfile != NULL) {
    FILE *fp;

    if ((fp = fopen(pidfile, "w")) == NULL) {
      perror(pidfile);
      close(cs);
      close(ds);
      return EX_CANTCREAT;
    } else {
      fprintf(fp, "%d\n", (int)getpid());
      fclose(fp);
    }
  }

  openlog(prog, LOG_PID | (optF ? LOG_PERROR : 0), LOG_DAEMON);
  if (!optF && optn)
    NgSetErrLog(nglog, nglogx);

  /* Start governor thread if enabled */
  if (optL && governor_config.enabled) {
    /* Configure kernel governor sysctls */
    governor_configure_kernel();

    /* Start governor polling thread */
    if (governor_start(cs, lbpath) != 0) {
      syslog(LOG_ERR, "Failed to start governor thread");
      close(cs);
      close(ds);
      return EX_OSERR;
    }
  }

  memset(&act, '\0', sizeof act);
  act.sa_handler = Farewell;
  act.sa_flags = 0;
  sigemptyset(&act.sa_mask);
  sigaction(SIGHUP, &act, NULL);
  sigaction(SIGINT, &act, NULL);
  sigaction(SIGQUIT, &act, NULL);
  sigaction(SIGTERM, &act, NULL);

  while (!ReceivedSignal) {
    if (*provider)
      syslog(LOG_INFO, "Listening as provider %s", provider);
    else
      syslog(LOG_INFO, "Listening");

    switch (sz = NgRecvData(ds, response, sizeof response, rhook)) {
      case -1:
        syslog(LOG_INFO, "NgRecvData: %m");
        break;
      case 0:
        syslog(LOG_INFO, "NgRecvData: socket closed");
        break;
      default:
        if (optd) {
          char *dbuf, *ptr;

          ptr = dbuf = alloca(sz * 2 + 1);
          for (f = 0; f < sz; f++, ptr += 2)
            sprintf(ptr, "%02x", (u_char)response[f]);
          *ptr = '\0';
          syslog(LOG_INFO, "Got %d bytes of data: %s", sz, dbuf);
        }
    }
    if (sz <= 0) {
      ret = EX_UNAVAILABLE;
      break;
    }
    Spawn(prog, acname, provider, exec, ngc, cs, ds, response, sz, optd);
  }

  if (pidfile)
    remove(pidfile);

  /* Stop governor thread if running */
  governor_stop();

  if (ReceivedSignal) {
    syslog(LOG_INFO, "Received signal %d, exiting", ReceivedSignal);

    signal(ReceivedSignal, SIG_DFL);
    raise(ReceivedSignal);

    /* NOTREACHED */

    ret = -ReceivedSignal;
  }

  close(ds);
  return ret;
}
